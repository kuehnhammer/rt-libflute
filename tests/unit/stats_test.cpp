// Encoder + Decoder operational counters.
//
// The library exposes a fixed schema of counters (LibFlute::EncoderStats,
// LibFlute::DecoderStats); consumers pick which they want to surface
// to operators. These tests pin the counters' meaning by exercising
// each one against an end-to-end scenario where the expected count is
// trivial to derive.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "Decoder.h"
#include "Encoder.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "fixtures.hpp"
#include "flute_types.h"

namespace {

std::vector<char> MakeBuffer(std::size_t F, std::uint8_t seed = 0xA5) {
    std::vector<char> b(F);
    for (std::size_t i = 0; i < F; ++i) {
        b[i] = static_cast<char>((seed + i * 13U) & 0xFFU);
    }
    return b;
}

// Glue Encoder→Decoder under one mutex-free pump loop. Returns when
// either the encoder is idle or `received_file` was set.
struct Harness {
    LibFlute::Decoder decoder{1};
    std::shared_ptr<LibFlute::File> received;
    std::vector<char> data;

    Harness() {
        decoder.register_completion_callback(
            [&](std::shared_ptr<LibFlute::File> f) { received = std::move(f); });
    }
};

}  // namespace

// All counters start at zero on a freshly-constructed Encoder/Decoder.
TEST(EncoderStats, FreshEncoderHasAllZeroCounters) {
    LibFlute::Encoder enc(/*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
                           [](std::span<const std::uint8_t>) { return true; });
    auto s = enc.stats();
    EXPECT_EQ(s.packets_emitted,        0U);
    EXPECT_EQ(s.packets_failed,         0U);
    EXPECT_EQ(s.bytes_emitted,          0U);
    EXPECT_EQ(s.source_symbols_emitted, 0U);
    EXPECT_EQ(s.repair_symbols_emitted, 0U);
    EXPECT_EQ(s.files_queued,           0U);
    EXPECT_EQ(s.files_transmitted,      0U);
    EXPECT_EQ(s.fdt_packets_emitted,    0U);
}

TEST(DecoderStats, FreshDecoderHasAllZeroCounters) {
    LibFlute::Decoder dec(/*tsi=*/1);
    auto s = dec.stats();
    EXPECT_EQ(s.packets_received,            0U);
    EXPECT_EQ(s.bytes_received,              0U);
    EXPECT_EQ(s.packets_dropped_total(),     0U);
    EXPECT_EQ(s.source_symbols_received,     0U);
    EXPECT_EQ(s.repair_symbols_received,     0U);
    EXPECT_EQ(s.files_completed,             0U);
    EXPECT_EQ(s.files_discarded_incomplete,  0U);
    EXPECT_EQ(s.bytes_discarded_incomplete,  0U);
    EXPECT_EQ(s.md5sum_fail,                 0U);
    EXPECT_EQ(s.fdts_accepted,               0U);
    EXPECT_EQ(s.fdts_rejected_expired,       0U);
    EXPECT_EQ(s.fdts_rejected_stale,         0U);
}

// One round-trip end-to-end: the encoder counts every byte dispatched,
// the decoder counts every byte received, both report files_queued ==
// files_transmitted == files_completed == 1, and the byte totals match.
TEST(StatsIntegration, OneFileRoundTripCountersMatch) {
    Harness h;
    h.data = MakeBuffer(8192);

    LibFlute::Encoder encoder(
        /*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            h.decoder.feed_packet(p);
            return true;
        });

    auto data_copy = h.data;
    auto toi = encoder.send("test.bin", "application/octet-stream",
                              LibFlute::Encoder::seconds_since_epoch() + 60,
                              data_copy.data(), data_copy.size(),
                              LibFlute::FecScheme::CompactNoCode, false);
    ASSERT_NE(toi, 0U);

    encoder.flush();
    ASSERT_NE(h.received, nullptr);

    auto es = encoder.stats();
    auto ds = h.decoder.stats();

    // Encoder: queued one file, transmitted one, emitted >=1 packet.
    EXPECT_EQ(es.files_queued,      1U);
    EXPECT_EQ(es.files_transmitted, 1U);
    EXPECT_GT(es.packets_emitted,   0U);
    EXPECT_EQ(es.packets_failed,    0U);
    EXPECT_GE(es.fdt_packets_emitted, 1U);   // at least one FDT packet
    EXPECT_GT(es.source_symbols_emitted, 0U);
    EXPECT_EQ(es.repair_symbols_emitted, 0U) << "CompactNoCode never emits repair";

    // Decoder: same packet count & bytes; ≥1 FDT accepted (the
    // encoder emits a fresh FDT both when the file is queued and
    // again once the file has been transmitted, with the latter
    // listing zero entries — both are valid FDT instances); one file
    // completed; no drops.
    EXPECT_EQ(ds.packets_received, es.packets_emitted);
    EXPECT_EQ(ds.bytes_received,   es.bytes_emitted);
    EXPECT_EQ(ds.packets_dropped_total(), 0U);
    EXPECT_GE(ds.fdts_accepted, 1U);
    EXPECT_EQ(ds.fdts_rejected_expired, 0U);
    EXPECT_EQ(ds.fdts_rejected_stale,   0U);
    EXPECT_EQ(ds.files_completed,       1U);
    EXPECT_GT(ds.source_symbols_received, 0U);
    EXPECT_EQ(ds.repair_symbols_received, 0U);
}

// A packet for the wrong TSI bumps dropped_wrong_tsi.
TEST(DecoderStats, WrongTsiPacketIncrementsDroppedCounter) {
    LibFlute::Decoder dec(/*tsi=*/1);

    libflute_test::DataPacketSpec spec;
    spec.tsi = 99;             // foreign
    spec.toi = 0;
    spec.codepoint = 0;
    spec.add_ext_fdt = true;
    spec.fdt_instance_id = 1;
    spec.add_ext_fti = true;
    spec.fti_transfer_length = 64;
    spec.fti_encoding_symbol_length = 64;
    spec.fti_max_source_block_length = 1;
    spec.symbol_bytes = std::vector<std::uint8_t>(64, 0xAB);

    auto packet = libflute_test::BuildDataPacket(spec);
    dec.feed_packet({packet.data(), packet.size()});

    auto s = dec.stats();
    EXPECT_EQ(s.packets_received, 1U);
    EXPECT_EQ(s.dropped_wrong_tsi, 1U);
    EXPECT_EQ(s.packets_dropped_total(), 1U);
    EXPECT_EQ(s.fdts_accepted, 0U);
}

// A truncated packet (< 4 bytes for LCT base) bumps dropped_malformed.
TEST(DecoderStats, MalformedPacketIncrementsDroppedCounter) {
    LibFlute::Decoder dec(/*tsi=*/1);
    std::array<std::uint8_t, 3> too_short = {0x10, 0x00, 0x01};
    dec.feed_packet({too_short.data(), too_short.size()});

    auto s = dec.stats();
    EXPECT_EQ(s.packets_received, 1U);
    EXPECT_EQ(s.dropped_malformed, 1U);
    EXPECT_EQ(s.fdts_accepted, 0U);
}

// A stale (older instance_id) FDT packet bumps dropped_stale_fdt and
// fdts_rejected_stale stays at zero (it counts FDT *instances*
// rejected at parse-completion, not per-packet).
TEST(DecoderStats, StaleFdtPacketIncrementsDroppedCounter) {
    LibFlute::Decoder dec(/*tsi=*/1);

    auto build_fdt_packet = [](std::uint32_t instance_id) {
        const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)"
            "\n<FDT-Instance Expires=\"5000000000\" "
            "FEC-OTI-FEC-Encoding-ID=\"0\" "
            "FEC-OTI-Maximum-Source-Block-Length=\"1\" "
            "FEC-OTI-Encoding-Symbol-Length=\"256\">\n"
            "  <File TOI=\"5\" Content-Location=\"a.bin\" "
            "Content-Length=\"64\" Transfer-Length=\"64\"/>\n"
            "</FDT-Instance>\n";
        libflute_test::DataPacketSpec s;
        s.tsi = 1;
        s.toi = 0;
        s.codepoint = 0;
        s.add_ext_fdt = true;
        s.fdt_instance_id = instance_id;
        s.add_ext_fti = true;
        s.fti_transfer_length = xml.size();
        s.fti_encoding_symbol_length = static_cast<std::uint16_t>(xml.size());
        s.fti_max_source_block_length = 1;
        s.symbol_bytes.assign(xml.begin(), xml.end());
        return libflute_test::BuildDataPacket(s);
    };

    auto v2 = build_fdt_packet(2);
    dec.feed_packet({v2.data(), v2.size()});
    auto v1 = build_fdt_packet(1);  // older
    dec.feed_packet({v1.data(), v1.size()});

    auto s = dec.stats();
    EXPECT_EQ(s.packets_received, 2U);
    EXPECT_EQ(s.fdts_accepted, 1U);
    EXPECT_EQ(s.dropped_stale_fdt, 1U);
}

// Manually evicting a never-completed file via
// remove_file_with_content_location() bumps the "discarded_incomplete"
// counters. (Eviction via remove_expired_files() takes the same path
// once the age gate trips; we test the deterministic name-based
// eviction here to avoid wall-clock timing.)
TEST(DecoderStats, EvictedIncompleteFileCountsAsDiscarded) {
    Harness h;
    h.data = MakeBuffer(2048);

    // One-shot encoder; we'll deliberately never feed packets to the
    // decoder so the file stays incomplete.
    std::vector<std::vector<std::uint8_t>> captured;
    LibFlute::Encoder encoder(
        /*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            captured.emplace_back(p.begin(), p.end());
            return true;
        });
    auto data_copy = h.data;
    encoder.send("incomplete.bin", "application/octet-stream",
                  LibFlute::Encoder::seconds_since_epoch() + 60,
                  data_copy.data(), data_copy.size());
    encoder.flush();

    // Feed only the FDT packet (TOI=0). File for TOI=1 is allocated
    // but no symbols arrive.
    ASSERT_FALSE(captured.empty());
    h.decoder.feed_packet({captured[0].data(), captured[0].size()});

    auto pre = h.decoder.stats();
    EXPECT_EQ(pre.fdts_accepted, 1U);
    EXPECT_EQ(pre.files_completed, 0U);
    EXPECT_EQ(pre.files_discarded_incomplete, 0U);

    // Manually evict the still-incomplete file by content location.
    h.decoder.remove_file_with_content_location("incomplete.bin");

    auto post = h.decoder.stats();
    EXPECT_EQ(post.files_discarded_incomplete, 1U);
    EXPECT_EQ(post.bytes_discarded_incomplete, h.data.size());
    EXPECT_EQ(post.files_completed, 0U);
}

#ifdef RAPTOR_ENABLED
// Raptor lossy round-trip: source vs repair symbol counters split as
// expected. With 10% loss, the receiver drops some symbol packets; on
// completion the decoder reports both source and repair receipts.
TEST(DecoderStats, RaptorLossyRoundTripDistinguishesSourceVsRepair) {
    Harness h;
    h.data = MakeBuffer(200000);

    std::size_t file_packet_idx = 0;
    constexpr int kDropEveryNth = 10;

    LibFlute::Encoder encoder(
        /*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            // Drop 1-in-N file packets (TOI != 0) to force repair.
            if (p.size() >= 12) {
                const std::uint16_t toi =
                    static_cast<std::uint16_t>((p[10] << 8) | p[11]);
                if (toi != 0) {
                    ++file_packet_idx;
                    if (file_packet_idx % kDropEveryNth == 0) return true;
                }
            }
            h.decoder.feed_packet(p);
            return true;
        });
    auto data_copy = h.data;
    encoder.send("raptor.bin", "application/octet-stream",
                  LibFlute::Encoder::seconds_since_epoch() + 60,
                  data_copy.data(), data_copy.size(),
                  LibFlute::FecScheme::Raptor);
    encoder.flush();
    h.decoder.flush_pending_decodes();
    ASSERT_NE(h.received, nullptr);

    auto es = encoder.stats();
    auto ds = h.decoder.stats();

    // Encoder emitted some repair (Raptor surplus_packet_ratio=1.15).
    EXPECT_GT(es.repair_symbols_emitted, 0U)
        << "Raptor encoder should have emitted at least one repair symbol";
    EXPECT_GT(es.source_symbols_emitted, 0U);

    // Decoder received both kinds (some of each were dropped, but
    // enough of each made it through for the file to complete).
    EXPECT_GT(ds.source_symbols_received, 0U);
    EXPECT_GT(ds.repair_symbols_received, 0U)
        << "Decoder should have observed at least one repair symbol arrive "
           "(otherwise the loss-injection harness isn't actually engaging "
           "the FEC repair path).";
    EXPECT_EQ(ds.files_completed, 1U);
}

// Honours the FDT-carried mbms2012:FEC-Redundancy-Level: encoder is
// configured with FRL=50 (= 1.50·K emit overhead), enough source
// packets are dropped that completion requires repair ESIs in
// [1.15·K, 1.50·K) — i.e. above the decoder's hardcoded-default
// surplus_packet_ratio of 1.15. Without the FDT plumbing the
// per-block symbols vector is sized to 1.15·K and File::put_symbol
// throws on those higher ESIs ("Encoding Symbol ID too high"), so
// the file never completes. With the plumbing in place the decoder
// matches the encoder's emit overhead and the file completes off
// the repair path.
TEST(DecoderStats, RaptorLossyRoundTripHonoursFdtRedundancyLevel) {
    Harness h;
    // 200 KB / T≈1456 ⇒ K≈138 (one source block at MTU=1500). Single
    // block keeps the per-ESI accounting exact and avoids cross-block
    // averaging effects.
    h.data = MakeBuffer(200000);

    std::size_t file_packet_idx = 0;
    // 25 % loss: enough source missed that the decoder needs ~0.25·K
    // repair to complete, which is above the 0.15·K hardcoded
    // default surplus.
    constexpr int kDropEveryNth = 4;

    LibFlute::Encoder encoder(
        /*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            // Drop 1-in-N file packets (TOI != 0) to force repair use.
            if (p.size() >= 12) {
                const std::uint16_t toi =
                    static_cast<std::uint16_t>((p[10] << 8) | p[11]);
                if (toi != 0) {
                    ++file_packet_idx;
                    if (file_packet_idx % kDropEveryNth == 0) return true;
                }
            }
            h.decoder.feed_packet(p);
            return true;
        });

    // FRL=50 ⇒ encoder emits up to 1.50·K ESIs per block — well above
    // the 1.15·K default bound. The decoder MUST receive this value
    // via the FDT to size its ESI buffers correctly.
    LibFlute::FileTransmissionConfig fec_config{};
    fec_config.scheme                = LibFlute::FecScheme::Raptor;
    fec_config.fec_redundancy_level  = 50U;

    auto data_copy = h.data;
    encoder.send("raptor_frl50.bin", "application/octet-stream",
                  LibFlute::Encoder::seconds_since_epoch() + 60,
                  data_copy.data(), data_copy.size(),
                  fec_config);
    encoder.flush();
    h.decoder.flush_pending_decodes();

    ASSERT_NE(h.received, nullptr)
        << "File should have completed via the repair path. Without the "
           "FRL plumbing the decoder caps its per-block symbol buffer at "
           "1.15·K and throws on encoder ESIs in [1.15·K, 1.50·K) — see "
           "File.cpp:201 ('Encoding Symbol ID too high').";

    auto ds = h.decoder.stats();
    EXPECT_EQ(ds.files_completed, 1U);
    EXPECT_GT(ds.source_symbols_received, 0U);
    EXPECT_GT(ds.repair_symbols_received, 0U)
        << "Decoder should have observed repair symbols arrive AND "
           "consumed at least some of them (matched-FRL bound lets "
           "high-ESI repair through).";
}

// Lossless multi-block Raptor reception: the codec auto-finalises each
// source block on its K-th source ESI, after which the remaining
// repair-ESI packets for that block arrive while the FILE is still
// pending (other blocks haven't completed). Those packets are not
// consumed by the decoder — the codec already discarded them as
// IsDecoded() was true. They must NOT inflate repair_symbols_received,
// because the consumer surfaces rep / (src + rep) as "FEC consumption"
// and expects zero on a clean channel.
TEST(DecoderStats, RaptorLosslessMultiBlockDoesNotCountUnusedRepair) {
    Harness h;
    // R10 partitioning is Z = ceil(Kt / K_max=8192). With MTU=1500 the
    // codec picks T = 1456, so a 14 MB file needs Kt = ceil(14e6/1456)
    // = 9616 source symbols → Z = 2 source blocks. That's the smallest
    // size that exercises the "block 0 complete, file not complete"
    // window where unused repair symbols arrive.
    h.data = MakeBuffer(14UL * 1024UL * 1024UL);

    LibFlute::Encoder encoder(
        /*tsi=*/1, /*mtu=*/1500, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> p) {
            h.decoder.feed_packet(p);
            return true;
        });
    auto data_copy = h.data;
    encoder.send("multiblock.bin", "application/octet-stream",
                  LibFlute::Encoder::seconds_since_epoch() + 60,
                  data_copy.data(), data_copy.size(),
                  LibFlute::FecScheme::Raptor);
    encoder.flush();
    h.decoder.flush_pending_decodes();
    ASSERT_NE(h.received, nullptr);

    auto ds = h.decoder.stats();
    EXPECT_EQ(ds.files_completed, 1U);
    EXPECT_GT(ds.source_symbols_received, 0U);
    EXPECT_EQ(ds.repair_symbols_received, 0U)
        << "Lossless reception must not count repair symbols the "
           "codec didn't use to decode any block.";
}
#endif  // RAPTOR_ENABLED
