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
#include <openssl/md5.h>
#include "base64.h"
#include "spdlog/spdlog.h"

#include "File_FEC_CompactNoCode.h"

#ifdef ENABLE_RAPTOR10
#include "File_FEC_Raptor10.h"
#endif

std::shared_ptr<LibFlute::File> LibFlute::File::create_file(LibFlute::FileDeliveryTable::FileEntry entry, bool enable_md5)
{
  switch (entry.fec_oti.encoding_id) {
    case FecScheme::CompactNoCode: return std::make_shared<LibFlute::File_FEC_CompactNoCode>(std::move(entry), enable_md5);
#ifdef ENABLE_RAPTOR10
    case FecScheme::Raptor10: return std::make_shared<LibFlute::File_FEC_Raptor10>(std::move(entry), enable_md5);
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
          bool copy_data,
          bool enable_md5)
{
  switch (fec_oti.encoding_id) {
    case FecScheme::CompactNoCode: return std::make_shared<LibFlute::File_FEC_CompactNoCode>(
         toi, std::move(fec_oti), std::move(content_location), std::move(content_type), expires, 
         data, length, copy_data, enable_md5);
#ifdef ENABLE_RAPTOR10
    case FecScheme::Raptor10: return std::make_shared<LibFlute::File_FEC_Raptor10>(
         toi, std::move(fec_oti), std::move(content_location), std::move(content_type), expires, 
         data, length, copy_data, enable_md5);
#endif
    default: return nullptr;
  }
}

LibFlute::File::File(LibFlute::FileDeliveryTable::FileEntry entry, bool enable_md5)
  : _meta(std::move(entry))
  , _enable_md5(enable_md5)
  , _received_at( time(nullptr) )
{
  // Allocate a data buffer
  _buffer = (char*)malloc(_meta.content_length);
  if (_buffer == nullptr)
  {
    throw "Failed to allocate file buffer";
  }
  _own_buffer = true;
}

LibFlute::File::File(uint32_t toi,
    FecOti fec_oti,
    std::string content_location,
    std::string content_type,
    uint64_t expires,
    char* data,
    size_t length,
    bool copy_data, 
    bool enable_md5) 
  : _enable_md5(enable_md5)
{
  if (copy_data) {
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

  if (_enable_md5) {
    unsigned char md5[MD5_DIGEST_LENGTH];
    MD5((const unsigned char*)data, length, md5);
    _meta.content_md5 = base64_encode(md5, MD5_DIGEST_LENGTH);
  }
}

LibFlute::File::~File()
{
  if (_own_buffer && _buffer != nullptr)
  {
    free(_buffer);
  }
}

auto LibFlute::File::check_md5() -> void 
{
  if (_complete && !_meta.content_md5.empty()) {
    unsigned char md5[MD5_DIGEST_LENGTH];
    MD5((const unsigned char*)buffer(), length(), md5);

    auto content_md5 = base64_decode(_meta.content_md5);
    if (memcmp(md5, content_md5.c_str(), MD5_DIGEST_LENGTH) != 0) {
      reset();
    }
  }
}
