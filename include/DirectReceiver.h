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

#include <cstddef>
#include <cstdint>
#include <span>

#include "ReceiverBase.h"

namespace LibFlute {

// FLUTE receiver that consumes ALC packet payloads handed in directly by
// the caller, with no socket / io_service / kernel network stack
// involved. Use this when the transport is something other than UDP —
// e.g. ALC packets reassembled from RLC SDUs in a 5G Broadcast pipeline.
//
// Lifecycle: construct with the session TSI, register a completion
// callback, then call feed_packet() for each received ALC payload.
//
// Thread safety: the same as ReceiverBase. feed_packet may be called
// from any thread; serialization on the file map is internal.
class DirectReceiver : public ReceiverBase {
 public:
  explicit DirectReceiver(uint64_t tsi) : ReceiverBase(tsi) {}

  // Feed one ALC packet (post-UDP, including LCT/ALC headers) into the
  // receiver. Malformed packets are logged and dropped; this never
  // throws.
  void feed_packet(std::span<const std::uint8_t> alc_payload) {
    // ReceiverBase::handle_received_packet only reads from the buffer
    // (the parsers never mutate input bytes), so the const_cast is
    // safe. The signature stays char*-non-const for back-compat with
    // the existing socket/pcap derivatives that hand in their own
    // mutable receive buffer.
    handle_received_packet(
        const_cast<char*>(reinterpret_cast<const char*>(alc_payload.data())),
        alc_payload.size());
  }

  void stop() override {}
};

}  // namespace LibFlute
