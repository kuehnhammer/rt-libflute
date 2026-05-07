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
#include <openssl/evp.h>         // for EVP_DigestFinal_ex, EVP_DigestInit_ex
#include <openssl/md5.h>         // for MD5_DIGEST_LENGTH
#include <openssl/types.h>       // for EVP_MD, EVP_MD_CTX
#include <cstdio>               // for sprintf
#include <cstdlib>              // for malloc, free
#include <ctime>                // for time
#include <algorithm>             // for all_of, min, max
#include <cassert>               // for assert
#include <cmath>                 // for ceil, floor
#include <cstdint>               // for uint16_t
#include <cstring>               // for memcmp, memcpy
#include <memory>                // for shared_ptr, __shared_ptr_access, dyn...
#include <stdexcept>             // for runtime_error
#include <string>                // for string, basic_string
#include <utility>               // for pair, move
#include "EncodingSymbol.h"      // for EncodingSymbol
#include "base64.h"              // for base64_decode, base64_encode
#include "fec/FecTransformer.h"  // for FecTransformer
#include "spdlog/spdlog.h"       // for debug, error, warn

#ifdef RAPTOR_ENABLED
#include "fec/RaptorFEC.h"
#endif

LibFlute::File::File(LibFlute::FileDeliveryTable::FileEntry entry)
  : _meta( std::move(entry) )
  , _received_at( time(nullptr) )
{
  spdlog::debug("Creating File from FileEntry");
  // Allocate a data buffer
  spdlog::debug("Allocating buffer");
  if (_meta.fec_transformer){
    _buffer = (char*) _meta.fec_transformer->allocate_file_buffer(_meta.fec_oti.transfer_length);
  } else {
    _buffer = (char*) malloc(_meta.fec_oti.transfer_length);
  }
  if (_buffer == nullptr)
  {
    throw std::runtime_error("Failed to allocate file buffer");
  }
  _own_buffer = true;

  calculate_partitioning();
  create_blocks();
}

LibFlute::File::File(uint32_t toi,
    const FecOti& fec_oti,
    std::string content_location,
    std::string content_type,
    uint64_t expires,
    char* data,
    size_t length,
    bool copy_data,
    std::optional<unsigned> fec_redundancy_level,
    unsigned fec_worker_threads)
{
  if (data == nullptr) {
    spdlog::error("File pointer is null");
    throw std::runtime_error("Invalid file");
  }

  spdlog::debug("Creating File from data");

  if (copy_data) {
    spdlog::debug("Allocating buffer");
    _buffer = (char*)malloc(length);
    if (_buffer == nullptr)
    {
      throw std::runtime_error("Failed to allocate file buffer");
    }
    memcpy(_buffer, data, length);
    _own_buffer = true;
  } else {
    _buffer = data;
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> md5;
  if ( calculate_md5(data, length, md5.data()) < 0 ){
    throw std::runtime_error("Failed to calculate md5");
  }

  _meta.toi = toi;
  _meta.content_location = std::move(content_location);
  _meta.content_type = std::move(content_type);
  _meta.content_length = length;
  _meta.content_md5 = base64_encode({std::begin(md5), std::end(md5)}, MD5_DIGEST_LENGTH);
  _meta.expires = expires;
  _meta.fec_oti = fec_oti;

  switch (_meta.fec_oti.encoding_id) {
    case FecScheme::CompactNoCode:
      _meta.fec_transformer = nullptr;
      _meta.fec_oti.transfer_length = length;
      break;
#ifdef RAPTOR_ENABLED
    case FecScheme::Raptor:
    case FecScheme::RaptorQ: {
      // Hand the FecOti through directly so RaptorFEC can pick up the
      // caller-supplied scheme + scheme-specific info (xMB / SDP path).
      // We populate transfer_length on the FecOti before construction
      // so RaptorFEC sees the final F.
      FecOti raptor_oti = fec_oti;
      raptor_oti.transfer_length = length;
      auto raptor = std::make_shared<RaptorFEC>(raptor_oti,
                                                  fec_redundancy_level,
                                                  fec_worker_threads);
      _meta.fec_transformer = raptor;
      _meta.fec_oti.transfer_length        = length;
      _meta.fec_oti.encoding_symbol_length = raptor->T;
      // RFC 5052 §3.4.2: max_source_block_length is in SYMBOLS, not
      // bytes. Earlier code multiplied by T; that broke source/repair
      // classification (symbols with ESI in [0, K) are source, ESI ≥ K
      // is repair) on both sides since both sender and receiver were
      // looking at K*T instead of K.
      _meta.fec_oti.max_source_block_length = raptor->K;
      break;
    }
#endif
    default:
      throw std::runtime_error("FEC scheme not supported or not yet implemented");
  }

  calculate_partitioning();
  create_blocks();
}

LibFlute::File::~File()
{
  spdlog::debug("Destroying File");
  if (_own_buffer && _buffer != nullptr)
  {
    spdlog::debug("Freeing buffer");
    free(_buffer);
  }
}

auto LibFlute::File::put_symbol( const LibFlute::EncodingSymbol& symbol ) -> void
{
  if(_complete) {
    spdlog::debug("Not handling symbol {} , SBN {} since file is already complete",symbol.id(),symbol.source_block_number());
    return;
  }
  if (symbol.source_block_number() >= _source_blocks.size()) {
    throw std::runtime_error("Source Block number too high");
  }

  SourceBlock& source_block = _source_blocks[symbol.source_block_number()];

  if (source_block.complete) {
    // Bonus / repair symbols arriving after the source block was
    // already decoded — normal Raptor case, just unused redundancy.
    // Trace-level only (the encoder routinely emits more symbols
    // than needed for repair tolerance).
    spdlog::trace("Ignoring symbol {} since block {} is already complete",
                  symbol.id(), symbol.source_block_number());
    return;
  }

  if (symbol.id() >= source_block.symbols.size()) {
    throw std::runtime_error("Encoding Symbol ID too high");
  }

  LibFlute::Symbol& target_symbol = source_block.symbols[symbol.id()];

  if (!target_symbol.complete) {
    symbol.decode_to(target_symbol.data, target_symbol.length);
    target_symbol.complete = true;
    ++source_block.completed_symbol_count;
    if (_meta.fec_transformer) {
      _meta.fec_transformer->process_symbol(source_block,target_symbol,symbol.id());
    }
    check_source_block_completion(source_block);
    check_file_completion();
  }

}

auto LibFlute::File::check_source_block_completion( SourceBlock& block ) -> void
{
  const bool was_complete = block.complete;
  if (_meta.fec_transformer) {
    block.complete = _meta.fec_transformer->check_source_block_completion(block);
  } else {
    // CompactNoCode: block is complete iff every Symbol's `complete`
    // bit is set. completed_symbol_count is maintained as those bits
    // transition false→true, so a counter compare is exact and
    // avoids an O(K) std::all_of after every packet.
    block.complete =
        (block.completed_symbol_count == block.symbols.size());
  }
  if (!was_complete && block.complete) {
    ++_complete_block_count;
  }
}

auto LibFlute::File::check_file_completion() -> void
{
  // O(1): _complete_block_count is bumped exactly once per block as
  // its complete bit transitions in check_source_block_completion.
  // The previous std::all_of was hot on the encoder path (called
  // per dispatched packet) and quadratic in Z for CompactNoCode
  // files with thousands of blocks.
  _complete = (_complete_block_count == _source_blocks.size());

  if (_complete && !_meta.content_md5.empty()) {
      if(_meta.fec_transformer){
          _meta.fec_transformer->extract_file(_source_blocks);
      }
  }
}

auto LibFlute::File::calculate_partitioning() -> void
{
  if (_meta.fec_transformer && _meta.fec_transformer->calculate_partitioning()){
    _nof_source_symbols = _meta.fec_transformer->nof_source_symbols;
    _nof_source_blocks = _meta.fec_transformer->nof_source_blocks;
    _large_source_block_length = _meta.fec_transformer->large_source_block_length;
    _small_source_block_length = _meta.fec_transformer->small_source_block_length;
    _nof_large_source_blocks = _meta.fec_transformer->nof_large_source_blocks;
    return;
  }
  // Calculate source block partitioning (RFC5052 9.1) 
  _nof_source_symbols = ceil((double)_meta.fec_oti.transfer_length / (double)_meta.fec_oti.encoding_symbol_length);
  _nof_source_blocks = ceil((double)_nof_source_symbols / (double)_meta.fec_oti.max_source_block_length);
  _large_source_block_length = ceil((double)_nof_source_symbols / (double)_nof_source_blocks);
  _small_source_block_length = floor((double)_nof_source_symbols / (double)_nof_source_blocks);
  _nof_large_source_blocks = _nof_source_symbols - _small_source_block_length * _nof_source_blocks;
}

auto LibFlute::File::create_blocks() -> void
{
  // Create the required source blocks and encoding symbols

  if (_meta.fec_transformer){
    int bytes_read = 0;
    _source_blocks = _meta.fec_transformer->create_blocks(_buffer, &bytes_read);
    if (_source_blocks.empty()) {
      spdlog::error("FEC Transformer failed to create source blocks");
      throw std::runtime_error("FEC Transformer failed to create source blocks");
    }
    return;
  }

  // CompactNoCode: SBN runs 0..nof_source_blocks-1, dense, so reserve
  // up front and emplace_back. Each SourceBlock carries a vector of
  // symbols whose data pointers reference into the user buffer (no
  // copy).
  _source_blocks.reserve(_nof_source_blocks);
  auto* buffer_ptr = _buffer;
  size_t remaining_size = _meta.fec_oti.transfer_length;
  for (uint32_t number = 0;
       number < _nof_source_blocks && remaining_size > 0;
       ++number) {
    LibFlute::SourceBlock block;
    block.id = number;
    const uint32_t block_length = (number < _nof_large_source_blocks)
                                    ? _large_source_block_length
                                    : _small_source_block_length;
    block.symbols.reserve(block_length);

    for (uint32_t i = 0; i < block_length; ++i) {
      const size_t symbol_length =
          std::min(remaining_size, (size_t)_meta.fec_oti.encoding_symbol_length);
      assert(buffer_ptr + symbol_length <= _buffer + _meta.fec_oti.transfer_length);

      block.symbols.push_back(LibFlute::Symbol{
          .data     = buffer_ptr,
          .length   = symbol_length,
          .complete = false,
      });

      remaining_size -= symbol_length;
      buffer_ptr     += symbol_length;
      if (remaining_size <= 0) break;
    }
    _source_blocks.push_back(std::move(block));
  }
}

auto LibFlute::File::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol>
{
  const int nof_symbols = std::floor(
      (float)max_size / (float)_meta.fec_oti.encoding_symbol_length);
  int cnt = 0;
  std::vector<EncodingSymbol> symbols;
  spdlog::debug("Attempting to queue {} symbols", nof_symbols);

  // Vector indexed by SBN. _emit_cursor_sbn is the next block to try;
  // SourceBlock::emit_cursor is the next ESI within that block. Both
  // advance forward only — net cost is O(symbols emitted), not O(Z²)
  // or O(K²) per block.
  while (_emit_cursor_sbn < _source_blocks.size() && cnt < nof_symbols) {
    auto& blk = _source_blocks[_emit_cursor_sbn];
    if (blk.complete) {
      ++_emit_cursor_sbn;
      continue;
    }
    // Lazy materialisation hook for FEC schemes that defer block fill
    // (Raptor) — Symbol[0].data == nullptr is the "not yet
    // materialised" placeholder convention. CompactNoCode + the
    // Raptor receiver path always populate Symbol::data in
    // create_blocks() and so skip this check (default no-op
    // prepare_for_emit). Only triggers once per block per pass.
    if (_meta.fec_transformer && !blk.symbols.empty() &&
        blk.symbols[0].data == nullptr) {
      _meta.fec_transformer->prepare_for_emit(blk);
    }
    while (blk.emit_cursor < blk.symbols.size() && cnt < nof_symbols) {
      auto& sym = blk.symbols[blk.emit_cursor];
      if (!sym.complete && !sym.queued) {
        symbols.emplace_back(blk.emit_cursor, blk.id, sym.data, sym.length,
                              _meta.fec_oti.encoding_id);
        sym.queued = true;
        ++cnt;
      }
      ++blk.emit_cursor;
    }
    if (blk.emit_cursor >= blk.symbols.size()) {
      ++_emit_cursor_sbn;
    } else {
      break;  // hit nof_symbols cap mid-block; resume here next call
    }
  }
  return symbols;
}

auto LibFlute::File::try_decode_pending() -> void
{
  if (!_meta.fec_transformer) {
    return;  // CompactNoCode: nothing to do, completion already tracked.
  }
  if (_meta.fec_transformer->try_decode_pending(_source_blocks)) {
    // At least one source block decoded successfully. The FEC layer
    // sets SourceBlock.complete directly when decoding finishes, so
    // the per-block transition counter that check_source_block_-
    // completion maintains was not bumped — re-derive it from the
    // current block.complete bits before running the file-level
    // check.
    _complete_block_count = static_cast<uint32_t>(std::count_if(
        _source_blocks.begin(), _source_blocks.end(),
        [](const auto& blk) { return blk.complete; }));
    check_file_completion();
    if (_complete && _meta.fec_transformer) {
      _meta.fec_transformer->extract_file(_source_blocks);
    }
  }
}

auto LibFlute::File::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
{
  for (const auto& symbol : symbols) {
    if (symbol.source_block_number() >= _source_blocks.size()) continue;
    auto& block = _source_blocks[symbol.source_block_number()];
    if (symbol.id() < block.symbols.size()) {
      auto& sym = block.symbols[symbol.id()];
      sym.queued = false;
      if (success && !sym.complete) {
        ++block.completed_symbol_count;
      }
      sym.complete = success;
    }
    check_source_block_completion(block);
    check_file_completion();
  }
  if (!success && !symbols.empty()) {
    // Dispatch failed: the symbols are no longer queued. Rewind both
    // cursors so the next get_next_symbols re-finds them.
    uint32_t min_sbn = symbols.front().source_block_number();
    uint32_t min_esi = symbols.front().id();
    for (const auto& s : symbols) {
      if (s.source_block_number() < min_sbn) {
        min_sbn = s.source_block_number();
        min_esi = s.id();
      } else if (s.source_block_number() == min_sbn && s.id() < min_esi) {
        min_esi = s.id();
      }
    }
    _emit_cursor_sbn = std::min(_emit_cursor_sbn, min_sbn);
    if (min_sbn < _source_blocks.size()) {
      auto& blk = _source_blocks[min_sbn];
      blk.emit_cursor = std::min(blk.emit_cursor, min_esi);
    }
  }
}

auto LibFlute::calculate_md5(char *input, size_t length, unsigned char *result) -> int
{
  // simple implementation based on openssl docs (https://www.openssl.org/docs/man3.0/man3/EVP_DigestInit_ex.html)
  if (input == nullptr || length == 0U) {
    spdlog::error("MD5 called with invalid input");
    return -1;
  }
  spdlog::debug("MD5 calculation called for input length {}", length);

  EVP_MD_CTX*   context = EVP_MD_CTX_new();
  const EVP_MD* md = EVP_md5();
  unsigned int  md_len;

  EVP_DigestInit_ex(context, md, nullptr);
  EVP_DigestUpdate(context, input, length);
  EVP_DigestFinal_ex(context, result, &md_len);
  EVP_MD_CTX_free(context);

  char buf [EVP_MAX_MD_SIZE * 2] = {}; //NOLINT
  for (auto i = 0UL; i < md_len; i++){
    sprintf(&buf[i*2], "%02x", result[i]);
  }
  spdlog::debug("MD5 Digest is {}", buf);

  return static_cast<int>(md_len);
}
