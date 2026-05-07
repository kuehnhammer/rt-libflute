// libflute end-to-end round-trip benchmark.
//
// Round-trips deterministic data buffers through Encoder → Decoder
// in-process (no sockets) and times each scenario. Prints encoder /
// decoder wall-clock, throughput, packet counts, and FEC overhead.
//
// Built only when -DLIBFLUTE_BUILD_BENCH=ON. Not a gtest target;
// run it directly:
//   ./build/tests/bench/flute_bench
//
// Scenarios:
//   F = 10 MB / 100 MB / 500 MB,
//   FEC = CompactNoCode (lossless baseline, no FEC),
//         R10     (RFC 5053, K_max = 8192,  5% surplus, 2 source drops),
//         RaptorQ (RFC 6330, K_max = 56403, 5% surplus, 2 source drops).
//   Both Raptor / RaptorQ scenarios are gated on RAPTOR_ENABLED.
//
// Why source-symbol drops rather than lossless: the decoder's lossless
// short-circuit (every source ESI received → permute by ESI memcpy)
// reduces "lossless Raptor decode" to a CompactNoCode-with-extra-emit-
// overhead measurement — it never runs the matrix factor that's the
// actual cost of the FEC. Dropping two source ESIs in block 0 forces
// TryDecode through the matrix-factor / inactivation-decoder path
// for that one block, while keeping the loss small enough to recover
// inside any plausible repair budget. The 5 % surplus + 2-drop pattern
// targets the realistic broadcast regime: low loss, FEC-driven recovery.
//
// W (sub-block size target) defaults to TS 26.346 §B.3.4.1's normative
// 256 KB for R10 file delivery; the same value is used for RaptorQ
// (RFC 6330 §4.3 leaves WS as a deployment knob, no normative pin).
// At W=256 KB the autodetect picks N>1 for multi-MB blocks, so the
// codec's sub-block-interleaved path is what gets measured. Override
// with FLUTE_BENCH_W_BYTES=N for legacy comparison runs at N=1.
//
// Defaults can be overridden via env vars:
//   FLUTE_BENCH_SIZES_MB="10,100"      // comma-separated
//   FLUTE_BENCH_MTU=1500
//   FLUTE_BENCH_W_BYTES=262144         // sub-block size target (R10
//                                      // TS 26.346 normative). Bump to
//                                      // 16777216 to reproduce the
//                                      // pre-N>1 N=1 regime.
//   FLUTE_BENCH_SKIP_RAPTOR=1          // skip both R10 and RaptorQ scenarios
//   FLUTE_BENCH_DROP_EVERY=20          // drop every Nth file packet
//                                      // (TOI ≠ 0). When set, the bench
//                                      // also runs each Raptor / RaptorQ
//                                      // scenario in this loss regime
//                                      // (replacing the targeted-drop
//                                      // pattern in that one row).
//                                      // 0 = no extra sweep.
//   FLUTE_BENCH_NO_DECODE=1            // make the encoder's PacketCallback
//                                      // a no-op (skip decoder.feed_packet).
//                                      // Isolates pure encoder wall-clock.
//                                      // The bench will report
//                                      // ROUND-TRIP FAILED but encoder time
//                                      // is the meaningful number.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Decoder.h"
#include "Encoder.h"
#include "File.h"
#include "FileDeliveryTable.h"
#include "flute_types.h"
#include "spdlog/spdlog.h"

namespace {

using Clock = std::chrono::steady_clock;

double DurationMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

double DurationS(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

void FillDeterministic(char* p, std::size_t n) {
    // Cheap PRNG that doesn't bottleneck on the data generation.
    std::uint64_t s = 0xA5A5A5A5A5A5A5A5ULL;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = static_cast<char>((s >> 56) & 0xFFU);
    }
}

struct ScenarioResult {
    std::size_t F = 0;
    LibFlute::FecScheme fec = LibFlute::FecScheme::CompactNoCode;
    Clock::duration encoder_time{};
    Clock::duration decoder_time{};   // wall-clock time spent in feed_packet
    Clock::duration total_time{};
    bool ok = false;
    int drop_every = 0;
    std::size_t dropped_count = 0;
    std::size_t targeted_src_drops = 0;   // count of (sbn, esi) source-ESI drops
    std::optional<unsigned> redundancy_level;
    std::uint64_t sub_block_size_target = 0;
    LibFlute::EncoderStats es{};
    LibFlute::DecoderStats ds{};
};

// Inspect the LCT half-word TOI without round-tripping through
// AlcPacket. The Encoder always emits half_word_flag=1 / toi_flag=0,
// so TOI sits at offset 10..11 (LCT base 4 + CCI 4 + TSI half 2).
inline std::uint16_t ToiOf(std::span<const std::uint8_t> packet) {
    if (packet.size() < 12) return 0xFFFF;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[10]) << 8) | packet[11]);
}

// LCT codepoint at offset 3 (the 4th byte of the LCT base header).
// Maps directly to the FEC encoding ID — 0 = CompactNoCode, 1 = R10,
// 6 = RaptorQ — which is what we need to choose the FEC Payload ID
// byte layout when peeking at SBN/ESI below.
inline std::uint8_t CodepointOf(std::span<const std::uint8_t> packet) {
    if (packet.size() < 4) return 0xFFU;
    return packet[3];
}

// FEC Payload ID lives at offset 12 (LCT base 4 + CCI 4 + TSI half 2 +
// TOI half 2). Layout depends on the codepoint: R10 / CompactNoCode
// pack SBN(16) + ESI(16) per RFC 5052 §3.4.1; RaptorQ uses
// SBN(8) + ESI(24) per RFC 6330 §3.2.
struct PacketIds {
    std::uint16_t toi = 0xFFFFU;
    std::uint32_t sbn = 0;
    std::uint32_t esi = 0;
};
inline PacketIds IdsOf(std::span<const std::uint8_t> packet) {
    PacketIds ids;
    if (packet.size() < 16) return ids;
    ids.toi = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[10]) << 8) | packet[11]);
    if (packet[3] == 6) {
        ids.sbn = packet[12];
        ids.esi = (static_cast<std::uint32_t>(packet[13]) << 16) |
                   (static_cast<std::uint32_t>(packet[14]) <<  8) |
                    static_cast<std::uint32_t>(packet[15]);
    } else {
        ids.sbn = (static_cast<std::uint32_t>(packet[12]) << 8) | packet[13];
        ids.esi = (static_cast<std::uint32_t>(packet[14]) << 8) | packet[15];
    }
    return ids;
}

const char* FecName(LibFlute::FecScheme s) {
    switch (s) {
        case LibFlute::FecScheme::CompactNoCode: return "CompactNoCode";
        case LibFlute::FecScheme::Raptor:        return "R10";
        case LibFlute::FecScheme::RaptorQ:       return "RaptorQ";
        default:                                  return "?";
    }
}

struct ScenarioConfig {
    std::size_t                F;
    LibFlute::FecScheme        fec;
    unsigned                   mtu;
    // (SBN, ESI) pairs the harness deliberately drops on the wire.
    // Used to force the receiver into the lossy-decode (matrix factor)
    // path — pick ESIs in [0, K) so the loss hits *source* symbols and
    // the lossless short-circuit (= every source ESI received → permute
    // by ESI memcpy) never fires.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> targeted_drops;
    // Per-file FEC redundancy override (Rel-11 attribute). nullopt =
    // RaptorFEC default (15%).
    std::optional<unsigned>    redundancy_level;
    // Sub-block size target (RFC 5053 §4.2 / RFC 6330 §4.3 W). Drives
    // the autodetect for N (sub-blocks per source block). 0 = use
    // RaptorFEC's internal default (16 MB → biased toward N=1).
    std::uint64_t              sub_block_size_target = 0;
    int                        drop_every = 0;
    bool                       no_decode  = false;
};

ScenarioResult RunScenario(const ScenarioConfig& cfg) {
    ScenarioResult r;
    r.F   = cfg.F;
    r.fec = cfg.fec;
    r.drop_every = cfg.drop_every;
    r.redundancy_level = cfg.redundancy_level;

    auto data = std::make_unique<char[]>(cfg.F);
    FillDeterministic(data.get(), cfg.F);

    LibFlute::Decoder decoder(/*tsi=*/16);
    std::shared_ptr<LibFlute::File> received;
    decoder.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) { received = std::move(f); });

    Clock::duration decode_time_acc{};
    std::size_t file_packet_idx = 0;
    std::size_t dropped = 0;
    std::size_t targeted_dropped = 0;

    LibFlute::Encoder encoder(
        /*tsi=*/16, cfg.mtu, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> packet) -> bool {
            if (cfg.no_decode) {
                return true;
            }
            if (ToiOf(packet) != 0) {
                // Targeted source-ESI drops: parse the FEC payload ID
                // out of the packet (scheme-aware via codepoint) and
                // drop if (sbn, esi) is in the configured set. Each
                // matched drop counts once even if a packet bundles
                // multiple symbols — the bundle-vs-single distinction
                // is below the encoder's typical 1-symbol-per-packet
                // emit cadence at mtu 1500 anyway.
                if (!cfg.targeted_drops.empty()) {
                    const auto ids = IdsOf(packet);
                    for (const auto& [sbn, esi] : cfg.targeted_drops) {
                        if (ids.sbn == sbn && ids.esi == esi) {
                            ++targeted_dropped;
                            ++dropped;
                            return true;
                        }
                    }
                }
                if (cfg.drop_every > 0) {
                    ++file_packet_idx;
                    if (file_packet_idx %
                            static_cast<std::size_t>(cfg.drop_every) == 0) {
                        ++dropped;
                        return true;
                    }
                }
            }
            const auto t0 = Clock::now();
            decoder.feed_packet(packet);
            decode_time_acc += Clock::now() - t0;
            return true;
        });

    LibFlute::FileTransmissionConfig fec_cfg;
    fec_cfg.oti.encoding_id        = cfg.fec;
    fec_cfg.fec_redundancy_level   = cfg.redundancy_level;
    fec_cfg.sub_block_size_target  = cfg.sub_block_size_target;

    const auto t_start = Clock::now();
    auto toi = encoder.send("bench.bin", "application/octet-stream",
                              LibFlute::Encoder::seconds_since_epoch() + 60,
                              data.get(), cfg.F, fec_cfg,
                              /*copy_buffer=*/false);
    if (toi == 0) {
        std::fprintf(stderr, "encoder.send() failed for F=%zu fec=%s\n",
                      cfg.F, FecName(cfg.fec));
        return r;
    }
    encoder.flush();
    const auto t_end = Clock::now();
    r.total_time   = t_end - t_start;
    r.encoder_time = r.total_time - decode_time_acc;
    r.decoder_time = decode_time_acc;
    r.es           = encoder.stats();
    r.ds           = decoder.stats();
    r.dropped_count         = dropped;
    r.targeted_src_drops    = targeted_dropped;
    r.sub_block_size_target = cfg.sub_block_size_target;

    r.ok = (received != nullptr) &&
           received->complete() &&
           received->length() == cfg.F &&
           std::memcmp(received->buffer(), data.get(), cfg.F) == 0;
    return r;
}

void PrintHeader(unsigned mtu) {
    std::printf("== libflute round-trip benchmark (mtu=%u) ==\n\n", mtu);
    std::printf("%-7s  %-13s  %-9s  %-7s  %12s  %12s  %12s  %10s  %10s  %14s  %s\n",
                "F (MB)", "FEC", "loss", "W",
                "packets", "src syms", "rep syms",
                "enc (ms)", "dec (ms)", "throughput", "overhead");
    std::printf("%-7s  %-13s  %-9s  %-7s  %12s  %12s  %12s  %10s  %10s  %14s  %s\n",
                "-------", "-------------", "---------", "-------",
                "------------", "------------", "------------",
                "----------", "----------", "--------------", "--------");
}

// Format W as a short human-readable suffix (e.g. "256K", "16M").
// Returns "default" for sentinel 0 — the bench uses 0 to mean
// "let RaptorFEC pick" on schemes where W doesn't apply (e.g.
// CompactNoCode rows).
std::string WLabel(std::uint64_t w) {
    if (w == 0)               return "default";
    if (w >= 1ULL << 30)      return std::to_string(w >> 30) + "G";
    if (w >= 1ULL << 20)      return std::to_string(w >> 20) + "M";
    if (w >= 1ULL << 10)      return std::to_string(w >> 10) + "K";
    return std::to_string(w);
}

void PrintRow(const ScenarioResult& r) {
    char loss_label[24] = "lossless";
    if (r.targeted_src_drops > 0) {
        std::snprintf(loss_label, sizeof(loss_label),
                       "%zusrc-drop", r.targeted_src_drops);
    } else if (r.drop_every > 0) {
        std::snprintf(loss_label, sizeof(loss_label),
                       "1in%d", r.drop_every);
    }
    if (!r.ok) {
        // In FLUTE_BENCH_NO_DECODE mode the receiver never sees
        // packets, so round-trip will fail; print encoder timing
        // anyway since that's the meaningful column for that mode.
        if (std::getenv("FLUTE_BENCH_NO_DECODE") != nullptr) {
            const double mb_nd = static_cast<double>(r.F) /
                                 (1024.0 * 1024.0);
            std::printf("%-7.0f  %-13s  %-9s  %-7s  enc=%.1f ms  "
                        "dec=%.1f ms  (no-decode mode)\n",
                        mb_nd, FecName(r.fec), loss_label,
                        WLabel(r.sub_block_size_target).c_str(),
                        DurationMs(r.encoder_time),
                        DurationMs(r.decoder_time));
            return;
        }
        std::printf("%-7.0f  %-13s  %-9s  %-7s  ROUND-TRIP FAILED (%zu drops)\n",
                    static_cast<double>(r.F) / (1024.0 * 1024.0),
                    FecName(r.fec), loss_label,
                    WLabel(r.sub_block_size_target).c_str(),
                    r.dropped_count);
        return;
    }
    const double mb = static_cast<double>(r.F) / (1024.0 * 1024.0);
    const double mb_emitted =
        static_cast<double>(r.es.bytes_emitted) / (1024.0 * 1024.0);
    const double tput_mb_s = mb / DurationS(r.total_time);
    const double overhead_pct =
        r.es.source_symbols_emitted > 0
            ? 100.0 * static_cast<double>(r.es.repair_symbols_emitted) /
                  static_cast<double>(r.es.source_symbols_emitted)
            : 0.0;
    std::printf("%-7.0f  %-13s  %-9s  %-7s  %12llu  %12llu  %12llu  %10.1f  %10.1f  %11.1f MB/s  %6.1f%% (%6.2f MB)\n",
                mb, FecName(r.fec), loss_label,
                WLabel(r.sub_block_size_target).c_str(),
                static_cast<unsigned long long>(r.es.packets_emitted),
                static_cast<unsigned long long>(r.es.source_symbols_emitted),
                static_cast<unsigned long long>(r.es.repair_symbols_emitted),
                DurationMs(r.encoder_time), DurationMs(r.decoder_time),
                tput_mb_s, overhead_pct, mb_emitted - mb);
}

std::vector<std::size_t> ParseSizesEnv() {
    std::vector<std::size_t> out;
    const char* s = std::getenv("FLUTE_BENCH_SIZES_MB");
    if (s == nullptr || *s == '\0') {
        out = {10, 100, 500};
    } else {
        const char* p = s;
        while (*p) {
            char* end = nullptr;
            unsigned long mb = std::strtoul(p, &end, 10);
            if (end == p) break;
            if (mb > 0) out.push_back(static_cast<std::size_t>(mb));
            p = (*end == ',') ? end + 1 : end;
        }
        if (out.empty()) out = {10};
    }
    for (auto& mb : out) mb *= 1024U * 1024U;  // MB → bytes
    return out;
}

}  // namespace

int main() {
    spdlog::set_level(spdlog::level::warn);

    unsigned mtu = 1500;
    if (const char* m = std::getenv("FLUTE_BENCH_MTU"); m && *m) {
        mtu = static_cast<unsigned>(std::strtoul(m, nullptr, 10));
    }
    const bool skip_raptor = std::getenv("FLUTE_BENCH_SKIP_RAPTOR") != nullptr;
    int drop_every = 0;
    if (const char* d = std::getenv("FLUTE_BENCH_DROP_EVERY"); d && *d) {
        drop_every = static_cast<int>(std::strtol(d, nullptr, 10));
    }
    const bool no_decode = std::getenv("FLUTE_BENCH_NO_DECODE") != nullptr;

    // Per-scheme sub-block size target W. RFC 5053 §4.2 and RFC 6330
    // §4.3 use W differently:
    //   * R10 (§4.2): W bounds N (sub-blocks per source block); Z is
    //     K_max-bounded only. TS 26.346 §B.3.4.1 mandates 256 KB for
    //     MBMS file delivery — our default.
    //   * RaptorQ (§4.3): WS bounds K·T (per-block working memory),
    //     driving Z. Setting WS too small forces Z past RaptorQ's
    //     8-bit SBN wire-format ceiling (256). 16 MB is the typical
    //     deployment value; RFC 6330 leaves it as a knob.
    // FLUTE_BENCH_W_BYTES=N overrides BOTH defaults (sweep mode);
    // unset uses the per-scheme values above.
    std::uint64_t bench_w_r10     = 256ULL * 1024ULL;
    std::uint64_t bench_w_raptorq = 16ULL * 1024ULL * 1024ULL;
    if (const char* w = std::getenv("FLUTE_BENCH_W_BYTES"); w && *w) {
        const auto v = std::strtoull(w, nullptr, 10);
        bench_w_r10     = v;
        bench_w_raptorq = v;
    }

    // Default Raptor / RaptorQ scenarios run with a low repair budget
    // (5 % surplus) and deliberate source-symbol loss in block 0. This
    // forces the receiver into the matrix-factor decode path — the
    // lossless short-circuit (every source ESI received → permute by
    // ESI memcpy) doesn't fire when source ESIs are missing, so the
    // measurement reflects the actual cost of FEC recovery rather
    // than CompactNoCode-with-extra-emit-overhead. The two drops sit
    // in block 0 (always present, always source for our F sizes) so
    // exactly one block per file pays the decode cost.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>
        kSrcDrops{{0u, 5u}, {0u, 7u}};
    constexpr unsigned kBenchRedundancyPercent = 5u;

    PrintHeader(mtu);
    for (auto F : ParseSizesEnv()) {
        ScenarioConfig cnc{};
        cnc.F = F;
        cnc.fec = LibFlute::FecScheme::CompactNoCode;
        cnc.mtu = mtu;
        cnc.no_decode = no_decode;
        PrintRow(RunScenario(cnc));

#ifdef RAPTOR_ENABLED
        if (!skip_raptor) {
            for (auto fec : {LibFlute::FecScheme::Raptor,
                              LibFlute::FecScheme::RaptorQ}) {
                ScenarioConfig sc{};
                sc.F                     = F;
                sc.fec                   = fec;
                sc.mtu                   = mtu;
                sc.targeted_drops        = kSrcDrops;
                sc.redundancy_level      = kBenchRedundancyPercent;
                sc.sub_block_size_target =
                    (fec == LibFlute::FecScheme::Raptor) ? bench_w_r10
                                                         : bench_w_raptorq;
                sc.no_decode             = no_decode;
                PrintRow(RunScenario(sc));

                // Optional per-Nth-packet drop sweep on top, when the
                // user wants the lossy-budget regime instead of (or
                // alongside) the targeted source-loss measurement.
                if (drop_every > 0) {
                    sc.targeted_drops.clear();
                    sc.drop_every = drop_every;
                    PrintRow(RunScenario(sc));
                }
            }
        }
#else
        (void)skip_raptor;
        (void)drop_every;
        (void)bench_w_r10;
        (void)bench_w_raptorq;
#endif
        std::fflush(stdout);
    }
    return 0;
}
