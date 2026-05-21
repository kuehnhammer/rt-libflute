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

#include <atomic>
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

// Decoder operational counters. Snapshot returned by
// Decoder::stats(). Lifetime-cumulative; values monotonically
// non-decreasing. The library exposes the full counter set; consumers
// pick what they want to surface to operators.
struct DecoderStats {
  // Packets observed at feed_packet().
  std::uint64_t packets_received = 0;
  std::uint64_t bytes_received   = 0;

  // Per-packet drop reasons. packets_dropped_total() sums them.
  // (Expired-FDT rejection is NOT a packet drop — by the time the
  // expiry check fires the bytes have already been accumulated; see
  // fdts_rejected_expired below for that counter.)
  std::uint64_t dropped_wrong_tsi      = 0;
  std::uint64_t dropped_malformed      = 0;  // parser threw
  std::uint64_t dropped_truncated      = 0;  // header_length > packet bytes
  std::uint64_t dropped_stale_fdt      = 0;  // TOI=0 packet for older FDT
  std::uint64_t dropped_no_matching_file = 0;  // TOI != 0, no FDT entry yet
                                                // OR file already complete

  std::uint64_t packets_dropped_total() const {
    return dropped_wrong_tsi + dropped_malformed + dropped_truncated +
           dropped_stale_fdt + dropped_no_matching_file;
  }

  // Encoding-symbol breakdown. For CompactNoCode every symbol is a
  // source symbol. For Raptor, ESI < K (file's max_source_block_length)
  // is "source", ESI >= K is "repair".
  std::uint64_t source_symbols_received = 0;
  std::uint64_t repair_symbols_received = 0;

  // File-level counters.
  std::uint64_t files_completed       = 0;
  // Incomplete files evicted via remove_expired_files() or
  // remove_file_with_content_location() — i.e. reception was started
  // (the FDT announced this TOI) but the receiver never accumulated
  // enough symbols to finish, even with FEC repair if applicable.
  std::uint64_t files_discarded_incomplete = 0;
  // Sum of declared transfer_length over all files counted in
  // files_discarded_incomplete. Useful for "what % of announced
  // payload couldn't be recovered" dashboards.
  std::uint64_t bytes_discarded_incomplete = 0;

  // Files that completed structurally (every source symbol arrived,
  // FEC decode succeeded if applicable) but whose assembled bytes did
  // NOT match the Content-MD5 attribute the FDT advertised. RFC 6726
  // §3.4.2 treats Content-MD5 as a decoded-object integrity service;
  // a mismatch is surfaced to the operator instead of the application
  // (the completion callback is suppressed for failed files).
  std::uint64_t md5sum_fail            = 0;

  std::uint64_t fdts_accepted          = 0;
  std::uint64_t fdts_rejected_expired  = 0;
  std::uint64_t fdts_rejected_stale    = 0;
};

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

  /// @param tsi  Transport Stream Identifier for this session.
  /// @param fec_dec_worker_threads  Worker count for the parallel
  ///        TryDecode pool used by Raptor / RaptorQ files at end-of-
  ///        transmission. 0 ⇒ sequential matrix-solve (today's
  ///        behaviour). Non-zero ⇒ up to N threads run TryDecode
  ///        concurrently across distinct SBNs of one file. Useful at
  ///        Z ≥ 2 with distributed loss (multiple SBNs need matrix-
  ///        solve, e.g. burst-fade reception); no benefit when loss
  ///        concentrates in one SBN. Each per-SBN Decoder owns its
  ///        own scratch (~K·T-class) so workers don't contend.
  explicit Decoder(std::uint64_t tsi, unsigned fec_dec_worker_threads = 0);
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

  /// Snapshot of the decoder's operational counters. Lock-free; the
  /// returned value is decoupled from internal state.
  DecoderStats stats() const;

  /// Force an end-of-transmission decode pass on every in-flight
  /// File. Each File's FEC transformer (Raptor / RaptorQ) runs
  /// try_decode_pending() on its accumulated receive state — the
  /// matrix-solve path that recovers files with ≥1 lost source
  /// symbol. Files that already auto-finalised (lossless reception)
  /// are skipped; a single call drains every recoverable pending
  /// file. Completion callbacks fire for files that decode
  /// successfully.
  ///
  /// Use case: the carousel-driven sender has stopped emitting
  /// FDTs (file enqueue / completion are the only triggers
  /// libflute uses for FDT emit, and empty FDTs are intentionally
  /// suppressed to defend against commercial-middleware crashes
  /// on zero-file FDT-Instance documents). Without an FDT diff
  /// signalling "TOI no longer in flight", the receiver's
  /// abandoned-TOI logic doesn't fire — call this method when the
  /// pump knows the session has ended (end-of-stream timeout, LCT
  /// Close-Session, application-level signal). Idempotent;
  /// returns the number of files completed by this call.
  std::size_t flush_pending_decodes();

 private:
  std::mutex          _files_mutex;
  std::uint64_t       _tsi;
  unsigned            _fec_dec_worker_threads;
  NowProvider         _now             = ntp_seconds_now;
  std::unique_ptr<FileDeliveryTable> _fdt;
  std::map<std::uint64_t, std::shared_ptr<File>> _files;
  CompletionCallback _completion_cb;

  // Lock-free counters. See DecoderStats for semantics.
  struct AtomicStats {
    std::atomic<std::uint64_t> packets_received{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> dropped_wrong_tsi{0};
    std::atomic<std::uint64_t> dropped_malformed{0};
    std::atomic<std::uint64_t> dropped_truncated{0};
    std::atomic<std::uint64_t> dropped_stale_fdt{0};
    std::atomic<std::uint64_t> dropped_no_matching_file{0};
    std::atomic<std::uint64_t> source_symbols_received{0};
    std::atomic<std::uint64_t> repair_symbols_received{0};
    std::atomic<std::uint64_t> files_completed{0};
    std::atomic<std::uint64_t> files_discarded_incomplete{0};
    std::atomic<std::uint64_t> bytes_discarded_incomplete{0};
    std::atomic<std::uint64_t> md5sum_fail{0};
    std::atomic<std::uint64_t> fdts_accepted{0};
    std::atomic<std::uint64_t> fdts_rejected_expired{0};
    std::atomic<std::uint64_t> fdts_rejected_stale{0};
  } _stats;
};

}  // namespace LibFlute
