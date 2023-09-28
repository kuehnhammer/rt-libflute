// libflute - FLUTE/ALC library
//
// Copyright (C) 2022 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
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
#include "File_FEC_CompactNoCode.h"
#include <cmath>
#include <cassert>
#include <algorithm>

LibFlute::File_FEC_CompactNoCode::File_FEC_CompactNoCode(LibFlute::FileDeliveryTable::FileEntry entry, bool enable_md5)
  : File( std::move(entry), enable_md5 )
{
  calculate_partitioning();
  create_blocks();
}

LibFlute::File_FEC_CompactNoCode::File_FEC_CompactNoCode(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data,
          bool enable_md5)
  : File(toi, std::move(fec_oti), std::move(content_location), std::move(content_type), 
          expires, data, length, copy_data, enable_md5)
{
  _meta.fec_oti.transfer_length = _meta.content_length;
  calculate_partitioning();
  create_blocks();
}

auto LibFlute::File_FEC_CompactNoCode::calculate_partitioning() -> void
{
  // Calculate source block partitioning (RFC5052 9.1) 
  _nof_source_symbols = std::ceil((double)_meta.content_length / (double)_meta.fec_oti.encoding_symbol_length);
  _nof_source_blocks = std::ceil((double)_nof_source_symbols / (double)_meta.fec_oti.max_source_block_length);
  _large_source_block_length = std::ceil((double)_nof_source_symbols / (double)_nof_source_blocks);
  _small_source_block_length = floor((double)_nof_source_symbols / (double)_nof_source_blocks);
  _nof_large_source_blocks = _nof_source_symbols - _small_source_block_length * _nof_source_blocks;
}

auto LibFlute::File_FEC_CompactNoCode::create_blocks() -> void
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

auto LibFlute::File_FEC_CompactNoCode::put_symbol( const LibFlute::EncodingSymbol& symbol ) -> void
{
  if (symbol.source_block_number() > _source_blocks.size()) {
    throw "Source Block number too high";
  } 
  SourceBlock& source_block = _source_blocks[ symbol.source_block_number() ];

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
} 

auto LibFlute::File_FEC_CompactNoCode::check_source_block_completion( SourceBlock& block ) -> void
{
  block.complete = std::all_of(block.symbols.begin(), block.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
}
auto LibFlute::File_FEC_CompactNoCode::check_file_completion() -> void
{
  _complete = std::all_of(_source_blocks.begin(), _source_blocks.end(), [](const auto& block){ return block.second.complete; });
  if (_enable_md5) {
    check_md5();
  }
}

auto LibFlute::File_FEC_CompactNoCode::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol> 
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

auto LibFlute::File_FEC_CompactNoCode::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
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

auto LibFlute::File_FEC_CompactNoCode::reset() -> void
{
  for (auto& block : _source_blocks) {
    for (auto& symbol : block.second.symbols) {
      symbol.second.complete = false;
    }
    block.second.complete = false;
  } 
  _complete = false;
}
