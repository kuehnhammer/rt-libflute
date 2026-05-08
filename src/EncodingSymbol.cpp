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

#include "EncodingSymbol.h"
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>    // for htons, ntohs (no WSAStartup needed)
#else
#  include <netinet/in.h>  // for htons, ntohs
#endif
#include <algorithm>        // for min
#include <cmath>            // for ceil
#include <cstring>          // for memcpy
#include <stdexcept>        // for runtime_error
#include "spdlog/spdlog.h"  // for warn

namespace {

template <typename T>
T read_unaligned(const char* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

void write_unaligned_u16(char* p, uint16_t v) {
  std::memcpy(p, &v, sizeof(v));
}

}  // namespace

auto LibFlute::EncodingSymbol::from_payload(char* encoded_data, size_t data_len, const FecOti& fec_oti, ContentEncoding encoding) -> std::vector<EncodingSymbol>
{
  auto source_block_number = 0;
  auto encoding_symbol_id = 0;
  std::vector<EncodingSymbol> symbols;

  if (encoding != ContentEncoding::NONE) {
    throw std::runtime_error("Only unencoded content is supported");
  }

  if (data_len < 4) {
    throw std::runtime_error("Payload too short to contain SBN/ESI header");
  }

  switch (fec_oti.encoding_id) {
    case FecScheme::CompactNoCode:
    case FecScheme::Raptor:
      // RFC 5052 §3.4.1 / RFC 5053 §3.2: FEC Payload ID = SBN(16) + ESI(16).
      source_block_number = ntohs(read_unaligned<uint16_t>(encoded_data));
      encoded_data += 2;
      encoding_symbol_id = ntohs(read_unaligned<uint16_t>(encoded_data));
      encoded_data += 2;
      data_len -= 4;
      break;
    case FecScheme::RaptorQ: {
      // RFC 6330 §3.2: FEC Payload ID = SBN(8) + ESI(24).
      source_block_number = static_cast<uint8_t>(encoded_data[0]);
      encoding_symbol_id  = (static_cast<uint8_t>(encoded_data[1]) << 16) |
                            (static_cast<uint8_t>(encoded_data[2]) << 8)  |
                             static_cast<uint8_t>(encoded_data[3]);
      encoded_data += 4;
      data_len     -= 4;
      break;
    }
    default:
      throw std::runtime_error("Invalid FEC encoding ID. Supported: CompactNoCode, R10, RaptorQ.");
  }

  if (fec_oti.encoding_symbol_length == 0) {
    throw std::runtime_error("FEC OTI encoding_symbol_length is zero");
  }

  // Carve the payload into encoding_symbol_length-sized chunks. RFC 5052
  // §9.1: when transfer_length is not an exact multiple of T, the LAST
  // symbol of the source block carries the residual bytes (< T). We
  // track remaining_data per iteration so the trailing symbol reports
  // the true residual length, not T.
  size_t remaining_data = data_len;
  while (remaining_data > 0) {
    const size_t this_symbol_len =
        std::min(remaining_data, static_cast<size_t>(fec_oti.encoding_symbol_length));
    switch (fec_oti.encoding_id) {
      default:
      case FecScheme::CompactNoCode:
      case FecScheme::Raptor:
        symbols.emplace_back(encoding_symbol_id, source_block_number,
                             encoded_data, this_symbol_len, fec_oti.encoding_id);
        break;
    }
    encoded_data    += fec_oti.encoding_symbol_length;
    remaining_data  -= this_symbol_len;
    encoding_symbol_id++;
  }

  return symbols;
}

auto LibFlute::EncodingSymbol::to_payload(const std::vector<EncodingSymbol>& symbols, char* encoded_data, size_t data_len, const FecOti& fec_oti) -> size_t
{
  size_t len = 0;
  auto* ptr = encoded_data;
  auto first_symbol = symbols.begin();
  switch (fec_oti.encoding_id) {
    case FecScheme::CompactNoCode:
    case FecScheme::Raptor:
      // RFC 5052 §3.4.1 / RFC 5053 §3.2: SBN(16) + ESI(16).
      write_unaligned_u16(ptr, htons(first_symbol->source_block_number()));
      ptr += 2;
      write_unaligned_u16(ptr, htons(first_symbol->id()));
      ptr += 2;
      len += 4;
      break;
    case FecScheme::RaptorQ: {
      // RFC 6330 §3.2: SBN(8) + ESI(24).
      const auto sbn = first_symbol->source_block_number();
      const auto esi = first_symbol->id();
      ptr[0] = static_cast<char>(sbn & 0xFFU);
      ptr[1] = static_cast<char>((esi >> 16) & 0xFFU);
      ptr[2] = static_cast<char>((esi >>  8) & 0xFFU);
      ptr[3] = static_cast<char>( esi        & 0xFFU);
      ptr += 4;
      len += 4;
      break;
    }
    default:
      throw std::runtime_error("Invalid FEC encoding ID. Supported: CompactNoCode, R10, RaptorQ.");
  }

  for (const auto& symbol : symbols) {
    if (symbol.len() <= data_len) {
      auto symbol_len = symbol.encode_to(ptr, data_len);
      data_len -= symbol_len;
      ptr += symbol_len;
      len += symbol_len;
    }
  }
  return len;
}

auto LibFlute::EncodingSymbol::decode_to(char* buffer, size_t max_length) const -> void {
  switch (_fec_scheme) {
    case FecScheme::CompactNoCode:
    case FecScheme::Raptor:
    case FecScheme::RaptorQ:
      // All three schemes carry the symbol bytes verbatim in the
      // encoding-symbol payload at this layer — RFC 5053 / RFC 6330
      // systematic property. The codec-specific repair logic happens
      // upstream in the FecTransformer, not here.
      if (_data_len <= max_length) {
        memcpy(buffer, _encoded_data, _data_len);
      }
      break;
    default:
      spdlog::warn("EncodingSymbol::decode_to() called for unknown fec scheme {}",(int)_fec_scheme);
      throw std::runtime_error("EncodingSymbol::decode_to() called for unknown fec scheme");
  }
}

auto LibFlute::EncodingSymbol::encode_to(char* buffer, size_t max_length) const -> size_t {
  switch (_fec_scheme) {
    default:
    case FecScheme::CompactNoCode:
    case FecScheme::Raptor:
    case FecScheme::RaptorQ:
      if (_data_len <= max_length) {
        memcpy(buffer, _encoded_data, _data_len);
        return _data_len;
      }
      break;
  }
  return 0;
}
