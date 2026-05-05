// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License"). You may not use this file
// except in compliance with the License. You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.
//
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "FileDeliveryTable.h"
#include "flute_types.h"

namespace LibFlute {

class File;

// FLUTE/ALC transmit-side encoder.
//
// The encoder builds an ALC packet stream from objects queued via
// send(). The CALLER is responsible for transport — each generated
// packet is handed back via the PacketCallback registered at
// construction. There are no sockets, no event loop, and no internal
// threads: pump packets out by calling send_next_packet() (or flush())
// from whatever loop already drives your application.
//
// A typical application either:
//   - calls flush() until it returns 0, then sleeps until
//     next_send_due() (rate-limited path), or
//   - polls send_next_packet() in a tight loop until done (when
//     rate_limit_kbps == 0).
//
// Thread-safety: send() and the pump methods are mutex-guarded; it is
// safe to enqueue from a different thread than the one driving the
// pump. PacketCallback is invoked from whatever thread calls the pump.
class Encoder {
 public:
  using PacketCallback    = std::function<bool(std::span<const std::uint8_t>)>;
  using CompletionCallback = std::function<void(std::uint32_t toi)>;
  using Clock              = std::chrono::steady_clock;

  /**
   * @param tsi              Transport Stream Identifier for this session.
   * @param mtu              Path MTU; the encoder sizes ALC packets so
   *                         the IP+UDP+LCT+SBN overhead fits within mtu.
   * @param rate_limit_kbps  Transmit rate cap in kilobits/sec; 0 disables
   *                         pacing (caller is responsible for not
   *                         saturating downstream transports).
   * @param packet_cb        Invoked with each generated packet's bytes.
   *                         Returns true on successful dispatch; the
   *                         encoder uses this signal to mark the
   *                         underlying symbols as transmitted.
   */
  Encoder(std::uint64_t tsi, unsigned mtu, std::uint32_t rate_limit_kbps,
          PacketCallback packet_cb);
  ~Encoder();

  Encoder(const Encoder&)            = delete;
  Encoder& operator=(const Encoder&) = delete;

  /**
   * Queue an object for transmission. Returns its assigned TOI
   * (1..0xFFFF, never 0 which is reserved for the FDT) or 0 on error.
   *
   * Buffer ownership: by default the encoder takes a non-owning
   * pointer to `data` and the CALLER must keep the buffer alive until
   * the completion callback fires. Pass copy_buffer=true to have the
   * encoder copy it.
   */
  std::uint16_t send(std::string content_location,
                      std::string content_type,
                      std::uint64_t expires_ntp_seconds,
                      char* data, std::size_t length,
                      FecScheme fec_scheme = FecScheme::CompactNoCode,
                      bool copy_buffer = false);

  /**
   * Generate at most one ALC packet and dispatch it via packet_cb.
   * Returns true iff a packet was emitted. Returns false if there is
   * nothing to send OR if rate limiting defers the next packet (use
   * next_send_due() to find out which).
   */
  bool send_next_packet();

  /**
   * Pump packets back-to-back until either send_next_packet() returns
   * false or the supplied limit is reached. Returns the number of
   * packets actually sent.
   *
   * @param max_packets  Soft cap on pumped packets. Default = unlimited.
   */
  std::size_t flush(std::size_t max_packets =
                        std::numeric_limits<std::size_t>::max());

  /**
   * Time at which the next packet is due. nullopt means the encoder
   * is idle (no queued bytes). A time in the past means a packet is
   * ready right now (call send_next_packet()).
   */
  std::optional<Clock::time_point> next_send_due() const;

  /// True iff at least one queued File still has symbols to transmit.
  bool has_pending() const;

  void register_completion_callback(CompletionCallback cb);

  /// Stop accepting new packets via send_next_packet(). send() still
  /// queues but nothing will be emitted until the encoder is destroyed
  /// or a fresh one is constructed.
  void stop();

  /// NTP-epoch second count (RFC 5905). Useful for filling in
  /// FDT/Cache-Control Expires fields.
  static std::uint64_t seconds_since_epoch();

 private:
  void queue_fdt_locked();
  void file_transmitted_locked(std::uint32_t toi);

  mutable std::mutex _mutex;
  PacketCallback     _packet_cb;
  CompletionCallback _completion_cb;

  std::uint64_t _tsi;
  std::uint32_t _max_payload;
  std::uint32_t _rate_limit_kbps;

  std::unique_ptr<FileDeliveryTable> _fdt;
  std::map<std::uint32_t, std::shared_ptr<File>> _files;

  std::uint16_t _next_toi = 1;
  FecOti        _fec_oti{};

  Clock::time_point _next_send_due = Clock::time_point::min();
  bool              _running       = true;
};

}  // namespace LibFlute
