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
#include <boost/bind/bind.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>                                                  // for pair
#include "AlcPacket.h"
#include "EncodingSymbol.h"
#include "File.h"                                                   // for File
#include "IpSec.h"
#include "flute_types.h"
#include "spdlog/spdlog.h"



LibFlute::ReceiverBase::ReceiverBase ( const std::string& address,
    unsigned short port, uint64_t tsi)
    : _mcast_address(address)
    , _mcast_port(port)
    , _tsi(tsi)
{
}


auto LibFlute::ReceiverBase::handle_completed_file(const std::shared_ptr<LibFlute::File>& file)
  -> void
{
  auto toi = file->meta().toi;
  spdlog::debug("File with TOI {} completed", toi);
  if (toi != 0 && _completion_cb) {
    _completion_cb(file);
  }

  if (toi == 0) { // parse complete FDT
    _fdt = std::make_unique<LibFlute::FileDeliveryTable>(
        _current_fdt_instance, file->buffer(), file->length());

    // check for new files
    for (const auto& file_entry : _fdt->file_entries()) {
      spdlog::info("FDT instance {}: {}", _fdt->instance_id(), file_entry.content_location);
      // automatically receive all files in the FDT
      if (_files.find(file_entry.toi) == _files.end()) {
        spdlog::info("Starting reception for file with TOI {}: {} ({})", file_entry.toi,
            file_entry.content_location, file_entry.content_type);
        auto file = LibFlute::File::create_file(file_entry);
        if (file) { 
          _files.emplace(file_entry.toi, file);
        }
      }
    }

    //check for files that are no longer announced, but were not completed
    for (auto file_it = _files.begin(); file_it != _files.end(); ) {
      bool still_present = false;
      for (const auto& file_entry : _fdt->file_entries()) {
        if (file_entry.toi == file_it->second->meta().toi) {
          still_present = true;
          break;
        }
      }

      if (still_present || file_it->second->complete()) {
        ++file_it;
      } else {
        spdlog::info("File with TOI {} is no longer in the FDT", file_it->first);
        file_it->second->dump_status();
        file_it = _files.erase(file_it);
      }

    } 
  }

   _files.erase(toi);
}

auto LibFlute::ReceiverBase::handle_received_packet(char* data, size_t bytes) -> void
{
  try {
    auto alc = LibFlute::AlcPacket(data, bytes);

    if (alc.tsi() != _tsi) {
      spdlog::warn("Discarding packet for unknown TSI {}", alc.tsi());
      return;
    }

    const std::lock_guard<std::mutex> lock(_files_mutex);

    if (alc.toi() == 0 && (!_receiving_fdt || _current_fdt_instance != alc.fdt_instance_id())) {
      _receiving_fdt = true;
      _current_fdt_instance = alc.fdt_instance_id();
      _files.erase(alc.toi());
      FileDeliveryTable::FileEntry fe{0, "", static_cast<uint32_t>(alc.fec_oti().transfer_length), "", "", 0, alc.fec_oti()};
      auto file = LibFlute::File::create_file(fe);
      if (file) { 
        _files.emplace(alc.toi(), file);
      }
    }

    if (_files.find(alc.toi()) != _files.end() && !_files[alc.toi()]->complete()) {
      auto encoding_symbols = LibFlute::EncodingSymbol::from_payload(
          data + alc.header_length(), 
          bytes - alc.header_length(),
          _files[alc.toi()]->fec_oti(),
          alc.content_encoding());

      for (const auto& symbol : encoding_symbols) {
        spdlog::debug("received TOI {} SBN {} ID {}", 
            alc.toi(), symbol.source_block_number(), symbol.id() );
        _files[alc.toi()]->put_symbol(symbol);
      }

      auto* file = _files[alc.toi()].get();
      if (_files[alc.toi()]->complete()) {
        handle_completed_file(_files[alc.toi()]);
      }
    } 
  } catch (std::exception& ex) {
    spdlog::warn("Failed to decode ALC/FLUTE packet: {}", ex.what());
  }
}

auto LibFlute::ReceiverBase::file_list() -> std::vector<std::shared_ptr<LibFlute::File>>
{
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
