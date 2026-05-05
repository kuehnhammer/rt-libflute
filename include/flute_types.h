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
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "tinyxml2.h"

#pragma once
/** \mainpage LibFlute - ALC/FLUTE library
 *
 * libflute is a transport-agnostic FLUTE/ALC library. The library does
 * not open sockets, run an event loop, or spawn threads; the caller
 * drives both sides:
 *  - LibFlute::Encoder (in include/Encoder.h) — TX side. Generates
 *    ALC packet bytes via a PacketCallback the consumer registers;
 *    the consumer is responsible for actually transmitting them
 *    (sendto on a UDP socket, RLC SDU dispatch, …).
 *  - LibFlute::Decoder (in include/Decoder.h) — RX side. Accepts
 *    ALC packet payloads via feed_packet(); the consumer is
 *    responsible for receiving and reassembling them.
 *
 * The example apps in examples/ demonstrate plain POSIX-socket
 * transports on top of these classes.
 */

#include <string>
namespace LibFlute {
  /**
   *  Content Encodings
   */
  enum class ContentEncoding {
    NONE,
    ZLIB,
    DEFLATE,
    GZIP
  };

  /**
   *  Error correction schemes. From the registry for FEC schemes http://www.iana.org/assignments/rmt-fec-parameters (RFC 5052)
   */
  enum class FecScheme {
    CompactNoCode,
    Raptor,
    Reed_Solomon_GF_2_m,
    LDPC_Staircase_Codes,
    LDPC_Triangle_Codes,
    Reed_Solomon_GF_2_8,
    RaptorQ
  };

  struct Symbol {
    char* data = nullptr;
    size_t length = 0;
    bool complete = false;
    bool queued = false;
  };

  // SourceBlock holds the symbols for one RFC 5052 source block.
  // ESIs run densely from 0..symbols.size()-1, so a vector indexed by
  // ESI is the natural fit — std::map adds per-symbol heap allocation
  // and O(log K) lookup with no upside (the keys are dense integers).
  // For Raptor, slots [0, K) hold source symbols and [K, target_K)
  // hold repair symbols; for CompactNoCode there are no repair slots.
  // emit_cursor is a per-block hint used by File::get_next_symbols to
  // avoid O(K²) scans when emitting all symbols of a block one packet
  // at a time.
  //
  // The Raptor encoder owns its per-block symbol-data scratch inside
  // RaptorFEC itself (so SourceBlock stays cache-friendly for
  // CompactNoCode files which can have thousands of blocks).
  // Symbol::data references either (a) the user file buffer
  // (CompactNoCode encoder + receiver side, regardless of FEC), or
  // (b) RaptorFEC's per-block scratch (Raptor encoder side).
  struct SourceBlock {
    uint32_t id = 0;
    bool complete = false;
    std::vector<Symbol> symbols;
    uint32_t emit_cursor = 0;
  };

  struct FecOti {
    FecScheme encoding_id;
    uint64_t transfer_length;
    uint32_t encoding_symbol_length;
    uint32_t max_source_block_length;
    std::string scheme_specific_info;
  };

};
