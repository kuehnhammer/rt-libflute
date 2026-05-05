// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License").  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.
//

#include "fec/RaptorFEC.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

#include <tinyxml2.h>

#include "spdlog/spdlog.h"
#include "base64.h"

LibFlute::RaptorFEC::RaptorFEC(unsigned int transfer_length, unsigned int max_payload)
    : F(transfer_length)
    , P(max_payload)
{
  double g = fmin( fmin(ceil((double)P*1024/(double)F), (double)P/(double)Al), 10.0f);
  spdlog::debug("double g = fmin( fmin(ceil((double)P*1024/F), (double)P/(double)Al), 10.0f");
  spdlog::debug("G = {} = min( ceil({}*1024/{}), {}/{}, 10.0f)", g, P, F, P, Al);
  G = (unsigned int) g;

  T = (unsigned int) floor((double)P/(double)(Al*g)) * Al;
  spdlog::debug("T = (unsigned int) floor((double)P/(double)(Al*g)) * Al");
  spdlog::debug("T = {} = floor({}/({}*{})) * {}", T, P, Al, g, Al);

  if (T % Al) {
    spdlog::error(" Symbol size T should be a multiple of symbol alignment parameter Al");
    throw std::runtime_error("Symbol size doesn't align");
  }

  Kt = ceil((double)F/(double)T);
  spdlog::debug("double Kt = ceil((double)F/(double)T)");
  spdlog::debug("Kt = {} = ceil({}/{})", Kt, F, T);

  if (Kt < 4) {
    spdlog::error("Input file is too small, it must be a minimum of 4 Symbols");
    throw std::runtime_error("Input is less than 4 symbols");
  }

  Z = (unsigned int) ceil((double)Kt/(double)8192);
  spdlog::debug("Z = (unsigned int) ceil(Kt/8192)");
  spdlog::debug("Z = {} = ceil({}/8192)", Z, Kt);

  // RFC 5053 §4.4.1.2: split Kt source symbols across Z source blocks
  // as evenly as possible. Block i in [0, ZL) holds KL symbols;
  // i in [ZL, Z) holds KS = KL - 1 (or KL if Kt is an exact multiple
  // of Z). The earlier "K = min(Kt, 8192) + remainder-in-last-block"
  // partitioning could leave the trailing block with K < kJKMinK = 4
  // (e.g. Kt = 8195 → 3 symbols in the trailing block; bitstem-r10's
  // Encoder::Create rejected it).
  KS = Kt / Z;
  KL = (Kt % Z == 0) ? KS : (KS + 1);
  ZL = Kt - Z * KS;
  ZS = Z - ZL;
  K  = KL;
  spdlog::debug("§4.4.1.2 partitioning: Z={} ZL={} ZS={} KL={} KS={}",
                Z, ZL, ZS, KL, KS);

  N = fmin( ceil( ceil((double)Kt/(double)Z) * (double)T/(double)W ), (double)T/(double)Al );
  spdlog::debug("N = fmin( ceil( ceil(Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al )");
  spdlog::debug("N = {} = min( ceil( ceil({}/{}) * {}/{} ) , {}/{} )", N, Kt, Z, T, W, T, Al);

  nof_source_symbols        = (unsigned int) Kt;
  nof_source_blocks         = Z;
  small_source_block_length = KS * T;
  large_source_block_length = KL * T;
  nof_large_source_blocks   = ZL;
}

LibFlute::RaptorFEC::~RaptorFEC() = default;

bool LibFlute::RaptorFEC::calculate_partitioning() {
  return true;
}

void *LibFlute::RaptorFEC::allocate_file_buffer(int min_length) {
  assert(min_length <= (int)(Z * target_K(0) * T));
  return malloc(Z * target_K(0) * T);
}

LibFlute::RaptorFEC::DecoderCtx&
LibFlute::RaptorFEC::ensure_dec_ctx(std::uint16_t sbn) {
  auto it = _dec_ctxs.find(sbn);
  if (it != _dec_ctxs.end()) {
    return it->second;
  }

  // Construct a fresh decoder for this source block. Per RFC 5053
  // §4.4.1.2, blocks 0..ZL-1 have KL source symbols and blocks
  // ZL..Z-1 have KS = KL or KL-1. Only the very last block of the
  // entire object may have a partial last source symbol when F isn't
  // a clean multiple of T.
  const unsigned int nsymbs = block_K(sbn);
  const unsigned long byte_off = block_byte_offset(sbn);
  unsigned long blocksize = static_cast<unsigned long>(nsymbs) * T;
  if (byte_off + blocksize > F) {
    blocksize = F - byte_off;
  }

  spdlog::debug("Constructing r10 decoder for SBN {}: K={} blocksize={}",
                sbn, nsymbs, blocksize);

  auto dec = bitstem::r10::fast::Decoder::Create(
      static_cast<std::uint16_t>(nsymbs), T);
  if (!dec.has_value()) {
    spdlog::error("r10::fast::Decoder::Create failed for SBN {} K={}",
                  sbn, nsymbs);
    throw std::runtime_error("r10 decoder construction failed");
  }

  DecoderCtx ctx;
  ctx.dec = std::move(dec);
  ctx.K = static_cast<std::uint16_t>(nsymbs);
  ctx.block_size = blocksize;
  auto [iter, _inserted] = _dec_ctxs.emplace(sbn, std::move(ctx));
  return iter->second;
}

bool LibFlute::RaptorFEC::process_symbol(LibFlute::SourceBlock& srcblk,
                                          LibFlute::Symbol& symbol,
                                          unsigned int id) {
  assert(symbol.length == T);
  DecoderCtx& ctx = ensure_dec_ctx(static_cast<std::uint16_t>(srcblk.id));
  if (ctx.decoded) {
    spdlog::warn("Skipped processing of symbol for finished block: SBN {}, ESI {}",
                 srcblk.id, id);
    return true;
  }
  ctx.dec->AddReceivedSymbol(
      id,
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(symbol.data), symbol.length));
  return true;
}

bool LibFlute::RaptorFEC::check_source_block_completion(LibFlute::SourceBlock& srcblk) {
  if (is_encoder) {
    const bool complete = std::all_of(
        srcblk.symbols.begin(), srcblk.symbols.end(),
        [](const auto& s) { return s.complete; });
    if (complete) {
      // RaptorFEC::create_block allocates each symbol's data buffer
      // via `new char[T]`; release them once the block is fully
      // transmitted.
      for (auto& s : srcblk.symbols) {
        delete[] s.data;
        s.data = nullptr;
      }
    }
    return complete;
  }

  // Decoder side. Per-symbol completion-check is now CHEAP — we just
  // report whether this block has already been decoded. The actual
  // TryDecode call is deferred to try_decode_pending(), which the
  // FLUTE layer triggers once the file's transmission has ended
  // (e.g. its TOI is no longer listed in the FDT). Calling TryDecode
  // per received symbol — as the previous code did — paid the
  // schedule-construction cost ~K × 1.15 times per source block,
  // which dominated the round-trip wall time at K = 8192.
  auto it = _dec_ctxs.find(static_cast<std::uint16_t>(srcblk.id));
  if (it == _dec_ctxs.end()) return false;
  return it->second.decoded;
}

bool LibFlute::RaptorFEC::try_decode_pending(
    std::vector<LibFlute::SourceBlock>& blocks) {
  if (is_encoder) return false;

  bool any_decoded = false;
  for (auto& srcblk : blocks) {
    auto it = _dec_ctxs.find(static_cast<std::uint16_t>(srcblk.id));
    if (it == _dec_ctxs.end()) continue;        // no symbols received
    DecoderCtx& ctx = it->second;
    if (ctx.decoded) continue;                   // already done
    if (ctx.dec->TryDecode()) {
      ctx.decoded   = true;
      srcblk.complete = true;
      any_decoded   = true;
      spdlog::debug("Raptor: decoded source block {} on end-of-transmission trigger",
                    srcblk.id);
    } else {
      spdlog::debug("Raptor: decode failed for source block {} (insufficient symbols)",
                    srcblk.id);
    }
  }
  return any_decoded;
}

void LibFlute::RaptorFEC::extract_finished_block(LibFlute::SourceBlock& srcblk,
                                                  DecoderCtx& ctx) {
  if (!ctx.decoded) {
    spdlog::warn("extract_finished_block called on non-decoded SBN {}", srcblk.id);
    return;
  }
  // The new r10 API hands us the recovered K source bytes as one
  // contiguous span. The OLD glue iterated `dc->pp[esi]` and copied
  // each intermediate symbol back into a per-symbol buffer that
  // happened to alias the file buffer; we just memcpy the source
  // span directly into the file buffer at this block's offset.
  //
  // The per-symbol data pointers in srcblk.symbols still point into
  // the file buffer at the right offsets (set up by create_blocks on
  // the receive path), so an alternative would be a per-symbol
  // memcpy. But a single block-level memcpy is simpler and equivalent
  // in result.
  if (srcblk.symbols.empty()) {
    return;
  }
  // First symbol's data pointer is the start of the block in the
  // file buffer (create_blocks lays out symbols at
  // block_byte_offset(sbn) + i*T).
  std::byte* dst = reinterpret_cast<std::byte*>(srcblk.symbols.front().data);
  const auto src = ctx.dec->SourceBlock();
  std::memcpy(dst, src.data(), ctx.block_size);
  spdlog::debug("Raptor Decoder: extracted decoded source block {} ({} bytes)",
                srcblk.id, ctx.block_size);
}

bool LibFlute::RaptorFEC::extract_file(std::vector<SourceBlock>& blocks) {
  for (auto& srcblk : blocks) {
    auto it = _dec_ctxs.find(static_cast<std::uint16_t>(srcblk.id));
    if (it == _dec_ctxs.end()) continue;
    extract_finished_block(srcblk, it->second);
  }
  return true;
}

unsigned int LibFlute::RaptorFEC::target_K(int blockno) {
  // Per-block source-symbol count from §4.4.1.2 partitioning, plus
  // at least one repair symbol so the receiver always has a margin
  // to recover from packet loss.
  const unsigned int k = block_K(static_cast<unsigned int>(blockno));
  const unsigned int target = static_cast<unsigned int>(k * surplus_packet_ratio);
  return (target > k) ? target : k + 1;
}

LibFlute::SourceBlock LibFlute::RaptorFEC::create_block(char *buffer,
                                                         int *bytes_read,
                                                         int blockid) {
  struct SourceBlock source_block;
  source_block.id = blockid;

  // Per-block parameters from §4.4.1.2. nsymbs is exact; blocksize
  // is nsymbs*T except for the very last block when F isn't a clean
  // multiple of T (the last source symbol then has < T bytes of real
  // data and is zero-padded below). The caller (create_blocks) has
  // already advanced `buffer` to block_byte_offset(blockid).
  const unsigned int  nsymbs    = block_K(static_cast<unsigned int>(blockid));
  const unsigned long byte_off  = block_byte_offset(static_cast<unsigned int>(blockid));
  unsigned long       blocksize = static_cast<unsigned long>(nsymbs) * T;
  if (byte_off + blocksize > F) {
    blocksize = F - byte_off;
  }
  // bitstem-r10 requires the encoder's source-symbol span to be
  // exactly nsymbs * T bytes (RFC 5053 §5.4 source-block layout). For
  // every block except possibly the last the file buffer is already
  // a multiple of T, so no copy is needed. For the trailing block
  // when F is not a multiple of T, pad with zeros into a temporary
  // buffer; the receiver also operates on a K-padded buffer and
  // truncates to F when the file is handed back.
  const unsigned int padded_size = nsymbs * T;
  std::vector<std::byte> padded;
  std::span<const std::byte> source_span;
  if (blocksize == padded_size) {
    source_span = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buffer), blocksize);
  } else {
    padded.assign(padded_size, std::byte{0});
    std::memcpy(padded.data(), buffer, blocksize);
    source_span = std::span<const std::byte>(padded.data(), padded_size);
  }

  spdlog::debug("Constructing r10 encoder for SBN {}: K={} blocksize={} (padded={})",
                blockid, nsymbs, blocksize, padded_size);

  auto enc = bitstem::r10::fast::Encoder::Create(
      static_cast<std::uint16_t>(nsymbs), source_span, T);
  if (!enc.has_value()) {
    spdlog::error("r10::fast::Encoder::Create failed for SBN {} K={}",
                  blockid, nsymbs);
    throw std::runtime_error("Error creating r10 encoder");
  }

  const unsigned int symbols_to_emit = target_K(blockid);
  source_block.symbols.reserve(symbols_to_emit);
  for (unsigned int esi = 0; esi < symbols_to_emit; ++esi) {
    LibFlute::Symbol sym{};
    sym.data   = new char[T];
    sym.length = T;
    enc->EncodeSymbol(esi,
                      std::span<std::byte>(
                          reinterpret_cast<std::byte*>(sym.data), T));
    source_block.symbols.push_back(sym);
  }
  *bytes_read += blocksize;
  return source_block;
}

std::vector<LibFlute::SourceBlock>
LibFlute::RaptorFEC::create_blocks(char *buffer, int *bytes_read) {
  if (!bytes_read) {
    throw std::invalid_argument("bytes_read pointer shouldn't be null");
  }
  if (N != 1) {
    throw std::invalid_argument(
        "Currently the encoding only supports 1 sub-block per block");
  }

  std::vector<LibFlute::SourceBlock> block_vec;
  block_vec.reserve(Z);
  *bytes_read = 0;

  for (unsigned int sbn = 0; sbn < Z; ++sbn) {
    if (!is_encoder) {
      // Receiver lays out one Symbol per slot at the §4.4.1.2 block
      // offset; process_symbol fills these in as packets arrive and
      // extract_finished_block writes the recovered source block
      // back over them.
      LibFlute::SourceBlock block;
      block.id = sbn;
      const unsigned long blk_off        = block_byte_offset(sbn);
      const unsigned int  symbols_to_read = target_K(sbn);
      block.symbols.reserve(symbols_to_read);
      for (unsigned int i = 0; i < symbols_to_read; ++i) {
        LibFlute::Symbol sym{};
        sym.data     = buffer + blk_off + static_cast<unsigned long>(i) * T;
        sym.length   = T;
        block.symbols.push_back(sym);
      }
      block_vec.push_back(std::move(block));
    } else {
      block_vec.push_back(create_block(buffer + block_byte_offset(sbn),
                                         bytes_read, sbn));
    }
  }
  return block_vec;
}

bool LibFlute::RaptorFEC::parse_fdt_info(tinyxml2::XMLElement *file) {
  is_encoder = false;

  const char* val = 0;
  uint32_t content_length = 0;
  val = file->Attribute("Content-Length");
  if (val != nullptr) {
    content_length = strtoull(val, nullptr, 0);
  }

  val = file->Attribute("Transfer-Length");
  if (val != nullptr) {
    F = strtoull(val, nullptr, 0);
  } else {
    F = content_length;
  }

  val = file->Attribute("FEC-OTI-Encoding-Symbol-Length");
  if (val != nullptr) {
    T = strtoul(val, nullptr, 0);
  } else {
    throw std::runtime_error(
        "Required field \"FEC-OTI-Encoding-Symbol-Length\" missing for object in FDT");
  }

  std::string scheme_specific_info;
  val = file->Attribute("FEC-OTI-Scheme-Specific-Info");
  if (val != nullptr) {
    scheme_specific_info = base64_decode((const std::string&)val);
  }
  if (scheme_specific_info.length() != 4) {
    throw std::runtime_error("Missing or malformed scheme specific info for Raptor FEC");
  }

  Z  = (uint8_t)scheme_specific_info[0] << 8;
  Z |= (uint8_t)scheme_specific_info[1];
  N  = (uint8_t)scheme_specific_info[2];
  Al = (uint8_t)scheme_specific_info[3];

  if (T % Al) {
    throw std::runtime_error(
        "Symbol size T is not a multiple of Al. Invalid configuration from sender");
  }

  Kt                 = ceil((double)F / (double)T);
  nof_source_symbols = (unsigned int) Kt;

  // RFC 5053 §4.4.1.2: same KL/KS/ZL/ZS partitioning as the sender;
  // both sides MUST agree (Z is on the wire via Scheme-Specific-Info,
  // so KL/KS/ZL/ZS follow deterministically from Kt/Z).
  KS = Kt / Z;
  KL = (Kt % Z == 0) ? KS : (KS + 1);
  ZL = Kt - Z * KS;
  ZS = Z - ZL;
  K  = KL;

  nof_source_blocks         = Z;
  small_source_block_length = KS * T;
  large_source_block_length = KL * T;
  nof_large_source_blocks   = ZL;

  return true;
}

bool LibFlute::RaptorFEC::add_fdt_info(tinyxml2::XMLElement *file) {
  file->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned) FecScheme::Raptor);
  file->SetAttribute("FEC-OTI-Encoding-Symbol-Length", T);
  file->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", K);

  // RFC 6726 §3.4.2 + RFC 5053 §3.2: the per-FEC-scheme parameters
  // ride in a single FEC-OTI-Scheme-Specific-Info attribute as
  // base64. Layout for Raptor: Z(2 bytes BE) + N(1) + Al(1) = 4 bytes.
  std::array<unsigned char, 4> ssi{};
  ssi[0] = static_cast<unsigned char>((Z >> 8) & 0xFFU);
  ssi[1] = static_cast<unsigned char>(Z & 0xFFU);
  ssi[2] = static_cast<unsigned char>(N);
  ssi[3] = static_cast<unsigned char>(Al);
  std::string ssi_b64 = base64_encode({ssi.begin(), ssi.end()}, ssi.size());
  file->SetAttribute("FEC-OTI-Scheme-Specific-Info", ssi_b64.c_str());

  is_encoder = true;
  return true;
}
