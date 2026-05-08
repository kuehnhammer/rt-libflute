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
#include "fec/rfc6330_kprime_table.h"

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
                                std::uint64_t sub_block_size_target,
                                std::uint8_t sub_symbol_size_min_multiplier)
    // Init order matches member declaration order in RaptorFEC.h
    // (Wreorder-ctor under -Werror).
    : _fec_worker_threads(fec_worker_threads)
    , _bitstem_scheme(to_bitstem_scheme(fec_oti.encoding_id))
    , _fec_scheme(fec_oti.encoding_id)
    , F(fec_oti.transfer_length)
    , P(fec_oti.encoding_symbol_length)
{
  if (fec_redundancy_level.has_value()) {
    surplus_packet_ratio = 1.0f + static_cast<float>(*fec_redundancy_level) / 100.0f;
  }
  if (sub_block_size_target != 0) {
    W = static_cast<unsigned long>(sub_block_size_target);
  }
  if (sub_symbol_size_min_multiplier > 0) {
    SS = sub_symbol_size_min_multiplier;
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

  // RFC 6330 §4.3 KL(n): the maximum K' value in §5.6 Table 2 such
  // that K' <= WS / (Al · ceil(T/(Al·n))). Returns 0 if no K' fits
  // (WS too small for the codec's smallest table entry).
  auto kl_for_n = [&](unsigned n) -> std::uint32_t {
    if (n == 0) return 0;
    const auto t1n  = (T + (Al * n) - 1u) / (Al * n);  // ceil(T/(Al·n))
    if (t1n == 0) return 0;
    const auto bound = static_cast<std::uint64_t>(W) /
                       (static_cast<std::uint64_t>(Al) * t1n);
    auto it = std::upper_bound(rfc6330::kKPrimeTable.begin(),
                                rfc6330::kKPrimeTable.end(),
                                static_cast<std::uint32_t>(
                                    std::min<std::uint64_t>(
                                        bound,
                                        std::numeric_limits<std::uint32_t>::max())));
    if (it == rfc6330::kKPrimeTable.begin()) return 0;
    return *(it - 1);
  };

  // Z + N derivation: caller-supplied SSI wins; otherwise dispatch
  // on scheme.
  //
  // RFC 5053 §4.2 (R10): K_max=8192 bounds Z; W shapes N below.
  // RFC 6330 §4.3 (RaptorQ): the example parameter derivation
  //   algorithm is computed verbatim here. Z = ceil(Kt/KL(N_max)),
  //   N = min n in [1, N_max] s.t. ceil(Kt/Z) <= KL(n). Both Z and
  //   N are determined by the algorithm; they are NOT independent
  //   knobs.
  if (!_ssi_caller_supplied) {
    if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
      const unsigned n_max = T / (static_cast<unsigned>(SS) * Al);
      if (n_max < 1u) {
        throw std::runtime_error(
            "RaptorFEC §4.3: N_max < 1, T must be at least SS·Al octets");
      }
      const std::uint32_t kl_max = kl_for_n(n_max);
      if (kl_max == 0u) {
        throw std::runtime_error(
            "RaptorFEC §4.3: WS is too small for any K' in Table 2 "
            "at this T / Al / N_max combination — increase WS");
      }
      Z = static_cast<unsigned>(
          (static_cast<std::uint64_t>(Kt) + kl_max - 1ULL) / kl_max);
      if (Z < 1u) Z = 1u;
      spdlog::debug("Z = {} = ceil(Kt={}/KL(N_max={})={}) (RFC 6330 §4.3)",
                    Z, Kt, n_max, kl_max);
    } else {
      // RFC 5053 §4.2: K_max-bounded only. N is derived later.
      Z = static_cast<unsigned int>(
          (static_cast<std::uint64_t>(Kt) + K_max - 1ULL) / K_max);
      if (Z < 1u) Z = 1u;
      spdlog::debug("Z = {} = ceil(Kt/K_max) (RFC 5053 §4.2, K_max={})",
                    Z, K_max);
    }
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

  // N derivation: caller-supplied SSI wins; otherwise dispatch.
  //
  // RFC 5053 §4.2 (R10): N is the codec's cache-friendliness knob.
  //   N = min(ceil(ceil(Kt/Z)·T/W), T/Al). Small W spawns more
  //   sub-blocks per source block.
  //
  // RFC 6330 §4.3 (RaptorQ): N is the SMALLEST n in [1, N_max] such
  //   that ceil(Kt/Z) ≤ KL(n). This minimises sub-block count
  //   subject to the working-memory bound — the spec doesn't permit
  //   the encoder to pick a different value.
  if (!_ssi_caller_supplied) {
    if (_fec_scheme == LibFlute::FecScheme::RaptorQ) {
      const unsigned int k_per_block = (Kt + Z - 1u) / Z;
      const unsigned int n_max = T / (static_cast<unsigned>(SS) * Al);
      unsigned int n_pick = 0;
      for (unsigned n = 1; n <= n_max; ++n) {
        if (kl_for_n(n) >= k_per_block) {
          n_pick = n;
          break;
        }
      }
      if (n_pick == 0) {
        throw std::runtime_error(
            "RaptorFEC §4.3: no n in [1, N_max] satisfies KL(n) ≥ "
            "ceil(Kt/Z); WS is too tight for the chosen Z");
      }
      N = n_pick;
      spdlog::debug("N = {} (RFC 6330 §4.3; smallest n with KL(n) ≥ "
                    "ceil(Kt/Z)={} given WS={})",
                    N, k_per_block, W);
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
  // min_length is only used by the assert; under NDEBUG (Release)
  // the assert is gone and the param looks unused.
  (void)min_length;
  assert(min_length <= (int)(Z * target_K(0) * T));
  return malloc(Z * target_K(0) * T);
}

// Lazily construct one Decoder per SBN, bound at construction to
// the K·T slice of File::_buffer at block_byte_offset(sbn). The
// codec's lent-buffer Decoder writes received + reconstructed
// source bytes directly into that span — no per-block extract
// memcpy is needed.
LibFlute::RaptorFEC::DecoderCtx&
LibFlute::RaptorFEC::ensure_dec_ctx(std::uint16_t sbn) {
  if (_dec_file_buffer == nullptr) {
    throw std::runtime_error(
        "RaptorFEC::ensure_dec_ctx called before create_blocks");
  }
  auto it = _dec_ctxs.find(sbn);
  if (it != _dec_ctxs.end()) {
    return it->second;
  }

  const unsigned int   nsymbs   = block_K(sbn);
  const unsigned long  byte_off = block_byte_offset(sbn);
  unsigned long        blocksize = static_cast<unsigned long>(nsymbs) * T;
  if (byte_off + blocksize > F) {
    blocksize = F - byte_off;
  }

  // Lent-buffer span: K·T bytes. The file buffer is over-allocated
  // to Z·KL·target_K-overhead·T (see allocate_file_buffer) so the
  // K·T slice always lies within it, even for the trailing block
  // when F % T != 0.
  const std::span<std::byte> span(
      reinterpret_cast<std::byte*>(_dec_file_buffer + byte_off),
      static_cast<std::size_t>(nsymbs) * T);

  spdlog::debug("Constructing FEC decoder for SBN {}: K={} blocksize={} (lent buffer)",
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
  // Source-order layout: tells the codec our lent buffer (= File::-
  // _buffer slice) is laid out as K source symbols × T bytes
  // contiguous, matching the file's natural byte order. At N>1 the
  // codec wrapper de-interleaves into its inner sub-block-major
  // working buffer on TryDecode and back out into the lent buffer
  // on completion — without this hint, a wire-cberner-compatible
  // sender's bytes land sub-block-major in File::_buffer and the
  // receiver's file content diverges.
  params.layout = bitstem::fec::LayoutHint::kSourceOrder;
  auto dec = bitstem::fec::fast::Decoder::Create(params, span);
  if (!dec.has_value()) {
    spdlog::error("bitstem::fec::Decoder::Create(span) failed for SBN {} K={} scheme={} Al={} N={}",
                  sbn, nsymbs, static_cast<int>(_bitstem_scheme),
                  params.Al, params.N);
    throw std::runtime_error("FEC decoder construction failed");
  }

  DecoderCtx ctx;
  ctx.dec        = std::move(dec);
  ctx.K          = static_cast<std::uint16_t>(nsymbs);
  ctx.block_size = static_cast<std::uint32_t>(blocksize);
  auto [iter, _inserted] = _dec_ctxs.emplace(sbn, std::move(ctx));
  return iter->second;
}

bool LibFlute::RaptorFEC::process_symbol(LibFlute::SourceBlock& srcblk,
                                          unsigned int id,
                                          std::span<const std::byte> bytes) {
  assert(bytes.size() == T);
  DecoderCtx& ctx = ensure_dec_ctx(static_cast<std::uint16_t>(srcblk.id));
  if (ctx.dec->IsDecoded()) {
    // Block already decoded (almost always via the codec's lossless
    // auto-finalise on the K-th source ESI). Subsequent repair-
    // symbol packets carry redundancy we no longer need.
    spdlog::trace("Skipped symbol after early decode: SBN {}, ESI {}",
                  srcblk.id, id);
    return true;
  }
  ctx.dec->AddReceivedSymbol(id, bytes);
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

  // Decoder side. With bitstem-fec ≥ 0.12.1 the codec's
  // AddReceivedSymbol auto-finalises on the K-th source ESI:
  // IsDecoded() flips, the lent buffer (= File::_buffer slice)
  // already holds the K source symbols. No mid-stream TryDecode
  // call is needed for the lossless path.
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
  return it->second.dec.has_value() && it->second.dec->IsDecoded();
}

bool LibFlute::RaptorFEC::try_decode_pending(
    std::vector<LibFlute::SourceBlock>& blocks) {
  if (is_encoder) return false;

  bool any_decoded = false;
  for (auto& srcblk : blocks) {
    auto it = _dec_ctxs.find(static_cast<std::uint16_t>(srcblk.id));
    if (it == _dec_ctxs.end()) continue;        // no symbols received
    DecoderCtx& ctx = it->second;
    if (ctx.dec->IsDecoded()) continue;          // already done
    if (ctx.dec->TryDecode()) {
      srcblk.complete = true;
      any_decoded     = true;
      spdlog::debug("Raptor: decoded source block {} on end-of-transmission trigger",
                    srcblk.id);
    } else {
      spdlog::debug("Raptor: decode failed for source block {} (insufficient symbols)",
                    srcblk.id);
    }
  }
  return any_decoded;
}

bool LibFlute::RaptorFEC::extract_file(std::vector<SourceBlock>& /*blocks*/) {
  // Lent-buffer Decoder writes recovered source bytes straight into
  // File::_buffer during AddReceivedSymbol / TryDecode. Nothing to
  // extract here — the file's source bytes are already in place.
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
        // Source-order layout: _enc_scratch holds the file slice
        // verbatim (memcpy from _enc_src_buffer), source-order. At
        // N>1 this lets the codec skip its internal transpose into
        // sub-block-major and feed the inner codec directly.
        params.layout = bitstem::fec::LayoutHint::kSourceOrder;
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
        params.layout = bitstem::fec::LayoutHint::kSourceOrder;
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

  // Decoder side: capture the file buffer pointer so the lent-buffer
  // Decoder API can derive K·T-byte spans per source block (one
  // Decoder lazy-constructed per SBN in ensure_dec_ctx).
  _dec_file_buffer = buffer;
  _dec_ctxs.clear();
  for (unsigned int sbn = 0; sbn < Z; ++sbn) {
    auto& block = block_vec[sbn];
    block.id = sbn;
    const unsigned long blk_off        = block_byte_offset(sbn);
    const unsigned int  symbols_to_read = target_K(sbn);
    block.symbols.reserve(symbols_to_read);
    for (unsigned int i = 0; i < symbols_to_read; ++i) {
      LibFlute::Symbol sym{};
      // Symbol::data still points into the file buffer for diagnostic
      // / future use, but the lent-buffer Decoder no longer needs it
      // — AddReceivedSymbol writes via the K·T span captured in
      // ensure_dec_ctx.
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
