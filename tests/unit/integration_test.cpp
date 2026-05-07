// End-to-end round-trip integration tests.
//
// Drives the consumer-facing Encoder + Decoder surfaces directly:
// the encoder's PacketCallback shoves bytes straight into the
// decoder's feed_packet(). No sockets, no transport simulation, no
// loss model — this is "the parts work together correctly when
// nothing goes wrong on the wire". Loss / repair under the FEC
// schemes is the FEC layer's own test surface.
//
// Coverage is parameterised over (F, mtu, max_sbl) tuples that
// exercise the source-block-partitioning paths from RFC 5052 §9.1:
//   - F < T (single partial symbol)
//   - F = T (single full symbol)
//   - F = T * max_sbl (one full block exactly)
//   - F just over a block boundary (partial-last-block)
//   - F across many blocks (multi-block, partial residue)
//
// Raptor (RFC 5053) round-trips are gated on RAPTOR_ENABLED so the
// CompactNoCode tests stay green in non-Raptor builds.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "Decoder.h"
#include "Encoder.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "flute_types.h"

namespace {

// Deterministic byte pattern so a corrupted symbol shows up as a
// single byte miscompare rather than a random-looking failure.
std::vector<char> MakeBuffer(std::size_t F, std::uint8_t seed = 0x55) {
    std::vector<char> b(F);
    for (std::size_t i = 0; i < F; ++i) {
        b[i] = static_cast<char>((seed + i * 7U) & 0xFFU);
    }
    return b;
}

struct RoundTripResult {
    std::shared_ptr<LibFlute::File> received;
    std::size_t packet_count    = 0;
    std::size_t dropped_count   = 0;
    std::size_t total_bytes     = 0;
};

// Inspect the LCT half-word TOI without round-tripping through
// AlcPacket. The Encoder always emits half_word_flag=1 / toi_flag=0,
// so TOI sits at offset 10..11 (LCT base 4 + CCI 4 + TSI half 2).
inline std::uint16_t ToiOf(std::span<const std::uint8_t> packet) {
    if (packet.size() < 12) return 0xFFFF;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[10]) << 8) | packet[11]);
}

// One-shot end-to-end round-trip. Encoder pushes packets directly
// into Decoder; the test observes the resulting received File via
// the Decoder's completion callback.
//
// `drop_every_nth_file_packet`: if >0, drop every Nth file packet
// (TOI != 0) to simulate channel loss. FDT packets (TOI=0) always
// pass — without the FDT the receiver has no metadata. Loss is
// deterministic (every Nth) so test outcomes stay reproducible.
RoundTripResult RoundTrip(const std::vector<char>& data,
                            unsigned mtu, std::uint64_t tsi,
                            LibFlute::FecScheme fec_scheme,
                            int drop_every_nth_file_packet = 0) {
    LibFlute::Decoder decoder(tsi);

    RoundTripResult result;
    decoder.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) { result.received = std::move(f); });

    std::size_t file_packet_idx = 0;
    LibFlute::Encoder encoder(
        tsi, mtu, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> packet) -> bool {
            ++result.packet_count;
            result.total_bytes += packet.size();

            const std::uint16_t toi = ToiOf(packet);
            if (toi != 0 && drop_every_nth_file_packet > 0) {
                ++file_packet_idx;
                if (file_packet_idx % static_cast<std::size_t>(
                                          drop_every_nth_file_packet) == 0) {
                    ++result.dropped_count;
                    // Return true so the encoder marks the symbol(s)
                    // transmitted and moves on. From the encoder's
                    // perspective the bytes left the wire; from the
                    // receiver's perspective they never arrive.
                    return true;
                }
            }
            decoder.feed_packet(packet);
            return true;
        });

    auto data_copy = data;  // encoder takes a non-owning pointer
    auto toi = encoder.send(
        "test.bin", "application/octet-stream",
        LibFlute::Encoder::seconds_since_epoch() + 60,
        data_copy.data(), data_copy.size(), fec_scheme,
        /*copy_buffer=*/false);
    EXPECT_NE(toi, 0U) << "encoder.send() failed";

    // No rate limiting → flush() drains everything.
    encoder.flush();

    return result;
}

}  // namespace

// -------- CompactNoCode round-trip ------------------------------------------

class CompactNoCodeRoundTrip
    : public ::testing::TestWithParam<std::tuple<std::size_t, unsigned>> {};

TEST_P(CompactNoCodeRoundTrip, FileBytesMatchAfterTransport) {
    const auto [F, mtu] = GetParam();
    const auto data = MakeBuffer(F);

    auto result = RoundTrip(data, mtu, /*tsi=*/16,
                              LibFlute::FecScheme::CompactNoCode);

    ASSERT_NE(result.received, nullptr)
        << "completion callback never fired — file did not finish receiving";
    EXPECT_TRUE(result.received->complete());
    EXPECT_EQ(result.received->meta().content_location, "test.bin");
    EXPECT_EQ(result.received->meta().content_type, "application/octet-stream");
    ASSERT_EQ(result.received->length(), F);
    EXPECT_EQ(std::memcmp(result.received->buffer(), data.data(), F), 0)
        << "received buffer differs from sent (F=" << F << ", mtu=" << mtu << ")";
    EXPECT_GT(result.packet_count, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, CompactNoCodeRoundTrip,
    ::testing::Values(
        // (F, mtu)
        // Encoder takes mtu and subtracts ~44 bytes of IP+UDP+LCT+SBN
        // overhead to derive its encoding-symbol size T. The cases
        // below exercise the source-block-partitioning paths from
        // RFC 5052 §9.1 across realistic MTUs.
        std::make_tuple<std::size_t, unsigned>(1U,    1500U),  // sub-symbol partial
        std::make_tuple<std::size_t, unsigned>(64U,   1500U),  // small, F < T
        std::make_tuple<std::size_t, unsigned>(1456U, 1500U),  // single full symbol (T = mtu - 44)
        std::make_tuple<std::size_t, unsigned>(1457U, 1500U),  // T + 1, two-symbol residue
        std::make_tuple<std::size_t, unsigned>(8192U,  576U),  // small MTU, multi-block
        std::make_tuple<std::size_t, unsigned>(65536U, 1500U), // 64 KiB
        std::make_tuple<std::size_t, unsigned>(262144U, 1500U) // 256 KiB, deep multi-block
        ),
    [](const ::testing::TestParamInfo<CompactNoCodeRoundTrip::ParamType>& info) {
        return "F" + std::to_string(std::get<0>(info.param)) + "_mtu" +
               std::to_string(std::get<1>(info.param));
    });

// -------- Raptor round-trip (RFC 5053, gated on RAPTOR_ENABLED) -------------

#ifdef RAPTOR_ENABLED

class RaptorRoundTrip
    : public ::testing::TestWithParam<std::tuple<std::size_t, unsigned>> {};

TEST_P(RaptorRoundTrip, FileBytesMatchAfterTransport) {
    const auto [F, mtu] = GetParam();
    const auto data = MakeBuffer(F);

    auto result = RoundTrip(data, mtu, /*tsi=*/16,
                              LibFlute::FecScheme::Raptor);

    ASSERT_NE(result.received, nullptr);
    EXPECT_TRUE(result.received->complete());
    ASSERT_EQ(result.received->length(), F);
    EXPECT_EQ(std::memcmp(result.received->buffer(), data.data(), F), 0)
        << "received buffer differs from sent (F=" << F << ", mtu=" << mtu << ")";
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, RaptorRoundTrip,
    ::testing::Values(
        // Pick (F, mtu) that exercise different K (source-symbol
        // counts) inside the Raptor source block. The bitstem-r10
        // codec's symbol-alignment constraint (Al = 4) is enforced by
        // the encoder when constructing the OTI.
        std::make_tuple<std::size_t, unsigned>(4096U,    1500U),
        std::make_tuple<std::size_t, unsigned>(65536U,   1500U),
        std::make_tuple<std::size_t, unsigned>(200000U,  1500U),
        // RFC 5053 §4.4.1.2 partitioning regression. F=11,931,920 with
        // mtu=1500 yields T=1456 and Kt=8195. Under the old fixed-K
        // partitioning (K=min(Kt,8192) + remainder) the last block
        // carries 3 symbols, which falls below bitstem-r10's
        // kJKMinK=4 and makes Encoder::Create return nullopt → the
        // entire encoder.send() fails. With proper §4.4.1.2
        // KL/KS/ZL/ZS distribution the symbols are split evenly
        // (KL=4098, KS=4097, ZL=1) so both blocks are well above
        // the codec's K-floor.
        std::make_tuple<std::size_t, unsigned>(11931920U, 1500U)
        ),
    [](const ::testing::TestParamInfo<RaptorRoundTrip::ParamType>& info) {
        return "F" + std::to_string(std::get<0>(info.param)) + "_mtu" +
               std::to_string(std::get<1>(info.param));
    });

// Raptor under simulated packet loss. The encoder produces ~15%
// repair symbols on top of the source-symbol set (RaptorFEC's
// surplus_packet_ratio); dropping a fraction of file packets forces
// the decoder to actually run the inactivation-decoding path
// (lib/raptor's Encoder/Decoder) rather than just memcpy'ing source
// symbols into place.
//
// Drop rates are chosen to stay within the FEC budget for the given
// (F, mtu) combination so tests remain deterministic. Smaller files
// (F<10K) have too few repair symbols per packet to tolerate any
// loss with the current 1.15 surplus ratio, so the lossy variant
// targets the larger sizes only.
class RaptorLossyRoundTrip
    : public ::testing::TestWithParam<
          std::tuple<std::size_t, unsigned, int>> {};

TEST_P(RaptorLossyRoundTrip, FileBytesMatchAfterRaptorRepairsLoss) {
    const auto [F, mtu, drop_every] = GetParam();
    const auto data = MakeBuffer(F);

    auto result = RoundTrip(data, mtu, /*tsi=*/16,
                              LibFlute::FecScheme::Raptor,
                              drop_every);

    EXPECT_GT(result.dropped_count, 0U)
        << "loss harness didn't actually drop any packets — test is "
           "passing trivially without exercising Raptor repair";
    ASSERT_NE(result.received, nullptr)
        << "Raptor decoder couldn't reconstruct after dropping "
        << result.dropped_count << " of " << result.packet_count
        << " packets";
    EXPECT_TRUE(result.received->complete());
    ASSERT_EQ(result.received->length(), F);
    EXPECT_EQ(std::memcmp(result.received->buffer(), data.data(), F), 0)
        << "Reconstructed buffer differs from sent (F=" << F
        << ", drop 1/" << drop_every << ", " << result.dropped_count
        << " of " << result.packet_count << " packets dropped)";
}

INSTANTIATE_TEST_SUITE_P(
    LossPattern, RaptorLossyRoundTrip,
    ::testing::Values(
        // (F, mtu, drop-every-Nth-file-packet)
        // F=65536  → ~52 file packets, ~7 packets of FEC budget.
        // F=200000 → ~160 file packets, ~21 packets of FEC budget.
        // Drop rates picked to stay comfortably within budget so the
        // decoder always succeeds; ratcheting these up will surface
        // the codec's actual repair limit.
        std::make_tuple<std::size_t, unsigned, int>(65536U,  1500U, 20),  // ~5%
        std::make_tuple<std::size_t, unsigned, int>(200000U, 1500U, 10),  // ~10%
        std::make_tuple<std::size_t, unsigned, int>(200000U, 1500U, 20)   // ~5%
        ),
    [](const ::testing::TestParamInfo<RaptorLossyRoundTrip::ParamType>& info) {
        return "F" + std::to_string(std::get<0>(info.param)) +
               "_mtu" + std::to_string(std::get<1>(info.param)) +
               "_drop1in" + std::to_string(std::get<2>(info.param));
    });

// FileTransmissionConfig::sub_block_size_target (= W in the RFC's
// notation) drives the autodetect for N (sub-blocks per source
// block). With the default W=16 MB the autodetect lands on N=1 for
// every realistic broadcast file, which is what the codec accepts
// today. This test covers the param-flow for a W large enough that
// N still autodetects to 1 — proves the plumbing works end-to-end
// without depending on bitstem-fec's sub-block-interleave path
// being implemented.
TEST(RaptorWConfig, LargeWStillRoundTripsAtN1) {
    constexpr std::size_t F = 200000;
    constexpr unsigned    mtu = 1500;
    const auto data = MakeBuffer(F);

    LibFlute::Decoder decoder(/*tsi=*/16);
    std::shared_ptr<LibFlute::File> received;
    decoder.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) { received = std::move(f); });

    LibFlute::Encoder encoder(
        /*tsi=*/16, mtu, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            decoder.feed_packet(p);
            return true;
        });

    LibFlute::FileTransmissionConfig cfg;
    cfg.oti.encoding_id = LibFlute::FecScheme::Raptor;
    cfg.sub_block_size_target = 16ULL * 1024ULL * 1024ULL;  // 16 MB → N=1

    auto data_copy = data;
    auto toi = encoder.send("w-test.bin", "application/octet-stream",
                              LibFlute::Encoder::seconds_since_epoch() + 60,
                              data_copy.data(), data_copy.size(), cfg,
                              /*copy_buffer=*/false);
    ASSERT_NE(toi, 0U);
    encoder.flush();

    ASSERT_NE(received, nullptr);
    EXPECT_TRUE(received->complete());
    ASSERT_EQ(received->length(), F);
    EXPECT_EQ(std::memcmp(received->buffer(), data.data(), F), 0);
}

// Small W (TS 26.346 §B.3.4.1's normative 256 KB for R10 file
// delivery) makes the autodetect formula pick N>1 for blocks larger
// than W — which today causes bitstem-fec's Encoder::Create to
// return nullopt because sub-block interleaving isn't implemented
// yet. The test is **disabled** until the codec ships N>1; once it
// does, drop the DISABLED_ prefix and this becomes the regression
// guard for sub-block correctness.
TEST(RaptorWConfig, DISABLED_SmallWForcesNGreaterThan1) {
    constexpr std::size_t F = 1u << 20;  // 1 MiB
    constexpr unsigned    mtu = 1500;
    const auto data = MakeBuffer(F);

    LibFlute::Decoder decoder(/*tsi=*/16);
    std::shared_ptr<LibFlute::File> received;
    decoder.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) { received = std::move(f); });

    LibFlute::Encoder encoder(
        /*tsi=*/16, mtu, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            decoder.feed_packet(p);
            return true;
        });

    LibFlute::FileTransmissionConfig cfg;
    cfg.oti.encoding_id = LibFlute::FecScheme::Raptor;
    cfg.sub_block_size_target = 256ULL * 1024ULL;  // TS 26.346 §B.3.4.1 R10

    auto data_copy = data;
    auto toi = encoder.send("w-test.bin", "application/octet-stream",
                              LibFlute::Encoder::seconds_since_epoch() + 60,
                              data_copy.data(), data_copy.size(), cfg,
                              /*copy_buffer=*/false);
    ASSERT_NE(toi, 0U);
    encoder.flush();

    ASSERT_NE(received, nullptr);
    EXPECT_TRUE(received->complete());
    ASSERT_EQ(received->length(), F);
    EXPECT_EQ(std::memcmp(received->buffer(), data.data(), F), 0);
}

// RaptorQ (RFC 6330) round-trip. Wire-format is symmetric with R10
// at the libflute layer (LCT codepoint 6, FEC Payload ID = SBN(8) +
// ESI(24), 4-byte SSI laid out as Z(1)+N(2)+Al(1)). Codec is the
// in-tree reference RaptorQ implementation in bitstem-fec.
class RaptorQRoundTrip
    : public ::testing::TestWithParam<std::tuple<std::size_t, unsigned>> {};

TEST_P(RaptorQRoundTrip, FileBytesMatchAfterTransport) {
    const auto [F, mtu] = GetParam();
    const auto data = MakeBuffer(F);

    auto result = RoundTrip(data, mtu, /*tsi=*/16,
                              LibFlute::FecScheme::RaptorQ);

    ASSERT_NE(result.received, nullptr);
    EXPECT_TRUE(result.received->complete());
    ASSERT_EQ(result.received->length(), F);
    EXPECT_EQ(std::memcmp(result.received->buffer(), data.data(), F), 0)
        << "received buffer differs from sent (F=" << F << ", mtu=" << mtu << ")";
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, RaptorQRoundTrip,
    ::testing::Values(
        // RaptorQ K range starts at 1 (vs R10's 4) so smaller files
        // round-trip too. K_max = 56403 keeps even multi-MB files
        // inside a single source block, which makes the partitioning
        // path different from the multi-block R10 case at the same F.
        std::make_tuple<std::size_t, unsigned>(4096U,    1500U),
        std::make_tuple<std::size_t, unsigned>(65536U,   1500U),
        std::make_tuple<std::size_t, unsigned>(200000U,  1500U)
        ),
    [](const ::testing::TestParamInfo<RaptorQRoundTrip::ParamType>& info) {
        return "F" + std::to_string(std::get<0>(info.param)) + "_mtu" +
               std::to_string(std::get<1>(info.param));
    });

#endif  // RAPTOR_ENABLED
