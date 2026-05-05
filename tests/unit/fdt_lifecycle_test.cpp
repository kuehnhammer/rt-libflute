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
    xml += "\n<FDT-Instance Expires=\"2208988800\"";
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
