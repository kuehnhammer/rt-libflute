// libflute - FLUTE/ALC library
//
// Demo transmitter: opens a plain POSIX UDP socket and pumps packets
// from a LibFlute::Encoder to a multicast (or unicast) destination.
// Network handling lives entirely here in the example, not in the
// library — this is the pattern consumers should mirror.
//
#include <getopt.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Encoder.h"
#include "Version.h"
#include "flute_types.h"
#include "spdlog/sinks/syslog_sink.h"
#include "spdlog/spdlog.h"

namespace {

struct Args {
  const char* mcast_target = "238.1.1.95";
  unsigned short mcast_port = 40085;
  unsigned short mtu = 1500;
  std::uint32_t rate_limit_kbps = 1000;
  unsigned log_level = 2;
  unsigned fec = 0;
  std::uint64_t tsi = 16;
  char** files = nullptr;
};

// getopt_long is POSIX. argp was GNU-libc-only, which kept the
// examples Linux-host-only; this replacement compiles on macOS too
// (BSD libc) without changing the user-facing CLI surface.
const option long_options[] = {
    {"target",     required_argument, nullptr, 'm'},
    {"port",       required_argument, nullptr, 'p'},
    {"mtu",        required_argument, nullptr, 't'},
    {"rate-limit", required_argument, nullptr, 'r'},
    {"fec",        required_argument, nullptr, 'f'},
    {"tsi",        required_argument, nullptr, 's'},
    {"log-level",  required_argument, nullptr, 'l'},
    {"help",       no_argument,       nullptr, 'h'},
    {"version",    no_argument,       nullptr, 'V'},
    {nullptr, 0, nullptr, 0},
};
constexpr const char* kShortOptions = "m:p:t:r:f:s:l:hV";

void print_usage(const char* prog) {
  std::fprintf(stderr,
      "FLUTE/ALC transmitter demo (plain POSIX UDP).\n"
      "Usage: %s [OPTIONS] FILE...\n"
      "\n"
      "  -m, --target IP         Target multicast address (default: 238.1.1.95)\n"
      "  -p, --port PORT         Target port (default: 40085)\n"
      "  -t, --mtu BYTES         Path MTU to size ALC packets for (default: 1500)\n"
      "  -r, --rate-limit KBPS   Transmit rate limit in kbps; 0 = unlimited (default: 1000)\n"
      "  -f, --fec FEC           FEC scheme: 0 = Compact No-Code, 1 = Raptor (default: 0)\n"
      "  -s, --tsi TSI           Session TSI (default: 16)\n"
      "  -l, --log-level LEVEL   Log verbosity 0..6 (default: 2)\n"
      "  -h, --help              Show this help and exit\n"
      "  -V, --version           Show version and exit\n",
      prog);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  int c;
  while ((c = ::getopt_long(argc, argv, kShortOptions,
                             long_options, nullptr)) != -1) {
    switch (c) {
      case 'm': args.mcast_target = optarg; break;
      case 'p': args.mcast_port      = static_cast<unsigned short>(strtoul(optarg, nullptr, 10)); break;
      case 't': args.mtu             = static_cast<unsigned short>(strtoul(optarg, nullptr, 10)); break;
      case 'r': args.rate_limit_kbps = static_cast<std::uint32_t>(strtoul(optarg, nullptr, 10)); break;
      case 'f': args.fec             = static_cast<unsigned>(strtoul(optarg, nullptr, 10)); break;
      case 's': args.tsi             = strtoull(optarg, nullptr, 10); break;
      case 'l': args.log_level       = static_cast<unsigned>(strtoul(optarg, nullptr, 10)); break;
      case 'h': print_usage(argv[0]); return 0;
      case 'V':
        std::fprintf(stdout, "%d.%d.%d\n",
                     VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        return 0;
      case '?': return 1;  // getopt already printed the error
      default:  return 1;
    }
  }
  if (optind >= argc) {
    print_usage(argv[0]);
    return 1;
  }
  args.files = &argv[optind];

  auto syslog_sink = spdlog::syslog_logger_mt(
      "syslog", "flute-transmitter", LOG_PID | LOG_PERROR | LOG_CONS);
  spdlog::set_default_logger(syslog_sink);
  spdlog::set_level(static_cast<spdlog::level::level_enum>(args.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f] [%^%l%$] %v");

  // Mmap each input file. The Encoder takes a non-owning pointer; the
  // mmap stays alive until completion-callback frees it.
  struct Mapped {
    std::string path;
    char* buffer = nullptr;
    std::size_t length = 0;
    std::uint16_t toi = 0;
  };
  std::vector<Mapped> files;
  for (int j = 0; args.files && args.files[j]; ++j) {
    int fd = open(args.files[j], O_RDONLY);
    if (fd < 0) {
      spdlog::error("open({}): {}", args.files[j], std::strerror(errno));
      continue;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
      close(fd);
      continue;
    }
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
      spdlog::error("mmap({}): {}", args.files[j], std::strerror(errno));
      continue;
    }
    files.push_back({args.files[j], static_cast<char*>(p),
                      static_cast<std::size_t>(st.st_size), 0});
  }
  if (files.empty()) {
    spdlog::error("no usable input files");
    return 1;
  }

  // Open a plain UDP socket. Multicast TTL 8, loopback enabled so a
  // local receiver on the same host can pick the packets up.
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    spdlog::error("socket: {}", std::strerror(errno));
    return 1;
  }
  unsigned char ttl = 8;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  unsigned char loop = 1;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port   = htons(args.mcast_port);
  if (inet_pton(AF_INET, args.mcast_target, &dst.sin_addr) != 1) {
    spdlog::error("invalid target address: {}", args.mcast_target);
    close(sock);
    return 1;
  }

  // Encoder: PacketCallback dispatches via sendto. Returns true on
  // successful dispatch so the encoder marks symbols transmitted.
  LibFlute::Encoder encoder(
      args.tsi, args.mtu, args.rate_limit_kbps,
      [sock, &dst](std::span<const std::uint8_t> bytes) -> bool {
        ssize_t n = sendto(sock, bytes.data(), bytes.size(), 0,
                            reinterpret_cast<const sockaddr*>(&dst),
                            sizeof(dst));
        if (n < 0) {
          spdlog::warn("sendto: {}", std::strerror(errno));
          return false;
        }
        return true;
      });

  encoder.register_completion_callback([&](std::uint32_t toi) {
    for (auto& f : files) {
      if (f.toi == toi) {
        spdlog::info("transmitted {} (TOI {})", f.path, toi);
        munmap(f.buffer, f.length);
        f.buffer = nullptr;
      }
    }
  });

  for (auto& f : files) {
    f.toi = encoder.send(
        f.path, "application/octet-stream",
        LibFlute::Encoder::seconds_since_epoch() + 60,
        f.buffer, f.length,
        static_cast<LibFlute::FecScheme>(args.fec),
        /*copy_buffer=*/false);
    if (f.toi > 0) {
      spdlog::info("queued {} ({} bytes, TOI {})", f.path, f.length, f.toi);
    }
  }

  // Pump packets until everything's been transmitted. Honour the
  // encoder's rate-limit deadline by sleeping until the next due time.
  while (encoder.has_pending()) {
    if (!encoder.send_next_packet()) {
      auto due = encoder.next_send_due();
      if (due) {
        auto now = std::chrono::steady_clock::now();
        if (*due > now) std::this_thread::sleep_until(*due);
      } else {
        break;
      }
    }
  }
  // Drain any FDT-only packets the encoder might still have queued.
  encoder.flush();

  close(sock);
  return 0;
}
