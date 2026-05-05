// libflute - FLUTE/ALC library
//
// Demo transmitter: opens a plain POSIX UDP socket and pumps packets
// from a LibFlute::Encoder to a multicast (or unicast) destination.
// Network handling lives entirely here in the example, not in the
// library — this is the pattern consumers should mirror.
//
#include <argp.h>
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

argp_option options[] = {
    {"target", 'm', "IP", 0, "Target multicast address (default: 238.1.1.95)", 0},
    {"port", 'p', "PORT", 0, "Target port (default: 40085)", 0},
    {"mtu", 't', "BYTES", 0, "Path MTU to size ALC packets for (default: 1500)", 0},
    {"rate-limit", 'r', "KBPS", 0, "Transmit rate limit in kbps; 0 = unlimited (default: 1000)", 0},
    {"fec", 'f', "FEC", 0, "FEC scheme: 0 = Compact No-Code, 1 = Raptor (default: 0)", 0},
    {"tsi", 's', "TSI", 0, "Session TSI (default: 16)", 0},
    {"log-level", 'l', "LEVEL", 0, "Log verbosity 0..6 (default: 2)", 0},
    {nullptr, 0, nullptr, 0, nullptr, 0},
};

error_t parse_opt(int key, char* arg, argp_state* state) {
  auto* a = static_cast<Args*>(state->input);
  switch (key) {
    case 'm': a->mcast_target = arg; break;
    case 'p': a->mcast_port    = static_cast<unsigned short>(strtoul(arg, nullptr, 10)); break;
    case 't': a->mtu           = static_cast<unsigned short>(strtoul(arg, nullptr, 10)); break;
    case 'r': a->rate_limit_kbps = static_cast<std::uint32_t>(strtoul(arg, nullptr, 10)); break;
    case 'f': a->fec           = static_cast<unsigned>(strtoul(arg, nullptr, 10)); break;
    case 's': a->tsi           = strtoull(arg, nullptr, 10); break;
    case 'l': a->log_level     = static_cast<unsigned>(strtoul(arg, nullptr, 10)); break;
    case ARGP_KEY_NO_ARGS: argp_usage(state); break;
    case ARGP_KEY_ARG:
      a->files = &state->argv[state->next - 1];
      state->next = state->argc;
      break;
    default: return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

void print_version(FILE* stream, argp_state*) {
  std::fprintf(stream, "%d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
}

}  // namespace

void (*argp_program_version_hook)(FILE*, argp_state*) = print_version;

int main(int argc, char** argv) {
  Args args;
  argp argp_spec = {options, parse_opt, "[FILE...]",
                     "FLUTE/ALC transmitter demo (plain POSIX UDP).",
                     nullptr, nullptr, nullptr};
  argp_parse(&argp_spec, argc, argv, 0, nullptr, &args);

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
