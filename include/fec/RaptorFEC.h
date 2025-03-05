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
#pragma once

#include <cstdint>              // for uint16_t, uint32_t
#include <map>                   // for map
#include "fec/FecTransformer.h"  // for FecTransformer
#include "flute_types.h"         // for SourceBlock, Symbol
namespace tinyxml2 { class XMLElement; }

namespace LibFlute {
  class RaptorFEC : public FecTransformer {

    private:

      bool is_encoder = true;

      unsigned int target_K(int blockno);

      Symbol translate_symbol(struct enc_context *encoder_ctx);

      LibFlute::SourceBlock create_block(char *buffer, int *bytes_read, int blockid);

      const float surplus_packet_ratio = 1.15; // adds 15% transmission overhead in exchange for protection against up to 15% packet loss. Assuming 1 symbol per packet, for smaller files packets may contain up to 10 symbols per packet but small files are much less vulnerable to packet loss anyways

      void extract_finished_block(LibFlute::SourceBlock& srcblk, struct dec_context *dc);

    public: 

      RaptorFEC(unsigned int transfer_length, unsigned int max_payload);

      RaptorFEC() {};

      ~RaptorFEC();

      bool check_source_block_completion(SourceBlock& srcblk);

      std::map<uint16_t, SourceBlock> create_blocks(char *buffer, int *bytes_read);

      bool process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symb, unsigned int id);

      bool calculate_partitioning();

      bool parse_fdt_info(tinyxml2::XMLElement *file);

      bool add_fdt_info(tinyxml2::XMLElement *file);

      void *allocate_file_buffer(int min_length);

      bool extract_file(std::map<uint16_t, SourceBlock> blocks);

      std::map<uint16_t, struct dec_context* > decoders; // map of source block number to decoders

      uint32_t nof_source_symbols = 0;
      uint32_t nof_source_blocks = 0;
      uint32_t large_source_block_length = 0;
      uint32_t small_source_block_length = 0;
      uint32_t nof_large_source_blocks = 0;

      unsigned int F; // object size in bytes
      unsigned int Al = 4; // symbol alignment: 4
      unsigned int T; // symbol size in bytes
      unsigned long W = 16*1024*1024; // target on sub block size- set default to 16 MB, to keep the number of sub-blocks, N, = 1 (you probably only need W >= 11MB to achieve this, assuming an ethernet mtu of ~1500 bytes, but we round to the nearest power of 2)
      unsigned int G; // number of symbols per packet
      unsigned int Z; // number of source blocks
      unsigned int N; // number of sub-blocks per source block
      unsigned int K; // number of symbols in a source block
      unsigned int Kt; // total number of symbols
      unsigned int P; // maximum payload size: e.g. 1436 for ipv4 over 802.3

  };
};
