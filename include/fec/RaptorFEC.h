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

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "fec/FecTransformer.h"
#include "flute_types.h"

#include "bitstem/r10/r10.hpp"

namespace tinyxml2 { class XMLElement; }

namespace LibFlute {
  // Glue between LibFlute's FEC abstraction and the bitstem::r10::fast
  // Encoder / Decoder. One r10::fast::Decoder is constructed per source
  // block on first received symbol and reused across all subsequent
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
        std::optional<bitstem::r10::fast::Decoder> dec;
        std::uint16_t K = 0;            // source-symbol count for THIS block
        std::uint32_t block_size = 0;   // bytes -- usually K*T, smaller for last block
        bool decoded = false;           // cached IsDecoded() so we don't re-call TryDecode
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
      // SBN currently materialised in _enc_scratch, or -1 if scratch
      // is empty. Used as a no-op guard if prepare_for_emit() is
      // re-entered for the same block (shouldn't happen with the
      // forward-only emit cursor, but cheap insurance).
      int _enc_scratch_sbn = -1;
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

      // 15% repair-symbol overhead; protects against ~15% packet loss.
      // (Smaller files may pack up to 10 symbols per packet but are
      // less vulnerable to loss to begin with.)
      const float surplus_packet_ratio = 1.15f;

    public:

      RaptorFEC(unsigned int transfer_length, unsigned int max_payload);

      RaptorFEC() {};

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
      unsigned long W = 16*1024*1024; // sub-block size target -- 16 MB keeps N=1
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
