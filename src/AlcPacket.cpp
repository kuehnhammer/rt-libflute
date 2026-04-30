// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.
//
#include "AlcPacket.h"
#include <netinet/in.h>      // for ntohl, htons, ntohs, htonl
#include <cstdlib>          // for calloc, free
#include <cstring>           // for memcpy
#include <stdexcept>         // for runtime_error
#include <utility>           // for move
#include "EncodingSymbol.h"  // for EncodingSymbol
#include "spdlog/spdlog.h"   // for warn

namespace {

// Read an unsigned integer from a (possibly unaligned) buffer position
// without violating strict aliasing or alignment requirements. Modern
// compilers fold this to a plain load on architectures that allow it.
template <typename T>
T read_unaligned(const char* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

void write_unaligned_u16(char* p, uint16_t v) {
  std::memcpy(p, &v, sizeof(v));
}

void write_unaligned_u32(char* p, uint32_t v) {
  std::memcpy(p, &v, sizeof(v));
}

}  // namespace

LibFlute::AlcPacket::AlcPacket(char* data, size_t len)
{
  // Helper that aborts the parse if `need` more bytes aren't available
  // beyond `consumed`. Centralizes bounds checking so we never read past
  // the buffer end on malformed/truncated input (e.g. cell-edge RLC
  // reassembly artefacts).
  auto remaining = [&](size_t consumed) -> size_t {
    return (consumed > len) ? 0 : (len - consumed);
  };

  if (len < 4) {
    throw std::runtime_error("Packet too short");
  }

  std::memcpy(&_lct_header, data, 4);
  if (_lct_header.version != 1) {
    throw std::runtime_error("Unsupported LCT version");
  }

  // Total declared LCT header length, in bytes. The wire field is 8 bits
  // but the spec only reserves 4; cap at the declared packet length so a
  // bogus value can't drive header_length() past the buffer end.
  const size_t declared_header_len =
      static_cast<size_t>(_lct_header.lct_header_len) * 4;
  if (declared_header_len < 4 || declared_header_len > len) {
    throw std::runtime_error("LCT header length exceeds packet size");
  }

  size_t consumed = 4;
  char* hdr_ptr = data + consumed;
  if (_lct_header.congestion_control_flag != 0) {
    throw std::runtime_error("Unsupported CCI field length");
  }
  // [TODO] read CCI
  if (remaining(consumed) < 4) {
    throw std::runtime_error("Truncated packet: missing CCI");
  }
  hdr_ptr += 4;
  consumed += 4;

  if (_lct_header.half_word_flag == 0 && _lct_header.tsi_flag == 0) {
    throw std::runtime_error("TSI field not present");
  }
  auto tsi_shift = 0;
  if (_lct_header.half_word_flag == 1) {
    if (remaining(consumed) < 2) {
      throw std::runtime_error("Truncated packet: missing TSI half-word");
    }
    _tsi = ntohs(read_unaligned<uint16_t>(hdr_ptr));
    tsi_shift = 16;
    hdr_ptr += 2;
    consumed += 2;
  }
  if (_lct_header.tsi_flag == 1) {
    if (remaining(consumed) < 4) {
      throw std::runtime_error("Truncated packet: missing TSI word");
    }
    _tsi |= static_cast<uint64_t>(ntohl(read_unaligned<uint32_t>(hdr_ptr))) << tsi_shift;
    hdr_ptr += 4;
    consumed += 4;
  }

  if (_lct_header.close_session_flag == 0 && _lct_header.half_word_flag == 0 && _lct_header.toi_flag == 0) {
    throw std::runtime_error("TOI field not present");
  }
  auto toi_shift = 0;
  if (_lct_header.half_word_flag == 1) {
    if (remaining(consumed) < 2) {
      throw std::runtime_error("Truncated packet: missing TOI half-word");
    }
    _toi = ntohs(read_unaligned<uint16_t>(hdr_ptr));
    toi_shift = 16;
    hdr_ptr += 2;
    consumed += 2;
  }
  switch (_lct_header.toi_flag) {
    case 0:
      break;
    case 1:
      if (remaining(consumed) < 4) {
        throw std::runtime_error("Truncated packet: missing TOI word");
      }
      _toi |= static_cast<uint64_t>(ntohl(read_unaligned<uint32_t>(hdr_ptr))) << toi_shift;
      hdr_ptr += 4;
      consumed += 4;
      break;
    case 2:
      if (toi_shift > 0) {
        throw std::runtime_error("TOI fields over 64 bits in length are not supported");
      }
      if (remaining(consumed) < 8) {
        throw std::runtime_error("Truncated packet: missing TOI double-word");
      }
      _toi = ntohl(read_unaligned<uint32_t>(hdr_ptr));
      hdr_ptr += 4;
      consumed += 4;
      _toi |= static_cast<uint64_t>(ntohl(read_unaligned<uint32_t>(hdr_ptr))) << 32;
      hdr_ptr += 4;
      consumed += 4;
      break;
    default:
      throw std::runtime_error("TOI fields over 64 bits in length are not supported");
  }

  switch (_lct_header.codepoint) {
    case 0:
      _fec_oti.encoding_id = FecScheme::CompactNoCode;
      break;
    case 1:
      _fec_oti.encoding_id = FecScheme::Raptor;
      break;
    default:
      throw std::runtime_error("Only the Compact No-Code and Raptor FEC schemes are supported");
  }

  auto expected_header_len = 2 +
   _lct_header.congestion_control_flag +
   _lct_header.half_word_flag +
   _lct_header.tsi_flag +
   _lct_header.toi_flag;

  // Number of extension-header bytes remaining in the LCT region; clamp
  // against the declared LCT header size so a malicious lct_header_len
  // can't make us walk past the LCT region into payload territory.
  if (declared_header_len < static_cast<size_t>(expected_header_len) * 4) {
    throw std::runtime_error("LCT header length too small for declared fields");
  }
  size_t ext_header_len =
      declared_header_len - static_cast<size_t>(expected_header_len) * 4;
  if (remaining(consumed) < ext_header_len) {
    throw std::runtime_error("Truncated packet: extension headers exceed packet size");
  }

  while (ext_header_len > 0) {
    // Each extension header is at minimum 4 bytes (1B HET + either
    // an HEL byte or implicit length 1 word, plus 2B of content).
    if (ext_header_len < 4) {
      throw std::runtime_error("Truncated extension header");
    }
    uint8_t het = static_cast<uint8_t>(*hdr_ptr);
    hdr_ptr += 1;
    consumed += 1;
    uint8_t hel = 0;
    if (het < 128) {
      hel = static_cast<uint8_t>(*hdr_ptr);
      hdr_ptr += 1;
      consumed += 1;
      if (hel == 0) {
        throw std::runtime_error("Invalid zero-length extension header");
      }
    }

    // Total length in bytes consumed by this extension (HET+HEL+content).
    const size_t this_ext_bytes = (het < 128)
        ? static_cast<size_t>(hel) * 4
        : 4;  // het >= 128 means implicit 1-word (4-byte) extension
    if (this_ext_bytes > ext_header_len) {
      throw std::runtime_error("Extension header runs past LCT header end");
    }

    switch (static_cast<AlcPacket::HeaderExtension>(het)) {
      case EXT_NOP:
      case EXT_AUTH:
      case EXT_TIME: {
        hdr_ptr += 3;
        consumed += 3;
        break;  // ignored
      }
      case EXT_FTI: {
        switch (_fec_oti.encoding_id) {
          case FecScheme::CompactNoCode:
            if (hel != 4) {
              throw std::runtime_error("Invalid length for EXT_FTI header extension for Compact No Code FEC scheme");
            }
            // RFC 5052 §3.4.3: transfer-length is 48 bits, split as 16
            // upper bits + 32 lower bits.
            _fec_oti.transfer_length =
                static_cast<uint64_t>(ntohs(read_unaligned<uint16_t>(hdr_ptr))) << 32;
            hdr_ptr += 2;
            consumed += 2;
            _fec_oti.transfer_length |=
                static_cast<uint64_t>(ntohl(read_unaligned<uint32_t>(hdr_ptr)));
            hdr_ptr += 4;
            consumed += 4;
            hdr_ptr += 2;  // reserved
            consumed += 2;
            _fec_oti.encoding_symbol_length =
                ntohs(read_unaligned<uint16_t>(hdr_ptr));
            hdr_ptr += 2;
            consumed += 2;
            _fec_oti.max_source_block_length =
                ntohl(read_unaligned<uint32_t>(hdr_ptr));
            hdr_ptr += 4;
            consumed += 4;
            break;
          case FecScheme::Raptor:
            //TODO
            spdlog::warn("Raptor FEC support in EXT_FTI header extension is still in progress");
            throw std::runtime_error("Raptor FEC support in EXT_FTI header extension is still in progress");
          default:
            throw std::runtime_error("Unsupported FEC scheme");
        }
        break;
      }
      case EXT_FDT: {
        uint8_t flute_version = (*hdr_ptr & 0xF0) >> 4;
        if (flute_version != 1) {
          throw std::runtime_error("Only FLUTE version 1 is supported");
        }
        _fdt_instance_id = (*hdr_ptr & 0x0F) << 16;
        hdr_ptr++;
        consumed++;
        _fdt_instance_id |= ntohs(read_unaligned<uint16_t>(hdr_ptr));
        hdr_ptr += 2;
        consumed += 2;
        break;
      }
      case EXT_CENC: {
        uint8_t encoding = static_cast<uint8_t>(*hdr_ptr);
        switch (encoding) {
          case 0: _content_encoding = ContentEncoding::NONE; break;
          case 1: _content_encoding = ContentEncoding::ZLIB; break;
          case 2: _content_encoding = ContentEncoding::DEFLATE; break;
          case 3: _content_encoding = ContentEncoding::GZIP; break;
        }
        hdr_ptr += 3;
        consumed += 3;
        break;
      }
    }

    ext_header_len -= this_ext_bytes;
  }
}

LibFlute::AlcPacket::AlcPacket(uint16_t tsi, uint16_t toi, LibFlute::FecOti fec_oti, const std::vector<LibFlute::EncodingSymbol>& symbols, size_t max_size, uint32_t fdt_instance_id) // NOLINT
  : _fec_oti(std::move(fec_oti))
{
  auto lct_header_len = 3;
  if (toi == 0) { // Add extensions for FDT
    lct_header_len += 5;
  }

  auto max_packet_length = max_size +
    static_cast<long>(lct_header_len) * 4
    + 4 ;

  _buffer = (char*)calloc(max_packet_length, sizeof(char));

  auto* lct_header = (lct_header_t*)_buffer;

  lct_header->version = 1;
  lct_header->half_word_flag = 1;
  if (_fec_oti.encoding_id == LibFlute::FecScheme::CompactNoCode) {
    lct_header->codepoint = 0;
  } else if (_fec_oti.encoding_id == LibFlute::FecScheme::Raptor) {
    lct_header->codepoint = 1;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }
  lct_header->lct_header_len = lct_header_len;
  lct_header->codepoint = (uint8_t)_fec_oti.encoding_id;
  auto* hdr_ptr = _buffer + 4;
  auto* payload_ptr = _buffer + 4UL * lct_header_len;

  auto payload_size = EncodingSymbol::to_payload(symbols, payload_ptr, max_size, _fec_oti);
  _len = 4L * lct_header_len + payload_size;

  hdr_ptr += 4; // CCI = 0

  write_unaligned_u16(hdr_ptr, htons(tsi));
  hdr_ptr += 2;

  write_unaligned_u16(hdr_ptr, htons(toi));
  hdr_ptr += 2;

  if (toi == 0) { // Add extensions for FDT
    *((uint8_t*)hdr_ptr) = EXT_FDT;
    hdr_ptr += 1;
    *((uint8_t*)hdr_ptr) = 1 << 4 | (fdt_instance_id & 0x000F0000) >> 16;
    hdr_ptr += 1;
    write_unaligned_u16(hdr_ptr, htons(fdt_instance_id & 0x0000FFFF));
    hdr_ptr += 2;

    *((uint8_t*)hdr_ptr) = EXT_FTI;
    hdr_ptr += 1;
    *((uint8_t*)hdr_ptr) = 4; // HEL
    hdr_ptr += 1;
    // RFC 5052 §3.4.3: transfer-length is 48 bits → upper 16 bits here.
    write_unaligned_u16(hdr_ptr,
        htons(static_cast<uint16_t>((_fec_oti.transfer_length >> 32) & 0xFFFFu)));
    hdr_ptr += 2;
    write_unaligned_u32(hdr_ptr,
        htonl(static_cast<uint32_t>(_fec_oti.transfer_length & 0xFFFFFFFFu)));
    hdr_ptr += 4;
    hdr_ptr += 2; // reserved
    write_unaligned_u16(hdr_ptr, htons(_fec_oti.encoding_symbol_length));
    hdr_ptr += 2;
    write_unaligned_u32(hdr_ptr, htonl(_fec_oti.max_source_block_length));
  }
}

LibFlute::AlcPacket::~AlcPacket()
{
  if (_buffer != nullptr) {
    free(_buffer);
  }
}
