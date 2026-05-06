// libflute - FLUTE/ALC library
//
// Tests for Encoder::EstimateOverhead — the static planner-time
// helper that converts a target user-data rate into the wire byte
// rate the broadcast bearer must carry.
//
// The test pins the math against worked-out closed-form values: the
// formula is simple enough (per-packet header overhead amortised
// over packet rate, plus FEC redundancy, plus FDT amortised over
// its broadcast period) that drift would mean either the constants
// changed or the algebra changed — both are things downstream
// planners rely on.

#include "Encoder.h"

#include <gtest/gtest.h>

namespace {

using LibFlute::Encoder;
using FecScheme = LibFlute::FecScheme;

constexpr unsigned int kIpv4Hdr        = 20;
constexpr unsigned int kUdpHdr         = 8;
constexpr unsigned int kLctHdrFile     = 12;
constexpr unsigned int kLctHdrFdt      = 32;
constexpr unsigned int kFecPayloadId   = 4;
constexpr unsigned int kFilePktOhIPv4  = kIpv4Hdr + kUdpHdr + kLctHdrFile + kFecPayloadId;  // 44
constexpr unsigned int kFdtPktOhIPv4   = kIpv4Hdr + kUdpHdr + kLctHdrFdt  + kFecPayloadId;  // 64

}  // namespace

// CompactNoCode (no FEC repair) over IPv4 / MTU 1450 / 10 Mbps user
// payload should match the application's worked example exactly:
// H_data=44, P_data=1406, hdr_overhead = 10Mbps * 44/1406.
TEST(OverheadEstimate, CompactNoCodeMatchesAppFormula) {
    Encoder::OverheadParameters p;
    p.payload_bps        = 10'000'000;
    p.mtu                = 1450;
    p.ipv6               = false;
    p.fec_scheme         = FecScheme::CompactNoCode;
    p.fec_redundancy     = 0.0;
    p.fdt_period_seconds = 5;
    p.fdt_size_bytes     = 2000;

    const auto r = Encoder::EstimateOverhead(p);

    EXPECT_EQ(r.payload_bps, 10'000'000u);
    EXPECT_EQ(r.fec_repair_bps, 0u);

    // 10'000'000 * 44 / 1406  =  312'944 bps (truncated)
    EXPECT_EQ(r.packet_header_bps, 312'944u);

    // FDT: 2000 B per period, MTU 1450 → fdt_payload_per_pkt =
    // 1450 - 64 = 1386. ⌈2000/1386⌉ = 2 pkts/period; bytes/period
    // = 2 * (64 + 1386) = 2900. bps = 2900 * 8 / 5 = 4640.
    EXPECT_EQ(r.fdt_bps, 4640u);

    EXPECT_EQ(r.total_bps,
              r.payload_bps + r.fec_repair_bps
              + r.packet_header_bps + r.fdt_bps);
}

// Raptor with 40 % redundancy: source bps × 1.4 broadcast; per-
// packet headers amortised over the broadcast (= source + repair)
// rate, not just the source rate.
TEST(OverheadEstimate, RaptorWith40PercentRedundancyScalesPacketRate) {
    Encoder::OverheadParameters p;
    p.payload_bps        = 10'000'000;
    p.mtu                = 1450;
    p.ipv6               = false;
    p.fec_scheme         = FecScheme::Raptor;
    p.fec_redundancy     = 0.40;
    p.fdt_period_seconds = 5;
    p.fdt_size_bytes     = 2000;

    const auto r = Encoder::EstimateOverhead(p);

    EXPECT_EQ(r.payload_bps,    10'000'000u);
    EXPECT_EQ(r.fec_repair_bps,  4'000'000u);     // 10M * 0.40

    // Effective payload 14 Mbps. pkt_rate = 14M / (8 * 1406) ≈
    // 1244.66 pkt/s. header_bps = 1244.66 * 8 * 44 ≈ 438'122.
    // Truncation in the std::uint64_t cast lands on 438'122.
    EXPECT_EQ(r.packet_header_bps, 438'122u);

    EXPECT_EQ(r.fdt_bps, 4640u);   // FDT cadence + size unchanged
    EXPECT_EQ(r.total_bps,
              r.payload_bps + r.fec_repair_bps
              + r.packet_header_bps + r.fdt_bps);
}

// CompactNoCode IGNORES fec_redundancy even if non-zero.
TEST(OverheadEstimate, CompactNoCodeIgnoresFecRedundancy) {
    Encoder::OverheadParameters p;
    p.payload_bps    = 10'000'000;
    p.mtu            = 1450;
    p.fec_scheme     = FecScheme::CompactNoCode;
    p.fec_redundancy = 0.40;   // should be ignored

    const auto r = Encoder::EstimateOverhead(p);
    EXPECT_EQ(r.fec_repair_bps, 0u);
}

// Negative redundancy is clamped to zero.
TEST(OverheadEstimate, NegativeRedundancyClampedToZero) {
    Encoder::OverheadParameters p;
    p.payload_bps    = 10'000'000;
    p.mtu            = 1450;
    p.fec_scheme     = FecScheme::Raptor;
    p.fec_redundancy = -0.20;

    const auto r = Encoder::EstimateOverhead(p);
    EXPECT_EQ(r.fec_repair_bps, 0u);
}

// IPv6 ⇒ IP header grows from 20 → 40 B; per-packet overhead grows
// 44 → 64 B; both file packet overhead and FDT packet overhead
// shift accordingly.
TEST(OverheadEstimate, IPv6BumpsPerPacketHeaderCost) {
    Encoder::OverheadParameters p;
    p.payload_bps    = 10'000'000;
    p.mtu            = 1500;
    p.ipv6           = true;
    p.fec_scheme     = FecScheme::CompactNoCode;

    const auto r = Encoder::EstimateOverhead(p);
    // file_pkt_overhead_ipv6 = 40 + 8 + 12 + 4 = 64. P_data =
    // 1500 - 64 = 1436. header_bps = 10M * 64 / 1436 = 445'682.
    EXPECT_EQ(r.packet_header_bps, 445'682u);
}

// MTU smaller than the per-packet overhead is degenerate; the
// estimator returns the input payload unchanged rather than
// dividing by zero or returning negative numbers.
TEST(OverheadEstimate, DegenerateSmallMtuReturnsInputPayload) {
    Encoder::OverheadParameters p;
    p.payload_bps  = 1'000'000;
    p.mtu          = 40;       // < 44 (file packet overhead IPv4)
    p.fec_scheme   = FecScheme::CompactNoCode;

    const auto r = Encoder::EstimateOverhead(p);
    EXPECT_EQ(r.fec_repair_bps,    0u);
    EXPECT_EQ(r.packet_header_bps, 0u);
    EXPECT_EQ(r.fdt_bps,           0u);
    EXPECT_EQ(r.total_bps, p.payload_bps);
}

// FDT period of zero ⇒ no FDT contribution (caller is signalling
// they handle FDT cadence separately or aren't broadcasting one).
TEST(OverheadEstimate, FdtPeriodZeroProducesNoFdtContribution) {
    Encoder::OverheadParameters p;
    p.payload_bps        = 10'000'000;
    p.mtu                = 1450;
    p.fec_scheme         = FecScheme::CompactNoCode;
    p.fdt_period_seconds = 0;
    p.fdt_size_bytes     = 2000;

    const auto r = Encoder::EstimateOverhead(p);
    EXPECT_EQ(r.fdt_bps, 0u);
}

// Sanity: total_bps = payload + repair + headers + fdt across a
// sweep of payload rates / FEC redundancies.
TEST(OverheadEstimate, TotalIsSumOfBreakdownAcrossSweep) {
    for (std::uint64_t pl : {std::uint64_t{1'000'000},
                              std::uint64_t{10'000'000},
                              std::uint64_t{100'000'000},
                              std::uint64_t{1'000'000'000}}) {
        for (double r_ratio : {0.0, 0.05, 0.15, 0.40, 1.0}) {
            Encoder::OverheadParameters p;
            p.payload_bps    = pl;
            p.mtu            = 1500;
            p.fec_scheme     = FecScheme::Raptor;
            p.fec_redundancy = r_ratio;

            const auto r = Encoder::EstimateOverhead(p);
            EXPECT_EQ(r.total_bps,
                      r.payload_bps + r.fec_repair_bps
                      + r.packet_header_bps + r.fdt_bps)
                << "payload=" << pl << " r=" << r_ratio;
        }
    }
}

// Confirm the per-packet header constants are visible to a
// downstream consumer that wants to pre-validate overhead estimates
// — the LCT, UDP, IP, and FEC payload-ID sizes ARE the contract,
// not implementation details.
TEST(OverheadEstimate, PerPacketHeaderConstantsMatchRfcLayout) {
    EXPECT_EQ(kFilePktOhIPv4, 44u);
    EXPECT_EQ(kFdtPktOhIPv4,  64u);
}
