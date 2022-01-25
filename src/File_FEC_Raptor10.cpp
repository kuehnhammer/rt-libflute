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
#include "File_FEC_Raptor10.h"
#include "Partition.h"
#include "R10_Decoder.h"
#include "spdlog/spdlog.h"


LibFlute::File_FEC_Raptor10::File_FEC_Raptor10(LibFlute::FileDeliveryTable::FileEntry entry, bool enable_md5)
  : File( std::move(entry), enable_md5 )
{
  if (_meta.fec_oti.scheme_specific_info.length() != 4) {
    throw "Missing or malformed scheme specific info for Raptor FEC";
  }
  _receiving = true;
  _nof_source_blocks |= (uint8_t)_meta.fec_oti.scheme_specific_info[0] << 8;
  _nof_source_blocks |= (uint8_t)_meta.fec_oti.scheme_specific_info[1];
  _nof_sub_blocks = (uint8_t)_meta.fec_oti.scheme_specific_info[2];
  _symbol_alignment = (uint8_t)_meta.fec_oti.scheme_specific_info[3];

  calculate_partitioning();
  create_blocks();
}

LibFlute::File_FEC_Raptor10::File_FEC_Raptor10(uint32_t toi, 
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
  _receiving = false;
  unsigned W = 4096; // sub-block target size

  // RFC 5053  4.2
  unsigned Al = 4, Kmin = 1024, Kmax = 8192, Gmax = 10;
  unsigned P = (fec_oti.encoding_symbol_length - 4) & ~(Al - 1);
  unsigned F = length;
  unsigned G = std::min({(unsigned)(ceil(P*Kmin/F)), (unsigned)(P/Al), Gmax});
  auto T = floor(P/(Al*G))*Al;
  auto Kt = ceil(F/T);
  auto Z = ceil(Kt/Kmax);
  unsigned N = min((unsigned)ceil(ceil(Kt/Z)*T/W), (unsigned)(T/Al));

  _nof_source_blocks = Z;
  _nof_sub_blocks = N;
  _symbol_alignment = Al;
  _meta.fec_oti.encoding_symbol_length = T;
  _meta.fec_oti.scheme_specific_info.resize(4);
  _meta.fec_oti.scheme_specific_info[0] = (_nof_source_blocks & 0xFF00) >> 8;
  _meta.fec_oti.scheme_specific_info[1] = _nof_source_blocks & 0xFF;
  _meta.fec_oti.scheme_specific_info[2] = _nof_sub_blocks;
  _meta.fec_oti.scheme_specific_info[3] = _symbol_alignment;


  calculate_partitioning();
  create_blocks();

  for (auto& block : _source_blocks) {
    std::vector<uint32_t> ESI;
    for (auto& symbol : block.second.symbols) {
      block.second.raptor_src_symbols->symbol[symbol.first].data_reading( (uint8_t*)symbol.second.data, symbol.second.length );
      ESI.push_back(symbol.first);
      block.second.raptor_enc_symbols->ESIs.push_back(symbol.first);
    }
    for (auto& symbol : block.second.repair_symbols) {
      ESI.push_back(symbol.first);
      block.second.raptor_enc_symbols->ESIs.push_back(symbol.first);
    }
    LT_Encoding encoder(block.second.raptor_src_symbols.get());
    block.second.raptor_enc_symbols->symbol = encoder.LTEnc_Generate(ESI);
  }

  int k = 4;
}

auto LibFlute::File_FEC_Raptor10::calculate_partitioning() -> void
{
  uint32_t Kt = ceil(ceil((double)_meta.content_length / (double)_meta.fec_oti.encoding_symbol_length));
  auto p1 = Partition(Kt, _nof_source_blocks); 
  auto p2 = Partition(_meta.fec_oti.encoding_symbol_length / _symbol_alignment, _nof_source_blocks); 
  _nof_large_source_blocks = p1.get(2);
  _large_source_block_length = p1.get(0);
  _small_source_block_length = p1.get(1);
}

auto LibFlute::File_FEC_Raptor10::create_blocks() -> void
{
  // Create the required source blocks and encoding symbols
  auto buffer_ptr = _buffer;
  size_t remaining_size = _meta.content_length;
  for (int sbn = 0; sbn < _nof_source_blocks; sbn++) {
    SourceBlock block;
    auto symbol_id = 0;
    auto block_length = ( sbn < _nof_large_source_blocks ) ? _large_source_block_length : _small_source_block_length;

    unsigned nof_repair_symbols = _receiving ? 0 : std::max(3U, (unsigned)floor(block_length*0.1));
    spdlog::debug("SBN {}, len {}, nof_rep {}", sbn, block_length, nof_repair_symbols);
    block.raptor_enc_symbols = std::make_shared<Array_Data_Symbol>(block_length + nof_repair_symbols);
    block.raptor_enc_symbols->sym_len = _meta.fec_oti.encoding_symbol_length;

    block.raptor_src_symbols = std::make_shared<Array_Data_Symbol>(block_length, _meta.fec_oti.encoding_symbol_length);

    for (int i = 0; i < block_length; i++) {
      auto symbol_length = std::min(remaining_size, (size_t)_meta.fec_oti.encoding_symbol_length);
      assert(buffer_ptr + symbol_length <= _buffer + _meta.content_length);

      SourceBlock::SourceSymbol symbol{.data = buffer_ptr, .length = symbol_length, .complete = false, .queued = false};
      block.symbols[ symbol_id++ ] = symbol;
      
      remaining_size -= symbol_length;
      buffer_ptr += symbol_length;
      
      if (remaining_size <= 0) break;
    }

    for (int i = 0; i < nof_repair_symbols; i++) {
      SourceBlock::RepairSymbol symbol{.complete = false, .queued = false};
      block.repair_symbols[ symbol_id++ ] = symbol;
    }

    _source_blocks[sbn] = std::move(block);
  }
}

auto LibFlute::File_FEC_Raptor10::put_symbol( const LibFlute::EncodingSymbol& symbol ) -> void
{
  if (symbol.source_block_number() > _source_blocks.size()) {
    throw "Source Block number too high";
  } 
  SourceBlock& source_block = _source_blocks[ symbol.source_block_number() ];

  // if (symbol.id() == 2) return; // intentionally drop a symbol for testing purposes

  source_block.raptor_enc_symbols->ESIs.push_back(symbol.id());
  source_block.raptor_enc_symbols->symbol.emplace_back(_meta.fec_oti.encoding_symbol_length);
  source_block.raptor_enc_symbols->symbol.back().data_reading( symbol.buffer() );

      spdlog::debug("putting ESI {}, sz {}, sz {}", symbol.id(), source_block.raptor_enc_symbols->ESIs.size(), source_block.symbols.size());

  if (source_block.raptor_enc_symbols->ESIs.size() >= source_block.symbols.size()) {
    // attempt to decode
    try {
      R10_Decoder decoder(source_block.symbols.size(), _meta.fec_oti.encoding_symbol_length);
      auto source = decoder.Get_Source_Symbol(*source_block.raptor_enc_symbols.get(), source_block.raptor_enc_symbols->ESIs.size());
      spdlog::debug("R10 decoding succeeded");

      for (int i = 0; i < source_block.symbols.size(); i++) {
        auto& target_symbol = source_block.symbols[i];
      spdlog::debug("copy {} bytes into sym {}",  target_symbol.length, i);
        memcpy(target_symbol.data, &(source.symbol[i].s[0]), target_symbol.length);
        target_symbol.complete = true;
      }
    } catch(const char* ex) {
      spdlog::debug("R10 decoding failed: {}", ex);
    }
    check_source_block_completion(source_block);
    check_file_completion();
  }
} 

auto LibFlute::File_FEC_Raptor10::check_source_block_completion( SourceBlock& block ) -> void
{
  bool src_complete = std::all_of(block.symbols.begin(), block.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
  if (block.repair_symbols.size() == 0) {
    block.complete = src_complete;
  } else {
    bool repair_complete = std::all_of(block.repair_symbols.begin(), block.repair_symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
    block.complete = src_complete && repair_complete;
  }
}
auto LibFlute::File_FEC_Raptor10::check_file_completion() -> void
{
  _complete = std::all_of(_source_blocks.begin(), _source_blocks.end(), [](const auto& block){ return block.second.complete; });
  if (_enable_md5) {
    check_md5();
  }
}

auto LibFlute::File_FEC_Raptor10::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol> 
{
  auto block = _source_blocks.begin();
  int nof_symbols = std::floor((float)(max_size - 4) / (float)_meta.fec_oti.encoding_symbol_length);
  auto cnt = 0;
  std::vector<EncodingSymbol> symbols;
  
  for (auto& block : _source_blocks) {
    if (cnt > 0) break;

    if (!block.second.complete) {
      for (auto& symbol : block.second.symbols) {
        if (cnt >= nof_symbols) break;
    
        if (!symbol.second.complete && !symbol.second.queued) {
          symbols.emplace_back(symbol.first, block.first, 
              (char*)&(block.second.raptor_enc_symbols->symbol[symbol.first].s[0]), 
              _meta.fec_oti.encoding_symbol_length,
              _meta.fec_oti.encoding_id);
          symbol.second.queued = true;
          cnt++;
        }
      }
      if (cnt > 0) break;
      for (auto& symbol : block.second.repair_symbols) {
        if (cnt >= nof_symbols) break;
    
        if (!symbol.second.complete && !symbol.second.queued) {
          symbols.emplace_back(symbol.first, block.first, 
              (char*)&(block.second.raptor_enc_symbols->symbol[symbol.first].s[0]), 
              block.second.raptor_enc_symbols->symbol[symbol.first].d_len,
              _meta.fec_oti.encoding_id);
          symbol.second.queued = true;
          cnt++;
        }
      }
    }
  }
  return symbols;
}

auto LibFlute::File_FEC_Raptor10::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
{
  for (auto& symbol : symbols) {
    auto block = _source_blocks.find(symbol.source_block_number());
    if (block != _source_blocks.end()) {
      auto sym = block->second.symbols.find(symbol.id());
      if (sym != block->second.symbols.end()) {
        sym->second.queued = false;
        sym->second.complete = success;
      }
      auto rsym = block->second.repair_symbols.find(symbol.id());
      if (rsym != block->second.repair_symbols.end()) {
        rsym->second.queued = false;
        rsym->second.complete = success;
      }
      check_source_block_completion(block->second);
      check_file_completion();
    }
  }
}

auto LibFlute::File_FEC_Raptor10::reset() -> void
{
  for (auto& block : _source_blocks) {
    for (auto& symbol : block.second.symbols) {
      symbol.second.complete = false;
    }
    block.second.complete = false;
  } 
  _complete = false;
}
