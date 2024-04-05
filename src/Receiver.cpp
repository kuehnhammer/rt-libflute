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
#include "Receiver.h"
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



LibFlute::Receiver::Receiver ( const std::string& iface, const std::string& address,
    unsigned short port, uint64_t tsi, boost::asio::io_service& io_service)
  : ReceiverBase(address, port, tsi)
  , _socket(io_service)
{
    boost::asio::ip::udp::endpoint listen_endpoint(
        boost::asio::ip::address::from_string(iface), _mcast_port);
    _socket.open(listen_endpoint.protocol());
    _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
    _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));
    _socket.set_option(boost::asio::socket_base::receive_buffer_size(16*1024*1024));
    _socket.bind(listen_endpoint);

    // Join the multicast group.
    _socket.set_option(
        boost::asio::ip::multicast::join_group(
          boost::asio::ip::address::from_string(_mcast_address)));

    _socket.async_receive_from(
        boost::asio::buffer(_buffer.data(), max_length), _sender_endpoint,
        boost::bind(&LibFlute::Receiver::handle_receive_from, this, //NOLINT
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
}

auto LibFlute::Receiver::enable_ipsec(uint32_t spi, const std::string& key) -> void 
{
  LibFlute::IpSec::enable_esp(spi, _mcast_address, LibFlute::IpSec::Direction::In, key);
}

auto LibFlute::Receiver::handle_receive_from(const boost::system::error_code& error,
    size_t bytes_recvd) -> void
{
  if (!_running) {
    return;
  }

  if (!error)
  {
    spdlog::trace("Received {} bytes", bytes_recvd);
    handle_received_packet(_buffer.data(), bytes_recvd);

    _socket.async_receive_from(
        boost::asio::buffer(_buffer.data(), max_length), _sender_endpoint,
        boost::bind(&LibFlute::Receiver::handle_receive_from, this, //NOLINT
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
  }
  else 
  {
    spdlog::error("receive_from error: {}", error.message());
  }
}

