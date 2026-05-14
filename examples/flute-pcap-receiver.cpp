// libflute - FLUTE/ALC library
//
// Demo receiver: reads a pcap capture from disk, picks UDP packets
// matching the requested target/port, and feeds the payloads into a
// LibFlute::Decoder. Useful for offline replay and debugging.
//
// Network handling (libpcap dependency, Ethernet/IP/UDP parsing) lives
// in this example, not in the library — the library has no idea
// where the bytes came from.
//
#include <getopt.h>
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <pcap.h>
#include <syslog.h>

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

// Wire-layout structs for the bytes we read off pcap frames. Linux's
// <netinet/ip.h> ships `struct iphdr` and BSD/macOS's ships `struct
// ip` instead, with different field names for the same on-wire
// layout (RFC 791 + 768). Rather than #ifdef'ing per platform, parse
// the bytes ourselves — the IPv4 + UDP headers are fixed format and
// trivially POD-mappable.
#pragma pack(push, 1)
struct WireIPv4 {
  std::uint8_t  ver_ihl;     // version<<4 | ihl
  std::uint8_t  tos;
  std::uint16_t total_len;
  std::uint16_t id;
  std::uint16_t flags_frag;
  std::uint8_t  ttl;
  std::uint8_t  protocol;
  std::uint16_t check;
  std::uint32_t saddr;
  std::uint32_t daddr;
};
static_assert(sizeof(WireIPv4) == 20, "IPv4 header is 20 bytes on the wire");

struct WireUDP {
  std::uint16_t sport;
  std::uint16_t dport;
  std::uint16_t len;
  std::uint16_t check;
};
static_assert(sizeof(WireUDP) == 8, "UDP header is 8 bytes on the wire");
#pragma pack(pop)

struct Args {
  const char* capture_file = nullptr;
  const char* mcast_target = "238.1.1.95";
  unsigned short mcast_port = 40085;
  unsigned log_level = 2;
  std::uint64_t tsi = 16;
  const char* download_dir = nullptr;
};

// getopt_long is POSIX. argp was GNU-libc-only, which kept the
// examples Linux-host-only; this replacement compiles on macOS too
// (BSD libc) without changing the user-facing CLI surface.
const option long_options[] = {
    {"capture-file", required_argument, nullptr, 'c'},
    {"target",       required_argument, nullptr, 'm'},
    {"port",         required_argument, nullptr, 'p'},
    {"tsi",          required_argument, nullptr, 't'},
    {"log-level",    required_argument, nullptr, 'l'},
    {"download-dir", required_argument, nullptr, 'd'},
    {"help",         no_argument,       nullptr, 'h'},
    {"version",      no_argument,       nullptr, 'V'},
    {nullptr, 0, nullptr, 0},
};
constexpr const char* kShortOptions = "c:m:p:t:l:d:hV";

void print_usage(const char* prog) {
  std::fprintf(stderr,
      "FLUTE/ALC pcap-replay receiver demo.\n"
      "Usage: %s --capture-file=<pcap> [OPTIONS]\n"
      "\n"
      "  -c, --capture-file FILE   Pcap capture file to replay (REQUIRED)\n"
      "  -m, --target IP           Multicast (or unicast) target IP to filter (default: 238.1.1.95)\n"
      "  -p, --port PORT           UDP port (default: 40085)\n"
      "  -t, --tsi TSI             Session TSI (default: 16)\n"
      "  -l, --log-level LEVEL     Log verbosity 0..6 (default: 2)\n"
      "  -d, --download-dir DIR    Where to write received files (default: cwd)\n"
      "  -h, --help                Show this help and exit\n"
      "  -V, --version             Show version and exit\n",
      prog);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  int c;
  while ((c = ::getopt_long(argc, argv, kShortOptions,
                             long_options, nullptr)) != -1) {
    switch (c) {
      case 'c': args.capture_file = optarg; break;
      case 'm': args.mcast_target = optarg; break;
      case 'p': args.mcast_port   = static_cast<unsigned short>(strtoul(optarg, nullptr, 10)); break;
      case 't': args.tsi          = strtoull(optarg, nullptr, 10); break;
      case 'l': args.log_level    = static_cast<unsigned>(strtoul(optarg, nullptr, 10)); break;
      case 'd': args.download_dir = optarg; break;
      case 'h': print_usage(argv[0]); return 0;
      case 'V':
        std::fprintf(stdout, "%d.%d.%d\n",
                     VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        return 0;
      case '?': return 1;
      default:  return 1;
    }
  }

  if (args.capture_file == nullptr) {
    std::fprintf(stderr, "usage: --capture-file=<pcap>\n");
    return 1;
  }

  auto syslog_sink = spdlog::syslog_logger_mt(
      "syslog", "flute-pcap-receiver", LOG_PID | LOG_PERROR | LOG_CONS);
  spdlog::set_default_logger(syslog_sink);
  spdlog::set_level(static_cast<spdlog::level::level_enum>(args.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f] [%^%l%$] %v");

  // Open the pcap file. Build a BPF filter so libpcap drops anything
  // that isn't UDP for the requested target/port — saves us walking
  // every frame in C++.
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t* cap = pcap_open_offline(args.capture_file, errbuf);
  if (cap == nullptr) {
    spdlog::error("pcap_open_offline({}): {}", args.capture_file, errbuf);
    return 1;
  }

  std::string filter = "udp dst port " + std::to_string(args.mcast_port) +
                        " and dst host " + args.mcast_target;
  bpf_program bpf;
  if (pcap_compile(cap, &bpf, filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
    spdlog::error("pcap_compile({}): {}", filter, pcap_geterr(cap));
    pcap_close(cap);
    return 1;
  }
  if (pcap_setfilter(cap, &bpf) < 0) {
    spdlog::error("pcap_setfilter: {}", pcap_geterr(cap));
    pcap_freecode(&bpf);
    pcap_close(cap);
    return 1;
  }
  pcap_freecode(&bpf);

  LibFlute::Decoder decoder(args.tsi);
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
      });

  // Walk the captured frames. Assume Ethernet → IP v4 → UDP layout
  // (matches libflute's wire on real Ethernet links). If the pcap was
  // taken on a loopback interface the link type is DLT_NULL with a
  // 4-byte AF_* prefix; handle that branch too.
  pcap_pkthdr* hdr;
  const u_char* pkt;
  const int link_type = pcap_datalink(cap);
  while (pcap_next_ex(cap, &hdr, &pkt) == 1) {
    std::size_t link_offset = 0;
    if (link_type == DLT_EN10MB) {
      link_offset = 14;  // Ethernet
    } else if (link_type == DLT_NULL || link_type == DLT_LOOP) {
      link_offset = 4;   // BSD loopback header
    } else if (link_type == DLT_RAW) {
      link_offset = 0;
    } else {
      spdlog::warn("unsupported pcap link type {}", link_type);
      continue;
    }
    if (hdr->caplen < link_offset + sizeof(WireIPv4) + sizeof(WireUDP)) continue;

    const auto* ip = reinterpret_cast<const WireIPv4*>(pkt + link_offset);
    const std::size_t ip_hl =
        static_cast<std::size_t>(ip->ver_ihl & 0x0F) * 4U;
    const std::uint8_t ip_version =
        static_cast<std::uint8_t>((ip->ver_ihl >> 4) & 0x0F);
    if (ip_version != 4 || ip->protocol != IPPROTO_UDP) continue;
    if (hdr->caplen < link_offset + ip_hl + sizeof(WireUDP)) continue;

    const auto* udp =
        reinterpret_cast<const WireUDP*>(pkt + link_offset + ip_hl);
    const std::size_t udp_payload_offset = link_offset + ip_hl + sizeof(WireUDP);
    const std::size_t udp_payload_len =
        static_cast<std::size_t>(ntohs(udp->len)) - sizeof(WireUDP);
    if (hdr->caplen < udp_payload_offset + udp_payload_len) continue;

    decoder.feed_packet({pkt + udp_payload_offset, udp_payload_len});
  }

  pcap_close(cap);
  return 0;
}
