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
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

#include "fec/FecLoader.h"
#include "fec/FecTransformer.h"
#include "flute_types.h"

#include "bitstem/fec/fec_c.h"

namespace tinyxml2 { class XMLElement; }

namespace LibFlute {
  // Glue between LibFlute's FEC abstraction and the bitstem-fec C ABI
  // surface (libfec.so.0, accessed via FecLoader's dlopen + dlsym).
  // libflute has NO link-time dependency on libfec.so — production
  // builds without an FEC license drop the .so and the loader's
  // available()/has_scheme() probes route Raptor / RaptorQ files
  // through a clean "FEC unavailable" path.
  //
  // One Decoder is constructed per source block on first received
  // symbol and reused across all subsequent process_symbol() calls
  // for that block; one Encoder per K is cached across blocks of
  // the same K (RFC 5053 §4.4.1.2 bounds files to ≤ 2 distinct K
  // values, so a 2-slot cache suffices).
  //
  // Custom deleters route through FecLoader's destroy function
  // pointers — required because the handles are opaque C types and
  // unique_ptr's default deleter (std::default_delete) would call
  // delete on an incomplete type.
  struct EncoderDeleter {
    void operator()(bitstem_fec_encoder_t* e) const noexcept {
      if (e != nullptr) {
        FecLoader::instance().encoder_destroy(e);
      }
    }
  };
  struct DecoderDeleter {
    void operator()(bitstem_fec_decoder_t* d) const noexcept {
      if (d != nullptr) {
        FecLoader::instance().decoder_destroy(d);
      }
    }
  };
  using EncoderHandle = std::unique_ptr<bitstem_fec_encoder_t, EncoderDeleter>;
  using DecoderHandle = std::unique_ptr<bitstem_fec_decoder_t, DecoderDeleter>;

  class RaptorFEC : public FecTransformer {

    private:

      bool is_encoder = true;

      unsigned int target_K(int blockno);

      // Per-source-block decoder context. The codec's lent-buffer
      // Decoder is constructed once per SBN, bound at construction
      // to the K·T slice of File::_buffer at block_byte_offset(sbn);
      // subsequent AddReceivedSymbol / TryDecode calls write bytes
      // (received and reconstructed) directly into the file buffer.
      //
      // Per-SBN ownership is required because ALC packets arrive in
      // arbitrary SBN order — multiple SBNs accumulate receive state
      // concurrently and Decoder::Reset(span) clears that state, so
      // sharing a Decoder across SBNs (even at the same K) isn't
      // viable on the receive side. The per-instance scratch
      // (~K·T-class) is unavoidable; the encoder-side per-K pool's
      // Create-cost amortization comes from a process-wide schedule
      // cache inside bitstem-fec — that cache is encoder-only (the
      // per-block schedule on decode depends on the received-ESI
      // set, can't be cached by K alone), so the decoder gets no
      // equivalent benefit.
      struct DecoderCtx {
        DecoderHandle dec;
        std::uint16_t K = 0;            // source-symbol count for THIS block
        std::uint32_t block_size = 0;   // bytes -- usually K*T, smaller for last block
        // Total symbols add_received_symbol'd into this Decoder. Drives
        // the every-N opportunistic try_decode trigger in process_symbol:
        // mid-stream matrix-solve attempts pay off when the FDT-removal
        // trigger may be delayed (long sessions, sparse FDT updates).
        std::uint32_t received_count = 0;
        // ESIs already passed to add_received_symbol on this Decoder.
        // libflute used to dedup via the slot vector's
        // `target_symbol.complete` flag; the Raptor path no longer
        // touches that vector (the codec is free to use any ESI in
        // [0, K_max), and slot-by-ESI is the wrong storage shape).
        // Receivers MUST still filter duplicates so the Decoder stats
        // (source_symbols_received / repair_symbols_received) don't
        // count the same wire packet twice on retransmits.
        std::unordered_set<std::uint32_t> seen_esis;
      };

      // Per-source-block decoder state; survives across process_symbol()
      // calls for the same block.
      std::map<std::uint16_t, DecoderCtx> _dec_ctxs;
      // Pointer to File::_buffer, captured in create_blocks (decoder
      // side) — same pattern as _enc_src_buffer on the encoder side.
      // The lent span passed to Decoder::Create(params, span) is a
      // K·T slice of this buffer.
      char* _dec_file_buffer = nullptr;
      // Worker count for the parallel TryDecode pool used at
      // try_decode_pending (end-of-transmission FDT trigger). 0 ⇒
      // sequential. Mid-stream lossless decode happens via the
      // codec's auto-finalise inside AddReceivedSymbol regardless;
      // workers only kick in when ≥1 source ESI was lost in ≥1 block
      // and Z > 1 (distributed-fade reception pattern). Each worker
      // calls dec->TryDecode() on a different SBN's per-SBN Decoder;
      // no shared state contention since each Decoder owns its
      // scratch + a non-overlapping slice of File::_buffer.
      unsigned _dec_worker_threads = 0;

      // Encoder-side single shared symbol scratch. Holds at most
      // max(target_K) × T bytes — one source block's worth, laid out
      // as K source slots followed by (target_K − K) repair slots.
      // Allocated once on first prepare_for_emit() and reused across
      // every subsequent block: the encoder emits packets strictly
      // SBN-ascending, so block N+1 only enters scratch after block
      // N has fully transmitted (its Symbol::data pointers are
      // nulled in check_source_block_completion). Peak memory is
      // O(K_max × T) instead of O(Z × K × T).
      std::vector<char> _enc_scratch;
      // Cached per-K bitstem-r10 encoder, reused across blocks.
      // Keeping the Encoder alive across blocks means the
      // intermediate-symbols buffer (~12 MB at K=8000, T=1424) is
      // allocated once per RaptorFEC lifetime instead of once per
      // block, killing the kernel page-fault overhead that dominated
      // ~17 % of Encoder::Create cycles. Per RFC 5053 §4.4.1.2 a
      // file has at most 2 distinct K values (KL and KL-1), so a
      // 2-slot map is sufficient.
      struct EncSlot {
        std::uint16_t K = 0;
        EncoderHandle enc;
      };
      std::array<EncSlot, 2> _enc_slots;
      // SBN currently materialised in _enc_scratch, or -1 if scratch
      // is empty. Used as a no-op guard if prepare_for_emit() is
      // re-entered for the same block (shouldn't happen with the
      // forward-only emit cursor, but cheap insurance).
      int _enc_scratch_sbn = -1;
      // Worker count for the optional parallel-encode pool. 0 ⇒ inline
      // single-threaded fill (today's behaviour); non-zero ⇒ N workers
      // pre-encode blocks ahead of the pump (see Encoder::Encoder doc).
      // Plumbed in from Encoder via File ctor.
      unsigned _fec_worker_threads = 0;

      // Parallel-encode worker pool. Owned only when
      // _fec_worker_threads > 0; nullptr otherwise. Bound to the
      // RaptorFEC instance lifetime: spun up at the end of
      // calculate_partitioning() / create_blocks() (once Z is known)
      // and joined in ~RaptorFEC.
      //
      // Each worker holds its own scratch + EncSlot[2] cache, mirroring
      // the per-K Encoder reuse pattern used in the sequential path.
      // The bitstem-fec lib's per-K schedule cache transparently
      // amortises Creates across workers — first worker per K pays the
      // cold price, workers 2..N pay only the warm Create cost.
      //
      // Block dispatch: workers atomically claim the next pending SBN
      // from `_pool->next_sbn`, fill their scratch (memcpy → Reset →
      // EncodeSymbol loop) and write Symbol::data pointers into the
      // SourceBlock for the pump. They then wait for the pump's emit
      // path to mark the block complete (signalled via
      // check_source_block_completion → released[sbn]) before
      // recycling their scratch for the next claim.
      struct WorkerSlot {
        std::vector<char> scratch;
        std::array<EncSlot, 2> enc_slots;
        std::thread thread;
      };
      struct PoolState {
        // Per-SBN flags. Sized to Z in start_pool() and never resized.
        std::vector<std::atomic<bool>> ready;
        std::vector<std::atomic<bool>> released;
        std::atomic<int>  next_sbn{0};
        std::atomic<bool> stop{false};

        std::mutex              mtx;
        std::condition_variable ready_cv;     // pump waits, workers notify
        std::condition_variable released_cv;  // workers wait, pump notifies

        std::vector<WorkerSlot> workers;
      };
      std::unique_ptr<PoolState> _pool;
      // Base pointer to the SourceBlock array workers operate on.
      // Derived in prepare_for_emit() on the first call from the
      // pump-supplied SourceBlock& (which lives inside File's
      // _source_blocks vector — stable for the File's lifetime, and
      // hence for the RaptorFEC's lifetime). Workers index
      // `_src_blocks_data[sbn]` by SBN.
      LibFlute::SourceBlock* _src_blocks_data = nullptr;

      void start_pool(std::size_t Z);
      void stop_pool();
      void worker_loop(unsigned worker_id);
      void fill_block_into_worker_scratch(WorkerSlot& w,
                                            LibFlute::SourceBlock& srcblk);
      // User file buffer + length, captured in create_blocks() so
      // prepare_for_emit() can stripe each block's source bytes into
      // _enc_scratch on demand without create_blocks having to walk
      // the whole file up front.
      char*       _enc_src_buffer = nullptr;
      std::size_t _enc_src_buffer_len = 0;

      // Helpers.
      LibFlute::SourceBlock create_block_placeholder(int blockid);
      void                  fill_block_into_scratch(LibFlute::SourceBlock& srcblk);

      // Repair-symbol overhead. Default of 1.15× protects against ~15%
      // packet loss. The constructor overrides this from the caller's
      // FEC-Redundancy-Level (TS 26.346 Rel-11 mbms2012 attribute, an
      // integer percent ⇒ ratio = 1 + percent/100). Smaller files may
      // pack up to 10 symbols per packet but are less vulnerable to
      // loss to begin with.
      float surplus_packet_ratio = 1.15f;

      // FEC scheme picked at construction. Maps to bitstem_fec_scheme_t
      // for Encoder / Decoder Create calls. Default R10 keeps the
      // pre-multi-scheme call shape for the receive-side empty ctor.
      bitstem_fec_scheme_t _bitstem_scheme = BITSTEM_FEC_SCHEME_R10;
      // Wire-level FEC-Encoding-ID — also recorded so AlcPacket /
      // EncodingSymbol scheme dispatch can reach the right SSI byte
      // layout and FEC-Payload-ID width.
      LibFlute::FecScheme _fec_scheme = LibFlute::FecScheme::Raptor;
      // True iff Z / N / Al came from a caller-supplied
      // FEC-OTI-Scheme-Specific-Info on construction (xMB / SDP path).
      // calculate_partitioning() then preserves them instead of
      // recomputing from F / T / W; the autodetect path is the
      // libflute-internal-driven default when the caller leaves SSI
      // empty.
      bool _ssi_caller_supplied = false;

    public:

      // Encoder-side ctor. Consumes a populated FecOti — the caller
      // (File::File transmit ctor) provides scheme + transfer length +
      // max payload + (optional) caller-supplied scheme-specific info.
      // When `fec_oti.scheme_specific_info` is 4 bytes, Z/N/Al are
      // parsed out of it (per the scheme's RFC 5053 / RFC 6330 byte
      // layout) and used verbatim during partitioning instead of being
      // recomputed.
      //
      // `sub_block_size_target` (= W in the RFC's notation) drives the
      // autodetect for N. Sentinel 0 ⇒ default (16 MB; biases the
      // autodetect toward N=1 for codec compatibility while
      // bitstem-fec's sub-block-interleaved path is still landing).
      RaptorFEC(const FecOti& fec_oti,
                std::optional<unsigned> fec_redundancy_level = std::nullopt,
                unsigned fec_worker_threads = 0,
                std::uint64_t sub_block_size_target = 0,
                std::uint8_t sub_symbol_size_min_multiplier = 1);

      // Decoder-side empty ctor. The scheme is supplied so AlcPacket /
      // EncodingSymbol can dispatch on the correct FEC-Payload-ID width
      // before parse_fdt_info has had a chance to populate the rest.
      // `dec_worker_threads` ⇒ size of the parallel TryDecode pool used
      // at try_decode_pending (FDT end-of-transmission). 0 = sequential.
      // Bounded per-file by Z; useful at Z ≥ 2 with distributed loss
      // (multiple SBNs needing matrix-solve).
      // `fec_redundancy_level` ⇒ TS 26.346 Rel-11 mbms2012 attribute
      // carried per-File on the FDT. When present, drives the
      // per-block ESI buffer sizing (= K·(1 + FRL/100)) so the
      // decoder matches the encoder's emit overhead exactly. When
      // absent, the default 1.15 stays in force; the FDT-removal
      // trigger in Decoder.cpp is the safety net for the lossy path.
      explicit RaptorFEC(LibFlute::FecScheme scheme,
                          unsigned dec_worker_threads = 0,
                          std::optional<unsigned> fec_redundancy_level = std::nullopt);
      RaptorFEC() : RaptorFEC(LibFlute::FecScheme::Raptor, 0, std::nullopt) {}

      ~RaptorFEC() override;

      bool check_source_block_completion(SourceBlock& srcblk) override;

      std::vector<SourceBlock> create_blocks(char *buffer, int *bytes_read) override;

      bool process_symbol(LibFlute::SourceBlock& srcblk,
                          unsigned int id,
                          std::span<const std::byte> bytes) override;

      // Internal helper: lazily construct one Decoder per SBN, bound
      // to the K·T slice of File::_buffer at block_byte_offset(sbn).
      // Subsequent calls return the existing context.
      DecoderCtx& ensure_dec_ctx(std::uint16_t sbn);

      bool try_decode_pending(std::vector<LibFlute::SourceBlock>& blocks) override;

      void prepare_for_emit(LibFlute::SourceBlock& srcblk) override;

      // Lent-buffer Decoder writes source-symbol bytes directly into
      // File::_buffer via AddReceivedSymbol — File::put_symbol must
      // skip its own decode_to() copy.
      bool writes_symbol_bytes_directly() const override { return true; }

      bool calculate_partitioning() override;

      bool parse_fdt_info(tinyxml2::XMLElement *file) override;

      bool add_fdt_info(tinyxml2::XMLElement *file) override;

      void *allocate_file_buffer(int min_length) override;

      bool extract_file(std::vector<SourceBlock>& blocks) override;

      uint32_t nof_source_symbols = 0;
      uint32_t nof_source_blocks = 0;
      uint32_t large_source_block_length = 0;
      uint32_t small_source_block_length = 0;
      uint32_t nof_large_source_blocks = 0;

      unsigned int F;          // object size in bytes
      unsigned int Al = 4;     // symbol alignment
      unsigned int T;          // symbol size in bytes
      // Sub-block size target (RFC 5053 §4.2 W / RFC 6330 §4.3 WS).
      // Default 16 MB. Caller can override per-file via
      // FileTransmissionConfig::sub_block_size_target. TS 26.346
      // §B.3.4.1 mandates 256 KB for R10 file delivery; RaptorQ
      // leaves WS as a deployment knob.
      unsigned long W = 16UL * 1024UL * 1024UL;
      // RFC 6330 §4.3 SS — desired lower bound on sub-symbol size,
      // expressed as a multiplier on Al (so the actual lower bound
      // is SS·Al octets). Used in the §4.3 N derivation to cap N at
      // floor(T/(SS·Al)). RaptorQ-only.
      std::uint8_t SS = 1;
      unsigned int G;          // symbols per packet
      unsigned int Z;          // number of source blocks
      unsigned int N;          // sub-blocks per source block
      unsigned int Kt;         // total symbols across all blocks
      unsigned int P;          // max payload size

      // RFC 5053 §4.4.1.2 source-block partitioning. Block i carries
      // KL source symbols when i < ZL, otherwise KS. KL = KS or
      // KL = KS + 1; the split distributes Kt symbols across Z blocks
      // as evenly as possible. The previous "K = min(Kt, 8192) +
      // remainder-in-last-block" partitioning could leave the
      // trailing block with K < 4 (below bitstem-r10's kJKMinK) for
      // pathological Kt mod 8192 values.
      unsigned int KL = 0;
      unsigned int KS = 0;
      unsigned int ZL = 0;
      unsigned int ZS = 0;

      // Per-block K (number of source symbols).
      unsigned int block_K(unsigned int blockid) const {
        return (blockid < ZL) ? KL : KS;
      }

      // Byte offset of source-block `blockid` into the contiguous
      // source object buffer.
      unsigned long block_byte_offset(unsigned int blockid) const {
        if (blockid < ZL) {
          return static_cast<unsigned long>(blockid) * KL * T;
        }
        return static_cast<unsigned long>(ZL) * KL * T +
               static_cast<unsigned long>(blockid - ZL) * KS * T;
      }

      // K is retained as an alias for the largest per-block size,
      // since several existing call sites use it as an upper bound
      // (e.g. allocate_file_buffer's worst-case sizing).
      unsigned int K = 0;
  };
};
