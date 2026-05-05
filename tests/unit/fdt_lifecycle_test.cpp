// FDT lifecycle / FDT-Instance succession (RFC 6726 §3.3).
//
// Spec contract:
//   §3.3 ¶3   FDT Instance IDs SHOULD be monotonically increasing.
//             Receivers MUST NOT use a stale FDT once a newer one
//             has been observed.
//   §3.3 ¶5   A receiver SHOULD NOT delete a file based on its
//             absence from a new FDT-Instance — files in flight
//             persist across FDT updates.
//
// Tests drive DirectReceiver end-to-end via feed_packet(); each FDT
// is delivered as a single TOI=0 ALC packet whose payload is a
// minimal FDT-Instance XML.
//
// Out of scope (round 4):
//   - 2^20 wraparound circular comparison (RFC 6726 §3.3 says
//     instance IDs MAY wrap; needs explicit comparison API).
//   - Expires-time-based rejection (requires injectable clock).
//   - In-flight FDT-receive collision when v=N+1 arrives mid-v=N
//     (latent bug; fix-then-test deferred).

#include "DirectReceiver.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "File.h"
#include "FileDeliveryTable.h"
#include "fixtures.hpp"
#include "flute_types.h"

namespace {

std::span<const std::uint8_t> AsSpan(const std::vector<std::uint8_t>& v) {
    return {v.data(), v.size()};
}

// FDT-Instance XML carrying N File entries, each TOI listed at a
// fixed Content-Length. Used by every test in this file.
std::string BuildFdtXml(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& files,
                          std::uint32_t T, std::uint32_t max_sbl) {
    std::string xml;
    xml += R"(<?xml version="1.0" encoding="UTF-8"?>)";
    // NTP-epoch seconds for ~year 2058. Far enough out that
    // ReceiverBase's expiry check (RFC 6726 §3.3) treats the FDT as
    // valid in any realistic test-runtime clock. Tests that need to
    // exercise the EXPIRED branch override the receiver's clock to
    // produce a `now` that's past this value.
    xml += "\n<FDT-Instance Expires=\"5000000000\"";
    xml += " FEC-OTI-FEC-Encoding-ID=\"0\"";
    xml += " FEC-OTI-Maximum-Source-Block-Length=\"" + std::to_string(max_sbl) + "\"";
    xml += " FEC-OTI-Encoding-Symbol-Length=\"" + std::to_string(T) + "\">\n";
    for (auto [toi, size] : files) {
        xml += "  <File TOI=\"" + std::to_string(toi) + "\"";
        xml += " Content-Location=\"toi" + std::to_string(toi) + ".bin\"";
        xml += " Content-Length=\"" + std::to_string(size) + "\"";
        xml += " Transfer-Length=\"" + std::to_string(size) + "\"/>\n";
    }
    xml += "</FDT-Instance>\n";
    return xml;
}

// Send the given FDT XML as a single TOI=0 ALC packet with the given
// instance_id. T must be ≥ the XML size so it fits in one symbol.
void SendFdtPacket(LibFlute::DirectReceiver& rx, std::uint32_t instance_id,
                    const std::string& xml) {
    libflute_test::DataPacketSpec spec;
    spec.tsi = 1;
    spec.toi = 0;
    spec.codepoint = 0;                    // CompactNoCode
    spec.add_ext_fdt = true;
    spec.fdt_instance_id = instance_id;
    spec.add_ext_fti = true;
    spec.fti_transfer_length = xml.size();
    spec.fti_encoding_symbol_length = static_cast<std::uint16_t>(xml.size());
    spec.fti_max_source_block_length = 1;
    spec.symbol_bytes.assign(xml.begin(), xml.end());
    auto packet = libflute_test::BuildDataPacket(spec);
    rx.feed_packet(AsSpan(packet));
}

bool FileListContainsToi(const std::vector<std::shared_ptr<LibFlute::File>>& files,
                          std::uint32_t toi) {
    for (const auto& f : files) {
        if (f->meta().toi == toi) return true;
    }
    return false;
}

}  // namespace

// RFC 6726 §3.3: a new FDT-Instance with a higher instance_id is the
// authoritative replacement. New TOIs it announces become receivable.
TEST(FdtLifecycle, NewerFdtAcceptedAndAddsFileEntries) {
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    auto fdt_v1 = BuildFdtXml({{5, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/1, fdt_v1);
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 6));

    auto fdt_v2 = BuildFdtXml({{5, 64}, {6, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/2, fdt_v2);
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 6));
}

// RFC 6726 §3.3: an FDT-Instance with a stale (lower) instance_id
// MUST NOT be applied once a newer one has been observed. Without
// monotonicity, a receiver can be tricked into rolling back to a
// stale file set by a delayed, replayed, or out-of-order packet.
TEST(FdtLifecycle, OlderFdtIsRejectedAfterNewerSeen) {
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    auto fdt_v2 = BuildFdtXml({{5, 64}, {6, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/2, fdt_v2);
    ASSERT_TRUE(FileListContainsToi(rx.file_list(), 5));
    ASSERT_TRUE(FileListContainsToi(rx.file_list(), 6));

    // Now feed an older FDT (instance_id=1) listing a different TOI.
    // The older FDT must be rejected; TOI=7 must NOT appear in
    // file_list.
    auto fdt_v1 = BuildFdtXml({{5, 64}, {7, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/1, fdt_v1);
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 6));
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 7))
        << "Stale FDT (instance_id=1) should not introduce new TOIs after "
           "instance_id=2 was already accepted (RFC 6726 §3.3 monotonicity).";
}

// RFC 6726 §3.3: a receiver SHOULD NOT delete a file based on its
// absence from a newer FDT-Instance. In-flight files survive FDT
// updates; the FDT just declares what's *announced*, not what's been
// abandoned.
TEST(FdtLifecycle, RemovedFileEntryFromNewFdtDoesNotEvictExistingFile) {
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    // v=1 announces TOI 5 and TOI 6.
    auto fdt_v1 = BuildFdtXml({{5, 64}, {6, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/1, fdt_v1);
    ASSERT_TRUE(FileListContainsToi(rx.file_list(), 5));
    ASSERT_TRUE(FileListContainsToi(rx.file_list(), 6));

    // v=2 only announces TOI 6 (TOI 5 is no longer listed).
    auto fdt_v2 = BuildFdtXml({{6, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/2, fdt_v2);

    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 5))
        << "TOI 5 was previously announced; absence from v=2 must not evict it.";
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 6));
}

// Repeating the same FDT instance_id is a no-op — receivers already
// hold this version, so re-sending it MUST NOT cause file_list churn
// or drop in-flight reception.
TEST(FdtLifecycle, RepeatedFdtWithSameInstanceIdIsIdempotent) {
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    auto fdt = BuildFdtXml({{5, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/3, fdt);
    auto files_first = rx.file_list();
    ASSERT_EQ(files_first.size(), 1U);

    // Re-send the same FDT (same instance_id, same content). file_list
    // size MUST NOT grow; the existing TOI 5 file MUST be preserved.
    SendFdtPacket(rx, /*instance_id=*/3, fdt);
    auto files_second = rx.file_list();
    EXPECT_EQ(files_second.size(), files_first.size());
    EXPECT_TRUE(FileListContainsToi(files_second, 5));
}

// ---------------------------------------------------------------------------
// RFC 6726 §3.3 + RFC 1982 serial-number arithmetic over the 20-bit
// FDT-Instance-ID space.
// ---------------------------------------------------------------------------

// Direct unit tests of the IsNewerInstanceId helper. The helper is
// the contract that makes everything below testable; pin its
// behaviour explicitly.
TEST(FdtInstanceIdComparator, ForwardSmallStepIsNewer) {
    EXPECT_TRUE(LibFlute::FileDeliveryTable::IsNewerInstanceId(5, 3));
    EXPECT_TRUE(LibFlute::FileDeliveryTable::IsNewerInstanceId(1, 0));
}

TEST(FdtInstanceIdComparator, BackwardSmallStepIsNotNewer) {
    EXPECT_FALSE(LibFlute::FileDeliveryTable::IsNewerInstanceId(3, 5));
    EXPECT_FALSE(LibFlute::FileDeliveryTable::IsNewerInstanceId(0, 1));
}

TEST(FdtInstanceIdComparator, EqualIsNotNewer) {
    EXPECT_FALSE(LibFlute::FileDeliveryTable::IsNewerInstanceId(0, 0));
    EXPECT_FALSE(LibFlute::FileDeliveryTable::IsNewerInstanceId(42, 42));
}

TEST(FdtInstanceIdComparator, WraparoundForwardAcrossZero) {
    constexpr std::uint32_t kMax = (1u << 20) - 1u;
    EXPECT_TRUE(LibFlute::FileDeliveryTable::IsNewerInstanceId(0, kMax));
    EXPECT_TRUE(LibFlute::FileDeliveryTable::IsNewerInstanceId(5, kMax - 5));
}

TEST(FdtInstanceIdComparator, MidpointResolvesAsNotNewer) {
    // candidate = current + 2^19 sits at the diametric opposite. RFC
    // 1982 leaves this undefined; we resolve it as "not newer" so a
    // single ambiguous packet cannot replace the current FDT.
    constexpr std::uint32_t kHalf = 1u << 19;
    EXPECT_FALSE(LibFlute::FileDeliveryTable::IsNewerInstanceId(kHalf, 0));
}

// ---------------------------------------------------------------------------
// RFC 6726 §3.3 Expires-time-based rejection
// ---------------------------------------------------------------------------

// Direct: an FDT-Instance whose Expires has already passed must be
// reported as expired by FileDeliveryTable::is_expired(now).
TEST(FdtExpires, IsExpiredReturnsTrueWhenNowExceedsExpires) {
    LibFlute::FecOti oti{LibFlute::FecScheme::CompactNoCode, 0, 1024, 64, ""};
    LibFlute::FileDeliveryTable fdt(/*instance_id=*/1, oti);
    fdt.set_expires(3'000'000'000ULL);   // ~year 1995 (NTP)
    EXPECT_TRUE(fdt.is_expired(/*now=*/4'000'000'000ULL));
    EXPECT_FALSE(fdt.is_expired(/*now=*/2'500'000'000ULL));
    // Boundary: now == Expires is NOT yet expired (Expires is the
    // last instant the FDT is valid). RFC 6726 §3.3 wording: "MUST
    // NOT be relied upon AFTER ... Expires", i.e. strictly after.
    EXPECT_FALSE(fdt.is_expired(/*now=*/3'000'000'000ULL));
}

// Integration: with the receiver's clock fixed at a value past the
// FDT-Instance's Expires, the parsed FDT MUST be discarded — the
// receiver continues with its prior FDT (or none).
TEST(FdtLifecycle, ExpiredFdtInstanceIsRejectedAtParseTime) {
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    // Pin "now" to a value past the BuildFdtXml fixture's Expires.
    constexpr std::uint64_t kFutureNtp = 6'000'000'000ULL;  // ~year 2090
    rx.set_now_provider([] { return kFutureNtp; });

    auto fdt = BuildFdtXml({{5, 64}}, kT, kMaxSbl);  // Expires = 5e9 < 6e9
    SendFdtPacket(rx, /*instance_id=*/1, fdt);
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 5))
        << "Expired FDT-Instance must not introduce its declared TOIs into "
           "the receiver's file_list (RFC 6726 §3.3).";
}

// And the contrapositive: with the clock BEFORE the Expires
// timestamp, the FDT is valid and its TOIs become receivable.
TEST(FdtLifecycle, NonExpiredFdtInstanceIsAccepted) {
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    // Pin "now" before the fixture's Expires (5e9).
    rx.set_now_provider([] { return 4'000'000'000ULL; });

    auto fdt = BuildFdtXml({{5, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/1, fdt);
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 5));
}

// ---------------------------------------------------------------------------
// 20-bit circular instance-ID comparator (RFC 6726 §3.3 + RFC 1982)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// In-flight FDT collision (RFC 6726 §3.3 + ReceiverBase routing)
// ---------------------------------------------------------------------------

// When a v=N+1 FDT packet arrives BEFORE v=N's TOI=0 file has finished
// receiving, the receiver MUST NOT feed v=N+1 bytes into v=N's
// in-flight File (which was sized for v=N's transfer_length). Doing
// so corrupts the in-flight buffer with bytes from a different FDT.
//
// This test:
//   1. Sends symbol 0 of a 2-symbol v=1 FDT (ESI=0). The receiver
//      allocates a File for TOI=0 sized for v=1's full transfer
//      length and feeds in the first half.
//   2. Sends a single-symbol v=2 FDT (ESI=0, instance_id=2,
//      different transfer_length). v=2 is newer per RFC 6726 §3.3,
//      so the receiver MUST drop the v=1 in-flight File and start
//      fresh with a File sized for v=2.
//   3. Verifies file_list reflects v=2's TOIs, not v=1's.
TEST(FdtLifecycle, InFlightFdtCollisionWhenNewerInstanceArrivesMidReceive) {
    constexpr std::uint32_t kT_v1 = 64;          // v=1 symbol size
    constexpr std::uint32_t kMaxSbl = 4;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    // Build a v=1 FDT XML and pad it (via repeated File entries) so
    // that it spans at least 2 symbols of T=64.
    auto fdt_v1 = BuildFdtXml({{5, 64}, {15, 64}, {25, 64}}, kT_v1, kMaxSbl);
    ASSERT_GE(fdt_v1.size(), 2U * kT_v1);
    const std::uint16_t v1_total = static_cast<std::uint16_t>(fdt_v1.size());

    // Send symbol 0 of v=1: SBN=0 ESI=0 + first kT_v1 bytes.
    libflute_test::DataPacketSpec p1;
    p1.tsi = 1;
    p1.toi = 0;
    p1.codepoint = 0;
    p1.add_ext_fdt = true;
    p1.fdt_instance_id = 1;
    p1.add_ext_fti = true;
    p1.fti_transfer_length = v1_total;
    p1.fti_encoding_symbol_length = kT_v1;
    p1.fti_max_source_block_length = kMaxSbl;
    p1.sbn = 0;
    p1.esi = 0;
    p1.symbol_bytes.assign(fdt_v1.begin(), fdt_v1.begin() + kT_v1);
    auto packet1 = libflute_test::BuildDataPacket(p1);
    rx.feed_packet(AsSpan(packet1));

    // No file has yet been parsed; in particular no TOI=5/15/25 yet.
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 10));

    // Now send the v=2 FDT in a SINGLE symbol that fits in T_v2 bytes.
    // Different transfer_length, different OTI, different listed TOI.
    // T_v2 needs to accommodate the round-3 to_string() output, which
    // emits all MBMS xmlns declarations plus the sv:schemaVersion
    // marker — XML overhead alone is ~250 bytes before any File
    // entries. 512 leaves plenty of headroom for one File.
    constexpr std::uint32_t kT_v2 = 512;
    auto fdt_v2 = BuildFdtXml({{10, 64}}, kT_v2, /*max_sbl=*/1);
    ASSERT_LE(fdt_v2.size(), kT_v2);

    libflute_test::DataPacketSpec p2;
    p2.tsi = 1;
    p2.toi = 0;
    p2.codepoint = 0;
    p2.add_ext_fdt = true;
    p2.fdt_instance_id = 2;                       // newer
    p2.add_ext_fti = true;
    p2.fti_transfer_length = static_cast<std::uint64_t>(fdt_v2.size());
    p2.fti_encoding_symbol_length = static_cast<std::uint16_t>(fdt_v2.size());
    p2.fti_max_source_block_length = 1;
    p2.sbn = 0;
    p2.esi = 0;
    p2.symbol_bytes.assign(fdt_v2.begin(), fdt_v2.end());
    auto packet2 = libflute_test::BuildDataPacket(p2);
    rx.feed_packet(AsSpan(packet2));

    // v=2's FDT must have been parsed and committed; TOI=10 from v=2
    // is in file_list. v=1's TOIs (5/15/25) must NOT appear: v=1
    // never completed its own FDT-receive (preempted by v=2).
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 10))
        << "v=2 FDT (newer) should have replaced the in-flight v=1 File "
           "and announced its declared TOIs.";
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 15));
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 25));
}

// Symmetric case: in-flight v=2 must not be preempted by an OLDER
// v=1 packet. The v=1 packet is dropped without affecting v=2's
// in-flight File.
TEST(FdtLifecycle, InFlightFdtIgnoresOlderInstancePackets) {
    constexpr std::uint32_t kT_v2 = 64;
    constexpr std::uint32_t kMaxSbl = 4;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    auto fdt_v2 = BuildFdtXml({{10, 64}, {20, 64}, {30, 64}}, kT_v2, kMaxSbl);
    ASSERT_GE(fdt_v2.size(), 2U * kT_v2);
    const std::uint16_t v2_total = static_cast<std::uint16_t>(fdt_v2.size());

    // First half of v=2 FDT in flight.
    libflute_test::DataPacketSpec p_v2_part0;
    p_v2_part0.tsi = 1;
    p_v2_part0.toi = 0;
    p_v2_part0.codepoint = 0;
    p_v2_part0.add_ext_fdt = true;
    p_v2_part0.fdt_instance_id = 2;
    p_v2_part0.add_ext_fti = true;
    p_v2_part0.fti_transfer_length = v2_total;
    p_v2_part0.fti_encoding_symbol_length = kT_v2;
    p_v2_part0.fti_max_source_block_length = kMaxSbl;
    p_v2_part0.sbn = 0;
    p_v2_part0.esi = 0;
    p_v2_part0.symbol_bytes.assign(fdt_v2.begin(), fdt_v2.begin() + kT_v2);
    auto packet_v2 = libflute_test::BuildDataPacket(p_v2_part0);
    rx.feed_packet(AsSpan(packet_v2));

    // Now an OLD v=1 collision packet arrives — must be ignored.
    auto fdt_v1 = BuildFdtXml({{5, 64}}, /*T=*/256, /*max_sbl=*/1);
    libflute_test::DataPacketSpec p_v1;
    p_v1.tsi = 1;
    p_v1.toi = 0;
    p_v1.codepoint = 0;
    p_v1.add_ext_fdt = true;
    p_v1.fdt_instance_id = 1;
    p_v1.add_ext_fti = true;
    p_v1.fti_transfer_length = static_cast<std::uint64_t>(fdt_v1.size());
    p_v1.fti_encoding_symbol_length = static_cast<std::uint16_t>(fdt_v1.size());
    p_v1.fti_max_source_block_length = 1;
    p_v1.sbn = 0;
    p_v1.esi = 0;
    p_v1.symbol_bytes.assign(fdt_v1.begin(), fdt_v1.end());
    auto packet_v1 = libflute_test::BuildDataPacket(p_v1);
    rx.feed_packet(AsSpan(packet_v1));

    // v=1's TOI 5 must NOT appear; v=2 reception is still in-flight,
    // so its TOIs aren't visible yet either. Sanity-check: file_list
    // contains the still-in-flight TOI=0 entry only.
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_FALSE(FileListContainsToi(rx.file_list(), 10));
}

// Integration: when v=2^20-1 has been committed and v=0 arrives, the
// 20-bit circular comparator must accept v=0 as the newer FDT (it's
// one step forward across the wrap boundary). A linear `>` would
// reject it as 0 < (2^20 - 1).
TEST(FdtLifecycle, InstanceIdWraparoundAt2Pow20IsCircular) {
    constexpr std::uint32_t kT = 128;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint32_t kMaxInstanceId = (1u << 20) - 1u;
    LibFlute::DirectReceiver rx(/*tsi=*/1);

    auto fdt_old = BuildFdtXml({{5, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/kMaxInstanceId, fdt_old);
    ASSERT_TRUE(FileListContainsToi(rx.file_list(), 5));

    // Wrap: instance_id=0 is one step forward from 2^20-1.
    auto fdt_wrap = BuildFdtXml({{5, 64}, {6, 64}}, kT, kMaxSbl);
    SendFdtPacket(rx, /*instance_id=*/0, fdt_wrap);
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 5));
    EXPECT_TRUE(FileListContainsToi(rx.file_list(), 6))
        << "FDT with instance_id=0 should supersede instance_id=2^20-1 "
           "under RFC 6726 §3.3 circular comparison.";
}
