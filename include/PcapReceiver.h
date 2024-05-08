// libflute - FLUTE/ALC library
//
// Copyright (C) 2024 Klaus Kühnhammer (Bitstem GmbH)
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
#include "ReceiverBase.h" 
#include <pcap.h>
namespace LibFlute { class File; }
namespace boost::system { class error_code; }

namespace LibFlute {
  /**
   *  FLUTE receiver class. Construct an instance of this to receive files from a FLUTE/ALC session.
   */
  class PcapReceiver : public ReceiverBase {
    public:
     /**
      *  Default constructor.
      *
      *  @param pcap_file Path of the PCAP file to read
      *  @param address Multicast address
      *  @param port Target port 
      *  @param tsi TSI value of the session 
      *  @param io_service Boost io_service to run the socket operations in (must be provided by the caller)
      */
      PcapReceiver( const std::string& pcap_file, const std::string& address, 
          unsigned short port, uint64_t tsi, boost::asio::io_service& io_service, int skip_ms = 0);

     /**
      *  Destructor.
      */
      virtual ~PcapReceiver();

      void stop() override { _running = false; }

    private:
      static long tv_to_usecs(const struct timeval *tv);
      void process_packet();
      void read_packet();
      void check_packet();

      bool _running = true;
      pcap_t* _pcap_file;
      long _last_packet_time = {};

      const unsigned char* _packet_data = nullptr;
      struct pcap_pkthdr _packet_header = {};

      boost::asio::deadline_timer _packet_timer;
      long _skip_us = 0;
      long _total_time = 0;
  };
};
