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

#include <cstddef>       // for size_t
#include <cstdint>       // for uint8_t, uint32_t, uint64_t, uint16_t
#include <span>
#include <vector>         // for vector
#include "flute_types.h"  // for ContentEncoding, FecOti, ContentEncoding::NONE
namespace LibFlute { class EncodingSymbol; }

namespace LibFlute {
  /**
   *  A class for parsing and creating ALC packets
   */
  class AlcPacket {
    public:
     /**
      *  Create an ALC packet from payload data
      *
      *  @param data Received data to be parsed
      *  @param len Length of the buffer
      */
      AlcPacket(char* data, size_t len);

     /**
      *  Create an ALC packet from encoding symbols, writing the wire
      *  bytes into the caller-provided buffer. AlcPacket does NOT
      *  take ownership of `out_buffer`; the caller (typically the
      *  Encoder) holds a reusable scratch buffer across packets to
      *  avoid the per-packet calloc/free overhead the previous
      *  internally-allocating constructor incurred.
      *
      *  After return, `wire_size()` reports how many bytes of
      *  `out_buffer` carry the packet.
      *
      *  @param tsi Transport Stream Identifier
      *  @param toi Transport Object Identifier
      *  @param fec_oti OTI values
      *  @param symbols Vector of encoding symbols
      *  @param max_size Maximum encoding-symbol payload size
      *  @param fdt_instance_id FDT instance ID (only relevant for FDT with TOI=0)
      *  @param out_buffer Caller-owned span; size MUST be ≥ the worst-
      *                    case packet length for these parameters
      *                    (typically the path MTU).
      */
      AlcPacket(uint16_t tsi, uint16_t toi, FecOti fec_oti,
                 const std::vector<EncodingSymbol>& symbols,
                 size_t max_size, uint32_t fdt_instance_id,
                 std::span<uint8_t> out_buffer);

     /**
      *  Default destructor.
      */
      ~AlcPacket() = default;

     /**
      *  Get the TSI
      */
      uint64_t tsi() const { return _tsi; };

     /**
      *  Get the TOI
      */
      uint64_t toi() const { return _toi; };

     /**
      *  Get the FEC OTI values
      */
      const FecOti& fec_oti() const { return _fec_oti; };

     /**
      *  Get the LCT header length
      */
      size_t header_length() const  { return _lct_header.lct_header_len * 4; };

     /**
      *  Get the FDT instance ID 
      */
      uint32_t fdt_instance_id() const { return _fdt_instance_id; };

     /**
      *  Get the FEC scheme
      */
      FecScheme fec_scheme() const { return _fec_oti.encoding_id; };

     /**
      *  Get the content encoding
      */
      ContentEncoding content_encoding() const { return _content_encoding; };

     /**
      *  Number of wire bytes written into the producing constructor's
      *  out_buffer. The packet is the [out_buffer.data(),
      *  out_buffer.data() + wire_size()) range.
      */
      size_t wire_size() const { return _len; };

    private:
      uint64_t _tsi = 0;
      uint64_t _toi = 0;

      uint32_t _fdt_instance_id = 0;

      ContentEncoding _content_encoding = ContentEncoding::NONE;
      FecOti _fec_oti = {};

      // Bytes written by the producing constructor into the caller's
      // buffer. Unused by the parsing constructor.
      size_t _len = 0;

      // RFC5651 5.1 - LCT Header Format
      //
      // The bitfield layout depends on both the compiler ABI (MSVC vs.
      // GCC/Clang allocate bitfields differently) and the target's byte
      // ordering. GCC/Clang expose __BYTE_ORDER__; MSVC does not, but
      // every Windows target this build supports (x64, ARM64) is
      // little-endian, so an MSVC-on-LE assumption is safe. Wire-format
      // compatibility on MSVC is asserted by the static_assert below;
      // FLUTE delivery is not exercised on Windows in the current port,
      // but the layout is kept consistent so a future Windows OTA build
      // doesn't have to revisit this.
#if defined(_MSC_VER)
#  define LIBFLUTE_LCT_LITTLE_ENDIAN 1
#  pragma pack(push, 1)
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define LIBFLUTE_LCT_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define LIBFLUTE_LCT_LITTLE_ENDIAN 0
#else
#  error "Endianness can not be determined"
#endif

#if defined(_MSC_VER)
      struct lct_header_t {
#else
      struct __attribute__((packed)) lct_header_t {
#endif
#if LIBFLUTE_LCT_LITTLE_ENDIAN
        uint8_t res1:1;
        uint8_t source_packet_indicator:1;
        uint8_t congestion_control_flag:2;
        uint8_t version:4;

        uint8_t close_object_flag:1;
        uint8_t close_session_flag:1;
        uint8_t res:2;
        uint8_t half_word_flag:1;
        uint8_t toi_flag:2;
        uint8_t tsi_flag:1;
#else
        uint8_t version:4;
        uint8_t congestion_control_flag:2;
        uint8_t source_packet_indicator:1;
        uint8_t res1:1;

        uint8_t tsi_flag:1;
        uint8_t toi_flag:2;
        uint8_t half_word_flag:1;
        uint8_t res2:2;
        uint8_t close_session_flag:1;
        uint8_t close_object_flag:1;
#endif
        uint8_t lct_header_len;
        uint8_t codepoint;
      } _lct_header;
#if defined(_MSC_VER)
#  pragma pack(pop)
#endif
#undef LIBFLUTE_LCT_LITTLE_ENDIAN
      static_assert(sizeof(_lct_header) == 4);

      enum HeaderExtension { 
        EXT_NOP  =   0,
        EXT_AUTH =   1,
        EXT_TIME =   2,
        EXT_FTI  =  64,
        EXT_FDT  = 192,
        EXT_CENC = 193
      };

  };
};

