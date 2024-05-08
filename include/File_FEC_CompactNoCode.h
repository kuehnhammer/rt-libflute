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
#pragma once

#include "File.h"

namespace LibFlute {
  /**
   *  Represents a file being transmitted or received using Compact-No Code FEC
   */
  class File_FEC_CompactNoCode : public File {
    public:
      File_FEC_CompactNoCode(LibFlute::FileDeliveryTable::FileEntry entry);
      File_FEC_CompactNoCode(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data);
      virtual ~File_FEC_CompactNoCode() = default;

      virtual void put_symbol(const EncodingSymbol& symbol) override;
      virtual std::vector<EncodingSymbol> get_next_symbols(size_t max_size) override;
      virtual void mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) override;
      virtual void reset() override;

    private:
      void calculate_partitioning();
      void create_blocks();

      struct SourceBlock {
        bool complete = false;
        struct Symbol {
          char* data;
          size_t length;
          bool complete = false;
          bool queued = false;
        };
        std::map<uint16_t, Symbol> symbols; 
      };
      std::map<uint16_t, SourceBlock> _source_blocks; 

      void check_source_block_completion(SourceBlock& block);
      void check_file_completion();

      uint32_t _nof_source_blocks = 0;
      uint32_t _nof_source_symbols = 0;
      uint32_t _nof_large_source_blocks = 0;
      uint32_t _large_source_block_length = 0;
      uint32_t _small_source_block_length = 0;
  };
};
