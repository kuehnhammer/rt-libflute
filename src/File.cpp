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
#include "File.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "base64.h"
#include "spdlog/spdlog.h"
#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include "crypto++/md5.h"

#include "File_FEC_CompactNoCode.h"

#ifdef ENABLE_RAPTOR10
#include "File_FEC_Raptor10.h"
#endif

std::shared_ptr<LibFlute::File> LibFlute::File::create_file(LibFlute::FileDeliveryTable::FileEntry entry)
{
  switch (entry.fec_oti.encoding_id) {
    case FecScheme::CompactNoCode: return std::make_shared<LibFlute::File_FEC_CompactNoCode>(std::move(entry));
#ifdef ENABLE_RAPTOR10
    case FecScheme::Raptor10: return std::make_shared<LibFlute::File_FEC_Raptor10>(std::move(entry));
#endif
    default: return nullptr;
  }
}

std::shared_ptr<LibFlute::File> LibFlute::File::create_file(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data)
{
  switch (fec_oti.encoding_id) {
    case FecScheme::CompactNoCode: return std::make_shared<LibFlute::File_FEC_CompactNoCode>(
         toi, std::move(fec_oti), std::move(content_location), std::move(content_type), expires, 
         data, length, copy_data);
#ifdef ENABLE_RAPTOR10
    case FecScheme::Raptor10: return std::make_shared<LibFlute::File_FEC_Raptor10>(
         toi, std::move(fec_oti), std::move(content_location), std::move(content_type), expires, 
         data, length, copy_data);
#endif
    default: return nullptr;
  }
}

LibFlute::File::File(LibFlute::FileDeliveryTable::FileEntry entry)
  : _meta(std::move(entry))
  , _received_at( time(nullptr) )
{
  spdlog::debug("Creating File from FileEntry ({} bytes)", _meta.fec_oti.transfer_length);
}

LibFlute::File::File(uint32_t toi,
    FecOti fec_oti,
    std::string content_location,
    std::string content_type,
    uint64_t expires,
    char* data,
    size_t length,
    bool copy_data) 
{
  spdlog::debug("Creating File from data");
  if (copy_data) {
    spdlog::debug("Allocating buffer");
    _buffer = (char*)malloc(length);
    if (_buffer == nullptr)
    {
      throw "Failed to allocate file buffer";
    }
    memcpy(_buffer, data, length);
    _own_buffer = true;
  } else {
    _buffer = data;
  }

  _meta.toi = toi;
  _meta.content_location = std::move(content_location);
  _meta.content_type = std::move(content_type);
  _meta.content_length = length;
  _meta.expires = expires;
  _meta.fec_oti = std::move(fec_oti);

  std::string digest;
  CryptoPP::Weak::MD5 hash;
  hash.Update((const CryptoPP::byte*)data, length);
  digest.resize(hash.DigestSize());
  hash.Final((CryptoPP::byte*)&digest[0]);
  _meta.content_md5 = base64_encode(digest);
}

LibFlute::File::~File()
{
  spdlog::debug("Destroying File");
  if (_own_buffer && _buffer != nullptr)
  {
    spdlog::debug("Freeing buffer");
    free(_buffer);
  }
}

auto LibFlute::File::check_md5() -> void 
{
  if (_complete && !_meta.content_md5.empty()) {

    std::string digest;
    CryptoPP::Weak::MD5 hash;
    hash.Update((const CryptoPP::byte*)buffer(), length());
    digest.resize(hash.DigestSize());
    hash.Final((CryptoPP::byte*)&digest[0]);

    auto content_md5 = base64_decode(_meta.content_md5);
    if (digest != content_md5) {
      spdlog::info("MD5 mismatch, discarding");
      reset();
    }
  }
}
