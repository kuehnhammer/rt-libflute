// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License"). You may not use this file
// except in compliance with the License. You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.
//
#include "Decoder.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <utility>

#include "AlcPacket.h"
#include "EncodingSymbol.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "flute_types.h"
#include "spdlog/spdlog.h"

namespace LibFlute {

std::uint64_t ntp_seconds_now() {
  // RFC 5905: NTP-epoch seconds = Unix time + 2'208'988'800.
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count()) +
         2'208'988'800ULL;
}

Decoder::Decoder(std::uint64_t tsi, unsigned fec_dec_worker_threads)
    : _tsi(tsi)
    , _fec_dec_worker_threads(fec_dec_worker_threads) {}

void Decoder::register_completion_callback(CompletionCallback cb) {
  const std::lock_guard<std::mutex> lock(_files_mutex);
  _completion_cb = std::move(cb);
}

void Decoder::set_now_provider(NowProvider fn) {
  const std::lock_guard<std::mutex> lock(_files_mutex);
  _now = std::move(fn);
}

void Decoder::feed_packet(std::span<const std::uint8_t> alc_payload) {
  // AlcPacket parses the input read-only, but the existing
  // constructor signature takes a non-const char* (legacy from when
  // sockets handed it a mutable receive buffer). const_cast is safe.
  char* data = const_cast<char*>(
      reinterpret_cast<const char*>(alc_payload.data()));
  const std::size_t bytes = alc_payload.size();

  _stats.packets_received.fetch_add(1, std::memory_order_relaxed);
  _stats.bytes_received.fetch_add(bytes, std::memory_order_relaxed);

  // File completions are dispatched outside the mutex; collect them
  // here and invoke the callback after the lock is released so a
  // callback that re-enters the Decoder cannot deadlock.
  std::shared_ptr<File> completed_file;
  CompletionCallback    callback_snapshot;

  try {
    auto alc = AlcPacket(data, bytes);

    if (alc.tsi() != _tsi) {
      spdlog::warn("Discarding packet for unknown TSI {}", alc.tsi());
      _stats.dropped_wrong_tsi.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (bytes < alc.header_length()) {
      spdlog::warn("Discarding packet: header_length {} exceeds packet size {}",
                   alc.header_length(), bytes);
      _stats.dropped_truncated.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    {
      const std::lock_guard<std::mutex> lock(_files_mutex);

      // RFC 6726 §3.3 FDT-Instance routing.
      //
      //  (a) older than committed _fdt OR older than the in-flight
      //      TOI=0 File's instance — drop. Replays / out-of-order.
      //  (b) matches the in-flight File's instance — feed bytes via
      //      the lower path (existing logic).
      //  (c) newer than everything we know — replace any in-flight
      //      File (sized for the OLD instance's transfer_length)
      //      with a fresh one sized for THIS packet's EXT_FTI.
      //      Without this, a v=N+1 packet arriving mid-receive of
      //      v=N would feed bytes into the v=N File and corrupt
      //      both. (RFC 6726 §3.3 + RFC 1982 circular comparison.)
      if (alc.toi() == 0) {
        const std::uint32_t new_id = alc.fdt_instance_id();
        auto inflight_it = _files.find(0);
        const bool have_inflight = inflight_it != _files.end();
        const std::uint32_t inflight_id =
            have_inflight ? inflight_it->second->fdt_instance_id() : 0;

        const bool stale_vs_committed =
            _fdt && !FileDeliveryTable::IsNewerInstanceId(
                         new_id, _fdt->instance_id());
        const bool stale_vs_inflight =
            have_inflight && new_id != inflight_id &&
            !FileDeliveryTable::IsNewerInstanceId(new_id, inflight_id);

        if (stale_vs_committed || stale_vs_inflight) {
          spdlog::debug("Discarding stale FDT packet (instance_id {})",
                        new_id);
          _stats.dropped_stale_fdt.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        if (have_inflight && new_id != inflight_id) {
          spdlog::debug("FDT packet supersedes in-flight {} -> {}",
                        inflight_id, new_id);
          _files.erase(0);
        }

        if (_files.find(0) == _files.end()) {
          FileDeliveryTable::FileEntry fe{};
          fe.content_length = alc.fec_oti().transfer_length;
          fe.fec_oti        = alc.fec_oti();
          auto file = std::make_shared<File>(fe);
          file->set_fdt_instance_id(new_id);
          _files.emplace(0, file);
        }
      }

      const auto file_it = _files.find(alc.toi());
      const bool file_present = file_it != _files.end();
      const bool file_already_complete =
          file_present && file_it->second->complete();
      if (file_present && !file_already_complete) {
        auto encoding_symbols = EncodingSymbol::from_payload(
            data + alc.header_length(),
            bytes - alc.header_length(),
            _files[alc.toi()]->fec_oti(),
            alc.content_encoding());

        // Symbol-class breakdown. ESI < K (file's
        // max_source_block_length) is "source", ESI >= K is "repair".
        // CompactNoCode never sends repair so the receiver naturally
        // counts every symbol as source.
        const std::uint32_t k_block =
            _files[alc.toi()]->fec_oti().max_source_block_length;
        std::uint64_t src = 0, rep = 0;
        for (const auto& symbol : encoding_symbols) {
          spdlog::debug("received TOI {} SBN {} ID {}",
                        alc.toi(), symbol.source_block_number(), symbol.id());
          if (k_block > 0 && symbol.id() >= k_block) ++rep;
          else ++src;
          _files[alc.toi()]->put_symbol(symbol);
        }
        _stats.source_symbols_received.fetch_add(src, std::memory_order_relaxed);
        _stats.repair_symbols_received.fetch_add(rep, std::memory_order_relaxed);

        File* file = _files[alc.toi()].get();
        if (_files[alc.toi()]->complete()) {
          for (auto it = _files.begin(); it != _files.end();) {
            if (it->second.get() != file &&
                it->second->meta().content_location ==
                    file->meta().content_location) {
              spdlog::debug("Replacing file with TOI {}", it->first);
              it = _files.erase(it);
            } else {
              ++it;
            }
          }

          spdlog::debug("File with TOI {} completed", alc.toi());
          if (alc.toi() != 0) {
            _stats.files_completed.fetch_add(1, std::memory_order_relaxed);
            if (_completion_cb) {
              // Snapshot under the lock; dispatch after release.
              completed_file    = _files[alc.toi()];
              callback_snapshot = _completion_cb;
              _files.erase(alc.toi());
            }
          }

          if (alc.toi() == 0) {  // parse complete FDT
            auto candidate = std::make_unique<FileDeliveryTable>(
                alc.fdt_instance_id(), _files[alc.toi()]->buffer(),
                _files[alc.toi()]->length(),
                _fec_dec_worker_threads);
            _files.erase(alc.toi());

            // RFC 6726 §3.3: an FDT-Instance MUST NOT be relied upon
            // after its Expires time. The current FDT (if any) stays
            // in force; the expired candidate is dropped.
            if (candidate->is_expired(_now())) {
              spdlog::debug("Discarding expired FDT-Instance ID {}",
                            alc.fdt_instance_id());
              _stats.fdts_rejected_expired.fetch_add(1,
                                                       std::memory_order_relaxed);
            } else {
              // Capture old-FDT TOI set BEFORE replacing _fdt; TOIs in
              // the old set but not the new are "abandoned" — the
              // sender has stopped emitting symbols for them, so the
              // FEC layer should attempt a final decode pass with
              // whatever symbols arrived. RFC 6726 §3.3 ¶5 says the
              // file itself stays in file_list; only the decode
              // trigger fires.
              std::vector<std::uint32_t> old_tois;
              if (_fdt) {
                for (const auto& fe : _fdt->file_entries()) {
                  old_tois.push_back(fe.toi);
                }
              }

              _fdt = std::move(candidate);
              _stats.fdts_accepted.fetch_add(1, std::memory_order_relaxed);
              for (const auto& file_entry : _fdt->file_entries()) {
                if (_files.find(file_entry.toi) == _files.end()) {
                  spdlog::debug("Starting reception for TOI {}: {} ({})",
                                file_entry.toi, file_entry.content_location,
                                file_entry.content_type);
                  _files.emplace(file_entry.toi,
                                 std::make_shared<File>(file_entry));
                }
              }

              // Build the abandoned-TOI set and trigger a decode pass
              // on each one.
              std::vector<std::uint32_t> new_tois;
              for (const auto& fe : _fdt->file_entries()) {
                new_tois.push_back(fe.toi);
              }
              for (auto t : old_tois) {
                bool still_listed = false;
                for (auto n : new_tois) {
                  if (n == t) { still_listed = true; break; }
                }
                if (still_listed) continue;
                auto fit = _files.find(t);
                if (fit == _files.end()) continue;
                auto& file = fit->second;
                if (file->complete()) continue;
                spdlog::debug("FDT no longer lists TOI {}; "
                              "triggering end-of-transmission decode",
                              t);
                file->try_decode_pending();
                if (file->complete()) {
                  _stats.files_completed.fetch_add(
                      1, std::memory_order_relaxed);
                  if (_completion_cb && completed_file == nullptr) {
                    completed_file    = file;
                    callback_snapshot = _completion_cb;
                    _files.erase(fit);
                  }
                }
              }
            }
          }
        }
      } else if (file_already_complete) {
        // Bonus symbol(s) arriving after the file (typically a
        // Raptor source block) was already successfully decoded.
        // Not a drop — just unused redundancy. Silent skip; not
        // worth a counter because it's a normal Raptor scenario.
        spdlog::trace("Ignoring symbol for already-complete TOI {}",
                      alc.toi());
      } else {
        // No File for this TOI — receiver hasn't seen the FDT yet,
        // or the TOI is for a different session entirely.
        spdlog::trace("Discarding packet for unknown TOI {}", alc.toi());
        _stats.dropped_no_matching_file.fetch_add(1,
                                                    std::memory_order_relaxed);
      }
    }
  } catch (const std::exception& ex) {
    spdlog::warn("Failed to decode ALC/FLUTE packet: {}", ex.what());
    _stats.dropped_malformed.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (callback_snapshot && completed_file) {
    callback_snapshot(std::move(completed_file));
  }
}

std::vector<std::shared_ptr<File>> Decoder::file_list() {
  const std::lock_guard<std::mutex> lock(_files_mutex);
  std::vector<std::shared_ptr<File>> files;
  files.reserve(_files.size());
  for (auto& f : _files) {
    files.push_back(f.second);
  }
  return files;
}

void Decoder::remove_expired_files(unsigned max_age_seconds) {
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();) {
    auto age = std::time(nullptr) - it->second->received_at();
    if (it->second->meta().content_location != "bootstrap.multipart" &&
        age > max_age_seconds) {
      // Incomplete-at-eviction = the FEC layer (or the lossless
      // accumulator for CompactNoCode) couldn't recover the file
      // before its TTL expired. This is the closest signal we have
      // to "FEC repair gave up".
      if (it->first != 0 && !it->second->complete()) {
        _stats.files_discarded_incomplete.fetch_add(
            1, std::memory_order_relaxed);
        _stats.bytes_discarded_incomplete.fetch_add(
            it->second->meta().fec_oti.transfer_length,
            std::memory_order_relaxed);
      }
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}

void Decoder::remove_file_with_content_location(
    const std::string& content_location) {
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();) {
    if (it->second->meta().content_location == content_location) {
      if (it->first != 0 && !it->second->complete()) {
        _stats.files_discarded_incomplete.fetch_add(
            1, std::memory_order_relaxed);
        _stats.bytes_discarded_incomplete.fetch_add(
            it->second->meta().fec_oti.transfer_length,
            std::memory_order_relaxed);
      }
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}

DecoderStats Decoder::stats() const {
  DecoderStats s;
  s.packets_received          = _stats.packets_received.load(std::memory_order_relaxed);
  s.bytes_received            = _stats.bytes_received.load(std::memory_order_relaxed);
  s.dropped_wrong_tsi         = _stats.dropped_wrong_tsi.load(std::memory_order_relaxed);
  s.dropped_malformed         = _stats.dropped_malformed.load(std::memory_order_relaxed);
  s.dropped_truncated         = _stats.dropped_truncated.load(std::memory_order_relaxed);
  s.dropped_stale_fdt         = _stats.dropped_stale_fdt.load(std::memory_order_relaxed);
  s.dropped_no_matching_file  = _stats.dropped_no_matching_file.load(std::memory_order_relaxed);
  s.source_symbols_received   = _stats.source_symbols_received.load(std::memory_order_relaxed);
  s.repair_symbols_received   = _stats.repair_symbols_received.load(std::memory_order_relaxed);
  s.files_completed           = _stats.files_completed.load(std::memory_order_relaxed);
  s.files_discarded_incomplete = _stats.files_discarded_incomplete.load(std::memory_order_relaxed);
  s.bytes_discarded_incomplete = _stats.bytes_discarded_incomplete.load(std::memory_order_relaxed);
  s.fdts_accepted             = _stats.fdts_accepted.load(std::memory_order_relaxed);
  s.fdts_rejected_expired     = _stats.fdts_rejected_expired.load(std::memory_order_relaxed);
  s.fdts_rejected_stale       = _stats.fdts_rejected_stale.load(std::memory_order_relaxed);
  return s;
}

}  // namespace LibFlute
