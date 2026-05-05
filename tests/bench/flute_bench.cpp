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
//   FEC = CompactNoCode (no overhead) and Raptor (gated on
//         RAPTOR_ENABLED, ~15% surplus per RaptorFEC's
//         surplus_packet_ratio = 1.15).
//
// Defaults can be overridden via env vars:
//   FLUTE_BENCH_SIZES_MB="10,100"      // comma-separated
//   FLUTE_BENCH_MTU=1500
//   FLUTE_BENCH_SKIP_RAPTOR=1          // skip Raptor scenarios

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
    LibFlute::EncoderStats es{};
    LibFlute::DecoderStats ds{};
};

const char* FecName(LibFlute::FecScheme s) {
    switch (s) {
        case LibFlute::FecScheme::CompactNoCode: return "CompactNoCode";
        case LibFlute::FecScheme::Raptor:        return "Raptor";
        default:                                  return "?";
    }
}

ScenarioResult RunScenario(std::size_t F, LibFlute::FecScheme fec,
                              unsigned mtu) {
    ScenarioResult r;
    r.F   = F;
    r.fec = fec;

    auto data = std::make_unique<char[]>(F);
    FillDeterministic(data.get(), F);

    LibFlute::Decoder decoder(/*tsi=*/16);
    std::shared_ptr<LibFlute::File> received;
    decoder.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> f) { received = std::move(f); });

    Clock::duration decode_time_acc{};

    LibFlute::Encoder encoder(
        /*tsi=*/16, mtu, /*rate_limit_kbps=*/0,
        [&](std::span<const std::uint8_t> packet) -> bool {
            const auto t0 = Clock::now();
            decoder.feed_packet(packet);
            decode_time_acc += Clock::now() - t0;
            return true;
        });

    const auto t_start = Clock::now();
    auto toi = encoder.send("bench.bin", "application/octet-stream",
                              LibFlute::Encoder::seconds_since_epoch() + 60,
                              data.get(), F, fec, /*copy_buffer=*/false);
    if (toi == 0) {
        std::fprintf(stderr, "encoder.send() failed for F=%zu fec=%s\n",
                      F, FecName(fec));
        return r;
    }
    encoder.flush();
    const auto t_end = Clock::now();
    r.total_time   = t_end - t_start;
    r.encoder_time = r.total_time - decode_time_acc;
    r.decoder_time = decode_time_acc;
    r.es           = encoder.stats();
    r.ds           = decoder.stats();

    r.ok = (received != nullptr) &&
           received->complete() &&
           received->length() == F &&
           std::memcmp(received->buffer(), data.get(), F) == 0;
    return r;
}

void PrintHeader(unsigned mtu) {
    std::printf("== libflute round-trip benchmark (mtu=%u) ==\n\n", mtu);
    std::printf("%-7s  %-13s  %12s  %12s  %12s  %10s  %10s  %14s  %s\n",
                "F (MB)", "FEC",
                "packets", "src syms", "rep syms",
                "enc (ms)", "dec (ms)", "throughput", "overhead");
    std::printf("%-7s  %-13s  %12s  %12s  %12s  %10s  %10s  %14s  %s\n",
                "-------", "-------------",
                "------------", "------------", "------------",
                "----------", "----------", "--------------", "--------");
}

void PrintRow(const ScenarioResult& r) {
    if (!r.ok) {
        std::printf("%-7.0f  %-13s  ROUND-TRIP FAILED\n",
                    static_cast<double>(r.F) / (1024.0 * 1024.0),
                    FecName(r.fec));
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
    std::printf("%-7.0f  %-13s  %12llu  %12llu  %12llu  %10.1f  %10.1f  %11.1f MB/s  %6.1f%% (%6.2f MB)\n",
                mb, FecName(r.fec),
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

    PrintHeader(mtu);
    for (auto F : ParseSizesEnv()) {
        auto r = RunScenario(F, LibFlute::FecScheme::CompactNoCode, mtu);
        PrintRow(r);
#ifdef RAPTOR_ENABLED
        if (!skip_raptor) {
            auto rr = RunScenario(F, LibFlute::FecScheme::Raptor, mtu);
            PrintRow(rr);
        }
#else
        (void)skip_raptor;
#endif
        std::fflush(stdout);
    }
    return 0;
}
