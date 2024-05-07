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

#include <stddef.h>                   // for size_t
#include <stdint.h>                   // for uint64_t, uint32_t
#include <boost/asio.hpp>  // for io_service
#include <functional>                 // for function
#include <map>                        // for map
#include <memory>                     // for shared_ptr, unique_ptr
#include <mutex>                      // for mutex
#include <string>                     // for string
#include <vector>                     // for vector
#include "FileDeliveryTable.h"        // for FileDeliveryTable
namespace LibFlute { class File; }
namespace boost::system { class error_code; }

namespace LibFlute {
  /**
   *  Abstract FLUTE receiver base class. All receiver types inherit from this.
   */
  class ReceiverBase {
    public:
     /**
      *  Definition of a file reception completion callback function that can be
      *  registered through ::register_completion_callback.
      *
      *  @returns shared_ptr to the received file
      */
      typedef std::function<void(std::shared_ptr<LibFlute::File>)> completion_callback_t;

     /**
      *  Default constructor to be called from derived class.
      *
      *  @param address Multicast address
      *  @param port Target port 
      *  @param tsi TSI value of the session 
      */
      ReceiverBase ( const std::string& address, unsigned short port, uint64_t tsi,
          bool enable_md5 = true);

     /**
      *  Default destructor.
      */
      virtual ~ReceiverBase() = default;

     /**
      *  List all current files
      *
      *  @return Vector of all files currently in the FDT
      */
      std::vector<std::shared_ptr<LibFlute::File>> file_list();

     /**
      *  Remove files from the list that are older than max_age seconds
      */
      void remove_expired_files(unsigned max_age);

     /**
      *  Remove a file from the list that matches the passed content location
      */
      void remove_file_with_content_location(const std::string& cl);

     /**
      *  Register a callback for file reception notifications
      *
      *  @param cb Function to call on file completion
      */
      void register_completion_callback(completion_callback_t cb) { _completion_cb = cb; };

     /**
      *  Stop the receiver and clean up
      */
      virtual void stop() = 0;

      virtual size_t packet_offset() const { return _packet_offset; };
    protected:
     /**
      *  Decoding incoming data. To be called by derived class with packet payload.
      */
      void handle_received_packet(char* data, size_t bytes);

      std::string _mcast_address = {};
      unsigned short _mcast_port = {};
      uint64_t _tsi;
      size_t _packet_offset = 0;

    private:
      std::unique_ptr<LibFlute::FileDeliveryTable> _fdt;
      std::map<uint64_t, std::shared_ptr<LibFlute::File>> _files;
      std::mutex _files_mutex;

      completion_callback_t _completion_cb = nullptr;
      bool _enable_md5 = true;

  };
};
