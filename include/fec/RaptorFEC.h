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
      };

      // Per-source-block decoder state; survives across process_symbol()
      // calls for the same block.
      std::map<std::uint16_t, DecoderCtx> _dec_ctxs;

      // Helpers.
      DecoderCtx& ensure_dec_ctx(std::uint16_t sbn);
      LibFlute::SourceBlock create_block(char *buffer, int *bytes_read, int blockid);
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

      std::map<uint32_t, SourceBlock> create_blocks(char *buffer, int *bytes_read) override;

      bool process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symb, unsigned int id) override;

      bool calculate_partitioning() override;

      bool parse_fdt_info(tinyxml2::XMLElement *file) override;

      bool add_fdt_info(tinyxml2::XMLElement *file) override;

      void *allocate_file_buffer(int min_length) override;

      bool extract_file(std::map<uint32_t, SourceBlock> blocks) override;

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
      unsigned int K;          // symbols in a (regular) source block
      unsigned int Kt;         // total symbols across all blocks
      unsigned int P;          // max payload size
  };
};
