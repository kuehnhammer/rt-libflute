// Source-block partitioning — RFC 5052 §9.1 + RFC 6726 §3.2.
//
// RFC 5052 §9.1 algorithm:
//   T  = encoding_symbol_length
//   B  = max_source_block_length (in symbols)
//   F  = transfer_length (in bytes)
//
//   N_S          = ceil(F / T)                 // total source symbols
//   N_blk        = ceil(N_S / B)               // total source blocks
//   A_large      = ceil(N_S / N_blk)           // symbols/block, large
//   A_small      = floor(N_S / N_blk)          // symbols/block, small
//   N_large_blks = N_S - A_small * N_blk       // count of large blocks
//                                              // (the rest are small)
//
// The first N_large_blks source blocks have A_large symbols; the
// remaining N_blk - N_large_blks have A_small symbols. The total count
// of source symbols across all blocks equals N_S.
//
// File is opaque about its partitioning fields, so these tests verify
// behaviour through the public API:
//   - encoder side: enumerate get_next_symbols(very_large) and group
//     results by source_block_number
//   - receiver side: construct File from FileEntry, put_symbol() each
//     symbol, assert complete() flips on the last one

#include "File.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "EncodingSymbol.h"
#include "FileDeliveryTable.h"
#include "flute_types.h"

namespace {

LibFlute::FecOti CompactNoCodeOti(std::uint64_t F, std::uint32_t T,
                                   std::uint32_t max_sbl) {
    LibFlute::FecOti oti{};
    oti.encoding_id = LibFlute::FecScheme::CompactNoCode;
    oti.transfer_length = F;
    oti.encoding_symbol_length = T;
    oti.max_source_block_length = max_sbl;
    return oti;
}

// Hand-roll a sender-side File. Owns its data buffer so the File can
// safely reference it.
struct OwnedFile {
    std::vector<char> data;
    std::unique_ptr<LibFlute::File> file;
};

OwnedFile MakeSenderFile(std::uint64_t F, std::uint32_t T,
                          std::uint32_t max_sbl) {
    OwnedFile of;
    of.data.assign(F, 'x');
    auto oti = CompactNoCodeOti(F, T, max_sbl);
    of.file = std::make_unique<LibFlute::File>(
        /*toi=*/1, oti, /*content_location=*/std::string("part.bin"),
        /*content_type=*/std::string("application/octet-stream"),
        /*expires=*/0,
        of.data.data(), of.data.size(),
        /*copy_data=*/false);
    return of;
}

// Group the encoder's enumerated symbols by source_block_number so we
// can recover the per-block symbol count without peeking at private
// partitioning state.
std::map<std::uint32_t, std::vector<LibFlute::EncodingSymbol>>
EnumerateSymbolsBySbn(LibFlute::File& file, std::uint32_t T) {
    // Pull all symbols by repeatedly draining get_next_symbols with a
    // big budget — but we need to ALSO mark them complete to advance
    // past completed blocks. For partitioning checks we don't need
    // completion; just take one large pull.
    auto symbols = file.get_next_symbols(/*max_size=*/T * 1'000'000ULL);
    std::map<std::uint32_t, std::vector<LibFlute::EncodingSymbol>> byblk;
    for (auto& s : symbols) {
        byblk[s.source_block_number()].emplace_back(std::move(s));
    }
    return byblk;
}

}  // namespace

// RFC 5052 §9.1: total source symbols N_S = ceil(F / T). For F that's
// not an exact multiple of T, the last symbol is partial — but the
// COUNT of source symbols is still ceil(F/T).
TEST(FilePartitioning, NofSourceSymbolsCeilTransferLengthDivT) {
    constexpr std::uint32_t T = 1024;
    constexpr std::uint32_t kMaxSbl = 64;

    // Exact multiple: F = 4 * T → exactly 4 symbols.
    {
        auto of = MakeSenderFile(/*F=*/4U * T, T, kMaxSbl);
        auto byblk = EnumerateSymbolsBySbn(*of.file, T);
        std::size_t total = 0;
        for (const auto& [_, v] : byblk) total += v.size();
        EXPECT_EQ(total, 4U);
    }

    // Non-exact multiple: F = 4*T + 100 → ceil = 5 symbols.
    {
        auto of = MakeSenderFile(/*F=*/4U * T + 100U, T, kMaxSbl);
        auto byblk = EnumerateSymbolsBySbn(*of.file, T);
        std::size_t total = 0;
        for (const auto& [_, v] : byblk) total += v.size();
        EXPECT_EQ(total, 5U);
    }
}

// RFC 5052 §9.1 worked example: F = 65 KiB, T = 1024, B = 16.
//   N_S          = ceil(66560 / 1024)  = 65
//   N_blk        = ceil(65 / 16)       = 5
//   A_large      = ceil(65 / 5)        = 13
//   A_small      = floor(65 / 5)       = 13   (equal!)
//   N_large_blks = 65 - 13*5           = 0
// So all 5 blocks have 13 symbols. Pick a less-symmetric case for the
// "large vs small" coverage:
//   F = 60 KiB + 100, T = 1024, B = 16
//   N_S          = ceil(61540 / 1024)  = 61
//   N_blk        = ceil(61 / 16)       = 4
//   A_large      = ceil(61 / 4)        = 16
//   A_small      = floor(61 / 4)       = 15
//   N_large_blks = 61 - 15*4           = 1
// → block 0 has 16 symbols, blocks 1..3 have 15 symbols each.
TEST(FilePartitioning, MatchesRfc5052Section91WhenLargeAndSmallDiffer) {
    constexpr std::uint32_t T = 1024;
    constexpr std::uint32_t kMaxSbl = 16;
    constexpr std::uint64_t F = 60ULL * 1024ULL + 100ULL;  // 61540

    auto of = MakeSenderFile(F, T, kMaxSbl);
    auto byblk = EnumerateSymbolsBySbn(*of.file, T);

    ASSERT_EQ(byblk.size(), 4U);
    EXPECT_EQ(byblk[0].size(), 16U);
    EXPECT_EQ(byblk[1].size(), 15U);
    EXPECT_EQ(byblk[2].size(), 15U);
    EXPECT_EQ(byblk[3].size(), 15U);

    std::size_t total = 0;
    for (const auto& [_, v] : byblk) total += v.size();
    EXPECT_EQ(total, 61U);  // matches N_S
}

// File::get_next_symbols(max_size) MUST cap the returned symbol count
// at floor(max_size / T). At max_size < T it returns zero; at exactly
// k*T it returns ≤ k symbols (less if fewer remain).
TEST(FilePartitioning, GetNextSymbolsReturnsCorrectCountForGivenMaxSize) {
    constexpr std::uint32_t T = 256;
    constexpr std::uint32_t kMaxSbl = 8;
    constexpr std::uint64_t F = 8ULL * T;  // exactly 1 block of 8 symbols

    auto of = MakeSenderFile(F, T, kMaxSbl);

    // max_size below one symbol: zero symbols emitted.
    EXPECT_EQ(of.file->get_next_symbols(T - 1).size(), 0U);

    // max_size = 3 * T: at most 3 symbols.
    auto three = of.file->get_next_symbols(3U * T);
    EXPECT_EQ(three.size(), 3U);

    // mark them done so the next pull skips them; then ask for 100
    // symbol slots — we should only get the 5 remaining.
    of.file->mark_completed(three, /*success=*/true);
    auto rest = of.file->get_next_symbols(100U * T);
    EXPECT_EQ(rest.size(), 5U);
}

// Receiver-side end-to-end at the partitioning layer: build a File
// from an FDT entry, feed it every source symbol via put_symbol(), and
// verify that complete() flips exactly when the last symbol arrives.
// CompactNoCode (no FEC repair, no Raptor): completion = "every source
// symbol of every source block has been received".
TEST(FilePartitioning, CheckFileCompletionFiresWhenAllBlocksCompleteNoFec) {
    constexpr std::uint32_t T = 64;
    constexpr std::uint32_t kMaxSbl = 4;
    constexpr std::uint64_t F = 9ULL * T;  // 9 symbols, B=4 → 3 blocks

    LibFlute::FileDeliveryTable::FileEntry fe{};
    fe.toi = 1;
    fe.content_location = "rx.bin";
    fe.content_length = F;
    fe.content_md5 = "";
    fe.content_type = "";
    fe.expires = 0;
    fe.fec_oti = CompactNoCodeOti(F, T, kMaxSbl);
    fe.fec_transformer = nullptr;

    LibFlute::File rx_file(fe);
    EXPECT_FALSE(rx_file.complete());

    // Generate 9 unique symbols' worth of data and feed them all in.
    // Use a sender File over the same OTI to enumerate the symbol
    // structure (SBN/ESI partition exactly mirrors the receiver's
    // expected layout under RFC 5052 §9.1).
    auto sender = MakeSenderFile(F, T, kMaxSbl);
    auto sender_symbols = sender.file->get_next_symbols(/*max_size=*/F * 4);
    ASSERT_EQ(sender_symbols.size(), 9U);

    for (std::size_t i = 0; i < sender_symbols.size(); ++i) {
        rx_file.put_symbol(sender_symbols[i]);
        if (i + 1 < sender_symbols.size()) {
            EXPECT_FALSE(rx_file.complete()) << "early-complete at i=" << i;
        }
    }
    EXPECT_TRUE(rx_file.complete());
}
