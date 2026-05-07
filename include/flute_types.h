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
#include <cstdint>
#include <optional>
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
  // completed_symbol_count is maintained by File::put_symbol /
  // File::mark_completed: incremented once when a Symbol.complete
  // bit transitions false→true. The FEC layer uses it to answer
  // "is this block done?" in O(1) instead of std::all_of-ing the
  // symbols vector after every packet (which was O(K²) per block
  // on the encoder side at K=9200).
  struct SourceBlock {
    uint32_t id = 0;
    bool complete = false;
    std::vector<Symbol> symbols;
    uint32_t emit_cursor = 0;
    uint32_t completed_symbol_count = 0;
  };

  // FEC Object Transmission Information. Mirrors the FEC-OTI-* attribute
  // set TS 26.346 §7.2.10.1 mandates on every FDT-Instance / File. Any
  // field left at its sentinel value (0 / empty) is treated as "absent
  // in the FDT" — the serialiser omits the attribute, and the FEC
  // transformer fills in a scheme default and writes the resolved value
  // back into this struct so the FDT round-trip captures it.
  struct FecOti {
    FecScheme encoding_id;
    uint64_t transfer_length;
    uint32_t encoding_symbol_length;
    uint32_t max_source_block_length;
    std::string scheme_specific_info;
    // FEC-OTI-FEC-Instance-ID. Per RFC 5052 §4, only meaningful for
    // Under-Specified FEC schemes (Encoding-ID 128–255). The schemes
    // libflute itself emits — CompactNoCode (0), R10 (1), RaptorQ (6),
    // RS GF(2^8) (5) — are all Fully-Specified and leave this at 0;
    // the field exists for FDT round-trip fidelity on the receive
    // side and forward-compatibility with under-specified schemes.
    uint8_t instance_id = 0;
    // FEC-OTI-Max-Number-of-Encoding-Symbols. Upper bound on the ESI
    // a receiver may see for any source block (= K + max repair).
    // Sentinel 0 means "let the FEC transformer derive it from the
    // redundancy level" and the FDT serialiser omits the attribute.
    uint32_t max_number_of_encoding_symbols = 0;
  };

  // Per-file transmission config the caller hands to Encoder::send().
  // Bundles the FEC-OTI-* fields (TS 26.346 §7.2.10.1) with the Rel-11
  // FEC-Redundancy-Level attribute that lives next to FEC OTI in the
  // FDT but is not itself part of FEC OTI. Implicitly constructible
  // from FecScheme so call sites that only need to pick a scheme can
  // still write `encoder.send(..., FecScheme::Raptor)` unchanged.
  //
  // Caller-supplied fields override the encoder's FDT-Instance defaults
  // on a per-file basis (matching MBMS reader semantics for absent
  // attributes). Sentinel values (0 / empty / nullopt) mean "inherit
  // from the FDT-Instance default, then let the FEC transformer fill
  // in what's still missing".
  struct FileTransmissionConfig {
    FecOti oti{};
    // FEC-Redundancy-Level (Rel-11 mbms2012 attribute, xs:unsignedInt).
    // Per-file repair overhead expressed as an integer percent: 15 ⇒
    // K * 1.15 total symbols on the wire. nullopt ⇒ scheme default
    // (Raptor / RaptorQ pick their own; CompactNoCode ignores).
    std::optional<unsigned> fec_redundancy_level;
    // Sub-block size target W (RFC 5053 §4.2 / RFC 6330 §4.3 input).
    // Drives the autodetect for N (the number of sub-blocks per source
    // block). Sentinel 0 ⇒ use the scheme-internal default (currently
    // 16 MB across schemes — keeps N=1 and backward-compatible until
    // the codec ships sub-block interleaving). For TS 26.346-conformant
    // R10 file delivery the spec-mandated value is 256 KB (§B.3.4.1);
    // RaptorQ leaves it as a deployment knob (RFC 6330 §4.3).
    // Caller-driven so xMB / SDP-supplied derivation inputs reach
    // RaptorFEC's partitioning verbatim.
    std::uint64_t sub_block_size_target = 0;

    FileTransmissionConfig() = default;

    // NOLINTNEXTLINE(google-explicit-constructor) — implicit conversion
    // from FecScheme is the migration path for existing call sites.
    FileTransmissionConfig(FecScheme scheme) {
      oti.encoding_id = scheme;
    }
  };


};
