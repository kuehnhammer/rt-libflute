// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License"). You may not use this file
// except in compliance with the License. You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.
//
#include "Encoder.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "AlcPacket.h"
#include "EncodingSymbol.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "spdlog/spdlog.h"

namespace LibFlute {

namespace {

// IP+UDP+LCT-base+SBN/ESI overhead before the encoding-symbol payload.
//   IP v4 header           : 20 B
//   UDP header             :  8 B
//   LCT base + CCI + TSI/TOI half-word: 12 B
//   SBN(2) + ESI(2)        :  4 B
constexpr std::size_t kFixedAlcOverhead = 20 + 8 + 12 + 4;

// FDT-Instance "Expires" in NTP seconds, used when the encoder
// auto-generates the FDT XML for the receivers.
constexpr unsigned kFdtRepeatIntervalSeconds = 5;

}  // namespace

Encoder::Encoder(std::uint64_t tsi, unsigned mtu,
                  std::uint32_t rate_limit_kbps,
                  PacketCallback packet_cb)
    : _packet_cb(std::move(packet_cb)),
      _tsi(tsi),
      _max_payload(static_cast<std::uint32_t>(
          (mtu > kFixedAlcOverhead) ? (mtu - kFixedAlcOverhead) : 0)),
      _rate_limit_kbps(rate_limit_kbps),
      _packet_scratch(mtu) {
  constexpr std::uint32_t kDefaultMaxSourceBlockLength = 64;
  _fec_oti = FecOti{FecScheme::CompactNoCode, /*transfer_length*/ 0,
                     _max_payload, kDefaultMaxSourceBlockLength,
                     /*scheme_specific_info*/ ""};
  _fdt = std::make_unique<FileDeliveryTable>(/*instance_id*/ 1, _fec_oti);
}

Encoder::~Encoder() = default;

std::uint64_t Encoder::seconds_since_epoch() {
  // RFC 5905: NTP-epoch seconds = Unix time + 2'208'988'800.
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count()) +
         2'208'988'800ULL;
}

void Encoder::register_completion_callback(CompletionCallback cb) {
  const std::lock_guard<std::mutex> lock(_mutex);
  _completion_cb = std::move(cb);
}

void Encoder::stop() {
  const std::lock_guard<std::mutex> lock(_mutex);
  _running = false;
}

std::uint16_t Encoder::send(std::string content_location,
                              std::string content_type,
                              std::uint64_t expires_ntp_seconds,
                              char* data, std::size_t length,
                              FecScheme fec_scheme, bool copy_buffer) {
  const std::lock_guard<std::mutex> lock(_mutex);
  if (!data || length == 0) {
    return 0;
  }

  const std::uint16_t toi = _next_toi;
  _next_toi = static_cast<std::uint16_t>(_next_toi + 1);
  if (_next_toi == 0) {
    // 0 is reserved for the FDT (RFC 6726 §3.1); skip on wrap.
    _next_toi = 1;
  }

  FecOti file_oti = _fec_oti;
  file_oti.encoding_id = fec_scheme;

  std::shared_ptr<File> file;
  try {
    file = std::make_shared<File>(toi, file_oti, std::move(content_location),
                                   std::move(content_type),
                                   expires_ntp_seconds, data, length,
                                   copy_buffer);
  } catch (const std::exception& ex) {
    spdlog::error("Encoder::send: failed to create File: {}", ex.what());
    return 0;
  }

  _fdt->add(file->meta());
  _files.insert({toi, file});
  queue_fdt_locked();
  _stats.files_queued.fetch_add(1, std::memory_order_relaxed);
  return toi;
}

EncoderStats Encoder::stats() const {
  EncoderStats s;
  s.packets_emitted        = _stats.packets_emitted.load(std::memory_order_relaxed);
  s.packets_failed         = _stats.packets_failed.load(std::memory_order_relaxed);
  s.bytes_emitted          = _stats.bytes_emitted.load(std::memory_order_relaxed);
  s.source_symbols_emitted = _stats.source_symbols_emitted.load(std::memory_order_relaxed);
  s.repair_symbols_emitted = _stats.repair_symbols_emitted.load(std::memory_order_relaxed);
  s.files_queued           = _stats.files_queued.load(std::memory_order_relaxed);
  s.files_transmitted      = _stats.files_transmitted.load(std::memory_order_relaxed);
  s.fdt_packets_emitted    = _stats.fdt_packets_emitted.load(std::memory_order_relaxed);
  return s;
}

void Encoder::queue_fdt_locked() {
  // Refresh the FDT XML and queue it as the special TOI=0 File. Any
  // previous incomplete FDT is discarded — receivers handle in-flight
  // FDT supersession via the round-4 instance-ID monotonicity logic.
  _fdt->set_expires(seconds_since_epoch() +
                     kFdtRepeatIntervalSeconds * 2ULL);
  auto fdt_xml = _fdt->to_string();

  FecOti fdt_oti      = _fec_oti;
  fdt_oti.encoding_id = FecScheme::CompactNoCode;  // FDT is never FEC-encoded

  try {
    auto fdt_file = std::make_shared<File>(
        /*toi*/ 0, fdt_oti, std::string{}, std::string{},
        seconds_since_epoch() + kFdtRepeatIntervalSeconds * 2ULL,
        fdt_xml.data(), fdt_xml.size(),
        /*copy_data*/ true);
    fdt_file->set_fdt_instance_id(_fdt->instance_id());
    _files.insert_or_assign(0, std::move(fdt_file));
  } catch (const std::exception& ex) {
    spdlog::error("Encoder::queue_fdt: {}", ex.what());
  }
}

void Encoder::file_transmitted_locked(std::uint32_t toi) {
  if (toi == 0) {
    // FDT: nothing externally observable. The File for TOI=0 stays in
    // the map (complete) until the next queue_fdt_locked() replaces it.
    return;
  }
  _files.erase(toi);
  _fdt->remove(toi);
  queue_fdt_locked();
}

bool Encoder::has_pending() const {
  const std::lock_guard<std::mutex> lock(_mutex);
  for (const auto& [toi, file] : _files) {
    if (file && !file->complete()) {
      return true;
    }
  }
  return false;
}

std::optional<Encoder::Clock::time_point> Encoder::next_send_due() const {
  const std::lock_guard<std::mutex> lock(_mutex);
  if (!_running) return std::nullopt;
  for (const auto& [toi, file] : _files) {
    if (file && !file->complete()) {
      return _next_send_due;
    }
  }
  return std::nullopt;
}

bool Encoder::send_next_packet() {
  std::shared_ptr<File> completed_file;
  std::uint32_t completed_toi = 0;
  CompletionCallback dispatch_cb;

  {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (!_running) return false;
    if (Clock::now() < _next_send_due) return false;

    File* sending_file = nullptr;
    for (auto& [toi, file] : _files) {
      if (file && !file->complete()) {
        sending_file = file.get();
        break;
      }
    }
    if (sending_file == nullptr) return false;

    auto symbols = sending_file->get_next_symbols(_max_payload);
    if (symbols.empty()) {
      // Pathological: file isn't complete but no symbols left to queue
      // (every symbol is already queued or every block is somehow
      // complete-but-not-checked). Treat as nothing to send right now.
      return false;
    }

    AlcPacket packet(
        static_cast<std::uint16_t>(_tsi),
        static_cast<std::uint16_t>(sending_file->meta().toi),
        sending_file->meta().fec_oti, symbols, _max_payload,
        sending_file->fdt_instance_id(),
        std::span<std::uint8_t>(_packet_scratch.data(),
                                  _packet_scratch.size()));

    bool dispatched = false;
    if (_packet_cb) {
      dispatched = _packet_cb({_packet_scratch.data(), packet.wire_size()});
    }
    sending_file->mark_completed(symbols, dispatched);

    // Stats: count the packet, its bytes, the symbol breakdown, and
    // (if it was a TOI=0 FDT) bump the FDT-emission counter. Symbol
    // classification: for any file with a non-zero
    // max_source_block_length, ESI < K is "source", ESI >= K is
    // "repair". CompactNoCode never has repair (encoder targets
    // K source symbols only); the breakdown reduces to all-source.
    if (dispatched) {
      _stats.packets_emitted.fetch_add(1, std::memory_order_relaxed);
      _stats.bytes_emitted.fetch_add(packet.wire_size(),
                                       std::memory_order_relaxed);
      const std::uint32_t k_block =
          sending_file->meta().fec_oti.max_source_block_length;
      std::uint64_t src = 0, rep = 0;
      for (const auto& sym : symbols) {
        if (k_block > 0 && sym.id() >= k_block) ++rep;
        else ++src;
      }
      _stats.source_symbols_emitted.fetch_add(src, std::memory_order_relaxed);
      _stats.repair_symbols_emitted.fetch_add(rep, std::memory_order_relaxed);
      if (sending_file->meta().toi == 0) {
        _stats.fdt_packets_emitted.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      _stats.packets_failed.fetch_add(1, std::memory_order_relaxed);
    }

    // Rate limiting: schedule the next allowed send. The callback's
    // wall-time cost is paid out of this budget — i.e. if dispatch
    // took 1 ms, the next deadline is 1 ms closer.
    if (_rate_limit_kbps > 0 && dispatched) {
      const std::uint64_t bits = static_cast<std::uint64_t>(packet.wire_size()) * 8ULL;
      const std::uint64_t ns =
          (bits * 1'000'000ULL + _rate_limit_kbps - 1ULL) / _rate_limit_kbps;
      _next_send_due = Clock::now() + std::chrono::nanoseconds{ns};
    } else {
      _next_send_due = Clock::time_point::min();
    }

    if (sending_file->complete()) {
      completed_toi = sending_file->meta().toi;
      // For non-FDT files, we want to invoke the completion callback
      // outside the lock. Snapshot it here.
      if (completed_toi != 0) {
        dispatch_cb   = _completion_cb;
        completed_file = _files[completed_toi];
        _stats.files_transmitted.fetch_add(1, std::memory_order_relaxed);
      }
      file_transmitted_locked(completed_toi);
    }
  }

  if (dispatch_cb && completed_toi != 0) {
    dispatch_cb(completed_toi);
  }
  return true;
}

std::size_t Encoder::flush(std::size_t max_packets) {
  std::size_t sent = 0;
  while (sent < max_packets && send_next_packet()) {
    ++sent;
  }
  return sent;
}

}  // namespace LibFlute
