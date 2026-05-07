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
#include <vector>

#include <tinyxml2.h>

#include "spdlog/spdlog.h"
#include "base64.h"

namespace {
// Map libflute's wire-level FecScheme to the bitstem-fec codec
// selection enum. Throws on schemes RaptorFEC doesn't handle (those
// are routed to other transformers / CompactNoCode upstream).
bitstem::fec::Scheme to_bitstem_scheme(LibFlute::FecScheme s) {
  switch (s) {
    case LibFlute::FecScheme::Raptor:  return bitstem::fec::Scheme::kR10;
    case LibFlute::FecScheme::RaptorQ: return bitstem::fec::Scheme::kRaptorQ;
    default:
      throw std::invalid_argument(
          "RaptorFEC: unsupported FecScheme (only Raptor / RaptorQ)");
  }
}
}  // namespace

LibFlute::RaptorFEC::RaptorFEC(const FecOti& fec_oti,
                                std::optional<unsigned> fec_redundancy_level,
                                unsigned fec_worker_threads,
                                std::uint64_t sub_block_size_target)
    : F(fec_oti.transfer_length)
    , P(fec_oti.encoding_symbol_length)
    , _fec_worker_threads(fec_worker_threads)
    , _bitstem_scheme(to_bitstem_scheme(fec_oti.encoding_id))
    , _fec_scheme(fec_oti.encoding_id)
{
  if (fec_redundancy_level.has_value()) {
    surplus_packet_ratio = 1.0f + static_cast<float>(*fec_redundancy_level) / 100.0f;
  }
  if (sub_block_size_target != 0) {
    W = static_cast<unsigned long>(sub_block_size_target);
  }

  // Caller-supplied scheme-specific info path (xMB → SDP → libflute).
  // Per TS 26.346 §7.2.10.1 + RFC 5053 §3.2 / RFC 6330 §3.4.2, SSI is
  // 4 bytes carrying Z + N + Al; the byte allocation flips between
  // schemes (R10: Z(2)+N(1)+Al(1); RaptorQ: Z(1)+N(2)+Al(1)). When
  // present, those values override the autodetect path in
  // calculate_partitioning() — the encoder honors what the caller
  // (and therefore the receiver, via the FDT round-trip) was told.
  if (fec_oti.scheme_specific_info.size() == 4) {
    const auto& ssi = fec_oti.scheme_specific_info;
    const auto b = [&](std::size_t i) {
      return static_cast<std::uint32_t>(static_cast<std::uint8_t>(ssi[i]));
    };
    if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
      Z  = b(0);
      N  = (b(1) << 8) | b(2);
      Al = b(3);
    } else {
      Z  = (b(0) << 8) | b(1);
      N  = b(2);
      Al = b(3);
    }
    _ssi_caller_supplied = true;
  }

  // Source-block / sub-block partitioning. T (symbol size) and Kt
  // (total source-symbol count) are always derived from F + P; Z (and
  // therefore KL/KS/ZL/ZS), N, and Al come either from caller-supplied
  // SSI (the xMB / SDP path, _ssi_caller_supplied above) or from this
  // autodetect path.
  //
  // K_max scales with the chosen scheme — RFC 5053 caps R10 at 8192
  // source symbols per block; RFC 6330 §5.1.2 lets RaptorQ go up to
  // 56403. Larger K_max means fewer blocks for the same Kt, which
  // matters for large-file throughput on the RaptorQ path.
  const unsigned int K_max =
      (_fec_scheme == LibFlute::FecScheme::RaptorQ) ? 56403u : 8192u;

  double g = fmin( fmin(ceil((double)P*1024/(double)F), (double)P/(double)Al), 10.0f);
  spdlog::debug("G = {} = min( ceil({}*1024/{}), {}/{}, 10.0f)", g, P, F, P, Al);
  G = (unsigned int) g;

  T = (unsigned int) floor((double)P/(double)(Al*g)) * Al;
  spdlog::debug("T = {} = floor({}/({}*{})) * {}", T, P, Al, g, Al);
  if (T % Al) {
    spdlog::error("Symbol size T should be a multiple of symbol alignment parameter Al");
    throw std::runtime_error("Symbol size doesn't align");
  }

  Kt = ceil((double)F/(double)T);
  spdlog::debug("Kt = {} = ceil({}/{})", Kt, F, T);
  if (Kt < 4 && _fec_scheme == LibFlute::FecScheme::Raptor) {
    spdlog::error("R10 input is too small, it must be a minimum of 4 symbols");
    throw std::runtime_error("Input is less than 4 symbols");
  }

  // Z: caller-supplied wins; otherwise dispatch on scheme.
  //
  // RFC 5053 §4.2 (R10): only K_max bounds Z; W shapes N below.
  // RFC 6330 §4.3 (RaptorQ): WS additionally bounds K_per_block * T
  //   so the codec's working memory per source block fits in WS.
  //   K_max provides a hard ceiling on Z.
  if (!_ssi_caller_supplied) {
    const std::uint64_t z_by_kmax =
        (static_cast<std::uint64_t>(Kt) + K_max - 1ULL) / K_max;
    if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
      const std::uint64_t z_by_ws = (W > 0)
          ? (static_cast<std::uint64_t>(Kt) * T + W - 1ULL) / W
          : 1ULL;
      Z = static_cast<unsigned int>(std::max(z_by_kmax, z_by_ws));
      spdlog::debug("Z = {} = max(ceil(Kt/K'_max)={}, ceil(Kt*T/WS)={}) "
                    "(RFC 6330 §4.3, K'_max={}, WS={})",
                    Z, z_by_kmax, z_by_ws, K_max, W);
    } else {
      Z = static_cast<unsigned int>(z_by_kmax);
      spdlog::debug("Z = {} = ceil(Kt/K_max) (RFC 5053 §4.2, K_max={})",
                    Z, K_max);
    }
    if (Z < 1u) Z = 1u;
  } else {
    spdlog::debug("Z = {} (caller-supplied via SSI)", Z);
  }

  // Wire-format SBN range cap. R10 (RFC 5053 §3.2) uses a 16-bit SBN
  // → at most 65536 source blocks per object; RaptorQ (RFC 6330 §3.2)
  // uses an 8-bit SBN → 256 max. If the autodetect produced Z over
  // the cap (typically because the caller supplied a too-small WS for
  // a large RaptorQ object) the partition is wire-incompatible and
  // we refuse rather than silently clamping. The fix is operator-side:
  // increase WS or split the file.
  const std::uint64_t z_wire_cap =
      (_fec_scheme == LibFlute::FecScheme::RaptorQ) ? 256ULL : 65536ULL;
  if (Z > z_wire_cap) {
    throw std::runtime_error(std::string(
        "RaptorFEC: partitioning produced Z=") + std::to_string(Z) +
        " source blocks, exceeding the wire-format SBN cap of " +
        std::to_string(z_wire_cap) + " for this scheme. Increase WS "
        "(FileTransmissionConfig::sub_block_size_target) or split the "
        "object.");
  }

  // RFC 5053 §4.4.1.2 / RFC 6330 §4.4: split Kt source symbols across Z
  // source blocks as evenly as possible. Block i in [0, ZL) holds KL
  // symbols; i in [ZL, Z) holds KS = KL - 1 (or KL if Kt is an exact
  // multiple of Z).
  KS = Kt / Z;
  KL = (Kt % Z == 0) ? KS : (KS + 1);
  ZL = Kt - Z * KS;
  ZS = Z - ZL;
  K  = KL;
  spdlog::debug("§4.4.1.2 partitioning: Z={} ZL={} ZS={} KL={} KS={}",
                Z, ZL, ZS, KL, KS);

  // N: caller-supplied wins; otherwise dispatch on scheme.
  //
  // RFC 5053 §4.2 (R10): N is the codec's cache-friendliness knob —
  //   small W spawns more sub-blocks per source block. The formula
  //   N = min(ceil(ceil(Kt/Z)·T/W), T/Al) caps N at the per-Al
  //   ceiling. TS 26.346-conformant R10 file delivery uses W=256 KB
  //   which intentionally produces N>1 for multi-MiB blocks.
  //
  // RFC 6330 §4.3 (RaptorQ): WS is consumed in the Z computation
  //   above (so K_per_block · T ≤ WS by construction). Sub-block
  //   partitioning isn't needed for working-memory reasons and the
  //   spec leaves N=1 in the standard derivation. We follow.
  if (!_ssi_caller_supplied) {
    if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
      N = 1u;
      spdlog::debug("N = 1 (RFC 6330 §4.3; WS already absorbed in Z)");
    } else {
      N = fmin( ceil( ceil((double)Kt / (double)Z) * (double)T / (double)W ),
                (double)T / (double)Al );
      if (N < 1u) N = 1u;
      spdlog::debug("N = {} (RFC 5053 §4.2, W={}, T={}, Al={})",
                    N, W, T, Al);
    }
  } else {
    spdlog::debug("N = {} (caller-supplied via SSI)", N);
  }

  nof_source_symbols        = (unsigned int) Kt;
  nof_source_blocks         = Z;
  small_source_block_length = KS * T;
  large_source_block_length = KL * T;
  nof_large_source_blocks   = ZL;
}

LibFlute::RaptorFEC::RaptorFEC(LibFlute::FecScheme scheme)
    : _bitstem_scheme(to_bitstem_scheme(scheme))
    , _fec_scheme(scheme)
{}

LibFlute::RaptorFEC::~RaptorFEC() {
  // Tear down the parallel-encode worker pool, if active. Idempotent
  // when the pool was never spun up (default-constructed instances on
  // the receive side, or sequential transmit instances).
  stop_pool();
}

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

  // Mirrors the encode-side EncoderParams plumbing: scheme + Al + N
  // come from the parsed FEC OTI's scheme-specific info (set in
  // parse_fdt_info) and flow verbatim into the codec, so the
  // decoder's sub-block layout matches what the sender declared.
  bitstem::fec::DecoderParams params;
  params.K      = static_cast<std::uint16_t>(nsymbs);
  params.T      = static_cast<std::uint16_t>(T);
  params.scheme = _bitstem_scheme;
  params.Al     = static_cast<std::uint8_t>(Al);
  params.N      = static_cast<std::uint16_t>(N);
  auto dec = bitstem::fec::fast::Decoder::Create(params);
  if (!dec.has_value()) {
    spdlog::error("bitstem::fec::Decoder::Create failed for SBN {} K={} scheme={} Al={} N={}",
                  sbn, nsymbs, static_cast<int>(_bitstem_scheme),
                  params.Al, params.N);
    throw std::runtime_error("FEC decoder construction failed");
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
    // Block already decoded (almost always via the K+1 opportunistic
    // path in check_source_block_completion). Subsequent symbols are
    // expected redundancy from the encoder's repair overhead — drop
    // them silently. Used to be spdlog::warn here, which was free
    // pre-R10 (block decoded only at FDT end-of-transmission, never
    // mid-stream) but is hot now: ~0.15·K symbols per block fire it
    // after early decode lands.
    spdlog::trace("Skipped symbol after early decode: SBN {}, ESI {}",
                  srcblk.id, id);
    return true;
  }
  ctx.dec->AddReceivedSymbol(
      id,
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(symbol.data), symbol.length));
  if (id < ctx.K) {
    ++ctx.source_esi_count;
  }
  return true;
}

bool LibFlute::RaptorFEC::check_source_block_completion(LibFlute::SourceBlock& srcblk) {
  if (is_encoder) {
    // O(1) — File maintains completed_symbol_count as Symbol.complete
    // bits transition false→true. Previously this scanned the whole
    // symbols vector via std::all_of, which is called once per packet
    // and was therefore O(K_target²) per block (≈80 ms on a 100 MB
    // K=8000 file).
    const bool complete =
        (srcblk.completed_symbol_count == srcblk.symbols.size());
    if (complete) {
      // _enc_scratch is the FEC's single shared scratch buffer; the
      // next prepare_for_emit() will overwrite it for the next block.
      // Just nullify Symbol::data so the placeholder convention
      // (data == nullptr ↔ block not yet materialised) holds again
      // — File::get_next_symbols never revisits a complete block,
      // but other code paths (FDT round-trip diagnostics, future
      // re-emit logic) read Symbol.data and would see stale pointers
      // otherwise.
      if (_enc_scratch_sbn == static_cast<int>(srcblk.id)) {
        _enc_scratch_sbn = -1;
      }
      for (auto& s : srcblk.symbols) {
        s.data = nullptr;
      }
      // Parallel-encode path: signal the worker that filled this block
      // that its scratch can now be recycled for the next claim. The
      // worker is currently waiting on _pool->released_cv; setting the
      // released flag before notifying establishes the happens-before
      // edge for the worker's subsequent fill of the next SBN.
      if (_pool && srcblk.id < _pool->released.size()) {
        {
          const std::lock_guard<std::mutex> lk(_pool->mtx);
          _pool->released[srcblk.id].store(true, std::memory_order_release);
        }
        _pool->released_cv.notify_all();
      }
    }
    return complete;
  }

  // Decoder side. Per-symbol completion-check is cheap by design.
  // We fire TryDecode exactly once per block, at the precise moment
  // the lossless short-circuit becomes applicable: every source ESI
  // (id < K) for this block has arrived. At that point Decoder::-
  // TryDecode's lossless short-circuit permutes the receive buffer
  // by ESI in ~1 ms — no matrix work, no allocation pressure that
  // competes with concurrent encoder packet emission. The block is
  // then released for the rest of its repair-symbol stream to be
  // ignored at put_symbol's block.complete early-return.
  //
  // The expensive lossy path (≥1 source ESI lost ⇒ matrix factor
  // ~58 ms at K=8000) is intentionally NOT triggered here. Mid-
  // stream matrix factoring is a net regression in the bench
  // (cache+TLB contention with concurrent encoder work; +13 % wall
  // at drop_every=8000) for no compensating throughput benefit —
  // total CPU work is the same as batching at end-of-transmission.
  // The FDT-trigger try_decode_pending() path picks up any
  // undecoded blocks at the natural batch boundary.
  auto it = _dec_ctxs.find(static_cast<std::uint16_t>(srcblk.id));
  if (it == _dec_ctxs.end()) return false;
  DecoderCtx& ctx = it->second;
  if (ctx.decoded) return true;

  if (!ctx.attempted_lossless_kp1 &&
      ctx.source_esi_count == ctx.K) {
    ctx.attempted_lossless_kp1 = true;
    // Skip the bitstem-r10 lossless short-circuit entirely. Every
    // source ESI for this block has arrived, and File::put_symbol
    // wrote each one's bytes to file_buffer[block_offset + esi*T]
    // via Symbol::data — so the file buffer ALREADY holds the
    // correct K source symbols for this block. Calling TryDecode
    // would only round-trip the bytes through the Decoder's
    // _source_block (alloc K*T per block + permute by ESI + later
    // extract_finished_block memcpy back to the file buffer) for
    // a net no-op modulo K*T·4 of redundant copies and ~50 fresh
    // K*T allocations per file. Just mark the block decoded and
    // flag the extract path to skip the memcpy.
    ctx.decoded                       = true;
    ctx.skipped_via_lossless_libflute = true;
    return true;
  }
  return false;
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
  if (ctx.skipped_via_lossless_libflute) {
    // File buffer already holds the correct source bytes — every
    // received source ESI's put_symbol wrote them straight to the
    // file buffer via Symbol::data. Nothing to extract.
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

LibFlute::SourceBlock LibFlute::RaptorFEC::create_block_placeholder(int blockid) {
  // Build a SourceBlock with target_K Symbol slots whose data
  // pointers are nullptr — that's the signal File::get_next_symbols
  // checks to know it should call prepare_for_emit() before reading
  // any of them. No source-byte copy, no repair-symbol generation,
  // no Encoder::Create here: those happen lazily in
  // fill_block_into_scratch().
  struct SourceBlock source_block;
  source_block.id = blockid;
  const unsigned int symbols_to_emit = target_K(blockid);
  source_block.symbols.assign(symbols_to_emit, LibFlute::Symbol{});
  return source_block;
}

void LibFlute::RaptorFEC::fill_block_into_scratch(LibFlute::SourceBlock& srcblk) {
  if (_enc_src_buffer == nullptr) {
    throw std::runtime_error(
        "RaptorFEC::fill_block_into_scratch called before create_blocks");
  }
  const int          blockid        = static_cast<int>(srcblk.id);
  const unsigned int nsymbs         = block_K(static_cast<unsigned int>(blockid));
  const unsigned long byte_off      = block_byte_offset(static_cast<unsigned int>(blockid));
  unsigned long      blocksize      = static_cast<unsigned long>(nsymbs) * T;
  if (byte_off + blocksize > F) {
    blocksize = F - byte_off;
  }
  const unsigned int symbols_to_emit = target_K(blockid);
  const unsigned int padded_size     = nsymbs * T;
  // One scratch buffer reused across blocks. Sized to max K_target ×
  // T (which is just K_target × T for KL block since KL ≥ KS). Resize
  // upward only — std::vector::resize doesn't shrink-fit, so on
  // subsequent blocks the allocation is already in place and the
  // source memcpy below overwrites the old contents.
  const std::size_t scratch_bytes =
      static_cast<std::size_t>(symbols_to_emit) * T;
  if (_enc_scratch.size() < scratch_bytes) {
    _enc_scratch.resize(scratch_bytes);
  }
  // Source bytes go straight into slots [0, nsymbs). For the
  // trailing block when F % T ≠ 0, the partial last symbol's tail is
  // zero-padded — the only zero-fill we still need (≤ T bytes per
  // file, on the very last block). Repair slots [nsymbs, target_K)
  // are written in-place by EncodeSymbol below; that function does
  // its own std::fill at entry, so no zero-init needed here.
  std::memcpy(_enc_scratch.data(), _enc_src_buffer + byte_off, blocksize);
  if (blocksize < padded_size) {
    std::memset(_enc_scratch.data() + blocksize, 0, padded_size - blocksize);
  }

  spdlog::debug("Filling r10 scratch for SBN {}: K={} blocksize={} (padded={}) target_K={}",
                blockid, nsymbs, blocksize, padded_size, symbols_to_emit);

  // Per RFC 5053 §4.4.1.2 a file has at most 2 distinct K values
  // (KL and KL-1). Cache one Encoder per K — across both blocks of
  // a multi-block file AND across all RaptorFEC instances of a
  // session if the K stays stable, the 12 MB intermediate-symbols
  // buffer is allocated once instead of per block. Per the
  // r10_bench encode-reset measurement at K=8000 / T=1424, Reset
  // is 13.9 ms vs Create's 22.6 ms — a 39 % per-call saving.
  EncSlot* slot_ptr = nullptr;
  for (auto& slot : _enc_slots) {
    if (slot.enc.has_value() && slot.K == nsymbs) {
      slot_ptr = &slot;
      break;
    }
  }
  if (slot_ptr == nullptr) {
    for (auto& slot : _enc_slots) {
      if (!slot.enc.has_value()) {
        bitstem::fec::EncoderParams params;
        params.K      = static_cast<std::uint16_t>(nsymbs);
        params.T      = static_cast<std::uint16_t>(T);
        params.scheme = _bitstem_scheme;
        params.Al     = static_cast<std::uint8_t>(Al);
        params.N      = static_cast<std::uint16_t>(N);
        auto enc = bitstem::fec::fast::Encoder::Create(params);
        if (!enc.has_value()) {
          spdlog::error("bitstem::fec::Encoder::Create failed for SBN {} K={} scheme={} Al={} N={}",
                        blockid, nsymbs, static_cast<int>(_bitstem_scheme),
                        params.Al, params.N);
          throw std::runtime_error("Error creating FEC encoder");
        }
        slot.K   = static_cast<std::uint16_t>(nsymbs);
        slot.enc = std::move(*enc);
        slot_ptr = &slot;
        break;
      }
    }
    if (slot_ptr == nullptr) {
      throw std::runtime_error(
          "RaptorFEC: more than 2 distinct K values requested for one file");
    }
  }
  auto& enc = *slot_ptr->enc;
  enc.Reset(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(_enc_scratch.data()),
      padded_size));

  for (unsigned int esi = 0; esi < symbols_to_emit; ++esi) {
    char* slot = _enc_scratch.data() + esi * T;
    if (esi >= nsymbs) {
      enc.EncodeSymbol(esi,
                       std::span<std::byte>(
                           reinterpret_cast<std::byte*>(slot), T));
    }
    // Source ESIs (esi < nsymbs) reuse the bytes already memcpy'd in
    // — r10's LT for esi<K reproduces the source symbol verbatim.
    srcblk.symbols[esi].data     = slot;
    srcblk.symbols[esi].length   = T;
    srcblk.symbols[esi].complete = false;
    srcblk.symbols[esi].queued   = false;
  }
  _enc_scratch_sbn = blockid;
}

void LibFlute::RaptorFEC::prepare_for_emit(LibFlute::SourceBlock& srcblk) {
  if (!is_encoder) return;
  // Idempotent: same block already in scratch with valid Symbol
  // pointers means we're being re-entered (shouldn't happen on the
  // forward-only emit path, but cheap to guard).
  if (_enc_scratch_sbn == static_cast<int>(srcblk.id) &&
      !srcblk.symbols.empty() &&
      srcblk.symbols[0].data != nullptr) {
    return;
  }

  if (_fec_worker_threads == 0) {
    // Sequential path — pump thread fills inline.
    fill_block_into_scratch(srcblk);
    return;
  }

  // Parallel path. The first call lazily spins the worker pool: at
  // this point we have a stable address for the SourceBlock array
  // (File::_source_blocks lives until the File does), so subsequent
  // worker indexing is safe.
  if (!_pool) {
    _src_blocks_data = std::addressof(srcblk) - srcblk.id;
    start_pool(Z);
  }

  std::unique_lock<std::mutex> lk(_pool->mtx);
  _pool->ready_cv.wait(lk, [&]() {
    return _pool->ready[srcblk.id].load(std::memory_order_acquire);
  });
}

void LibFlute::RaptorFEC::start_pool(std::size_t z) {
  _pool = std::make_unique<PoolState>();
  _pool->ready    = std::vector<std::atomic<bool>>(z);
  _pool->released = std::vector<std::atomic<bool>>(z);
  // std::atomic<bool> default-inits to false but only since C++20 — be
  // explicit for clarity.
  for (auto& a : _pool->ready)    { a.store(false, std::memory_order_relaxed); }
  for (auto& a : _pool->released) { a.store(false, std::memory_order_relaxed); }

  // Cap workers at Z — extra workers buy nothing and just waste
  // threads sitting idle on the released_cv.
  const unsigned int n = std::min<unsigned int>(_fec_worker_threads,
                                                  static_cast<unsigned int>(z));
  _pool->workers.resize(n);
  for (unsigned int i = 0; i < n; ++i) {
    _pool->workers[i].thread = std::thread(&RaptorFEC::worker_loop, this, i);
  }
}

void LibFlute::RaptorFEC::stop_pool() {
  if (!_pool) return;
  {
    const std::lock_guard<std::mutex> lk(_pool->mtx);
    _pool->stop.store(true, std::memory_order_release);
  }
  _pool->released_cv.notify_all();
  _pool->ready_cv.notify_all();
  for (auto& w : _pool->workers) {
    if (w.thread.joinable()) {
      w.thread.join();
    }
  }
  _pool.reset();
}

void LibFlute::RaptorFEC::worker_loop(unsigned worker_id) {
  auto& w = _pool->workers[worker_id];
  int my_held_sbn = -1;  // SBN whose data this worker's scratch holds

  while (true) {
    // If we're holding a previous block, wait for the pump to finish
    // emitting it before recycling our scratch. The release flag is
    // set by check_source_block_completion().
    if (my_held_sbn >= 0) {
      std::unique_lock<std::mutex> lk(_pool->mtx);
      _pool->released_cv.wait(lk, [&]() {
        return _pool->stop.load(std::memory_order_acquire) ||
               _pool->released[my_held_sbn].load(std::memory_order_acquire);
      });
      if (_pool->stop.load(std::memory_order_acquire)) return;
    }

    // Claim next pending SBN. fetch_add is the load-balancing
    // primitive — first worker to wake gets the next block.
    const int sbn = _pool->next_sbn.fetch_add(1, std::memory_order_acq_rel);
    if (sbn >= static_cast<int>(_pool->ready.size())) {
      // No more blocks. Worker exits cleanly.
      return;
    }

    fill_block_into_worker_scratch(w, _src_blocks_data[sbn]);

    {
      const std::lock_guard<std::mutex> lk(_pool->mtx);
      _pool->ready[sbn].store(true, std::memory_order_release);
    }
    _pool->ready_cv.notify_all();
    my_held_sbn = sbn;
  }
}

void LibFlute::RaptorFEC::fill_block_into_worker_scratch(
    WorkerSlot& w, LibFlute::SourceBlock& srcblk) {
  // Mirror of fill_block_into_scratch() but operating on the worker's
  // own scratch + EncSlot[2] instead of the RaptorFEC's shared
  // members. Source bytes come from _enc_src_buffer (read-only,
  // shared safely across workers) at the block's offset; repair
  // symbols are written into the worker's scratch by the per-K
  // bitstem-r10 Encoder.
  const int          blockid = static_cast<int>(srcblk.id);
  const unsigned int nsymbs  = block_K(static_cast<unsigned int>(blockid));
  const unsigned long byte_off = block_byte_offset(static_cast<unsigned int>(blockid));
  unsigned long      blocksize = static_cast<unsigned long>(nsymbs) * T;
  if (byte_off + blocksize > F) {
    blocksize = F - byte_off;
  }
  const unsigned int symbols_to_emit = target_K(blockid);
  const unsigned int padded_size     = nsymbs * T;
  const std::size_t scratch_bytes =
      static_cast<std::size_t>(symbols_to_emit) * T;
  if (w.scratch.size() < scratch_bytes) {
    w.scratch.resize(scratch_bytes);
  }
  std::memcpy(w.scratch.data(), _enc_src_buffer + byte_off, blocksize);
  if (blocksize < padded_size) {
    std::memset(w.scratch.data() + blocksize, 0, padded_size - blocksize);
  }

  // Per-worker per-K Encoder cache. The bitstem-fec lib's per-K
  // schedule cache amortises Creates across workers — first worker
  // to hit a given K pays the cold price, subsequent workers (and
  // subsequent blocks of the same K within one worker) pay only the
  // warm Create cost. Keeping the cache per-worker means no shared
  // mutable state between threads on the encode hot path.
  EncSlot* slot_ptr = nullptr;
  for (auto& slot : w.enc_slots) {
    if (slot.enc.has_value() && slot.K == nsymbs) {
      slot_ptr = &slot;
      break;
    }
  }
  if (slot_ptr == nullptr) {
    for (auto& slot : w.enc_slots) {
      if (!slot.enc.has_value()) {
        bitstem::fec::EncoderParams params;
        params.K      = static_cast<std::uint16_t>(nsymbs);
        params.T      = static_cast<std::uint16_t>(T);
        params.scheme = _bitstem_scheme;
        params.Al     = static_cast<std::uint8_t>(Al);
        params.N      = static_cast<std::uint16_t>(N);
        auto enc = bitstem::fec::fast::Encoder::Create(params);
        if (!enc.has_value()) {
          throw std::runtime_error("Error creating FEC encoder");
        }
        slot.K   = static_cast<std::uint16_t>(nsymbs);
        slot.enc = std::move(*enc);
        slot_ptr = &slot;
        break;
      }
    }
    if (slot_ptr == nullptr) {
      throw std::runtime_error(
          "RaptorFEC: more than 2 distinct K values requested for one file");
    }
  }
  auto& enc = *slot_ptr->enc;
  enc.Reset(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(w.scratch.data()),
      padded_size));

  for (unsigned int esi = 0; esi < symbols_to_emit; ++esi) {
    char* slot = w.scratch.data() + esi * T;
    if (esi >= nsymbs) {
      enc.EncodeSymbol(esi,
                        std::span<std::byte>(
                            reinterpret_cast<std::byte*>(slot), T));
    }
    srcblk.symbols[esi].data     = slot;
    srcblk.symbols[esi].length   = T;
    srcblk.symbols[esi].complete = false;
    srcblk.symbols[esi].queued   = false;
  }
}

std::vector<LibFlute::SourceBlock>
LibFlute::RaptorFEC::create_blocks(char *buffer, int *bytes_read) {
  if (!bytes_read) {
    throw std::invalid_argument("bytes_read pointer shouldn't be null");
  }
  // N>1 (sub-block interleaving) is the codec's concern: the K·T
  // contiguous source bytes per block ARE the on-symbol layout per
  // RFC 5053 §4.4.1.2 / RFC 6330 §4.4.1.2. libflute hands them to
  // bitstem-fec and the lib re-lays-out into N sub-block buffers
  // internally. From File / SourceBlock POV the stride stays at T
  // bytes per ESI regardless of N. If the caller-or-autodetect picks
  // an N the codec doesn't yet support, Encoder::Create returns
  // nullopt and the per-block fill-into-scratch path throws — the
  // failure surfaces at first emit, not at create_blocks time.

  std::vector<LibFlute::SourceBlock> block_vec(Z);
  *bytes_read = 0;

  if (is_encoder) {
    // Encoder side: build placeholders only. The actual scratch fill
    // + repair-symbol generation is deferred to prepare_for_emit(),
    // invoked by File::get_next_symbols when the cursor first hits
    // each block. Peak memory is one block's worth (max K × T) of
    // scratch, not Z × that, and the per-block work happens
    // interleaved with packet dispatch — useful when the consumer
    // applies back-pressure (rate limiting, syscall blocking).
    _enc_src_buffer     = buffer;
    _enc_src_buffer_len = F;
    _enc_scratch_sbn    = -1;
    *bytes_read         = static_cast<int>(F);
    for (unsigned int sbn = 0; sbn < Z; ++sbn) {
      block_vec[sbn] = create_block_placeholder(static_cast<int>(sbn));
    }
    return block_vec;
  }

  // Decoder side: Symbol::data points directly into the receiver's
  // file buffer (one slot per ESI).
  for (unsigned int sbn = 0; sbn < Z; ++sbn) {
    auto& block = block_vec[sbn];
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

  // SSI byte layout flips between schemes (RFC 5053 §3.2 vs RFC 6330
  // §3.4.2): R10 = Z(2) + N(1) + Al(1); RaptorQ = Z(1) + N(2) + Al(1).
  if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
    Z  =  (uint8_t)scheme_specific_info[0];
    N  = ((uint8_t)scheme_specific_info[1] << 8) |
          (uint8_t)scheme_specific_info[2];
    Al =  (uint8_t)scheme_specific_info[3];
  } else {
    Z  = ((uint8_t)scheme_specific_info[0] << 8) |
          (uint8_t)scheme_specific_info[1];
    N  =  (uint8_t)scheme_specific_info[2];
    Al =  (uint8_t)scheme_specific_info[3];
  }

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
  file->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned) _fec_scheme);
  file->SetAttribute("FEC-OTI-Encoding-Symbol-Length", T);
  file->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", K);

  // RFC 5053 §3.2 / RFC 6330 §3.4.2: the per-FEC-scheme parameters
  // ride in a single 4-byte FEC-OTI-Scheme-Specific-Info attribute as
  // base64. Byte allocation differs by scheme:
  //   R10:     Z(2) + N(1) + Al(1)
  //   RaptorQ: Z(1) + N(2) + Al(1)
  std::array<unsigned char, 4> ssi{};
  if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
    ssi[0] = static_cast<unsigned char>(Z & 0xFFU);
    ssi[1] = static_cast<unsigned char>((N >> 8) & 0xFFU);
    ssi[2] = static_cast<unsigned char>(N & 0xFFU);
    ssi[3] = static_cast<unsigned char>(Al);
  } else {
    ssi[0] = static_cast<unsigned char>((Z >> 8) & 0xFFU);
    ssi[1] = static_cast<unsigned char>(Z & 0xFFU);
    ssi[2] = static_cast<unsigned char>(N);
    ssi[3] = static_cast<unsigned char>(Al);
  }
  std::string ssi_b64 = base64_encode({ssi.begin(), ssi.end()}, ssi.size());
  file->SetAttribute("FEC-OTI-Scheme-Specific-Info", ssi_b64.c_str());

  is_encoder = true;
  return true;
}
