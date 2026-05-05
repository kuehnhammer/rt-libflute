// Probe: how big is bitstem-r10's DecodingSchedule for the K values
// libflute actually exercises? Used to size the per-K schedule cache
// proposed as round-8 lever 3 in tests/COVERAGE.md.
//
// Output columns:
//   K        : source-symbol count
//   L        : matrix cols (K + S + H)
//   ops      : ScheduleOp count
//   c_bytes  : sizeof(c[]) = L * 4
//   ops_bytes: ops * sizeof(ScheduleOp) (≈ 12)
//   total    : c_bytes + ops_bytes (the bytes we'd cache per-K)

#include <bitstem/r10/r10.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bitstem/r10/internal/fast/decoder.hpp"
#include "bitstem/r10/internal/fast/matrix.hpp"

int main() {
    std::printf("%6s  %6s  %10s  %10s  %12s  %12s\n",
                "K", "L", "ops", "c_bytes", "ops_bytes", "total");
    std::printf("%6s  %6s  %10s  %10s  %12s  %12s\n",
                "------", "------", "----------", "----------",
                "------------", "------------");

    // K values exercised by tests/bench/flute_bench scenarios:
    //   F=10 MB / mtu=1500 → K ≈ 1080 (Z=1)
    //   F=100 MB → K ≈ 8192 (Z=9, KL≈8030)
    //   F=500 MB → K ≈ 8192 (Z=42, KL≈8174)
    //   F=11.4 MB → K = 4097 / 4098 (Z=2)
    // Plus a few corners.
    const std::vector<std::uint16_t> probes = {
        10, 100, 1024, 4097, 4098, 5000, 8000, 8192,
    };

    for (auto K : probes) {
        // Same matrix path the encoder takes (`Encoder::Create`):
        // BuildPreCodingMatrix(K) yields the K×L matrix the
        // inactivation decoder schedules into the L intermediate
        // symbols. The receiver path additionally folds in the
        // received ESI's LT rows; the schedule shape is in the same
        // ballpark size-wise.
        auto A = bitstem::r10::fast::BuildPreCodingMatrix(K);
        auto sched =
            bitstem::r10::fast::decoder::BuildDecodingScheduleInactivation(A);
        if (!sched.has_value()) {
            std::printf("%6u  -- BuildDecodingScheduleInactivation failed --\n",
                        static_cast<unsigned>(K));
            continue;
        }
        const std::size_t c_bytes   = sched->c.size() * sizeof(std::uint32_t);
        const std::size_t ops_bytes =
            sched->ops.size() *
            sizeof(bitstem::r10::fast::decoder::ScheduleOp);
        const std::size_t total = c_bytes + ops_bytes;
        std::printf("%6u  %6u  %10zu  %10zu  %12zu  %12zu\n",
                    static_cast<unsigned>(K),
                    sched->L, sched->ops.size(), c_bytes, ops_bytes, total);
    }
    return 0;
}
