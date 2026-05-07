/*  Copyright (C) Bitstem GmbH
 *  All rights reserved.
 *
 *  This source code is proprietary and confidential.
 *  Unauthorized copying, distribution, or disclosure of this file,
 *  in whole or in part, is strictly prohibited unless explicitly
 *  permitted in writing by Bitstem GmbH.
 *
 *  Author: Klaus Kuehnhammer <klaus@bitstem.com>
 */

// Encoder::send + FileTransmissionConfig contract tests.
//
// The caller-facing FEC parametrisation surface mirrors TS 26.346
// §7.2.10.1: caller hands the encoder a populated FecOti (plus the
// Rel-11 FEC-Redundancy-Level) and the encoder honours it on the
// per-file FDT entry. Sentinel fields (0 / empty / nullopt) keep the
// pre-populated-FecOti behaviour where the FEC transformer fills in
// scheme defaults.

#include "Encoder.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Decoder.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "flute_types.h"

namespace {

// Lightweight Encoder fixture — captures emitted packets so a test can
// verify the FDT XML the encoder produced after a send().
class CapturingEncoder {
public:
  CapturingEncoder()
      : _encoder(/*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
                 [this](std::span<const std::uint8_t> p) {
                   _packets.emplace_back(p.begin(), p.end());
                   return true;
                 }) {}

  LibFlute::Encoder& encoder() { return _encoder; }
  const std::vector<std::vector<std::uint8_t>>& packets() const {
    return _packets;
  }

private:
  std::vector<std::vector<std::uint8_t>> _packets;
  LibFlute::Encoder _encoder;
};

// Backwards-compat smoke test: bare FecScheme keeps working through
// the implicit FileTransmissionConfig conversion.
TEST(EncoderSend, ImplicitFromFecSchemeStillCompilesAndQueues) {
  CapturingEncoder cap;
  std::string payload = "hello";
  auto toi = cap.encoder().send(
      "x.bin", "application/octet-stream",
      LibFlute::Encoder::seconds_since_epoch() + 60, payload.data(),
      payload.size(), LibFlute::FecScheme::CompactNoCode,
      /*copy_buffer=*/true);
  EXPECT_NE(toi, 0U);
}

// Helper: extract the FDT XML out of the captured packet stream. The
// first emitted packet is always the FDT (TOI=0); rather than parse
// the LCT/ALC headers we just locate the XML root inside the bytes.
inline std::string ExtractFdtXml(const CapturingEncoder& cap) {
  for (const auto& p : cap.packets()) {
    std::string s(reinterpret_cast<const char*>(p.data()), p.size());
    auto pos = s.find("<FDT-Instance");
    if (pos != std::string::npos) {
      return s.substr(pos);
    }
  }
  return {};
}

// FEC-Redundancy-Level: TS 26.346 §7.3.2.11 / Rel-11 mbms2012
// attribute. The caller-supplied percent must round-trip into the
// FDT XML so the receiver can read it back.
TEST(EncoderSend, FecRedundancyLevelRoundTripsToFdtXml) {
  CapturingEncoder cap;

  LibFlute::FileTransmissionConfig cfg;
  cfg.scheme               = LibFlute::FecScheme::CompactNoCode;
  cfg.fec_redundancy_level = 15;

  std::string payload(2048, 'x');
  auto toi = cap.encoder().send(
      "x.bin", "application/octet-stream",
      LibFlute::Encoder::seconds_since_epoch() + 60, payload.data(),
      payload.size(), cfg, /*copy_buffer=*/true);
  ASSERT_NE(toi, 0U);
  cap.encoder().flush();

  const auto xml = ExtractFdtXml(cap);
  ASSERT_FALSE(xml.empty());
  EXPECT_NE(xml.find("FEC-Redundancy-Level=\"15\""), std::string::npos)
      << xml;
}

// FEC-OTI-FEC-Instance-ID: TS 26.346 §7.3.2.8 instance-id. When the
// caller leaves it at 0 (every fully-specified scheme libflute
// emits), the FDT serialiser MUST omit the attribute — spec marks
// it as optional and "absent ⇒ no instance ID applies".
TEST(EncoderSend, InstanceIdZeroOmittedFromFdtXml) {
  CapturingEncoder cap;

  LibFlute::FileTransmissionConfig cfg;
  cfg.scheme          = LibFlute::FecScheme::CompactNoCode;
  cfg.fec_instance_id = 0;

  std::string payload(1024, 'y');
  cap.encoder().send("y.bin", "application/octet-stream",
                     LibFlute::Encoder::seconds_since_epoch() + 60,
                     payload.data(), payload.size(), cfg,
                     /*copy_buffer=*/true);
  cap.encoder().flush();

  const auto xml = ExtractFdtXml(cap);
  ASSERT_FALSE(xml.empty());
  EXPECT_EQ(xml.find("FEC-OTI-FEC-Instance-ID"), std::string::npos)
      << "Instance-ID=0 should not be serialised to the FDT:\n" << xml;
}

// FEC-OTI-FEC-Instance-ID: non-zero round-trips to the FDT (forward
// compatibility with under-specified FEC schemes).
TEST(EncoderSend, NonZeroInstanceIdEmittedToFdtXml) {
  CapturingEncoder cap;

  LibFlute::FileTransmissionConfig cfg;
  cfg.scheme          = LibFlute::FecScheme::CompactNoCode;
  cfg.fec_instance_id = 7;

  std::string payload(1024, 'z');
  cap.encoder().send("z.bin", "application/octet-stream",
                     LibFlute::Encoder::seconds_since_epoch() + 60,
                     payload.data(), payload.size(), cfg,
                     /*copy_buffer=*/true);
  cap.encoder().flush();

  const auto xml = ExtractFdtXml(cap);
  ASSERT_FALSE(xml.empty());
  EXPECT_NE(xml.find("FEC-OTI-FEC-Instance-ID=\"7\""), std::string::npos)
      << xml;
}

#ifdef RAPTOR_ENABLED
// Parallel encode produces wire-identical packets to sequential — the
// worker pool only re-orders WHEN blocks are filled, not WHAT the
// encoded symbols look like, so a Decoder fed the parallel-encoder's
// output must reconstruct the file byte-for-byte.
//
// 12 MiB at MTU 1500 ⇒ K_total ≈ 8638, which spills into a 2-block
// partition (KL=4319 KS=4319). With workers=4 (clamped internally to
// min(N, Z)=2), both blocks fill in parallel, so this exercises the
// pool's claim/release dance — not just the single-worker degenerate
// case a sub-MiB file would produce.
TEST(EncoderSend, ParallelEncodeRoundTripsIdentically) {
  std::vector<char> payload(12u << 20);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>((i * 31U + 7U) & 0xFF);
  }

  auto run = [&](unsigned workers) -> std::vector<char> {
    LibFlute::Decoder decoder(/*tsi=*/16);
    std::shared_ptr<LibFlute::File> received;
    decoder.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) { received = std::move(f); });

    LibFlute::Encoder encoder(
        /*tsi=*/16, /*mtu=*/1500, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
          decoder.feed_packet(p);
          return true;
        },
        workers);
    auto data_copy = payload;
    encoder.send("parallel.bin", "application/octet-stream",
                 LibFlute::Encoder::seconds_since_epoch() + 60,
                 data_copy.data(), data_copy.size(),
                 LibFlute::FecScheme::Raptor, /*copy_buffer=*/true);
    encoder.flush();

    EXPECT_NE(received, nullptr);
    if (!received) return {};
    return std::vector<char>(received->buffer(),
                              received->buffer() + received->length());
  };

  const auto seq = run(0);
  const auto par = run(4);
  ASSERT_EQ(seq.size(), payload.size());
  ASSERT_EQ(par.size(), payload.size());
  EXPECT_EQ(seq, par)
      << "parallel encode (workers=4) reconstructed bytes differ from "
         "sequential — worker pool changed wire output, not just timing";
  EXPECT_EQ(par, std::vector<char>(payload.begin(), payload.end()))
      << "parallel-encoded file mismatches the source payload";
}

// FEC-Redundancy-Level (Rel-11) drives the runtime repair-symbol count.
// A higher redundancy_level emits more repair symbols per source block;
// this verifies the percent value the caller provided is honoured by
// the FEC transformer rather than overridden by a hardcoded constant.
TEST(EncoderSend, RaptorRedundancyLevelDrivesRepairSymbolCount) {
  auto run_with_redundancy = [](unsigned percent) -> std::uint64_t {
    CapturingEncoder cap;
    LibFlute::FileTransmissionConfig cfg;
    cfg.scheme               = LibFlute::FecScheme::Raptor;
    cfg.fec_redundancy_level = percent;

    // 200 kB file ⇒ Raptor partitions into a single block; the repair
    // count then directly reflects (target_K − K) ≈ K * percent / 100.
    std::string payload(200000, 'r');
    cap.encoder().send("r.bin", "application/octet-stream",
                       LibFlute::Encoder::seconds_since_epoch() + 60,
                       payload.data(), payload.size(), cfg,
                       /*copy_buffer=*/true);
    cap.encoder().flush();
    return cap.encoder().stats().repair_symbols_emitted;
  };

  const auto repair_5  = run_with_redundancy(5);
  const auto repair_50 = run_with_redundancy(50);
  EXPECT_GT(repair_50, repair_5)
      << "redundancy=50% should emit more repair symbols than 5%; got "
      << repair_50 << " vs " << repair_5;
  // A 50% repair budget should produce at minimum ~5x the repair the
  // 5% case does (round numbers are loose to absorb the integer K
  // rounding inside RaptorFEC's target_K formula).
  EXPECT_GE(repair_50, repair_5 * 5);
}
#endif  // RAPTOR_ENABLED

}  // namespace
