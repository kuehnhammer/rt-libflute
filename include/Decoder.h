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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "FileDeliveryTable.h"

namespace LibFlute {

class File;

/// Returns the current NTP-epoch second count (seconds since
/// 1900-01-01 00:00:00 UTC). Default clock used by Decoder for
/// FDT-Instance Expires checks. Tests inject their own clock via
/// Decoder::set_now_provider().
std::uint64_t ntp_seconds_now();

// FLUTE/ALC receive-side decoder.
//
// The decoder accepts already-extracted ALC packet payloads via
// feed_packet(); the CALLER is responsible for transport (recvfrom on
// a UDP socket, RLC SDU reassembly out of the broadcast pipeline,
// pcap replay, …). There are no sockets, no event loop, and no
// internal threads.
//
// Lifecycle:
//   1. Construct with the joined session's TSI.
//   2. Register a completion callback (optional but typical).
//   3. Call feed_packet() for every incoming ALC payload.
//   4. The callback fires once per file when its source symbols have
//      all been received (FEC-repaired, if applicable).
//
// Thread-safety: feed_packet() and the inspection methods are
// mutex-guarded. The completion callback is invoked from the same
// thread that called feed_packet() at the moment of completion;
// re-entrant calls into the Decoder from inside the callback are
// safe (the lock is released before dispatch).
class Decoder {
 public:
  using CompletionCallback = std::function<void(std::shared_ptr<File>)>;
  using NowProvider        = std::function<std::uint64_t()>;

  explicit Decoder(std::uint64_t tsi);
  ~Decoder() = default;

  Decoder(const Decoder&)            = delete;
  Decoder& operator=(const Decoder&) = delete;

  /// Feed one ALC packet payload (post-UDP / post-RLC-reassembly,
  /// including LCT and ALC headers). Malformed packets are logged and
  /// dropped; this never throws.
  void feed_packet(std::span<const std::uint8_t> alc_payload);

  /// All Files currently being received or already complete-but-not-
  /// yet-evicted. Snapshot semantics: the returned vector is owned by
  /// the caller and decoupled from internal state.
  std::vector<std::shared_ptr<File>> file_list();

  /// Evict files older than max_age_seconds. The "bootstrap.multipart"
  /// content_location is preserved by convention (legacy 5G-MAG quirk).
  void remove_expired_files(unsigned max_age_seconds);

  /// Evict any file matching the given Content-Location.
  void remove_file_with_content_location(const std::string& content_location);

  /// Replace the completion callback. Safe to call concurrently with
  /// feed_packet(); the previous callback is not invoked again after
  /// this returns.
  void register_completion_callback(CompletionCallback cb);

  /// Replace the clock used to evaluate FDT-Instance Expires (RFC
  /// 6726 §3.3). Default is ntp_seconds_now(). Tests use this to
  /// deterministically exercise the expired-FDT branch.
  void set_now_provider(NowProvider fn);

 private:
  std::mutex          _files_mutex;
  std::uint64_t       _tsi;
  NowProvider         _now             = ntp_seconds_now;
  std::unique_ptr<FileDeliveryTable> _fdt;
  std::map<std::uint64_t, std::shared_ptr<File>> _files;
  CompletionCallback _completion_cb;
};

}  // namespace LibFlute
