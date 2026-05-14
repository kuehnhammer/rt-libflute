// libflute - FLUTE/ALC library
//
// Demo receiver: opens a plain POSIX UDP socket (joining a multicast
// group when the target is multicast) and feeds incoming payloads
// into a LibFlute::Decoder. Network handling is the example's job,
// not the library's.
//
#include <getopt.h>
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

// getopt_long is POSIX. argp was GNU-libc-only, which kept the
// examples Linux-host-only; this replacement compiles on macOS too
// (BSD libc) without changing the user-facing CLI surface.
const option long_options[] = {
    {"interface",    required_argument, nullptr, 'i'},
    {"target",       required_argument, nullptr, 'm'},
    {"port",         required_argument, nullptr, 'p'},
    {"tsi",          required_argument, nullptr, 't'},
    {"log-level",    required_argument, nullptr, 'l'},
    {"download-dir", required_argument, nullptr, 'd'},
    {"num-files",    required_argument, nullptr, 'n'},
    {"help",         no_argument,       nullptr, 'h'},
    {"version",      no_argument,       nullptr, 'V'},
    {nullptr, 0, nullptr, 0},
};
constexpr const char* kShortOptions = "i:m:p:t:l:d:n:hV";

void print_usage(const char* prog) {
  std::fprintf(stderr,
      "FLUTE/ALC receiver demo (plain POSIX UDP).\n"
      "Usage: %s [OPTIONS]\n"
      "\n"
      "  -i, --interface IP        Local interface to bind to (default: 0.0.0.0)\n"
      "  -m, --target IP           Multicast (or unicast) address to receive on (default: 238.1.1.95)\n"
      "  -p, --port PORT           UDP port (default: 40085)\n"
      "  -t, --tsi TSI             Session TSI (default: 16)\n"
      "  -l, --log-level LEVEL     Log verbosity 0..6 (default: 2)\n"
      "  -d, --download-dir DIR    Where to write received files (default: cwd)\n"
      "  -n, --num-files N         Stop after N files received (default: never)\n"
      "  -h, --help                Show this help and exit\n"
      "  -V, --version             Show version and exit\n",
      prog);
}

bool ip_is_multicast(const sockaddr_in& a) {
  return (ntohl(a.sin_addr.s_addr) & 0xF0000000U) == 0xE0000000U;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  int c;
  while ((c = ::getopt_long(argc, argv, kShortOptions,
                             long_options, nullptr)) != -1) {
    switch (c) {
      case 'i': args.iface         = optarg; break;
      case 'm': args.mcast_target  = optarg; break;
      case 'p': args.mcast_port    = static_cast<unsigned short>(strtoul(optarg, nullptr, 10)); break;
      case 't': args.tsi           = strtoull(optarg, nullptr, 10); break;
      case 'l': args.log_level     = static_cast<unsigned>(strtoul(optarg, nullptr, 10)); break;
      case 'd': args.download_dir  = optarg; break;
      case 'n': args.nfiles        = static_cast<unsigned>(strtoul(optarg, nullptr, 10)); break;
      case 'h': print_usage(argv[0]); return 0;
      case 'V':
        std::fprintf(stdout, "%d.%d.%d\n",
                     VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        return 0;
      case '?': return 1;
      default:  return 1;
    }
  }

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
