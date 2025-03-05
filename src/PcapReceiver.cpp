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
#include "PcapReceiver.h"
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
#include "spdlog/spdlog.h"
#include "flute_types.h"
#include "spdlog/spdlog.h"
#include "netinet/ip.h"
#include "netinet/udp.h"


LibFlute::PcapReceiver::PcapReceiver ( const std::string& pcap_file, const std::string& address,
    unsigned short port, uint64_t tsi, boost::asio::io_service& io_service, unsigned skip_ms)
  : ReceiverBase(address, port, tsi)
  , _packet_timer( io_service )
{
  char errbuf[PCAP_ERRBUF_SIZE];

  _pcap_file = pcap_open_offline(pcap_file.c_str(), errbuf);
  if (_pcap_file == nullptr) {
    throw std::runtime_error("Can't open PCAP file: " + std::string(errbuf));
  }

  // Get the first packet to establish a time base
  read_packet();
  if (_packet_data != nullptr) {
    _last_packet_time = tv_to_usecs(&_packet_header.ts);
  } else {
    throw std::runtime_error("No packets found in file");
  }

  // start processing the file
  process_packet();
}

LibFlute::PcapReceiver::~PcapReceiver() {
  if (_pcap_file == nullptr) {
    pcap_close(_pcap_file);
  }
}

auto LibFlute::PcapReceiver::process_packet() -> void
{
  assert(_packet_data != nullptr);

  // Check if the destination matches the mcast address and port we need and
  // pass the payload on for FLUTE decoding if it does
  check_packet();

  // read the next packet
  read_packet();

  if (_packet_data == nullptr) {
    spdlog::info("Last packet processed, exiting.");
  } else {
    // Calculate how long we have to wait until processing the next packet
    auto packet_time = tv_to_usecs(&_packet_header.ts);
    auto delta = packet_time - _last_packet_time;
    _last_packet_time = packet_time;

    _packet_timer.expires_from_now(boost::posix_time::microseconds(delta));
    _packet_timer.async_wait( boost::bind(&PcapReceiver::process_packet, this)); //NOLINT
  }
}

auto LibFlute::PcapReceiver::read_packet() -> void
{
  assert(_pcap_file != nullptr);
  _packet_data = pcap_next(_pcap_file, &_packet_header);
}

auto LibFlute::PcapReceiver::check_packet() -> void
{
  assert(_packet_data != nullptr);

  auto ip_header = (struct ip*)(_packet_data);
  auto udp_header = (struct udphdr*)(_packet_data + (ip_header->ip_hl * 4));

  auto dest_address = std::string(inet_ntoa(ip_header->ip_dst));
  auto dest_port = ntohs(udp_header->uh_dport);

  if (dest_address == _mcast_address && dest_port == _mcast_port) {
    auto payload = (_packet_data + (ip_header->ip_hl * 4) + sizeof(struct udphdr));
    handle_received_packet((char*)payload, ntohs(udp_header->uh_ulen) - sizeof(struct udphdr));
  }
}

auto LibFlute::PcapReceiver::tv_to_usecs(const struct timeval *tv) -> long
{
	return tv->tv_sec * 1'000'000 + tv->tv_usec;
}
