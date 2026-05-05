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
#include "ReceiverBase.h"
#include <ctime>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>                                                  // for pair
#include "AlcPacket.h"
#include "EncodingSymbol.h"
#include "File.h"                                                   // for File
#include "flute_types.h"
#include "spdlog/spdlog.h"



LibFlute::ReceiverBase::ReceiverBase(uint64_t tsi)
    : _tsi(tsi)
{
}

void LibFlute::ReceiverBase::register_completion_callback(completion_callback_t cb)
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  _completion_cb = std::move(cb);
}

auto LibFlute::ReceiverBase::handle_received_packet(char* data, size_t bytes) -> void
{
  // File completions are dispatched outside the mutex; we collect them
  // here and invoke the callback after the lock is released so a callback
  // that re-enters ReceiverBase (file_list, remove_*) can't deadlock.
  std::shared_ptr<LibFlute::File> completed_file;
  completion_callback_t callback_snapshot;

  try {
    auto alc = LibFlute::AlcPacket(data, bytes);

    if (alc.tsi() != _tsi) {
      spdlog::warn("Discarding packet for unknown TSI {}", alc.tsi());
      return;
    }

    if (bytes < alc.header_length()) {
      spdlog::warn("Discarding packet: header_length {} exceeds packet size {}",
                   alc.header_length(), bytes);
      return;
    }

    {
      const std::lock_guard<std::mutex> lock(_files_mutex);

      // RFC 6726 §3.3: only newer FDT-Instance IDs supersede the
      // current FDT. Stale (lower-or-equal) IDs are dropped, both to
      // make repeats idempotent and to defend against out-of-order or
      // replayed packets that would otherwise roll the receiver back
      // to an older file set.
      // TODO (round 4): replace `>` with a circular comparator over
      // the 20-bit instance-ID space (RFC 6726 §3.3 wraparound).
      if (alc.toi() == 0 && (!_fdt || alc.fdt_instance_id() > _fdt->instance_id())) {
        if (_files.find(alc.toi()) == _files.end()) {
          FileDeliveryTable::FileEntry fe{};
          fe.content_length = alc.fec_oti().transfer_length;
          fe.fec_oti        = alc.fec_oti();
          _files.emplace(alc.toi(), std::make_shared<LibFlute::File>(fe));
        }
      }

      if (_files.find(alc.toi()) != _files.end() && !_files[alc.toi()]->complete()) {
        auto encoding_symbols = LibFlute::EncodingSymbol::from_payload(
            data + alc.header_length(),
            bytes - alc.header_length(),
            _files[alc.toi()]->fec_oti(),
            alc.content_encoding());

        for (const auto& symbol : encoding_symbols) {
          spdlog::debug("received TOI {} SBN {} ID {}", alc.toi(), symbol.source_block_number(), symbol.id());
          _files[alc.toi()]->put_symbol(symbol);
        }

        auto* file = _files[alc.toi()].get();
        if (_files[alc.toi()]->complete()) {
          for (auto it = _files.begin(); it != _files.end();)
          {
            if (it->second.get() != file && it->second->meta().content_location == file->meta().content_location)
            {
              spdlog::debug("Replacing file with TOI {}", it->first);
              it = _files.erase(it);
            }
            else
            {
              ++it;
            }
          }

          spdlog::debug("File with TOI {} completed", alc.toi());
          if (alc.toi() != 0 && _completion_cb) {
            // Snapshot under the lock; dispatch after we release it.
            completed_file = _files[alc.toi()];
            callback_snapshot = _completion_cb;
            _files.erase(alc.toi());
          }

          if (alc.toi() == 0) { // parse complete FDT
            _fdt = std::make_unique<LibFlute::FileDeliveryTable>(
                alc.fdt_instance_id(), _files[alc.toi()]->buffer(), _files[alc.toi()]->length());

            _files.erase(alc.toi());
            for (const auto& file_entry : _fdt->file_entries()) {
              // automatically receive all files in the FDT
              if (_files.find(file_entry.toi) == _files.end()) {
                spdlog::debug("Starting reception for file with TOI {}: {} ({})", file_entry.toi,
                    file_entry.content_location, file_entry.content_type);
                _files.emplace(file_entry.toi, std::make_shared<LibFlute::File>(file_entry));
              }
            }
          }
        }
      } else {
        spdlog::trace("Discarding packet for unknown or already completed file with TOI {}", alc.toi());
      }
    }
  } catch (const std::exception& ex) {
    spdlog::warn("Failed to decode ALC/FLUTE packet: {}", ex.what());
    return;
  }

  if (callback_snapshot && completed_file) {
    callback_snapshot(std::move(completed_file));
  }
}

auto LibFlute::ReceiverBase::file_list() -> std::vector<std::shared_ptr<LibFlute::File>>
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  std::vector<std::shared_ptr<LibFlute::File>> files;
  for (auto& f : _files) {
    files.push_back(f.second);
  }
  return files;
}

auto LibFlute::ReceiverBase::remove_expired_files(unsigned max_age) -> void
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();)
  {
    auto age = time(nullptr) - it->second->received_at();
    if ( it->second->meta().content_location != "bootstrap.multipart"  && age > max_age) {
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}

auto LibFlute::ReceiverBase::remove_file_with_content_location(const std::string& cl) -> void
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();)
  {
    if ( it->second->meta().content_location == cl) {
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}
