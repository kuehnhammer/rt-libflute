// libflute unit tests — ALC/LCT packet parsing.
//
// Spec sources (read-only oracles):
//   RFC 5651 §5.1   Layered Coding Transport (LCT) header format
//   RFC 5775        Asynchronous Layered Coding (ALC) protocol
//   RFC 6726 §3.4.1 EXT_FDT header extension (HET = 192)
//   RFC 6726 §3.4.3 EXT_CENC header extension (HET = 193)
//   RFC 5052 §3.4.3 EXT_FTI header extension (HET = 64)
//   RFC 5445        Compact No-Code FEC scheme
//
// Tests build raw byte buffers from the wire format and feed them to the
// AlcPacket parsing constructor. They do NOT use the producing constructor
// to avoid round-trip-with-itself coverage holes (the producer could be
// wrong in exactly the way the parser is wrong and tests would still pass).

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "AlcPacket.h"
#include "fixtures.hpp"

using libflute_test::AppendBytes;
using libflute_test::AppendBE16;
using libflute_test::AppendBE32;
using libflute_test::BuildDataPacket;
using libflute_test::BuildExtAuth;
using libflute_test::BuildExtCenc;
using libflute_test::BuildExtFdt;
using libflute_test::BuildExtFtiCompactNoCode;
using libflute_test::BuildExtNop;
using libflute_test::BuildExtTime;
using libflute_test::DataPacketSpec;
using libflute_test::EncodeLctBase;
using libflute_test::LctV1Base;

namespace {

// Helper: pass a byte vector to the AlcPacket char* constructor.
LibFlute::AlcPacket Parse(std::vector<std::uint8_t>& buf) {
    return LibFlute::AlcPacket(reinterpret_cast<char*>(buf.data()), buf.size());
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. RFC 5651 §5.1: LCT header is 4 bytes minimum. Anything shorter is malformed.
TEST(AlcPacket, RejectsPacketsShorterThan4Bytes) {
    std::vector<std::uint8_t> buf{0x10, 0x00, 0x01};   // 3 bytes
    EXPECT_THROW(Parse(buf), std::runtime_error);
}

// -----------------------------------------------------------------------------
// 2. RFC 5651 §3.1: LCT version MUST be 1.
TEST(AlcPacket, RejectsLctVersionNotEqualOne) {
    LctV1Base h;
    h.version = 2;
    h.lct_header_len = 1;   // arbitrary, parser rejects on version first
    auto base = EncodeLctBase(h);
    std::vector<std::uint8_t> buf(base.begin(), base.end());
    EXPECT_THROW(Parse(buf), std::runtime_error);
}

// -----------------------------------------------------------------------------
// 3. RFC 5651: LCT header length (in 32-bit words) must be ≥ 1 and the
// resulting byte length must not exceed the packet size.
TEST(AlcPacket, RejectsLctHeaderLengthOfZero) {
    LctV1Base h;
    h.lct_header_len = 0;
    auto base = EncodeLctBase(h);
    std::vector<std::uint8_t> buf(base.begin(), base.end());
    EXPECT_THROW(Parse(buf), std::runtime_error);
}

TEST(AlcPacket, RejectsLctHeaderLengthGreaterThanPacket) {
    LctV1Base h;
    h.lct_header_len = 5;   // claims 20 bytes of header
    auto base = EncodeLctBase(h);
    std::vector<std::uint8_t> buf(base.begin(), base.end());   // only 4 bytes
    EXPECT_THROW(Parse(buf), std::runtime_error);
}

// -----------------------------------------------------------------------------
// 4. RFC 5651 §5.1: half_word_flag=1 with tsi_flag=0 → 16-bit TSI carried in
// the half-word adjacent to the TOI half-word. This is the FLUTE common case.
TEST(AlcPacket, ParsesShortTsiHalfWordOnly) {
    DataPacketSpec spec;
    spec.tsi = 0xABCD;
    spec.toi = 0x0001;
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    EXPECT_EQ(p.tsi(), 0xABCDu);
    EXPECT_EQ(p.toi(), 0x0001u);
}

// -----------------------------------------------------------------------------
// 5. RFC 5651 §5.1: half_word_flag=1 + tsi_flag=1 → 48-bit TSI carried as
// 16-bit half-word OR'd into the high 16 bits of a 32-bit word.
TEST(AlcPacket, ParsesFullTsi48Bits) {
    LctV1Base h;
    h.version        = 1;
    h.cc_flag        = 0;
    h.tsi_flag       = 1;
    h.toi_flag       = 0;
    h.half_word_flag = 1;
    h.lct_header_len = 4;   // base + CCI + TSI(half + word) + TOI(half) = 4 words
    h.codepoint      = 0;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                                    // CCI
    AppendBE16(buf, static_cast<std::uint16_t>(0xCAFE));   // TSI half (upper 16)
    AppendBE32(buf, 0xDEADBEEF);                           // TSI word (lower 32)
    AppendBE16(buf, 0x0001);                                // TOI half

    // The implementation reads half-word THEN OR-shifts the word in by 16.
    // Per RFC 5651 the half-word is the LOWER bits when the word is shifted
    // up, giving a 48-bit value: tsi = (word << 16) | half_word.
    auto p = Parse(buf);
    EXPECT_EQ(p.tsi(), (static_cast<std::uint64_t>(0xDEADBEEF) << 16) |
                        static_cast<std::uint64_t>(0xCAFE));
}

// -----------------------------------------------------------------------------
// 6. RFC 5651: toi_flag values 0 / 1 / 2 (zero, 32-bit, 64-bit TOI extension)
// are all valid. Value 3 (96-bit) is currently unsupported by this code.
TEST(AlcPacket, ParsesToiFlagValueZero) {
    DataPacketSpec spec;
    spec.toi_flag = 0;
    spec.toi = 0x4242;
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    EXPECT_EQ(p.toi(), 0x4242u);
}

TEST(AlcPacket, ParsesToiFlagValueOne32BitExtension) {
    DataPacketSpec spec;
    spec.toi_flag = 1;             // adds one 32-bit word for upper TOI bits
    spec.toi = 0xBEEF;              // half-word part
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    // The padding word we added is zero, so upper 32 bits should be zero
    // and the visible TOI should equal the half-word value.
    EXPECT_EQ(p.toi(), 0xBEEFu);
}

TEST(AlcPacket, ParsesToiFlagValueTwo64BitExtension) {
    LctV1Base h;
    h.version        = 1;
    h.cc_flag        = 0;
    h.toi_flag       = 2;            // 2 full 32-bit words for TOI
    h.half_word_flag = 0;            // disable half-word; use full 64-bit TOI
    h.tsi_flag       = 1;            // need a TSI somewhere
    h.lct_header_len = 5;            // base(1)+CCI(1)+TSI(1)+TOI(2)
    h.codepoint      = 0;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                       // CCI
    AppendBE32(buf, 0x12345678);               // TSI 32-bit word
    AppendBE32(buf, 0xCAFEBABE);               // TOI upper 32 bits
    AppendBE32(buf, 0xDEADBEEF);               // TOI lower 32 bits
    // Minimal payload to keep CompactNoCode parser happy
    AppendBE16(buf, 0);                        // SBN
    AppendBE16(buf, 0);                        // ESI

    auto p = Parse(buf);
    // Implementation reads upper word first, then OR's lower into bits 0..31.
    EXPECT_EQ(p.toi(), (static_cast<std::uint64_t>(0xDEADBEEF) << 32) |
                        static_cast<std::uint64_t>(0xCAFEBABE));
}

// -----------------------------------------------------------------------------
// 7-8. RFC 6726 §5: codepoint field carries the FEC Encoding ID. By the
// FEC schemes registry, 0 = Compact No-Code (RFC 5445); 1 = Raptor
// (RFC 5053). Higher values are reserved or other schemes.
TEST(AlcPacket, MapsCodepointZeroToCompactNoCode) {
    DataPacketSpec spec;
    spec.codepoint = 0;
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    EXPECT_EQ(p.fec_scheme(), LibFlute::FecScheme::CompactNoCode);
}

TEST(AlcPacket, MapsCodepointOneToRaptor) {
    DataPacketSpec spec;
    spec.codepoint = 1;
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    EXPECT_EQ(p.fec_scheme(), LibFlute::FecScheme::Raptor);
}

// -----------------------------------------------------------------------------
// 9. RFC 6726 §3.4.1: EXT_FDT has HET = 192, FLUTE Version field, and a
// 20-bit FDT-Instance ID. RFC 6726 specifies Version = 2 (FLUTE v2). The
// libflute parser was written for FLUTE v1 (RFC 3926) and accepts only
// Version = 1.
TEST(AlcPacket, ParsesExtFdtVersionOneAndExtractsInstanceId) {
    // FLUTE v1 path (current implementation).
    DataPacketSpec spec;
    spec.toi = 0;
    spec.add_ext_fdt = true;
    spec.fdt_instance_id = 0xABCDE;   // 20-bit value
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    EXPECT_EQ(p.fdt_instance_id(), 0xABCDEu);
}

// RFC 6726 §3.4.1 specifies FLUTE Version = 2 in EXT_FDT (FLUTE v2);
// RFC 3926 (the older FLUTE) used Version = 1. An MBMS-conformant
// receiver MUST accept v2; a backward-compatible one accepts v1 too.
// Anything else is rejected to catch malformed packets early.
TEST(AlcPacket, AcceptsExtFdtVersionTwoPerRfc6726) {
    DataPacketSpec spec;
    spec.toi = 0;
    spec.add_ext_fdt = true;
    spec.fdt_instance_id = 0x12345;
    auto buf = BuildDataPacket(spec);
    // Patch the EXT_FDT FLUTE-version nibble from 1 → 2 in place. The
    // EXT_FDT extension is the FIRST extension after the LCT base + CCI
    // + TSI half + TOI half (offsets 0..11), so the HET=192 byte is at
    // offset 12 and the version+upper-nibble byte is at offset 13.
    ASSERT_EQ(buf[12], 192u);
    buf[13] = static_cast<std::uint8_t>(2u << 4 | (0x12345u >> 16) & 0x0Fu);
    auto p = Parse(buf);
    EXPECT_EQ(p.fdt_instance_id(), 0x12345u);
}

// FLUTE Version = 1 (RFC 3926) is still common in legacy senders. The
// receiver should accept it for back-compat — the existing
// ParsesExtFdtVersionOneAndExtractsInstanceId test covers this case.
// Versions other than 1 and 2 must be rejected.
TEST(AlcPacket, RejectsExtFdtVersionThreeAndAbove) {
    DataPacketSpec spec;
    spec.toi = 0;
    spec.add_ext_fdt = true;
    spec.fdt_instance_id = 0x42;
    auto buf = BuildDataPacket(spec);
    ASSERT_EQ(buf[12], 192u);
    buf[13] = static_cast<std::uint8_t>(3u << 4 | 0x0u);  // version 3
    EXPECT_THROW(Parse(buf), std::runtime_error);
}

// -----------------------------------------------------------------------------
// 10. RFC 6726 §3.4.3: EXT_CENC has HET = 193 and a single algorithm byte
// (0=NONE, 1=ZLIB, 2=DEFLATE, 3=GZIP).
TEST(AlcPacket, ParsesExtCencAlgorithmGzip) {
    DataPacketSpec spec;
    spec.toi = 0;
    spec.add_ext_fdt = true;             // EXT_CENC only valid alongside EXT_FDT
    spec.add_ext_cenc = true;
    spec.cenc_algorithm = 3;              // GZIP
    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);
    EXPECT_EQ(p.content_encoding(), LibFlute::ContentEncoding::GZIP);
}

// -----------------------------------------------------------------------------
// 11. RFC 5052 §3.4.3: EXT_FTI for Compact No-Code carries 48-bit transfer
// length, 16-bit encoding-symbol-length, and 32-bit max-source-block-length.
TEST(AlcPacket, ParsesExtFtiCompactNoCodeFields) {
    LctV1Base h;
    h.version        = 1;
    h.cc_flag        = 0;
    h.tsi_flag       = 0;
    h.toi_flag       = 0;
    h.half_word_flag = 1;
    h.codepoint      = 0;                 // CompactNoCode

    auto fti = BuildExtFtiCompactNoCode(
        /*transfer_length*/        std::uint64_t{0x123456789ABCULL},   // 48-bit
        /*encoding_symbol_length*/ 1024,
        /*max_source_block_length*/ 64);

    // Header words: base(1) + CCI(1) + TSI/TOI half(1) + EXT_FTI(4) = 7 words.
    h.lct_header_len = 7;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                   // CCI
    AppendBE16(buf, 1);                   // TSI half
    AppendBE16(buf, 5);                   // TOI half (>= 1, this is a data packet)
    AppendBytes(buf, fti);
    AppendBE16(buf, 0);                   // SBN
    AppendBE16(buf, 0);                   // ESI

    auto p = Parse(buf);
    EXPECT_EQ(p.fec_oti().transfer_length,         std::uint64_t{0x123456789ABCULL});
    EXPECT_EQ(p.fec_oti().encoding_symbol_length,  1024u);
    EXPECT_EQ(p.fec_oti().max_source_block_length, 64u);
}

// -----------------------------------------------------------------------------
// 12. RFC 5651 parse safety: an extension whose declared length runs past
// the LCT header end must be rejected, not silently accepted into payload.
TEST(AlcPacket, RejectsExtensionRunningPastLctHeaderEnd) {
    LctV1Base h;
    h.version        = 1;
    h.cc_flag        = 0;
    h.tsi_flag       = 0;
    h.toi_flag       = 0;
    h.half_word_flag = 1;
    h.codepoint      = 0;
    // Header words: base(1) + CCI(1) + TSI/TOI(1) = 3 words minimum.
    // We claim 4 (so 4 bytes of room for one extension), but the extension
    // claims HEL=2 (= 8 bytes), running past the LCT region.
    h.lct_header_len = 4;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                       // CCI
    AppendBE16(buf, 1);                       // TSI half
    AppendBE16(buf, 1);                       // TOI half
    // 4-byte slot for the extension. We write a HEL=2 (8-byte) extension.
    buf.push_back(64);                        // HET = EXT_FTI (HEL-bearing kind)
    buf.push_back(2);                         // HEL = 2 → 8 bytes total, > 4 we have
    AppendBE16(buf, 0);                       // 2 bytes content
    // Some payload bytes so total packet length exceeds LCT header length.
    AppendBE32(buf, 0xDEADBEEF);
    EXPECT_THROW(Parse(buf), std::runtime_error);
}

// -----------------------------------------------------------------------------
// RFC 5651 §3.2.5.1 EXT_NOP: variable-length no-op extension. The parser
// MUST advance past it by HEL × 4 bytes total (HEL words including the
// HET+HEL bytes themselves), preserving alignment for any following
// extension. Construct a packet whose LCT header carries EXT_NOP (HEL=1)
// followed by EXT_FDT, and verify that the EXT_FDT instance ID still
// parses correctly.
TEST(AlcPacket, ParsesExtNopHelOneFollowedByExtFdt) {
    // base(4) + CCI(4) + TSI/TOI half(4) + EXT_NOP(4) + EXT_FDT(4) = 20B = 5 words
    LctV1Base h;
    h.version = 1;
    h.half_word_flag = 1;
    h.codepoint = 0;
    h.lct_header_len = 5;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                  // CCI
    AppendBE16(buf, 1);                  // TSI half-word
    AppendBE16(buf, 7);                  // TOI half-word
    AppendBytes(buf, BuildExtNop(/*hel=*/1));
    AppendBytes(buf, BuildExtFdt(/*flute_version=*/1, /*instance_id=*/0xABCDE));

    auto p = Parse(buf);
    EXPECT_EQ(p.tsi(), 1u);
    EXPECT_EQ(p.toi(), 7u);
    EXPECT_EQ(p.fdt_instance_id(), 0xABCDEu);
}

// Two consecutive EXT_NOP (HEL=1) extensions in the LCT header. The
// parser MUST advance each by exactly 4 bytes; misalignment causes the
// second iteration to read garbage as the next HET byte.
TEST(AlcPacket, ParsesTwoConsecutiveExtNopHelOne) {
    // base + CCI + TSI/TOI half + 2 × EXT_NOP = 4 + 4 + 4 + 8 = 20B = 5 words
    LctV1Base h;
    h.version = 1;
    h.half_word_flag = 1;
    h.codepoint = 0;
    h.lct_header_len = 5;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                  // CCI
    AppendBE16(buf, 5);                  // TSI
    AppendBE16(buf, 11);                 // TOI
    AppendBytes(buf, BuildExtNop(/*hel=*/1));
    AppendBytes(buf, BuildExtNop(/*hel=*/1));

    auto p = Parse(buf);
    EXPECT_EQ(p.tsi(), 5u);
    EXPECT_EQ(p.toi(), 11u);
}

// EXT_NOP with HEL=2 (8-byte extension). Common in real packets when
// senders pad the LCT header for alignment. The parser MUST skip
// (HEL*4 - 2) = 6 content bytes, not a fixed 3.
TEST(AlcPacket, ParsesExtNopHelTwoFollowedByExtFdt) {
    // base + CCI + TSI/TOI half + EXT_NOP(HEL=2, 8B) + EXT_FDT(4B) = 24B = 6 words
    LctV1Base h;
    h.version = 1;
    h.half_word_flag = 1;
    h.codepoint = 0;
    h.lct_header_len = 6;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                  // CCI
    AppendBE16(buf, 9);                  // TSI
    AppendBE16(buf, 3);                  // TOI
    AppendBytes(buf, BuildExtNop(/*hel=*/2));
    AppendBytes(buf, BuildExtFdt(/*flute_version=*/1, /*instance_id=*/0x12345));

    auto p = Parse(buf);
    EXPECT_EQ(p.tsi(), 9u);
    EXPECT_EQ(p.toi(), 3u);
    EXPECT_EQ(p.fdt_instance_id(), 0x12345u);
}

// EXT_AUTH and EXT_TIME share the same skip-the-content code path as
// EXT_NOP. A receiver that doesn't validate the auth/time content MUST
// still advance past them correctly.
TEST(AlcPacket, ParsesExtAuthAndExtTimeFollowedByExtFdt) {
    // base + CCI + TSI/TOI half + EXT_AUTH(HEL=1) + EXT_TIME(HEL=1) + EXT_FDT
    //   = 4 + 4 + 4 + 4 + 4 + 4 = 24B = 6 words
    LctV1Base h;
    h.version = 1;
    h.half_word_flag = 1;
    h.codepoint = 0;
    h.lct_header_len = 6;

    std::vector<std::uint8_t> buf;
    AppendBytes(buf, EncodeLctBase(h));
    AppendBE32(buf, 0);                  // CCI
    AppendBE16(buf, 1);                  // TSI
    AppendBE16(buf, 1);                  // TOI
    AppendBytes(buf, BuildExtAuth(/*hel=*/1));
    AppendBytes(buf, BuildExtTime(/*hel=*/1));
    AppendBytes(buf, BuildExtFdt(/*flute_version=*/1, /*instance_id=*/0x55555));

    auto p = Parse(buf);
    EXPECT_EQ(p.fdt_instance_id(), 0x55555u);
}

// -----------------------------------------------------------------------------
// Round-trip: build a packet, parse it, all fields preserved.
TEST(AlcPacket, BuildAndParseRoundTripPreservesAllFields) {
    DataPacketSpec spec;
    spec.tsi = 0x1234;
    spec.toi = 0x5678;
    spec.codepoint = 1;                       // Raptor
    spec.add_ext_fdt = false;                 // TOI != 0
    spec.symbol_bytes = {0xAA, 0xBB, 0xCC, 0xDD};
    spec.sbn = 7;
    spec.esi = 42;

    auto buf = BuildDataPacket(spec);
    auto p = Parse(buf);

    EXPECT_EQ(p.tsi(), 0x1234u);
    EXPECT_EQ(p.toi(), 0x5678u);
    EXPECT_EQ(p.fec_scheme(), LibFlute::FecScheme::Raptor);
    EXPECT_EQ(p.content_encoding(), LibFlute::ContentEncoding::NONE);
    // header_length() is in BYTES, not words.
    EXPECT_EQ(p.header_length() % 4u, 0u);
    EXPECT_GE(p.header_length(), 12u);
}
