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
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <memory>
#include "AlcPacket.h"
#include "FileDeliveryTable.h"
#include "EncodingSymbol.h"

namespace LibFlute {
  /**
   *  Represents a file being transmitted or received
   */
  class File {
    public:
      /**
       *  Create a file from an FDT entry (used for reception) - factory method
       *
       *  @param entry FDT entry
       */
      static std::shared_ptr<File> create_file(LibFlute::FileDeliveryTable::FileEntry entry);

      /**
       *  Create a file from the given parameters (used for transmission)
       *
       *  @param toi TOI of the file
       *  @param content_location Content location URI to use
       *  @param content_type MIME type
       *  @param expires Expiry value (in seconds since the NTP epoch)
       *  @param data Pointer to the data buffer
       *  @param length Length of the buffer
       *  @param copy_data Copy the buffer. If false (the default), the caller must ensure the buffer remains valid 
       *                   while the file is being transmitted.
       */
      static std::shared_ptr<File> create_file(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data = false);

      /**
       *  Default destructor.
       */
      virtual ~File();


      /**
       *  Check if the file is complete
       */
      bool complete() const { return _complete; };

      /**
       *  Get the data buffer
       */
      char* buffer() const { return _buffer; };

      /**
       *  Get the data buffer length
       */
      size_t length() const { return _meta.content_length; };

      /**
       *  Get the FEC OTI values
       */
      const FecOti& fec_oti() const { return _meta.fec_oti; };

      /**
       *  Get the file metadata from its FDT entry
       */
      const LibFlute::FileDeliveryTable::FileEntry& meta() const { return _meta; };

      /**
       *  Timestamp of file reception
       */
      unsigned long received_at() const { return _received_at; };

      /**
       *  Log access to the file by incrementing a counter
       */
      void log_access() { _access_count++; };

      /**
       *  Get the access counter value
       */
      unsigned access_count() const { return _access_count; };

      /**
       *  Set the FDT instance ID
       */
      void set_fdt_instance_id( uint16_t id) { _fdt_instance_id = id; };

      /**
       *  Get the FDT instance ID
       */
      uint16_t fdt_instance_id() { return _fdt_instance_id; };


      //
      //  FEC-specific, to be imnplemented by derived classes
      //  
      
      /**
       *  Process the data from an incoming encoding symbol
       */
      virtual void put_symbol(const EncodingSymbol& symbol) = 0;

      /**
       *  Get the next encoding symbols that fit in max_size bytes
       */
      virtual std::vector<EncodingSymbol> get_next_symbols(size_t max_size) = 0;

      /**
       *  Mark encoding symbols as completed (transmitted)
       */
      virtual void mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) = 0;

      /**
       *  Reset all source symbols to incomplete state
       */
      virtual void reset() = 0;

      virtual void dump_status() {};

    protected:
      File(FileDeliveryTable::FileEntry entry);
      File(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data);

      void check_md5();

      bool _complete = false;;

      char* _buffer = nullptr;
      bool _own_buffer = false;

      LibFlute::FileDeliveryTable::FileEntry _meta;
      unsigned long _received_at;
      unsigned _access_count = 0;

      uint16_t _fdt_instance_id = 0;

  };
};
