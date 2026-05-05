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
#include <argp.h>
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
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

struct Args {
  const char* capture_file = nullptr;
  const char* mcast_target = "238.1.1.95";
  unsigned short mcast_port = 40085;
  unsigned log_level = 2;
  std::uint64_t tsi = 16;
  const char* download_dir = nullptr;
};

argp_option options[] = {
    {"capture-file", 'c', "FILE", 0, "Pcap capture file to replay (REQUIRED)", 0},
    {"target", 'm', "IP", 0, "Multicast (or unicast) target IP to filter (default: 238.1.1.95)", 0},
    {"port", 'p', "PORT", 0, "UDP port (default: 40085)", 0},
    {"tsi", 't', "TSI", 0, "Session TSI (default: 16)", 0},
    {"log-level", 'l', "LEVEL", 0, "Log verbosity 0..6 (default: 2)", 0},
    {"download-dir", 'd', "DIR", 0, "Where to write received files (default: cwd)", 0},
    {nullptr, 0, nullptr, 0, nullptr, 0},
};

error_t parse_opt(int key, char* arg, argp_state* state) {
  auto* a = static_cast<Args*>(state->input);
  switch (key) {
    case 'c': a->capture_file = arg; break;
    case 'm': a->mcast_target = arg; break;
    case 'p': a->mcast_port   = static_cast<unsigned short>(strtoul(arg, nullptr, 10)); break;
    case 't': a->tsi          = strtoull(arg, nullptr, 10); break;
    case 'l': a->log_level    = static_cast<unsigned>(strtoul(arg, nullptr, 10)); break;
    case 'd': a->download_dir = arg; break;
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
  argp argp_spec = {options, parse_opt, nullptr,
                     "FLUTE/ALC pcap-replay receiver demo.",
                     nullptr, nullptr, nullptr};
  argp_parse(&argp_spec, argc, argv, 0, nullptr, &args);

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
    if (hdr->caplen < link_offset + sizeof(iphdr) + sizeof(udphdr)) continue;

    const auto* ip = reinterpret_cast<const iphdr*>(pkt + link_offset);
    const std::size_t ip_hl = static_cast<std::size_t>(ip->ihl) * 4U;
    if (ip->version != 4 || ip->protocol != IPPROTO_UDP) continue;
    if (hdr->caplen < link_offset + ip_hl + sizeof(udphdr)) continue;

    const auto* udp =
        reinterpret_cast<const udphdr*>(pkt + link_offset + ip_hl);
    const std::size_t udp_payload_offset = link_offset + ip_hl + sizeof(udphdr);
    const std::size_t udp_payload_len =
        static_cast<std::size_t>(ntohs(udp->len)) - sizeof(udphdr);
    if (hdr->caplen < udp_payload_offset + udp_payload_len) continue;

    decoder.feed_packet({pkt + udp_payload_offset, udp_payload_len});
  }

  pcap_close(cap);
  return 0;
}
