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
#include <vector>

#include "fec/FecTransformer.h"
#include "flute_types.h"

#include "bitstem/fec/fec.hpp"

namespace tinyxml2 { class XMLElement; }

namespace LibFlute {
  // Glue between LibFlute's FEC abstraction and the bitstem::fec::fast
  // Encoder / Decoder (currently in R10 mode; RaptorQ lands as a
  // sibling RaptorQFEC class wired against the same lib via
  // Scheme::kRaptorQ). One Decoder is constructed per source block on
  // first received symbol and reused across all subsequent
  // process_symbol() calls for that block.
  class RaptorFEC : public FecTransformer {

    private:

      bool is_encoder = true;

      unsigned int target_K(int blockno);

      // Per-block context. The encoder and decoder are mutually
      // exclusive at run time (a RaptorFEC instance is constructed
      // either as part of a Transmitter -> encoder, or by FDT parsing
      // on the receive side -> decoder), so they share storage as an
      // optional + a map respectively.
      struct DecoderCtx {
        std::optional<bitstem::fec::fast::Decoder> dec;
        std::uint16_t K = 0;            // source-symbol count for THIS block
        std::uint32_t block_size = 0;   // bytes -- usually K*T, smaller for last block
        bool decoded = false;           // cached IsDecoded() so we don't re-call TryDecode
        // True iff the block was completed via the libflute-side
        // lossless skip path (every source ESI received in its
        // natural file-buffer slot via Symbol::data ⇒ no bitstem-r10
        // TryDecode call, no extract_finished_block memcpy needed).
        // The file buffer already holds the correct source bytes;
        // extract_finished_block becomes a no-op for this block.
        bool skipped_via_lossless_libflute = false;
        // Source ESIs (id < K) received so far for this block.
        // When this reaches K we know the lossless short-circuit
        // inside Decoder::TryDecode will fire (every source symbol
        // arrived intact ⇒ permute by ESI, no matrix work). At that
        // moment the block can decode for the cost of a K×T memcpy
        // (~1 ms per block at K=8000, T=1424). For lossy reception
        // (≥1 source missing) we deliberately defer to the FDT
        // end-of-transmission try_decode_pending path so the
        // ~58 ms-per-block matrix factor is batched at the end and
        // doesn't compete with the encoder's per-packet work mid-
        // stream. A bench at drop_every=8000 (1 drop/block) shows
        // mid-stream matrix factoring is a net 13 % regression vs.
        // batched even though total CPU work is identical.
        std::uint32_t source_esi_count = 0;
        bool attempted_lossless_kp1 = false;
      };

      // Per-source-block decoder state; survives across process_symbol()
      // calls for the same block.
      std::map<std::uint16_t, DecoderCtx> _dec_ctxs;

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
        std::optional<bitstem::fec::fast::Encoder> enc;
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
      DecoderCtx& ensure_dec_ctx(std::uint16_t sbn);
      LibFlute::SourceBlock create_block_placeholder(int blockid);
      void                  fill_block_into_scratch(LibFlute::SourceBlock& srcblk);
      void extract_finished_block(LibFlute::SourceBlock& srcblk, DecoderCtx& ctx);

      // Repair-symbol overhead. Default of 1.15× protects against ~15%
      // packet loss. The constructor overrides this from the caller's
      // FEC-Redundancy-Level (TS 26.346 Rel-11 mbms2012 attribute, an
      // integer percent ⇒ ratio = 1 + percent/100). Smaller files may
      // pack up to 10 symbols per packet but are less vulnerable to
      // loss to begin with.
      float surplus_packet_ratio = 1.15f;

      // FEC scheme picked at construction. Maps to bitstem::fec::Scheme
      // for Encoder / Decoder Create calls. Default kR10 keeps the
      // pre-multi-scheme call shape for the receive-side empty ctor.
      bitstem::fec::Scheme _bitstem_scheme = bitstem::fec::Scheme::kR10;
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
      explicit RaptorFEC(LibFlute::FecScheme scheme);
      RaptorFEC() : RaptorFEC(LibFlute::FecScheme::Raptor) {}

      ~RaptorFEC() override;

      bool check_source_block_completion(SourceBlock& srcblk) override;

      std::vector<SourceBlock> create_blocks(char *buffer, int *bytes_read) override;

      bool process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symb, unsigned int id) override;

      bool try_decode_pending(std::vector<LibFlute::SourceBlock>& blocks) override;

      void prepare_for_emit(LibFlute::SourceBlock& srcblk) override;

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
