// libflute test fixtures — wire-format byte builders for RFC 5651 LCT and
// the RFC 6726 EXT_FDT / EXT_CENC / EXT_FTI extensions. Tests use these
// to build minimal valid (and deliberately malformed) packets without
// hand-counting bytes.
//
// Spec references:
//   RFC 5651 §5.1   LCT header format
//   RFC 6726 §3.4.1 EXT_FDT (HET = 192)
//   RFC 6726 §3.4.3 EXT_CENC (HET = 193)
//   RFC 5052 §3.4.3 EXT_FTI (HET = 64)

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace libflute_test {

// ---------------------------------------------------------------------------
// LCT v1 base header (RFC 5651 §5.1, 4 bytes).
//
// Wire byte 0 (MSB → LSB):
//   bits 0-3  V   (version, must be 1)
//   bits 4-5  C   (congestion_control_flag, cc_flag)
//   bit  6    PSI (reserved / source_packet_indicator)
//   bit  7    R   (reserved)
// Wire byte 1 (MSB → LSB):
//   bit  0    T   (tsi_flag)
//   bits 1-2  O   (toi_flag, 0..3)
//   bit  3    H   (half_word_flag)
//   bits 4-5  Res (reserved)
//   bit  6    A   (close_session_flag)
//   bit  7    B   (close_object_flag)
// Wire byte 2: HDR_LEN (length of the entire LCT header in 32-bit words)
// Wire byte 3: Codepoint (CP) — used by FLUTE for FEC encoding ID
//
// We build the byte directly so the test stays independent of the parser's
// bit-field struct layout.
struct LctV1Base {
    std::uint8_t version = 1;
    std::uint8_t cc_flag = 0;          // 0 → 32-bit CCI follows base
    std::uint8_t psi     = 0;
    std::uint8_t res_b0  = 0;
    std::uint8_t tsi_flag       = 0;   // see TSI/TOI helpers below
    std::uint8_t toi_flag       = 0;
    std::uint8_t half_word_flag = 0;
    std::uint8_t res_b1  = 0;
    std::uint8_t close_session_flag = 0;
    std::uint8_t close_object_flag  = 0;
    std::uint8_t lct_header_len = 0;   // in 32-bit words; caller sets after
                                        // assembling extensions
    std::uint8_t codepoint = 0;
};

inline std::array<std::uint8_t, 4> EncodeLctBase(const LctV1Base& h) {
    const std::uint8_t b0 = static_cast<std::uint8_t>(
        (h.version & 0x0F) << 4 |
        (h.cc_flag & 0x03) << 2 |
        (h.psi     & 0x01) << 1 |
        (h.res_b0  & 0x01));
    const std::uint8_t b1 = static_cast<std::uint8_t>(
        (h.tsi_flag       & 0x01) << 7 |
        (h.toi_flag       & 0x03) << 5 |
        (h.half_word_flag & 0x01) << 4 |
        (h.res_b1         & 0x03) << 2 |
        (h.close_session_flag & 0x01) << 1 |
        (h.close_object_flag  & 0x01));
    return {b0, b1, h.lct_header_len, h.codepoint};
}

// Append a big-endian uintN to a byte vector.
inline void AppendBE16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}
inline void AppendBE32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >>  8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}
inline void AppendBE48(std::vector<std::uint8_t>& v, std::uint64_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 40) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 32) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >>  8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

// ---------------------------------------------------------------------------
// EXT_FDT (RFC 6726 §3.4.1) — 4 bytes total, het >= 128 (no HEL byte).
//   byte 0: HET = 192
//   byte 1: V (4 bits, FLUTE version) | upper 4 bits of FDT-Instance ID
//   bytes 2-3: lower 16 bits of FDT-Instance ID
// FDT-Instance ID is 20 bits total.
inline std::array<std::uint8_t, 4> BuildExtFdt(std::uint8_t flute_version,
                                                std::uint32_t instance_id) {
    return {
        std::uint8_t{192},
        static_cast<std::uint8_t>((flute_version & 0x0F) << 4 |
                                   ((instance_id >> 16) & 0x0F)),
        static_cast<std::uint8_t>((instance_id >>  8) & 0xFF),
        static_cast<std::uint8_t>(instance_id        & 0xFF),
    };
}

// ---------------------------------------------------------------------------
// EXT_CENC (RFC 6726 §3.4.3) — 4 bytes total, het >= 128.
//   byte 0: HET = 193
//   byte 1: CENC algorithm (0=NONE, 1=ZLIB, 2=DEFLATE, 3=GZIP)
//   bytes 2-3: reserved (must be 0 on TX, ignored on RX)
inline std::array<std::uint8_t, 4> BuildExtCenc(std::uint8_t algorithm) {
    return {std::uint8_t{193}, algorithm, 0, 0};
}

// ---------------------------------------------------------------------------
// EXT_FTI for the Compact No-Code FEC scheme (RFC 5052 §3.4.3 + RFC 5445).
//   byte 0:  HET = 64
//   byte 1:  HEL = 4 (length in 32-bit words including HET+HEL)
//   bytes 2-3: upper 16 bits of Transfer Length (48-bit total)
//   bytes 4-7: lower 32 bits of Transfer Length
//   bytes 8-9: reserved (zeros)
//   bytes 10-11: Encoding Symbol Length (16 bits)
//   bytes 12-15: Maximum Source Block Length (32 bits)
//   = 16 bytes total
inline std::vector<std::uint8_t>
BuildExtFtiCompactNoCode(std::uint64_t transfer_length,
                          std::uint16_t encoding_symbol_length,
                          std::uint32_t max_source_block_length) {
    std::vector<std::uint8_t> b;
    b.reserve(16);
    b.push_back(64);   // HET
    b.push_back(4);    // HEL
    AppendBE16(b, static_cast<std::uint16_t>((transfer_length >> 32) & 0xFFFF));
    AppendBE32(b, static_cast<std::uint32_t>(transfer_length & 0xFFFFFFFF));
    AppendBE16(b, 0);   // reserved
    AppendBE16(b, encoding_symbol_length);
    AppendBE32(b, max_source_block_length);
    return b;
}

// ---------------------------------------------------------------------------
// Concatenate raw bytes into one buffer. Used to assemble a complete LCT
// packet from base + CCI + TSI/TOI + extensions + payload.
inline void AppendBytes(std::vector<std::uint8_t>& v,
                        const std::uint8_t* p, std::size_t n) {
    v.insert(v.end(), p, p + n);
}
template <std::size_t N>
inline void AppendBytes(std::vector<std::uint8_t>& v,
                        const std::array<std::uint8_t, N>& a) {
    v.insert(v.end(), a.begin(), a.end());
}
inline void AppendBytes(std::vector<std::uint8_t>& v,
                        const std::vector<std::uint8_t>& other) {
    v.insert(v.end(), other.begin(), other.end());
}

// Build a minimal valid Compact-No-Code FLUTE data packet (TOI > 0):
// LCT v1 base, 32-bit CCI, 16-bit TSI half-word, 16-bit TOI half-word,
// optional EXT_FDT / EXT_CENC, then SBN/ESI payload header + symbol bytes.
struct DataPacketSpec {
    std::uint16_t tsi = 1;
    std::uint16_t toi = 1;
    std::uint8_t  codepoint = 0;       // 0 = CompactNoCode
    bool          add_ext_fdt    = false;
    bool          add_ext_cenc   = false;
    bool          add_ext_fti    = false;     // CompactNoCode 16-byte FTI
    std::uint8_t  cenc_algorithm = 0;
    std::uint32_t fdt_instance_id = 0;
    std::uint64_t fti_transfer_length = 0;
    std::uint16_t fti_encoding_symbol_length = 0;
    std::uint32_t fti_max_source_block_length = 0;
    std::uint16_t sbn = 0;
    std::uint16_t esi = 0;
    std::vector<std::uint8_t> symbol_bytes{};   // payload after SBN/ESI
    std::uint8_t  toi_flag = 0;                  // 0 = TOI fits in half-word only
};

inline std::vector<std::uint8_t> BuildDataPacket(const DataPacketSpec& s) {
    std::vector<std::uint8_t> p;

    // Reserve LCT base + CCI + TSI half + TOI half + 0..2 extensions + payload
    LctV1Base h;
    h.version        = 1;
    h.cc_flag        = 0;
    h.tsi_flag       = 0;
    h.toi_flag       = s.toi_flag;
    h.half_word_flag = 1;
    h.codepoint      = s.codepoint;

    // Header word count: base(1) + CCI(1) + TSI/TOI half(1) = 3, plus
    // toi_flag extra words, plus extensions (EXT_FDT=1 word, EXT_CENC=1
    // word, EXT_FTI Compact-No-Code=4 words).
    std::uint8_t header_words = 3;
    header_words = static_cast<std::uint8_t>(header_words + s.toi_flag);
    if (s.add_ext_fdt)  header_words = static_cast<std::uint8_t>(header_words + 1);
    if (s.add_ext_cenc) header_words = static_cast<std::uint8_t>(header_words + 1);
    if (s.add_ext_fti)  header_words = static_cast<std::uint8_t>(header_words + 4);
    h.lct_header_len = header_words;

    AppendBytes(p, EncodeLctBase(h));
    AppendBE32(p, 0);                     // CCI = 0 (32 bits)
    AppendBE16(p, s.tsi);                  // TSI half-word
    AppendBE16(p, s.toi);                  // TOI half-word
    // toi_flag is the count of additional TOI 32-bit words; we pad zeros
    // so the packet stays parseable even if the test doesn't care.
    for (std::uint8_t i = 0; i < s.toi_flag; ++i) AppendBE32(p, 0);

    // Extensions go after the TSI/TOI region, before the payload.
    // RFC 6726 §3.4.1: when an FDT packet (TOI=0) is sent, EXT_FDT
    // and EXT_FTI are both expected; emit them in the order the
    // canonical libflute Transmitter does (EXT_FDT, then EXT_FTI),
    // followed by EXT_CENC if requested.
    if (s.add_ext_fdt)  AppendBytes(p, BuildExtFdt(1, s.fdt_instance_id));
    if (s.add_ext_fti) {
        AppendBytes(p, BuildExtFtiCompactNoCode(s.fti_transfer_length,
                                                  s.fti_encoding_symbol_length,
                                                  s.fti_max_source_block_length));
    }
    if (s.add_ext_cenc) AppendBytes(p, BuildExtCenc(s.cenc_algorithm));

    // Payload: SBN(16) + ESI(16) + symbol bytes (CompactNoCode/Raptor framing).
    AppendBE16(p, s.sbn);
    AppendBE16(p, s.esi);
    AppendBytes(p, s.symbol_bytes.data(), s.symbol_bytes.size());

    return p;
}

}  // namespace libflute_test
