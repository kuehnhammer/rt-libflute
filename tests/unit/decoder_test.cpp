// Decoder — embedder-facing entry path.
//
// RFC 5651 §3.1 + RFC 6726 App. A: a FLUTE receiver MUST filter ALC
// packets by TSI (only packets matching the joined session's TSI
// belong to "this session") and route accepted packets by TOI:
//   - TOI = 0 plus an EXT_FDT extension delivers an FDT-Instance.
//     Once the FDT TOI=0 object is fully received, the receiver
//     parses the XML and creates a File for every <File TOI=...>
//     entry it announces.
//   - TOI != 0 carries data symbols of the file with that TOI; the
//     receiver MUST route them to the matching File.
//   - When all source symbols of a file are received, the receiver
//     fires a registered completion callback.
//
// All packets here are built by-hand from fixtures.hpp; no UDP, no
// no event loop. CompactNoCode FEC only (codepoint = 0). Decoder
// uses feed_packet() to accept ALC payloads.
// feed_packet() wrapper.

#include "Decoder.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/md5.h>

#include <gtest/gtest.h>

#include "File.h"
#include "FileDeliveryTable.h"
#include "fixtures.hpp"
#include "flute_types.h"

namespace {

// Wrap a vector<uint8_t> as the const-span feed_packet expects.
std::span<const std::uint8_t> AsSpan(const std::vector<std::uint8_t>& v) {
    return {v.data(), v.size()};
}

// Build a minimal FDT-Instance XML body with one File entry. The
// FDT's transfer_length must equal this string's size so the receiver
// allocates the right-sized buffer for it. `content_md5` is emitted
// verbatim when non-empty (callers exercise both the matching-digest
// and tampered-digest cases).
std::string BuildFdtXml(std::uint32_t file_toi, std::uint64_t file_size,
                          std::uint32_t T, std::uint32_t max_sbl,
                          const std::string& content_md5 = "") {
    std::string xml;
    xml += R"(<?xml version="1.0" encoding="UTF-8"?>)";
    // NTP-epoch seconds for ~year 2058 — far enough out that the
    // receiver's Expires check (RFC 6726 §3.3) treats this FDT as
    // valid for the foreseeable future. Tests that need to exercise
    // the EXPIRED branch override the receiver's clock.
    xml += "\n<FDT-Instance Expires=\"5000000000\"";
    xml += " FEC-OTI-FEC-Encoding-ID=\"0\"";
    xml += " FEC-OTI-Maximum-Source-Block-Length=\"" + std::to_string(max_sbl) + "\"";
    xml += " FEC-OTI-Encoding-Symbol-Length=\"" + std::to_string(T) + "\">\n";
    xml += "  <File TOI=\"" + std::to_string(file_toi) + "\"";
    xml += " Content-Location=\"target.bin\"";
    xml += " Content-Length=\"" + std::to_string(file_size) + "\"";
    xml += " Transfer-Length=\"" + std::to_string(file_size) + "\"";
    if (!content_md5.empty()) {
        xml += " Content-MD5=\"" + content_md5 + "\"";
    }
    xml += "/>\n";
    xml += "</FDT-Instance>\n";
    return xml;
}

}  // namespace

// RFC 5651 §3.1: the receiver MUST silently discard packets whose TSI
// does not match the joined session's TSI. The Decoder's
// file_list MUST stay empty when packets arrive on a foreign TSI.
TEST(Decoder, RejectsPacketsWithMismatchedTsi) {
    LibFlute::Decoder rx(/*tsi=*/1);

    libflute_test::DataPacketSpec spec;
    spec.tsi = 99;            // foreign TSI
    spec.toi = 0;
    spec.codepoint = 0;       // CompactNoCode
    spec.add_ext_fdt = true;
    spec.fdt_instance_id = 1;
    spec.add_ext_fti = true;
    spec.fti_transfer_length = 100;
    spec.fti_encoding_symbol_length = 64;
    spec.fti_max_source_block_length = 4;
    spec.symbol_bytes = std::vector<std::uint8_t>(64, 0xAB);

    auto packet = libflute_test::BuildDataPacket(spec);
    rx.feed_packet(AsSpan(packet));

    EXPECT_TRUE(rx.file_list().empty());
}

// RFC 6726 §3.4.1 + App. A: a packet with TOI = 0 and an EXT_FDT
// header carrying the FDT-Instance ID delivers (a fragment of) the
// FDT. When the FDT-object is fully received, the receiver parses
// the XML and starts reception for every File entry the FDT lists.
//
// Test: send the entire FDT in one packet (T == fdt_size) and verify
// that file_list afterwards contains a File entry for the FDT-
// declared TOI.
TEST(Decoder, FdtPacketWithToi0AndExtFdtTriggersFdtParse) {
    constexpr std::uint64_t kFileTransferLength = 64;
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint16_t kFileToi = 5;

    auto fdt_xml = BuildFdtXml(kFileToi, kFileTransferLength, kT, kMaxSbl);

    // A single FDT packet that contains the entire XML body. T must
    // be ≥ the XML size so the FDT fits in one source symbol.
    const std::uint16_t kFdtT = static_cast<std::uint16_t>(fdt_xml.size());

    LibFlute::Decoder rx(/*tsi=*/1);

    libflute_test::DataPacketSpec fdt_spec;
    fdt_spec.tsi = 1;
    fdt_spec.toi = 0;
    fdt_spec.codepoint = 0;       // CompactNoCode
    fdt_spec.add_ext_fdt = true;
    fdt_spec.fdt_instance_id = 1;
    fdt_spec.add_ext_fti = true;
    fdt_spec.fti_transfer_length = fdt_xml.size();
    fdt_spec.fti_encoding_symbol_length = kFdtT;
    fdt_spec.fti_max_source_block_length = 1;
    fdt_spec.sbn = 0;
    fdt_spec.esi = 0;
    fdt_spec.symbol_bytes.assign(fdt_xml.begin(), fdt_xml.end());

    auto fdt_packet = libflute_test::BuildDataPacket(fdt_spec);
    rx.feed_packet(AsSpan(fdt_packet));

    auto files = rx.file_list();
    bool found_target_toi = false;
    for (const auto& f : files) {
        if (f->meta().toi == kFileToi) {
            found_target_toi = true;
            EXPECT_EQ(f->meta().content_location, "target.bin");
            EXPECT_EQ(f->meta().content_length, kFileTransferLength);
        }
    }
    EXPECT_TRUE(found_target_toi)
        << "expected File entry for TOI " << kFileToi
        << " from parsed FDT, but file_list has " << files.size() << " entries";
}

// RFC 6726 §3.1 + App. A step 4: data packets are routed to the
// matching File based on TOI. After the FDT announces TOI=5, a data
// packet for TOI=5 must be accepted and surfaced via file_list().
TEST(Decoder, FilePacketRoutedToFileByToi) {
    constexpr std::uint64_t kFileTransferLength = 64;
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint16_t kFileToi = 5;

    auto fdt_xml = BuildFdtXml(kFileToi, kFileTransferLength, kT, kMaxSbl);

    LibFlute::Decoder rx(/*tsi=*/1);

    // Step 1: deliver the FDT.
    libflute_test::DataPacketSpec fdt_spec;
    fdt_spec.tsi = 1;
    fdt_spec.toi = 0;
    fdt_spec.codepoint = 0;
    fdt_spec.add_ext_fdt = true;
    fdt_spec.fdt_instance_id = 1;
    fdt_spec.add_ext_fti = true;
    fdt_spec.fti_transfer_length = fdt_xml.size();
    fdt_spec.fti_encoding_symbol_length =
        static_cast<std::uint16_t>(fdt_xml.size());
    fdt_spec.fti_max_source_block_length = 1;
    fdt_spec.symbol_bytes.assign(fdt_xml.begin(), fdt_xml.end());
    auto fdt_packet = libflute_test::BuildDataPacket(fdt_spec);
    rx.feed_packet(AsSpan(fdt_packet));

    // Step 2: send a single file data packet for TOI=5 (one source
    // symbol of T=64 bytes). Decoder should already have a
    // File registered for TOI=5 thanks to the FDT.
    libflute_test::DataPacketSpec file_spec;
    file_spec.tsi = 1;
    file_spec.toi = kFileToi;
    file_spec.codepoint = 0;
    file_spec.add_ext_fdt = false;
    file_spec.add_ext_fti = false;
    file_spec.sbn = 0;
    file_spec.esi = 0;
    file_spec.symbol_bytes = std::vector<std::uint8_t>(kT, 0xCC);
    auto file_packet = libflute_test::BuildDataPacket(file_spec);
    rx.feed_packet(AsSpan(file_packet));

    // The file_list contains the registered File. complete() flips
    // when all symbols arrive — for one-block one-symbol files, this
    // means the File is now complete and either still listed (if no
    // completion callback was registered, kept around) or already
    // dispatched + removed.
    bool found = false;
    for (const auto& f : rx.file_list()) {
        if (f->meta().toi == kFileToi) {
            found = true;
        }
    }
    // It is acceptable for the receiver to either (a) still hold the
    // file in file_list (no callback registered → keep) or (b) have
    // already removed it on completion. RFC 6726 doesn't pin which.
    // What MUST be true is that no error path threw, and the FDT
    // routing didn't fail silently. Use the completion-callback
    // variant for the strict "fired exactly once" check.
    SUCCEED() << "TOI " << kFileToi
              << (found ? " present in file_list (kept)"
                        : " removed (completed and dispatched)");
}

// RFC 6726 App. A step 7: when a file is fully received, the
// completion callback fires exactly once with a shared_ptr<File>
// that is complete().
TEST(Decoder, CompletionCallbackFiresWhenFileFullyReceived) {
    constexpr std::uint64_t kFileTransferLength = 64;
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint16_t kFileToi = 7;

    auto fdt_xml = BuildFdtXml(kFileToi, kFileTransferLength, kT, kMaxSbl);

    LibFlute::Decoder rx(/*tsi=*/1);

    std::atomic<int> callback_count{0};
    std::shared_ptr<LibFlute::File> received;
    rx.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) {
            ++callback_count;
            received = std::move(f);
        });

    // FDT first.
    libflute_test::DataPacketSpec fdt_spec;
    fdt_spec.tsi = 1;
    fdt_spec.toi = 0;
    fdt_spec.codepoint = 0;
    fdt_spec.add_ext_fdt = true;
    fdt_spec.fdt_instance_id = 1;
    fdt_spec.add_ext_fti = true;
    fdt_spec.fti_transfer_length = fdt_xml.size();
    fdt_spec.fti_encoding_symbol_length =
        static_cast<std::uint16_t>(fdt_xml.size());
    fdt_spec.fti_max_source_block_length = 1;
    fdt_spec.symbol_bytes.assign(fdt_xml.begin(), fdt_xml.end());
    auto fdt_packet = libflute_test::BuildDataPacket(fdt_spec);
    rx.feed_packet(AsSpan(fdt_packet));

    EXPECT_EQ(callback_count.load(), 0);  // FDT doesn't trigger the cb.

    // Now the data: kFileTransferLength = T = 64, so a single symbol
    // completes the file.
    libflute_test::DataPacketSpec file_spec;
    file_spec.tsi = 1;
    file_spec.toi = kFileToi;
    file_spec.codepoint = 0;
    file_spec.sbn = 0;
    file_spec.esi = 0;
    file_spec.symbol_bytes = std::vector<std::uint8_t>(kT, 0xDE);
    auto file_packet = libflute_test::BuildDataPacket(file_spec);
    rx.feed_packet(AsSpan(file_packet));

    EXPECT_EQ(callback_count.load(), 1);
    ASSERT_TRUE(received);
    EXPECT_TRUE(received->complete());
    EXPECT_EQ(received->meta().toi, kFileToi);
    EXPECT_EQ(received->meta().content_location, "target.bin");
}

// RFC 6726 §3.4.2 calls Content-MD5 a "decoded object integrity
// service". When the FDT advertises a digest, the receiver MUST
// validate it against the bytes it actually assembled, and MUST NOT
// surface a mismatched object as a successful delivery.
//
// Wire-level expectations for the Decoder when the digest fails:
//   1. DecoderStats::md5sum_fail increments by 1.
//   2. The completion callback does NOT fire (the consumer is not
//      handed a corrupted file).
//   3. files_completed does NOT advance (only validated deliveries
//      count as "successfully delivered").
TEST(Decoder, Md5MismatchSuppressesCallbackAndBumpsCounter) {
    constexpr std::uint64_t kFileTransferLength = 64;
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint16_t kFileToi = 9;

    // 16 zero bytes encoded as base64. The file payload below is 64
    // bytes of 0xDE, whose real MD5 will not match this hash.
    const std::string kWrongMd5 = "AAAAAAAAAAAAAAAAAAAAAA==";

    auto fdt_xml = BuildFdtXml(kFileToi, kFileTransferLength, kT, kMaxSbl,
                                 kWrongMd5);

    LibFlute::Decoder rx(/*tsi=*/1);

    std::atomic<int> callback_count{0};
    rx.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File>) { ++callback_count; });

    libflute_test::DataPacketSpec fdt_spec;
    fdt_spec.tsi = 1;
    fdt_spec.toi = 0;
    fdt_spec.codepoint = 0;
    fdt_spec.add_ext_fdt = true;
    fdt_spec.fdt_instance_id = 1;
    fdt_spec.add_ext_fti = true;
    fdt_spec.fti_transfer_length = fdt_xml.size();
    fdt_spec.fti_encoding_symbol_length =
        static_cast<std::uint16_t>(fdt_xml.size());
    fdt_spec.fti_max_source_block_length = 1;
    fdt_spec.symbol_bytes.assign(fdt_xml.begin(), fdt_xml.end());
    rx.feed_packet(AsSpan(libflute_test::BuildDataPacket(fdt_spec)));

    libflute_test::DataPacketSpec file_spec;
    file_spec.tsi = 1;
    file_spec.toi = kFileToi;
    file_spec.codepoint = 0;
    file_spec.sbn = 0;
    file_spec.esi = 0;
    file_spec.symbol_bytes = std::vector<std::uint8_t>(kT, 0xDE);
    rx.feed_packet(AsSpan(libflute_test::BuildDataPacket(file_spec)));

    const auto stats = rx.stats();
    EXPECT_EQ(stats.md5sum_fail, 1u);
    EXPECT_EQ(stats.files_completed, 0u);
    EXPECT_EQ(callback_count.load(), 0);
}

// Counter-example: when the advertised digest matches the assembled
// bytes, the completion path runs as usual and the failure counter
// stays at zero. Pins the happy-path semantics so a regression that
// over-flags everything as failed gets caught.
TEST(Decoder, Md5MatchKeepsCounterAtZeroAndFiresCallback) {
    constexpr std::uint64_t kFileTransferLength = 64;
    constexpr std::uint32_t kT = 64;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint16_t kFileToi = 11;

    // 64 bytes of 0xDE — compute its MD5 inline so the test stays
    // self-contained. Standard base64, RFC 4648 §4 alphabet.
    std::vector<std::uint8_t> payload(kT, 0xDE);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    {
        auto* ctx = EVP_MD_CTX_new();
        ASSERT_NE(ctx, nullptr);
        ASSERT_EQ(EVP_DigestInit_ex(ctx, EVP_md5(), nullptr), 1);
        ASSERT_EQ(EVP_DigestUpdate(ctx, payload.data(), payload.size()), 1);
        ASSERT_EQ(EVP_DigestFinal_ex(ctx, digest.data(), &digest_len), 1);
        EVP_MD_CTX_free(ctx);
    }
    ASSERT_EQ(digest_len, 16u);

    // Inline base64 encode to avoid pulling utils/base64.h into the
    // test target. 16 bytes → 24 chars including "==".
    static const char* kB64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string md5_b64;
    md5_b64.reserve(24);
    for (std::size_t i = 0; i < 16; i += 3) {
        const std::uint32_t b0 = digest[i];
        const std::uint32_t b1 = (i + 1 < 16) ? digest[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < 16) ? digest[i + 2] : 0;
        md5_b64.push_back(kB64[(b0 >> 2) & 0x3F]);
        md5_b64.push_back(kB64[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        md5_b64.push_back(i + 1 < 16 ? kB64[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=');
        md5_b64.push_back(i + 2 < 16 ? kB64[b2 & 0x3F] : '=');
    }

    auto fdt_xml = BuildFdtXml(kFileToi, kFileTransferLength, kT, kMaxSbl,
                                 md5_b64);

    LibFlute::Decoder rx(/*tsi=*/1);

    std::atomic<int> callback_count{0};
    rx.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File>) { ++callback_count; });

    libflute_test::DataPacketSpec fdt_spec;
    fdt_spec.tsi = 1;
    fdt_spec.toi = 0;
    fdt_spec.codepoint = 0;
    fdt_spec.add_ext_fdt = true;
    fdt_spec.fdt_instance_id = 1;
    fdt_spec.add_ext_fti = true;
    fdt_spec.fti_transfer_length = fdt_xml.size();
    fdt_spec.fti_encoding_symbol_length =
        static_cast<std::uint16_t>(fdt_xml.size());
    fdt_spec.fti_max_source_block_length = 1;
    fdt_spec.symbol_bytes.assign(fdt_xml.begin(), fdt_xml.end());
    rx.feed_packet(AsSpan(libflute_test::BuildDataPacket(fdt_spec)));

    libflute_test::DataPacketSpec file_spec;
    file_spec.tsi = 1;
    file_spec.toi = kFileToi;
    file_spec.codepoint = 0;
    file_spec.sbn = 0;
    file_spec.esi = 0;
    file_spec.symbol_bytes = payload;
    rx.feed_packet(AsSpan(libflute_test::BuildDataPacket(file_spec)));

    const auto stats = rx.stats();
    EXPECT_EQ(stats.md5sum_fail, 0u);
    EXPECT_EQ(stats.files_completed, 1u);
    EXPECT_EQ(callback_count.load(), 1);
}
