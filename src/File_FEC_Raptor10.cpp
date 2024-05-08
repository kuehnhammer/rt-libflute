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
//#include "Partition.h"
//#include "R10_Decoder.h"
#include "spdlog/spdlog.h"


LibFlute::File_FEC_Raptor10::File_FEC_Raptor10(LibFlute::FileDeliveryTable::FileEntry entry)
  : File( std::move(entry) )
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

  // Allocate a data buffer
  size_t padded_buffer_size = _nof_large_source_blocks * _large_source_block_length +
    (_nof_source_blocks - _nof_large_source_blocks) * _small_source_block_length;
  _buffer = (char*)calloc(padded_buffer_size, sizeof(char));

  if (_buffer == nullptr)
  {
    throw "Failed to allocate file buffer";
  }
  _own_buffer = true;

  create_blocks();
}

LibFlute::File_FEC_Raptor10::File_FEC_Raptor10(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data)
  : File(toi, std::move(fec_oti), std::move(content_location), std::move(content_type), 
          expires, data, length, copy_data )
{
  _receiving = false;
  unsigned W = 4096; // sub-block target size

  // RFC 5053  4.2
  unsigned Al = 4, Kmin = 1024, Kmax = 8192, Gmax = 10;
  unsigned P = (_meta.fec_oti.encoding_symbol_length - 4) & ~(Al - 1);
  unsigned F = length;
  unsigned G = std::min({
      (unsigned)std::ceil((unsigned)(P * Kmin / F)), 
      (unsigned)(P/Al),
      Gmax
      });
  auto T = std::floor((unsigned)(P / (Al * G))) * Al;
  auto Kt = std::ceil(F / T);
  auto Z = ceil(Kt / Kmax);
  unsigned N = std::min((unsigned)ceil(ceil(Kt / Z) * T / W), (unsigned)(T / Al));

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

  /*
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
  */
}


auto LibFlute::File_FEC_Raptor10::partition(unsigned i, unsigned j) 
  -> std::array<unsigned, 4> 
{
  // 5.3.1.2
  unsigned il = std::ceil((double)i / (double)j);
  unsigned is = std::floor((double)i / (double)j);
  unsigned jl = i - is * j;
  unsigned js = j - jl;
  return { il, is, jl, js };
};

auto LibFlute::File_FEC_Raptor10::calculate_partitioning() -> void
{
  // 5.3.1.2
  spdlog::debug("Partitioning inputs:\n\
      F (transfer length) {} bytes\n\
      Al (symbol alignment) {} bytes\n\
      T (symbol size) {} bytes\n\
      Z (nr of source blocks) {}\n\
      N (nr of sub blocks) {}",
      _meta.content_length,
      _symbol_alignment,
      _meta.fec_oti.encoding_symbol_length,
      _nof_source_blocks,
      _nof_sub_blocks
      );

  uint32_t Kt = std::ceil((double)_meta.content_length / (double)_meta.fec_oti.encoding_symbol_length);

  auto [KL, KS, ZL, ZS] = partition(Kt, _nof_source_blocks);
  auto [TL, TS, NL, NS] = 
    partition(_meta.fec_oti.encoding_symbol_length / _symbol_alignment, _nof_sub_blocks);

  _nof_large_source_blocks = ZL;
  assert(_nof_source_blocks == _nof_large_source_blocks + ZS);
  _large_source_block_length = KL * _meta.fec_oti.encoding_symbol_length;
  _small_source_block_length = KS * _meta.fec_oti.encoding_symbol_length;

  _nof_large_sub_blocks = NL;
  assert(_nof_sub_blocks == _nof_large_sub_blocks + NS);
  _large_sub_block_symbol_size = TL * _symbol_alignment;
  _small_sub_block_symbol_size = TS * _symbol_alignment;

  spdlog::debug("Partitioning:\n\
      {} large source blocks, {} bytes\n\
      {} small source blocks, {} bytes\n\
      {} large sub blocks, {} bytes sub-symbol size\n\
      {} small sub blocks, {} bytes sub-symbol size",
      _nof_large_source_blocks, _large_source_block_length, ZS, _small_source_block_length,
      _nof_large_sub_blocks, _large_sub_block_symbol_size, NS, _small_sub_block_symbol_size);
}

auto LibFlute::File_FEC_Raptor10::create_blocks() -> void
{
  auto* buffer_ptr = _buffer;

  for (auto source_block_nr = 0U; source_block_nr < _nof_source_blocks; source_block_nr++) {
  spdlog::debug("nr {} nof large {}", source_block_nr, _nof_large_source_blocks);
    SourceBlock block {
      .sbn = source_block_nr,
      .size = (source_block_nr < _nof_large_source_blocks) ? 
        _large_source_block_length : _small_source_block_length,
      .nr_of_symbols = block.size / _meta.fec_oti.encoding_symbol_length
    };
    block.completed_symbols.resize(block.nr_of_symbols);

    spdlog::debug("Source block {}: {} bytes, K: {}", block.sbn, block.size, block.nr_of_symbols);

    for (auto sub_block_nr = 0U; sub_block_nr < _nof_sub_blocks; sub_block_nr++) {
      SubBlock sub_block;
      spdlog::debug("Sub block {}", sub_block_nr);

      size_t sub_symbol_size = (sub_block_nr < _nof_large_sub_blocks) ? 
        _large_sub_block_symbol_size : _small_sub_block_symbol_size;

      for (auto sub_symbol_idx = 0U; sub_symbol_idx < block.nr_of_symbols; sub_symbol_idx++) {
        SubSymbol sub_symbol {
          .size = sub_symbol_size,
          .data = buffer_ptr
        };
        spdlog::debug("Sub symbol {}: {} bytes at {}", sub_symbol_idx, sub_symbol.size,
           fmt::ptr(buffer_ptr));
        buffer_ptr += sub_symbol.size;

        sub_block.sub_symbols.emplace_back(sub_symbol);
      }
      block.sub_blocks.emplace_back(sub_block);
    }
    _source_blocks.emplace(source_block_nr, block);
  }
}

auto LibFlute::File_FEC_Raptor10::put_symbol( const LibFlute::EncodingSymbol& symbol ) -> void
{
  spdlog::debug("Incoming data for SBN {}, ESI {}: {} bytes",
      symbol.source_block_number(), symbol.id(), symbol.len());

  if (symbol.source_block_number() > _source_blocks.size()) {
    throw "Source Block number too high";
  } 
  auto& source_block = _source_blocks[ symbol.source_block_number() ];

  if (symbol.id() >= source_block.nr_of_symbols) {
    // handle repair symbol
    return;
  } 

  auto* received_data = symbol.buffer();
  for (auto& sub_block : source_block.sub_blocks) {
    memcpy(sub_block.sub_symbols[symbol.id()].data, received_data, 
        sub_block.sub_symbols[symbol.id()].size);
    received_data += sub_block.sub_symbols[symbol.id()].size;
  }
  source_block.completed_symbols[symbol.id()] = true;

  check_source_block_completion(source_block);
  check_file_completion();
  // if (symbol.id() == 2) return; // intentionally drop a symbol for testing purposes

  //source_block.raptor_enc_symbols->ESIs.push_back(symbol.id());
  //source_block.raptor_enc_symbols->symbol.emplace_back(_meta.fec_oti.encoding_symbol_length);
  //source_block.raptor_enc_symbols->symbol.back().data_reading( symbol.buffer() );

#if 0
  if (!source_block.complete &&
      source_block.raptor_enc_symbols->ESIs.size() >= source_block.symbols.size()) {
    // attempt to decode
    try {
      R10_Decoder decoder(source_block.symbols.size(), _meta.fec_oti.encoding_symbol_length);
      auto source = decoder.Get_Source_Symbol(*source_block.raptor_enc_symbols.get(), source_block.raptor_enc_symbols->ESIs.size());
      spdlog::trace("R10 decoding succeeded for TOI {} SBN {}, {} symbols", 
          _meta.toi, symbol.source_block_number(),
          source_block.symbols.size());

      for (int i = 0; i < source_block.symbols.size(); i++) {
        auto& target_symbol = source_block.symbols[i];
        memcpy(target_symbol.data, &(source.symbol[i].s[0]), target_symbol.length);
        target_symbol.complete = true;
      }
    } catch(const char* ex) {
      spdlog::trace("R10 decoding FAILED for TOI {} SBN {}, {} symbols", 
          _meta.toi, symbol.source_block_number(),
          source_block.symbols.size());
    }
    check_file_completion();
  }
#endif
} 

auto LibFlute::File_FEC_Raptor10::check_source_block_completion( SourceBlock& block ) -> void
{
  block.complete = std::all_of(block.completed_symbols.begin(), block.completed_symbols.end(), 
      [](bool b) { return b; });

#if 0
  if (_receiving || block.repair_symbols.size() == 0) {
    block.complete = src_complete;
  } else {
    bool repair_complete = std::all_of(block.repair_symbols.begin(), block.repair_symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
    block.complete = src_complete && repair_complete;
  }
#endif
}
auto LibFlute::File_FEC_Raptor10::check_file_completion() -> void
{
  _complete = std::all_of(_source_blocks.begin(), 
      _source_blocks.end(), [](const auto& block){ return block.second.complete; });
  check_md5();
}

auto LibFlute::File_FEC_Raptor10::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol> 
{
  auto block = _source_blocks.begin();
  int nof_symbols = std::floor((float)(max_size - 4) / (float)_meta.fec_oti.encoding_symbol_length);
  auto cnt = 0;
  std::vector<EncodingSymbol> symbols;
  
#if 0
  for (auto& block : _source_blocks) {
    if (cnt > 0) break;

    if (!block.second.complete) {
      for (auto& symbol : block.second.symbols) {
        if (cnt >= nof_symbols) break;
    
        if (!symbol.second.complete && !symbol.second.queued) {
  //        symbols.emplace_back(symbol.first, block.first, 
  //            (char*)&(block.second.raptor_enc_symbols->symbol[symbol.first].s[0]), 
  //            _meta.fec_oti.encoding_symbol_length,
  //            _meta.fec_oti.encoding_id);
          symbol.second.queued = true;
          cnt++;
        }
      }
      if (cnt > 0) break;
      for (auto& symbol : block.second.repair_symbols) {
        if (cnt >= nof_symbols) break;
    
        if (!symbol.second.complete && !symbol.second.queued) {
   //       symbols.emplace_back(symbol.first, block.first, 
   //           (char*)&(block.second.raptor_enc_symbols->symbol[symbol.first].s[0]), 
   //           block.second.raptor_enc_symbols->symbol[symbol.first].d_len,
   //           _meta.fec_oti.encoding_id);
          symbol.second.queued = true;
          cnt++;
        }
      }
    }
  }
#endif
  return symbols;
}

auto LibFlute::File_FEC_Raptor10::dump_status() -> void
{
  for (auto& [sbn, block] : _source_blocks) {
    spdlog::info("SBN {}: {}", sbn, block.complete);
    if (!block.complete) {
      std::vector<unsigned> symbols;
      for (auto i = 0U; i < block.nr_of_symbols; i++) {
        if (!block.completed_symbols[i]) {
          symbols.push_back(i);
        }
      }
      spdlog::info("Missing symbols: {}", fmt::join(symbols, ", "));
    }
  }
}

auto LibFlute::File_FEC_Raptor10::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
{
#if 0
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
#endif
}

auto LibFlute::File_FEC_Raptor10::reset() -> void
{
#if 0
  for (auto& block : _source_blocks) {
    for (auto& symbol : block.second.symbols) {
      symbol.second.complete = false;
    }
    block.second.complete = false;
  } 
  _complete = false;
#endif
}
