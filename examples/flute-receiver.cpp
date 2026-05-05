// libflute - FLUTE/ALC library
//
// Demo receiver: opens a plain POSIX UDP socket (joining a multicast
// group when the target is multicast) and feeds incoming payloads
// into a LibFlute::Decoder. Network handling is the example's job,
// not the library's.
//
#include <argp.h>
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "Decoder.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "Version.h"
#include "spdlog/sinks/syslog_sink.h"
#include "spdlog/spdlog.h"

namespace {

struct Args {
  const char* iface = "0.0.0.0";
  const char* mcast_target = "238.1.1.95";
  unsigned short mcast_port = 40085;
  unsigned log_level = 2;
  std::uint64_t tsi = 16;
  const char* download_dir = nullptr;
  unsigned nfiles = 0;
};

argp_option options[] = {
    {"interface", 'i', "IP", 0, "Local interface to bind to (default: 0.0.0.0)", 0},
    {"target", 'm', "IP", 0, "Multicast (or unicast) address to receive on (default: 238.1.1.95)", 0},
    {"port", 'p', "PORT", 0, "UDP port (default: 40085)", 0},
    {"tsi", 't', "TSI", 0, "Session TSI (default: 16)", 0},
    {"log-level", 'l', "LEVEL", 0, "Log verbosity 0..6 (default: 2)", 0},
    {"download-dir", 'd', "DIR", 0, "Where to write received files (default: cwd)", 0},
    {"num-files", 'n', "N", 0, "Stop after N files received (default: never)", 0},
    {nullptr, 0, nullptr, 0, nullptr, 0},
};

error_t parse_opt(int key, char* arg, argp_state* state) {
  auto* a = static_cast<Args*>(state->input);
  switch (key) {
    case 'i': a->iface = arg; break;
    case 'm': a->mcast_target = arg; break;
    case 'p': a->mcast_port = static_cast<unsigned short>(strtoul(arg, nullptr, 10)); break;
    case 't': a->tsi = strtoull(arg, nullptr, 10); break;
    case 'l': a->log_level = static_cast<unsigned>(strtoul(arg, nullptr, 10)); break;
    case 'd': a->download_dir = arg; break;
    case 'n': a->nfiles = static_cast<unsigned>(strtoul(arg, nullptr, 10)); break;
    default: return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

void print_version(FILE* stream, argp_state*) {
  std::fprintf(stream, "%d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
}

bool ip_is_multicast(const sockaddr_in& a) {
  return (ntohl(a.sin_addr.s_addr) & 0xF0000000U) == 0xE0000000U;
}

}  // namespace

void (*argp_program_version_hook)(FILE*, argp_state*) = print_version;

int main(int argc, char** argv) {
  Args args;
  argp argp_spec = {options, parse_opt, nullptr,
                     "FLUTE/ALC receiver demo (plain POSIX UDP).",
                     nullptr, nullptr, nullptr};
  argp_parse(&argp_spec, argc, argv, 0, nullptr, &args);

  auto syslog_sink = spdlog::syslog_logger_mt(
      "syslog", "flute-receiver", LOG_PID | LOG_PERROR | LOG_CONS);
  spdlog::set_default_logger(syslog_sink);
  spdlog::set_level(static_cast<spdlog::level::level_enum>(args.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f] [%^%l%$] %v");

  // Open a UDP socket bound to the requested interface + port. If the
  // target is an IPv4 multicast group, join it. Unicast just works
  // without a group join.
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    spdlog::error("socket: {}", std::strerror(errno));
    return 1;
  }
  int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port   = htons(args.mcast_port);
  if (inet_pton(AF_INET, args.iface, &bind_addr.sin_addr) != 1) {
    spdlog::error("invalid interface address: {}", args.iface);
    close(sock);
    return 1;
  }
  if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
    spdlog::error("bind: {}", std::strerror(errno));
    close(sock);
    return 1;
  }

  sockaddr_in target{};
  target.sin_family = AF_INET;
  if (inet_pton(AF_INET, args.mcast_target, &target.sin_addr) != 1) {
    spdlog::error("invalid target address: {}", args.mcast_target);
    close(sock);
    return 1;
  }
  if (ip_is_multicast(target)) {
    ip_mreq mreq{};
    mreq.imr_multiaddr = target.sin_addr;
    if (inet_pton(AF_INET, args.iface, &mreq.imr_interface) != 1) {
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
      spdlog::error("IP_ADD_MEMBERSHIP({}): {}", args.mcast_target,
                    std::strerror(errno));
      close(sock);
      return 1;
    }
  }

  LibFlute::Decoder decoder(args.tsi);

  unsigned files_received = 0;
  bool stop = false;
  decoder.register_completion_callback(
      [&](std::shared_ptr<LibFlute::File> file) {
        spdlog::info("received {} (TOI {}, {} bytes)",
                      file->meta().content_location, file->meta().toi,
                      file->length());
        std::string outpath;
        const char* slash = std::strrchr(file->meta().content_location.c_str(), '/');
        const char* fname = slash ? slash + 1 : file->meta().content_location.c_str();
        if (args.download_dir) {
          outpath = std::string(args.download_dir) + "/" + fname;
        } else {
          outpath = "flute_download_" + std::to_string(file->meta().toi) +
                    "-" + fname;
        }
        if (FILE* fd = std::fopen(outpath.c_str(), "wb"); fd != nullptr) {
          std::fwrite(file->buffer(), 1, file->length(), fd);
          std::fclose(fd);
        } else {
          spdlog::error("open({}) for write: {}", outpath, std::strerror(errno));
        }
        ++files_received;
        if (args.nfiles > 0 && files_received >= args.nfiles) {
          spdlog::warn("{} file(s) received; stopping", files_received);
          stop = true;
        }
      });

  std::array<std::uint8_t, 65536> buffer;
  while (!stop) {
    ssize_t n = recvfrom(sock, buffer.data(), buffer.size(), 0, nullptr, nullptr);
    if (n < 0) {
      if (errno == EINTR) continue;
      spdlog::error("recvfrom: {}", std::strerror(errno));
      break;
    }
    decoder.feed_packet({buffer.data(), static_cast<std::size_t>(n)});
  }

  close(sock);
  return 0;
}
