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
#include <netinet/in.h>     // for htons, ntohs
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
      source_block_number = ntohs(read_unaligned<uint16_t>(encoded_data));
      encoded_data += 2;
      encoding_symbol_id = ntohs(read_unaligned<uint16_t>(encoded_data));
      encoded_data += 2;
      data_len -= 4;
      break;
    default:
      throw std::runtime_error("Invalid FEC encoding ID. Only 2 FEC types are currently supported: compact no-code or raptor");
  }

  if (fec_oti.encoding_symbol_length == 0) {
    throw std::runtime_error("FEC OTI encoding_symbol_length is zero");
  }

  int nof_symbols = std::ceil((float)data_len / (float)fec_oti.encoding_symbol_length);
  for (int i = 0; i < nof_symbols; i++) {
    switch (fec_oti.encoding_id) {
      default:
      case FecScheme::CompactNoCode:
      case FecScheme::Raptor:
        symbols.emplace_back(encoding_symbol_id, source_block_number, encoded_data, std::min(data_len, (size_t)fec_oti.encoding_symbol_length), fec_oti.encoding_id);
        break;
    }
    encoded_data += fec_oti.encoding_symbol_length;
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
      write_unaligned_u16(ptr, htons(first_symbol->source_block_number()));
      ptr += 2;
      write_unaligned_u16(ptr, htons(first_symbol->id()));
      ptr += 2;
      len += 4;
      break;
    default:
      throw std::runtime_error("Invalid FEC encoding ID. Only 2 FEC types are currently supported: compact no-code or raptor");
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
      if (_data_len <= max_length) {
        memcpy(buffer, _encoded_data, _data_len);
        return _data_len;
      }
      break;
  }
  return 0;
}
