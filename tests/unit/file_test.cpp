// LibFlute::File and the free function calculate_md5.
//
// Spec context:
//   RFC 6726 §3.4.2: Content-MD5 is OPTIONAL, but when present it MUST be
//                    computed correctly so receivers can validate file
//                    integrity. The libflute sender computes MD5 inside
//                    the File constructor (when File is built from a
//                    data buffer); a broken MD5 path means the FDT
//                    advertises wrong digests to the network.

#include "File.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/md5.h>

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

// Sender-side File that owns its data buffer. The receiver-side File
// uses references into the sender's buffer through put_symbol's
// EncodingSymbol — so the sender must outlive every symbol fed.
struct OwnedSenderFile {
    std::vector<char> data;
    std::unique_ptr<LibFlute::File> file;
};

OwnedSenderFile MakeSender(std::vector<char> bytes, std::uint32_t T,
                            std::uint32_t max_sbl) {
    OwnedSenderFile of;
    of.data = std::move(bytes);
    auto oti = CompactNoCodeOti(of.data.size(), T, max_sbl);
    of.file = std::make_unique<LibFlute::File>(
        /*toi=*/1, oti, std::string("rx.bin"),
        std::string("application/octet-stream"),
        /*expires=*/0,
        of.data.data(), of.data.size(),
        /*copy_data=*/false);
    return of;
}

// Build a receiver-side File from a CompactNoCode FileEntry. The
// caller decides what Content-MD5 string to advertise — tests use this
// to exercise good, bad, and absent integrity hashes.
LibFlute::FileDeliveryTable::FileEntry MakeReceiverEntry(
        std::uint64_t F, std::uint32_t T, std::uint32_t max_sbl,
        std::string content_md5) {
    LibFlute::FileDeliveryTable::FileEntry fe{};
    fe.toi = 1;
    fe.content_location = "rx.bin";
    fe.content_length = F;
    fe.content_md5 = std::move(content_md5);
    fe.content_type = "";
    fe.expires = 0;
    fe.fec_oti = CompactNoCodeOti(F, T, max_sbl);
    fe.fec_transformer = nullptr;
    return fe;
}

}  // namespace

// calculate_md5 reports invalid input via a negative sentinel return
// value. That sentinel must be observable to callers via `< 0` — which
// requires the return type to be a signed integer. A previous unsigned
// return type silently turned -1 into UINT_MAX and broke the error
// path at every call site (notably the File constructor).
TEST(CalculateMd5, ReturnsNegativeSentinelOnNullInput) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
    auto rc = LibFlute::calculate_md5(/*input=*/nullptr, /*length=*/0,
                                       result.data());
    EXPECT_LT(rc, 0);
}

// Same check for the (input != nullptr, length == 0) case.
TEST(CalculateMd5, ReturnsNegativeSentinelOnZeroLength) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
    char dummy = 'x';
    auto rc = LibFlute::calculate_md5(&dummy, /*length=*/0, result.data());
    EXPECT_LT(rc, 0);
}

// Sanity check: with valid input, calculate_md5 returns the MD5 digest
// length (16 bytes). Without this we can't distinguish "the function
// works" from "the function silently returns a near-zero value".
TEST(CalculateMd5, ReturnsDigestLengthOnValidInput) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
    std::array<char, 4> input{'a', 'b', 'c', 'd'};
    auto rc = LibFlute::calculate_md5(input.data(), input.size(), result.data());
    EXPECT_EQ(rc, MD5_DIGEST_LENGTH);
}

// File constructor calls calculate_md5 internally. With data!=nullptr
// but length==0, the broken `< 0` check on an unsigned return value
// silently lets construction proceed and the resulting FileEntry holds
// a garbage MD5. RFC 6726 §3.4.2: Content-MD5 (if present) must be a
// correct hash; emitting garbage corrupts every receiver that
// validates. The sender-side File constructor MUST therefore reject
// length=0 input rather than swallow the error.
TEST(FileConstruction, RejectsZeroLengthDataBuffer) {
    char dummy = 'x';
    auto oti = CompactNoCodeOti(/*F=*/0, /*T=*/64, /*max_sbl=*/4);
    EXPECT_THROW(
        LibFlute::File(/*toi=*/1, oti, "empty.bin", "application/octet-stream",
                        /*expires=*/0, &dummy, /*length=*/0,
                        /*copy_data=*/false),
        std::runtime_error);
}

// RFC 6726 §3.4.2 declares Content-MD5 with XSD type xs:base64Binary,
// which is RFC 4648 §4 standard base64 (alphabet [A-Za-z0-9+/], pad '=').
// RFC 2616 §14.15 + RFC 1864 §2 fix the input at 128 bits, so the
// encoded form is exactly 24 characters ending in "==".
//
// Regression guard: an earlier version called base64_encode with the
// full 64-byte EVP_MAX_MD_SIZE stack buffer AND passed MD5_DIGEST_LENGTH
// where the overload expected a `bool url` flag. The integer coerced to
// true, selecting URL-safe alphabet [A-Za-z0-9-_] and pad '.', and the
// length blew up to 88 characters ending in "..". Either symptom would
// be caught here.
TEST(FileConstruction, ContentMd5MatchesKnownVector) {
    // MD5("abcd") = e2fc714c4727ee9395f324cd2e7f331f
    // base64 of that 16-byte digest (RFC 4648 §4) = "4vxxTEcn7pOV8yTNLn8zHw=="
    std::array<char, 4> input{'a', 'b', 'c', 'd'};
    auto oti = CompactNoCodeOti(/*F=*/input.size(), /*T=*/4, /*max_sbl=*/1);
    LibFlute::File f(/*toi=*/1, oti, "abcd.bin", "application/octet-stream",
                     /*expires=*/0, input.data(), input.size(),
                     /*copy_data=*/true);
    EXPECT_EQ(f.meta().content_md5, "4vxxTEcn7pOV8yTNLn8zHw==");
}

TEST(FileConstruction, ContentMd5IsWellFormedStandardBase64) {
    std::array<char, 4> input{'a', 'b', 'c', 'd'};
    auto oti = CompactNoCodeOti(/*F=*/input.size(), /*T=*/4, /*max_sbl=*/1);
    LibFlute::File f(/*toi=*/1, oti, "abcd.bin", "application/octet-stream",
                     /*expires=*/0, input.data(), input.size(),
                     /*copy_data=*/true);
    const auto& md5 = f.meta().content_md5;

    // 128-bit digest → ceil(16/3)*4 = 24 base64 chars, two trailing '='.
    ASSERT_EQ(md5.size(), 24u);
    EXPECT_EQ(md5.substr(22), "==");

    // RFC 4648 §4 alphabet only. The URL-safe alphabet [-_] and the
    // libflute-internal URL pad '.' are explicitly forbidden in the FDT
    // wire form (xs:base64Binary).
    for (std::size_t i = 0; i < 22; ++i) {
        const char c = md5[i];
        const bool standard =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/';
        EXPECT_TRUE(standard) << "Non-standard base64 char '" << c
                              << "' at index " << i << " in " << md5;
    }
}

// RFC 6726 §3.4.2: Content-MD5 is OPTIONAL, but its stated purpose is a
// "decoded object integrity service" — i.e. when the sender advertises
// one, the receiver is expected to validate it after the bytes arrive
// and surface any mismatch. The Decoder uses File::integrity_check_failed()
// to decide whether to honour or suppress the completion path.

TEST(FileReception, IntegrityCheckPassesWhenMd5MatchesPayload) {
    constexpr std::uint32_t kT = 4;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint64_t kF = 4;
    const std::string kMd5Abcd = "4vxxTEcn7pOV8yTNLn8zHw==";  // base64(MD5("abcd"))

    LibFlute::File rx(MakeReceiverEntry(kF, kT, kMaxSbl, kMd5Abcd));
    ASSERT_FALSE(rx.complete());
    EXPECT_FALSE(rx.integrity_check_failed());

    auto sender = MakeSender({'a', 'b', 'c', 'd'}, kT, kMaxSbl);
    for (auto& s : sender.file->get_next_symbols(/*max_size=*/kF * 4)) {
        rx.put_symbol(s);
    }

    EXPECT_TRUE(rx.complete());
    EXPECT_FALSE(rx.integrity_check_failed());
}

TEST(FileReception, IntegrityCheckFailsWhenMd5DoesNotMatchPayload) {
    constexpr std::uint32_t kT = 4;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint64_t kF = 4;
    // 16 zero bytes encoded as base64 — guaranteed not to match MD5("abcd")
    const std::string kWrongMd5 = "AAAAAAAAAAAAAAAAAAAAAA==";

    LibFlute::File rx(MakeReceiverEntry(kF, kT, kMaxSbl, kWrongMd5));

    auto sender = MakeSender({'a', 'b', 'c', 'd'}, kT, kMaxSbl);
    for (auto& s : sender.file->get_next_symbols(/*max_size=*/kF * 4)) {
        rx.put_symbol(s);
    }

    // Structural completion still holds — every source symbol arrived —
    // but integrity validation MUST flag the mismatch so the Decoder
    // can suppress the completion callback and bump the failure counter.
    EXPECT_TRUE(rx.complete());
    EXPECT_TRUE(rx.integrity_check_failed());
}

TEST(FileReception, IntegrityCheckSkippedWhenContentMd5Absent) {
    constexpr std::uint32_t kT = 4;
    constexpr std::uint32_t kMaxSbl = 1;
    constexpr std::uint64_t kF = 4;

    // Empty content_md5 = sender did not advertise an MD5. RFC 6726
    // §3.4.2 makes the attribute OPTIONAL; the receiver MUST NOT
    // synthesise a failure when there is nothing to compare against.
    LibFlute::File rx(MakeReceiverEntry(kF, kT, kMaxSbl, /*content_md5=*/""));

    auto sender = MakeSender({'a', 'b', 'c', 'd'}, kT, kMaxSbl);
    for (auto& s : sender.file->get_next_symbols(/*max_size=*/kF * 4)) {
        rx.put_symbol(s);
    }

    EXPECT_TRUE(rx.complete());
    EXPECT_FALSE(rx.integrity_check_failed());
}
