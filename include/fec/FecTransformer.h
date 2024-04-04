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

#include <stddef.h>
#include <stdint.h>
#include <map>
#include "tinyxml2.h"


#include <string>
namespace LibFlute {
  /**
   *  abstract class for FEC Object En/De-coding
   */
  class FecTransformer {

    public:

    virtual ~FecTransformer() = default;


    /**
     * @brief Attempt to decode a source block
     *
     * @param srcblk the source block that should be decoded
     * @return whether or not the decoding was successful
     */
    virtual bool check_source_block_completion(SourceBlock& srcblk) = 0;

    /**
     * @brief Encode a file into multiple source blocks
     *
     * @param buffer a pointer to the buffer containing the data
     * @param bytes_read a pointer to an integer to store the number of bytes read out of buffer
     * @return a map of source blocks that the object has been encoded to
     */
    virtual std::map<uint16_t, SourceBlock> create_blocks(char *buffer, int *bytes_read) = 0;

    /**
     * @brief Process a received symbol
     *
     * @param srcblk the source block this symbols corresponds to
     * @param symb the received symbol
     * @param id the symbols id
     * @return success or failure
     */
    virtual bool process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symb, unsigned int id) = 0;

    virtual bool calculate_partitioning() = 0;

    /**
     * @brief Attempt to parse relevent information for decoding from the FDT
     *
     * @return success status
     */
    virtual bool parse_fdt_info(tinyxml2::XMLElement *file) = 0;

    /**
     * @brief Add relevant information about the FEC Scheme which the decoder may need, to the FDT
     * 
     * @return success status
     */
    virtual bool add_fdt_info(tinyxml2::XMLElement *file) = 0;

    /**
     * @brief Allocate the size of the buffer needed for this encoding scheme (since it may be larger)
     * 
     * @param min_length this should be the size of the file (transfer length). This determines the minimum size of the returned buffer
     * @return 0 on failure, otherwise return a pointer to the buffer
     */
    virtual void *allocate_file_buffer(int min_length) = 0;

      /**
       * @brief Called after the file is marked as complete, to finish extraction/decoding (if necessary)
       *
       * @param blocks the source blocks of the file, stored in the File object
       */
    virtual bool extract_file(std::map<uint16_t, SourceBlock> blocks) = 0;

    uint32_t nof_source_symbols = 0;
    uint32_t nof_source_blocks = 0;
    uint32_t large_source_block_length = 0;
    uint32_t small_source_block_length = 0;
    uint32_t nof_large_source_blocks = 0;
    
  };
};
