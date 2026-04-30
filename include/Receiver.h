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
#include "ReceiverBase.h" 
namespace LibFlute { class File; }
namespace boost::system { class error_code; }

namespace LibFlute {
  /**
   *  FLUTE receiver class. Construct an instance of this to receive files from a FLUTE/ALC session.
   */
  class Receiver : public ReceiverBase {
    public:
     /**
      *  Default constructor.
      *
      *  @param iface Address of the (local) interface to bind the receiving socket to. 0.0.0.0 = any.
      *  @param address Multicast address
      *  @param port Target port 
      *  @param tsi TSI value of the session 
      *  @param io_service Boost io_service to run the socket operations in (must be provided by the caller)
      */
      Receiver( const std::string& iface, const std::string& address, 
          unsigned short port, uint64_t tsi, boost::asio::io_service& io_service);

     /**
      *  Default destructor.
      */
      virtual ~Receiver() = default;

     /**
      *  Enable IPSEC ESP decryption of FLUTE payloads.
      *
      *  @param spi Security Parameter Index value to use
      *  @param key AES key as a hex string (without leading 0x). Must be an even number of characters long.
      */
      void enable_ipsec( uint32_t spi, const std::string& aes_key);

      void stop() override { _running = false; }

    private:
      void handle_receive_from(const boost::system::error_code& error,
          size_t bytes_recvd);

      std::string _mcast_address;
      unsigned short _mcast_port;
      boost::asio::ip::udp::socket _socket;
      boost::asio::ip::udp::endpoint _sender_endpoint;

      enum { max_length = 2048 };
      std::array<char, max_length> _buffer;

      bool _running = true;
  };
};
