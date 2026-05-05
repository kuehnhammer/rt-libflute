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
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/md5.h>

#include <gtest/gtest.h>

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
