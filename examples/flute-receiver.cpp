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
#include <cstdio>
#include <iostream>
#include <argp.h>

#include <cstdlib>

#include <fstream>
#include <string>
#include <filesystem>
#include <libconfig.h++>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/asio/signal_set.hpp>

#include "spdlog/async.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/syslog_sink.h"

#include "Version.h"
#include "Receiver.h"
#include "PcapReceiver.h"                  // for Receiver
#include "File.h"


using libconfig::Config;
using libconfig::FileIOException;
using libconfig::ParseException;

static void print_version(FILE *stream, struct argp_state *state);
void (*argp_program_version_hook)(FILE *, struct argp_state *) = print_version;
const char *argp_program_bug_address = "Austrian Broadcasting Services <obeca@ors.at>";
static char doc[] = "FLUTE/ALC receiver demo";  // NOLINT

static struct argp_option options[] = {  // NOLINT
    {"interface", 'i', "IF", 0, "IP address of the interface to bind flute receivers to (default: 0.0.0.0)", 0},
    {"target", 'm', "IP", 0, "Multicast address to receive on (default: 238.1.1.95)", 0},
    {"port", 'p', "PORT", 0, "Multicast port (default: 40085)", 0},
    {"tsi", 't', "TSI", 0, "TSI to receive (default: 0)", 0},
    {"ipsec-key", 'k', "KEY", 0, "To enable IPSec/ESP decryption of packets, provide a hex-encoded AES key here", 0},
    {"capture-file", 'c', "FILE", 0, "Read input packets from a PCAP capture file instead of receiving from the network", 0},
    {"disable-md5",  '5', nullptr, 0,  "Disable MD5 verification" },
    {"log-level", 'l', "LEVEL", 0,
     "Log verbosity: 0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error, 5 = "
     "critical, 6 = none. Default: 2.",
     0},
    {nullptr, 0, nullptr, 0, nullptr, 0}};

/**
 * Holds all options passed on the command line
 */
struct ft_arguments {
  const char *flute_interface = {};  /**< file path of the config file. */
  const char *mcast_target = {};
  const char *capture_file = nullptr;
  bool enable_ipsec = false;
  const char *aes_key = {};
  unsigned short mcast_port = 40085;
  unsigned log_level = 2;        /**< log level */
  unsigned tsi = 0;
  bool md5_enabled = true;
  char **files;
};

/**
 * Parses the command line options into the arguments struct.
 */
static auto parse_opt(int key, char *arg, struct argp_state *state) -> error_t {
  auto arguments = static_cast<struct ft_arguments *>(state->input);
  switch (key) {
    case 'c':
      arguments->capture_file = arg;
      break;
    case 'm':
      arguments->mcast_target = arg;
      break;
    case 'i':
      arguments->flute_interface = arg;
      break;
    case '5':
      arguments->md5_enabled = false;
      break;
    case 'k':
      arguments->aes_key = arg;
      arguments->enable_ipsec = true;
      break;
    case 'p':
      arguments->mcast_port = static_cast<unsigned short>(strtoul(arg, nullptr, 10));
      break;
    case 'l':
      arguments->log_level = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    case 't':
      arguments->tsi = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc,
                           nullptr, nullptr,   nullptr};

/**
 * Print the program version in MAJOR.MINOR.PATCH format.
 */
void print_version(FILE *stream, struct argp_state * /*state*/) {
  fprintf(stream, "%s.%s.%s\n", std::to_string(VERSION_MAJOR).c_str(),
          std::to_string(VERSION_MINOR).c_str(),
          std::to_string(VERSION_PATCH).c_str());
}



/**
 *  Main entry point for the program.
 *  
 * @param argc  Command line agument count
 * @param argv  Command line arguments
 * @return 0 on clean exit, -1 on failure
 */
auto main(int argc, char **argv) -> int {
  struct ft_arguments arguments;
  /* Default values */
  arguments.mcast_target = "238.1.1.95";
  arguments.flute_interface= "0.0.0.0";

  // Parse the arguments
  argp_parse(&argp, argc, argv, 0, nullptr, &arguments);

  // Set up logging
  std::string ident = "flute-receiver";
  auto syslog_logger = spdlog::syslog_logger_mt("syslog", ident, LOG_PID | LOG_PERROR | LOG_CONS );

  spdlog::set_level(
      static_cast<spdlog::level::level_enum>(arguments.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f %z] [%^%l%$] [thr %t] %v");

  spdlog::set_default_logger(syslog_logger);
  spdlog::info("FLUTE receiver demo starting up");

  try {
    // Create a Boost io_service
    boost::asio::io_service io;

    std::shared_ptr<LibFlute::ReceiverBase> receiver;

    // Create the receiver
    if (arguments.capture_file != nullptr) {
      try {
      receiver = std::make_shared<LibFlute::PcapReceiver>(
          arguments.capture_file,
          arguments.mcast_target,
          arguments.mcast_port,
          arguments.tsi,
          io);
      } catch (std::runtime_error& ex) {
        spdlog::error("PCAP receiver error. {}", ex.what());
        exit(1);
      }
    } else {
      auto net_receiver = std::make_shared<LibFlute::Receiver>(
          arguments.flute_interface,
          arguments.mcast_target,
          arguments.mcast_port,
          arguments.tsi,
          io);

      // Configure IPSEC, if enabled
      if (arguments.enable_ipsec) 
      {
        net_receiver->enable_ipsec(1, arguments.aes_key);
      }

      receiver = net_receiver;
    }

    receiver->register_completion_callback(
        [](std::shared_ptr<LibFlute::File> file) { //NOLINT
        spdlog::info("{} (TOI {}) has been received",
            file->meta().content_location, file->meta().toi);
  //      FILE* fd = fopen(file->meta().content_location.c_str(), "wb");
  //      fwrite(file->buffer(), 1, file->length(), fd);
  //      fclose(fd);
        });

    
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait(
        boost::bind(&boost::asio::io_service::stop, &io)); // NOLINT

    // Start the IO service
    io.run();
  } catch (std::exception ex ) {
    spdlog::error("Exiting on unhandled exception: %s", ex.what());
  }

exit:
  return 0;
}
