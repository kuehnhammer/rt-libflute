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
  // Mirrors the SDP FEC parametrisation surface TS 26.346 actually
  // exposes for download delivery: §7.3.2.8 (Encoding-ID + optional
  // Instance-ID via `a=FEC-declaration:`) and §7.3.2.11 (per-file
  // `a=FEC-redundancy-level:` percent). The remaining FEC OTI fields
  // (T, K, Z, N, Al, max-encoding-symbols) are libflute's job — they
  // flow into the FDT XML at runtime per the partitioner, and the
  // caller has no way to express them in xMB/SDP anyway.
  //
  // `sub_block_size_target` is the only field with no SDP backing —
  // it's a deployment-time knob (TS 26.346 §B.3.4.1 normative 256 KB
  // for R10, deployment-tuned for RaptorQ per RFC 6330 §4.3) — and
  // sits here because it's a per-file partitioner input.
  //
  // Implicitly constructible from FecScheme so call sites that only
  // need to pick a scheme can still write
  // `encoder.send(..., FecScheme::Raptor)` unchanged.
  struct FileTransmissionConfig {
    // SDP §7.3.2.8 Encoding-ID. Maps to the wire-level FEC scheme;
    // identical to the value emitted in the FDT FEC-OTI-FEC-Encoding-ID
    // attribute.
    FecScheme scheme = FecScheme::CompactNoCode;

    // SDP §7.3.2.8 instance-id. Only meaningful for under-specified
    // FEC schemes (RFC 5052 §4); fully-specified schemes
    // (CompactNoCode, R10, RaptorQ, RS GF(2^8)) leave this at 0.
    std::uint8_t fec_instance_id = 0;

    // SDP §7.3.2.11 redundancy-level. Per-file repair overhead as an
    // integer percent: 15 ⇒ K * 1.15 total symbols on the wire.
    // nullopt ⇒ scheme default. Round-trips into the FDT's Rel-11
    // mbms2012:FEC-Redundancy-Level attribute.
    std::optional<unsigned> fec_redundancy_level;

    // RFC 5053 §4.2 W / RFC 6330 §4.3 WS. Sub-block / working-memory
    // size target. Sentinel 0 ⇒ use the scheme default (16 MB).
    // TS 26.346 §B.3.4.1 mandates 256 KB for R10 file delivery;
    // RaptorQ leaves it as a deployment knob. Not on the SDP wire —
    // the integrator picks this per their build/deployment config.
    std::uint64_t sub_block_size_target = 0;

    // RFC 6330 §4.3 SS — desired lower bound on sub-symbol size,
    // expressed as a multiplier on Al. The §4.3 algorithm picks
    // N ≤ T/(SS·Al), so SS=1 (default) leaves the algorithm free to
    // pick the largest N the partition allows; SS>1 forces fewer,
    // larger sub-symbols. RaptorQ-only knob; ignored for R10.
    std::uint8_t sub_symbol_size_min_multiplier = 1;

    FileTransmissionConfig() = default;

    // NOLINTNEXTLINE(google-explicit-constructor) — implicit conversion
    // from FecScheme keeps existing `encoder.send(..., FecScheme::X)`
    // call sites compiling.
    FileTransmissionConfig(FecScheme s) : scheme(s) {}
  };


};
