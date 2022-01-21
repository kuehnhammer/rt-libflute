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
#include "File.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <openssl/md5.h>
#include "base64.h"
#include "spdlog/spdlog.h"

#include "Partition.h"
#include "Array_Data_Types.h"
#include "R10_Decoder.h"

LibFlute::File::File(LibFlute::FileDeliveryTable::FileEntry entry)
  : _meta( std::move(entry) )
  , _received_at( time(nullptr) )
{
  // Allocate a data buffer
  _buffer = (char*)malloc(_meta.content_length);
  if (_buffer == nullptr)
  {
    throw "Failed to allocate file buffer";
  }
  _own_buffer = true;

  if (_meta.fec_oti.encoding_id == FecScheme::Raptor) {
    if (_meta.fec_oti.scheme_specific_info.length() != 4) {
      throw "Missing or malformed scheme specific info for Raptor FEC";
    }
    _nof_source_blocks |= (uint8_t)_meta.fec_oti.scheme_specific_info[0] << 8;
    _nof_source_blocks |= (uint8_t)_meta.fec_oti.scheme_specific_info[1];
    _nof_sub_blocks = (uint8_t)_meta.fec_oti.scheme_specific_info[2];
    _symbol_alignment = (uint8_t)_meta.fec_oti.scheme_specific_info[3];

    calculate_raptor_partitioning();
    create_raptor_blocks();

//     _meta.fec_oti.encoding_symbol_length); 
  } else if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode) {
    calculate_compactnocode_partitioning();
  create_blocks();
  }
}

LibFlute::File::File(uint32_t toi,
    FecOti fec_oti,
    std::string content_location,
    std::string content_type,
    uint64_t expires,
    char* data,
    size_t length,
    bool copy_data) 
{
  if (copy_data) {
    _buffer = (char*)malloc(length);
    if (_buffer == nullptr)
    {
      throw "Failed to allocate file buffer";
    }
    memcpy(_buffer, data, length);
    _own_buffer = true;
  } else {
    _buffer = data;
  }

  unsigned char md5[MD5_DIGEST_LENGTH];
  MD5((const unsigned char*)data, length, md5);

  _meta.toi = toi;
  _meta.content_location = std::move(content_location);
  _meta.content_type = std::move(content_type);
  _meta.content_length = length;
  _meta.content_md5 = base64_encode(md5, MD5_DIGEST_LENGTH);
  _meta.expires = expires;
  _meta.fec_oti = std::move(fec_oti);

  // for no-code
  if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode) { 
    _meta.fec_oti.transfer_length = length;
  } else {
    throw "Unsupported FEC scheme";
  }

  calculate_compactnocode_partitioning();
  create_blocks();
}

LibFlute::File::~File()
{
  if (_own_buffer && _buffer != nullptr)
  {
    free(_buffer);
  }
}

auto LibFlute::File::put_symbol( const LibFlute::EncodingSymbol& symbol ) -> void
{
  if (symbol.source_block_number() > _source_blocks.size()) {
    throw "Source Block number too high";
  } 
  SourceBlock& source_block = _source_blocks[ symbol.source_block_number() ];

  if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode) {
    if (symbol.id() > source_block.symbols.size()) {
      throw "Encoding Symbol ID too high";
    } 

    SourceBlock::Symbol& target_symbol = source_block.symbols[symbol.id()];

    if (!target_symbol.complete) {
      symbol.copy_encoded(target_symbol.data, target_symbol.length);
      target_symbol.complete = true;

      check_source_block_completion(source_block);
      check_file_completion();
    }
  } else if (_meta.fec_oti.encoding_id == FecScheme::Raptor) {
    //  if (symbol.id() == 2) return; // intentionally drop a symbol for testing purposes

    source_block.raptor_enc_symbols->ESIs.push_back(symbol.id());
    source_block.raptor_enc_symbols->symbol.emplace_back(_meta.fec_oti.encoding_symbol_length);
    source_block.raptor_enc_symbols->symbol.back().data_reading( symbol.buffer() );

    if (source_block.raptor_enc_symbols->ESIs.size() >= source_block.symbols.size()) {
      // attempt to decode
      try {
        R10_Decoder decoder(source_block.symbols.size(), _meta.fec_oti.encoding_symbol_length);
        auto source = decoder.Get_Source_Symbol(*source_block.raptor_enc_symbols.get(), source_block.raptor_enc_symbols->ESIs.size());
        spdlog::debug("R10 decoding succeeded");

        for (int i = 0; i < source_block.symbols.size(); i++) {
          SourceBlock::Symbol& target_symbol = source_block.symbols[i];
          std::copy(source.symbol[i].s.begin(), source.symbol[i].s.end(), target_symbol.data);
          target_symbol.complete = true;
        }
      } catch(const char* ex) {
        spdlog::debug("R10 decoding failed: {}", ex);
      }
      check_source_block_completion(source_block);
      check_file_completion();
    }
  }

}

auto LibFlute::File::check_source_block_completion( SourceBlock& block ) -> void
{
  block.complete = std::all_of(block.symbols.begin(), block.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
}

auto LibFlute::File::check_file_completion() -> void
{
  _complete = std::all_of(_source_blocks.begin(), _source_blocks.end(), [](const auto& block){ return block.second.complete; });

  if (_complete && _enable_md5_check && !_meta.content_md5.empty()) {
    //Array_Data_Symbol testing_symbol(SYMBOL_SIZE, SYMBOL_LEN);
    //check MD5 sum
    unsigned char md5[MD5_DIGEST_LENGTH];
    MD5((const unsigned char*)buffer(), length(), md5);

    auto content_md5 = base64_decode(_meta.content_md5);
    if (memcmp(md5, content_md5.c_str(), MD5_DIGEST_LENGTH) != 0) {
      spdlog::debug("MD5 mismatch for TOI {}, discarding", _meta.toi);
 
      // MD5 mismatch, try again
      for (auto& block : _source_blocks) {
        for (auto& symbol : block.second.symbols) {
          symbol.second.complete = false;
        }
        block.second.complete = false;
      } 
      _complete = false;
    }
  }
}

auto LibFlute::File::calculate_compactnocode_partitioning() -> void
{
  // Calculate source block partitioning (RFC5052 9.1) 
  _nof_source_symbols = ceil((double)_meta.content_length / (double)_meta.fec_oti.encoding_symbol_length);
  _nof_source_blocks = ceil((double)_nof_source_symbols / (double)_meta.fec_oti.max_source_block_length);
  _large_source_block_length = ceil((double)_nof_source_symbols / (double)_nof_source_blocks);
  _small_source_block_length = floor((double)_nof_source_symbols / (double)_nof_source_blocks);
  _nof_large_source_blocks = _nof_source_symbols - _small_source_block_length * _nof_source_blocks;
}

auto LibFlute::File::calculate_raptor_partitioning() -> void
{
  uint32_t Kt = ceil(ceil((double)_meta.content_length / (double)_meta.fec_oti.encoding_symbol_length));
  auto p1 = Partition(Kt, _nof_source_blocks); 
  auto p2 = Partition(_meta.fec_oti.encoding_symbol_length / _symbol_alignment, _nof_source_blocks); 
  _nof_large_source_blocks = p1.get(2);
  _large_source_block_length = p1.get(0);
  _small_source_block_length = p1.get(1);
}

auto LibFlute::File::create_blocks() -> void
{
  // Create the required source blocks and encoding symbols
  auto buffer_ptr = _buffer;
  size_t remaining_size = _meta.content_length;
  auto number = 0;
  while (remaining_size > 0) {
    SourceBlock block;
    auto symbol_id = 0;
    auto block_length = ( number < _nof_large_source_blocks ) ? _large_source_block_length : _small_source_block_length;

    for (int i = 0; i < block_length; i++) {
      auto symbol_length = std::min(remaining_size, (size_t)_meta.fec_oti.encoding_symbol_length);
      assert(buffer_ptr + symbol_length <= _buffer + _meta.content_length);

      SourceBlock::Symbol symbol{.data = buffer_ptr, .length = symbol_length, .complete = false};
      block.symbols[ symbol_id++ ] = symbol;
      
      remaining_size -= symbol_length;
      buffer_ptr += symbol_length;
      
      if (remaining_size <= 0) break;
    }
    _source_blocks[number++] = block;
  }
}

auto LibFlute::File::create_raptor_blocks() -> void
{
  // Create the required source blocks and encoding symbols
  auto buffer_ptr = _buffer;
  size_t remaining_size = _meta.content_length;
  auto number = 0;
  while (remaining_size > 0) {
    SourceBlock block;
    auto symbol_id = 0;
    auto block_length = ( number < _nof_large_source_blocks ) ? _large_source_block_length : _small_source_block_length;

    block.raptor_enc_symbols = std::make_shared<Array_Data_Symbol>(block_length);
    block.raptor_enc_symbols->sym_len = _meta.fec_oti.encoding_symbol_length;

    for (int i = 0; i < block_length; i++) {
      auto symbol_length = std::min(remaining_size, (size_t)_meta.fec_oti.encoding_symbol_length);
      assert(buffer_ptr + symbol_length <= _buffer + _meta.content_length);

      SourceBlock::Symbol symbol{.data = buffer_ptr, .length = symbol_length, .complete = false};
      block.symbols[ symbol_id++ ] = symbol;
      
      remaining_size -= symbol_length;
      buffer_ptr += symbol_length;
      
      if (remaining_size <= 0) break;
    }
    _source_blocks[number++] = std::move(block);
  }
}

auto LibFlute::File::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol> 
{
  auto block = _source_blocks.begin();
  int nof_symbols = std::ceil((float)(max_size - 4) / (float)_meta.fec_oti.encoding_symbol_length);
  auto cnt = 0;
  std::vector<EncodingSymbol> symbols;
  
  for (auto& block : _source_blocks) {
    if (cnt >= nof_symbols) break;

    if (!block.second.complete) {
      for (auto& symbol : block.second.symbols) {
        if (cnt >= nof_symbols) break;
    
        if (!symbol.second.complete && !symbol.second.queued) {
          symbols.emplace_back(symbol.first, block.first, symbol.second.data, symbol.second.length, _meta.fec_oti.encoding_id);
          symbol.second.queued = true;
          cnt++;
        }
      }
    }
  }
  return symbols;

}

auto LibFlute::File::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
{
  for (auto& symbol : symbols) {
    auto block = _source_blocks.find(symbol.source_block_number());
    if (block != _source_blocks.end()) {
      auto sym = block->second.symbols.find(symbol.id());
      if (sym != block->second.symbols.end()) {
        sym->second.queued = false;
        sym->second.complete = success;
      }
      check_source_block_completion(block->second);
      check_file_completion();
    }
  }
}
