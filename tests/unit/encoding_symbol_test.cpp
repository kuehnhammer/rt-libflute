// EncodingSymbol — RFC 5052 §3.5 (FEC Payload ID for Compact No-Code FEC)
// and the SBN/ESI framing carried in the ALC payload.
//
// Wire layout for FEC encoding ID 0 (Compact No-Code) and the FLUTE
// Raptor profile (RFC 5053 §3.4 references RFC 5052 framing):
//   bytes 0-1  Source Block Number   (network byte order)
//   bytes 2-3  Encoding Symbol ID    (network byte order)
//   bytes 4..  Symbol payload (one or more encoding symbols, each
//              encoding_symbol_length bytes; final symbol may be
//              shorter when transfer_length is not a multiple of T).
//
// These tests target the parser/producer in EncodingSymbol.cpp; they
// must hold for any RFC-conformant implementation, not just the
// current code.

#include "EncodingSymbol.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "flute_types.h"

namespace {

// Compose the SBN/ESI header in network byte order followed by a
// payload blob. Returns a heap-owned buffer because EncodingSymbol's
// from_payload takes a non-const char* (it stores pointers into the
// caller's buffer, so the caller must keep it alive).
std::vector<char> MakePayload(std::uint16_t sbn, std::uint16_t esi,
                               const std::vector<std::uint8_t>& body) {
    std::vector<char> p;
    p.reserve(4 + body.size());
    p.push_back(static_cast<char>((sbn >> 8) & 0xFF));
    p.push_back(static_cast<char>(sbn & 0xFF));
    p.push_back(static_cast<char>((esi >> 8) & 0xFF));
    p.push_back(static_cast<char>(esi & 0xFF));
    for (auto b : body) p.push_back(static_cast<char>(b));
    return p;
}

LibFlute::FecOti CompactNoCodeOti(std::uint32_t T,
                                   std::uint64_t transfer_length = 0,
                                   std::uint32_t max_sbl = 0) {
    LibFlute::FecOti oti{};
    oti.encoding_id = LibFlute::FecScheme::CompactNoCode;
    oti.transfer_length = transfer_length;
    oti.encoding_symbol_length = T;
    oti.max_source_block_length = max_sbl;
    return oti;
}

}  // namespace

// RFC 5052 §3.5: parser MUST recover Source Block Number and Encoding
// Symbol ID from the leading 4 bytes of the payload, big-endian.
TEST(EncodingSymbolFromPayload, ExtractsSbnAndEsiInNetworkByteOrder) {
    constexpr std::uint16_t kSbn = 0x1234;
    constexpr std::uint16_t kEsi = 0xABCD;
    const std::vector<std::uint8_t> body(8, 0xAA);  // one full symbol of T=8

    auto buf = MakePayload(kSbn, kEsi, body);
    const auto oti = CompactNoCodeOti(/*T=*/8);

    auto symbols = LibFlute::EncodingSymbol::from_payload(
        buf.data(), buf.size(), oti, LibFlute::ContentEncoding::NONE);

    ASSERT_EQ(symbols.size(), 1U);
    EXPECT_EQ(symbols[0].source_block_number(), kSbn);
    EXPECT_EQ(symbols[0].id(), kEsi);
    EXPECT_EQ(symbols[0].len(), 8U);
}

// RFC 6726 §3.4.3 EXT_CENC describes content-encoding negotiation for
// the *FDT*; data symbols are not transformed by the FLUTE layer. The
// EncodingSymbol parser is therefore contracted to only accept
// ContentEncoding::NONE — anything else must error.
TEST(EncodingSymbolFromPayload, RejectsContentEncodingOtherThanNone) {
    auto buf = MakePayload(0, 0, std::vector<std::uint8_t>(8, 0));
    const auto oti = CompactNoCodeOti(/*T=*/8);

    EXPECT_THROW(
        LibFlute::EncodingSymbol::from_payload(buf.data(), buf.size(), oti,
                                                LibFlute::ContentEncoding::ZLIB),
        std::runtime_error);
    EXPECT_THROW(
        LibFlute::EncodingSymbol::from_payload(buf.data(), buf.size(), oti,
                                                LibFlute::ContentEncoding::DEFLATE),
        std::runtime_error);
    EXPECT_THROW(
        LibFlute::EncodingSymbol::from_payload(buf.data(), buf.size(), oti,
                                                LibFlute::ContentEncoding::GZIP),
        std::runtime_error);
}

// A payload shorter than the 4-byte SBN/ESI header cannot be parsed.
// Permissive accept-and-truncate is forbidden by RFC 5052 §3.5: the
// FEC Payload ID is mandatory for Compact No-Code and Raptor.
TEST(EncodingSymbolFromPayload, RejectsPayloadShorterThanSbnEsiHeader) {
    std::vector<char> three_bytes = {0, 0, 0};
    const auto oti = CompactNoCodeOti(/*T=*/8);

    EXPECT_THROW(
        LibFlute::EncodingSymbol::from_payload(three_bytes.data(),
                                                three_bytes.size(), oti,
                                                LibFlute::ContentEncoding::NONE),
        std::runtime_error);

    std::vector<char> empty;
    EXPECT_THROW(
        LibFlute::EncodingSymbol::from_payload(empty.data(), empty.size(), oti,
                                                LibFlute::ContentEncoding::NONE),
        std::runtime_error);
}

// RFC 5052 §3.5 / RFC 5053 §3.4: a single ALC payload may carry N
// consecutive encoding symbols belonging to the same (SBN, starting-
// ESI) tuple. The first symbol takes the wire ESI; subsequent ones
// have ESI incremented by 1 per symbol.
TEST(EncodingSymbolFromPayload, MultiSymbolPayloadEmitsSequentialEsi) {
    constexpr std::uint32_t T = 4;
    constexpr std::uint16_t kStartEsi = 100;
    // 3 full symbols × 4 bytes = 12 bytes
    std::vector<std::uint8_t> body;
    for (std::uint8_t i = 0; i < 12; ++i) body.push_back(i);

    auto buf = MakePayload(/*sbn=*/7, kStartEsi, body);
    const auto oti = CompactNoCodeOti(T);

    auto symbols = LibFlute::EncodingSymbol::from_payload(
        buf.data(), buf.size(), oti, LibFlute::ContentEncoding::NONE);

    ASSERT_EQ(symbols.size(), 3U);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        EXPECT_EQ(symbols[i].source_block_number(), 7U);
        EXPECT_EQ(symbols[i].id(), kStartEsi + i);
        EXPECT_EQ(symbols[i].len(), T);
    }

    // Each symbol must round-trip back to its 4 source bytes.
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        std::array<char, T> out{};
        symbols[i].decode_to(out.data(), out.size());
        for (std::uint32_t j = 0; j < T; ++j) {
            EXPECT_EQ(static_cast<std::uint8_t>(out[j]),
                      static_cast<std::uint8_t>(body[i * T + j]))
                << "symbol " << i << " byte " << j;
        }
    }
}

// to_payload(from_payload(...)) must be the identity for SBN/ESI and
// the symbol bytes (RFC 5052 §3.5 wire format is bidirectional).
TEST(EncodingSymbolRoundTrip, ToPayloadFromPayloadPreservesEsiSequence) {
    constexpr std::uint32_t T = 6;
    constexpr std::uint16_t kStartEsi = 0x4242;
    constexpr std::uint16_t kSbn = 0x0F0F;

    // Two full symbols' worth of distinct bytes.
    std::vector<std::uint8_t> body;
    for (std::uint8_t i = 0; i < 2 * T; ++i) {
        body.push_back(static_cast<std::uint8_t>(0xC0 + i));
    }

    auto src_buf = MakePayload(kSbn, kStartEsi, body);
    const auto oti = CompactNoCodeOti(T);

    auto symbols = LibFlute::EncodingSymbol::from_payload(
        src_buf.data(), src_buf.size(), oti, LibFlute::ContentEncoding::NONE);
    ASSERT_EQ(symbols.size(), 2U);

    std::vector<char> out_buf(4 + 2 * T, 0);
    auto written = LibFlute::EncodingSymbol::to_payload(
        symbols, out_buf.data(), out_buf.size(), oti);
    EXPECT_EQ(written, 4U + 2U * T);

    // Re-parse the produced bytes; SBN/ESI/payload must match the input.
    auto reparsed = LibFlute::EncodingSymbol::from_payload(
        out_buf.data(), written, oti, LibFlute::ContentEncoding::NONE);
    ASSERT_EQ(reparsed.size(), 2U);
    EXPECT_EQ(reparsed[0].source_block_number(), kSbn);
    EXPECT_EQ(reparsed[0].id(), kStartEsi);
    EXPECT_EQ(reparsed[1].source_block_number(), kSbn);
    EXPECT_EQ(reparsed[1].id(), kStartEsi + 1);

    for (std::size_t i = 0; i < reparsed.size(); ++i) {
        std::vector<char> dec(T, 0);
        reparsed[i].decode_to(dec.data(), dec.size());
        for (std::uint32_t j = 0; j < T; ++j) {
            EXPECT_EQ(static_cast<std::uint8_t>(dec[j]),
                      static_cast<std::uint8_t>(body[i * T + j]))
                << "symbol " << i << " byte " << j;
        }
    }
}

// RFC 5052 §9.1 source-block partitioning: when transfer_length is not
// an exact multiple of the encoding_symbol_length T, the *last* symbol
// of the source block carries the residual bytes (length < T) per the
// "B small / B large" partitioning rules. Receivers MUST report the
// last-symbol length truthfully so the assembler can place it.
TEST(EncodingSymbolFromPayload, LastPartialSymbolHasCorrectLength) {
    constexpr std::uint32_t T = 8;
    // 1 full symbol (8 bytes) + 1 partial (3 bytes) = 11 bytes of body.
    std::vector<std::uint8_t> body(11, 0x55);

    auto buf = MakePayload(/*sbn=*/0, /*esi=*/0, body);
    const auto oti = CompactNoCodeOti(T);

    auto symbols = LibFlute::EncodingSymbol::from_payload(
        buf.data(), buf.size(), oti, LibFlute::ContentEncoding::NONE);

    ASSERT_EQ(symbols.size(), 2U);
    EXPECT_EQ(symbols[0].len(), 8U);
    EXPECT_EQ(symbols[1].len(), 3U);
}
