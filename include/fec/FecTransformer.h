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
#include <vector>
#include "tinyxml2.h"
#include "flute_types.h"


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
     * @return a vector of source blocks indexed by SBN. SBN is dense
     *         (0..Z-1) so a vector is the natural fit.
     */
    virtual std::vector<SourceBlock> create_blocks(char *buffer, int *bytes_read) = 0;

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
    virtual bool extract_file(std::vector<SourceBlock>& blocks) = 0;

    /**
     * @brief Trigger decode for every source block that has accumulated
     *        enough received symbols. Called once when the receiver
     *        decides the file's transmission has ended (e.g. the FDT
     *        no longer lists this file's TOI). Schemes that don't
     *        require a decode pass — CompactNoCode in particular —
     *        leave this as a no-op; the per-symbol path already
     *        flagged the source blocks complete as data arrived.
     *
     * @return true if at least one previously-undecoded block was
     *         decoded by this call.
     */
    virtual bool try_decode_pending(std::vector<SourceBlock>& /*blocks*/) {
      return false;
    }

    /**
     * @brief Encoder-side hook: prepare a source block's Symbols for
     *        emission. Called by File::get_next_symbols when the
     *        cursor first enters a block (Symbol[0].data == nullptr
     *        as the placeholder signal). Schemes that fill all
     *        scratch eagerly in create_blocks override this as a
     *        no-op (or leave the default).
     *
     *        For Raptor: lazily materialises the block into the FEC's
     *        single shared scratch buffer (one fill at a time, reused
     *        across blocks — peak memory is O(K_max × T) rather than
     *        O(Z × K × T)) and runs EncodeSymbol for the repair ESIs.
     */
    virtual void prepare_for_emit(SourceBlock& /*srcblk*/) {}

    uint32_t nof_source_symbols = 0;
    uint32_t nof_source_blocks = 0;
    uint32_t large_source_block_length = 0;
    uint32_t small_source_block_length = 0;
    uint32_t nof_large_source_blocks = 0;
    
  };
};
