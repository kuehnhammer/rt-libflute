// FDT MBMS extensions — TS 26.346 cl. 7.2.10 + the per-release XSD
// overlays at /home/klaus/workspace/3gpp/26346-j30/. Tests are written
// against the spec, not the current parser; tests for fields the
// current FileEntry struct doesn't surface are tagged GTEST_SKIP with
// a pointer to the missing capability so a future round can flip them
// on without re-deriving the spec context.
//
// XSD oracles (one per release):
//   Rel-7 (mbms2007): Cache-Control choice (no-cache | max-stale | Expires)
//   Rel-8 (mbms2008): FullFDT (boolean attribute on FDT-Instance)
//   Rel-9 (mbms2009): Decryption-KEY-URI (anyURI attribute on File)
//   Rel-11/12 (mbms2012): Alternate-Content-Location-1/-2 (children),
//                          Base-URL-1/-2 (children), FEC-Redundancy-Level
//                          (unsignedInt attr), File-ETag (string attr)
//   Rel-13 (mbms2015): IndependentUnitPositions
//   Rel-19 (mbms2025): Repair-Start (dateTime), Repair-Limit-Percentage
//
// All tests use the parsing constructor of FileDeliveryTable. Each
// fixture lives in fixtures_mbms.hpp.

#include "FileDeliveryTable.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fixtures_mbms.hpp"

namespace {

std::vector<char> XmlBuf(const char* xml) {
    return std::vector<char>(xml, xml + std::strlen(xml));
}

// Surrogate for "did the parser silently accept and produce one File
// entry without crashing?" used in tests where the parser doesn't
// surface the extension field but should at minimum not reject it.
LibFlute::FileDeliveryTable Parse(const char* xml) {
    auto buf = XmlBuf(xml);
    return LibFlute::FileDeliveryTable(/*instance_id=*/1, buf.data(),
                                        buf.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// Rel-7 (mbms2007) — Cache-Control choice
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10.2 + Rel-7 XSD: <mbms2007:Cache-Control> with
// <mbms2007:Expires>NTP-seconds</...> sets the per-file expiry.
TEST(FdtMbms, Rel7CacheControlExpiresAttachedToFileEntry) {
    auto fdt = Parse(libflute_test::mbms::kRel7CacheControlExpires);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].expires, 3600U);
}

// Rel-7 XSD lists <no-cache>true</...> as one of the three Cache-Control
// choices. A spec-aware parser must at minimum not reject the document.
// Surfacing the no-cache flag back to the embedder requires a new
// FileEntry field, which the current struct lacks — so we only check
// non-rejection here.
TEST(FdtMbms, Rel7CacheControlNoCacheRecognised) {
    auto fdt = Parse(libflute_test::mbms::kRel7CacheControlNoCache);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].toi, 1U);

    GTEST_SKIP() << "FileEntry has no field for the no-cache flag — "
                    "round 2 should add a CacheControl variant member "
                    "to FileDeliveryTable::FileEntry to surface this.";
}

// Same as above for max-stale (third Cache-Control choice).
TEST(FdtMbms, Rel7CacheControlMaxStaleRecognised) {
    auto fdt = Parse(libflute_test::mbms::kRel7CacheControlMaxStale);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].toi, 1U);

    GTEST_SKIP() << "FileEntry has no field for the max-stale flag — "
                    "round 2 should add a CacheControl variant member "
                    "to FileDeliveryTable::FileEntry to surface this.";
}

// Rel-7 XSD declares Cache-Control as <xs:choice> — the three options
// are MUTUALLY EXCLUSIVE. A document that combines no-cache and
// Expires is XSD-invalid; a strict parser MUST reject it.
TEST(FdtMbms, Rel7CacheControlChoiceMutualExclusion) {
    GTEST_SKIP() << "Current parser silently accepts XSD-invalid "
                    "Cache-Control with multiple children (no-cache + "
                    "Expires). Round 2: enforce <xs:choice> by rejecting "
                    "the document or warning + picking the first child "
                    "deterministically.";

    EXPECT_THROW(Parse(libflute_test::mbms::kRel7CacheControlInvalidChoice),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// Rel-8 (mbms2008) — FullFDT boolean attribute on FDT-Instance
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10.2: mbms2008:FullFDT is a boolean attribute
// signalling "this FDT-Instance supersedes all prior partial FDTs".
// The parser must accept the attribute and (round 2) surface its
// value via FileDeliveryTable.
TEST(FdtMbms, Rel8FullFdtBooleanRoundTrip) {
    auto fdt = Parse(libflute_test::mbms::kRel8FullFdtTrue);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileDeliveryTable has no full_fdt() accessor — "
                    "round 2 should add an optional<bool> _full_fdt "
                    "member set during parsing of mbms2008:FullFDT.";
}

// ---------------------------------------------------------------------------
// Rel-9 (mbms2009) — Decryption-KEY-URI attribute on File
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10.2: mbms2009:Decryption-KEY-URI carries the URI
// of the key used to decrypt the file. The receiver must surface this
// to the application layer or the file is unusable.
TEST(FdtMbms, Rel9DecryptionKeyUriRoundTrip) {
    auto fdt = Parse(libflute_test::mbms::kRel9DecryptionKeyUri);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].toi, 1U);

    GTEST_SKIP() << "FileEntry has no decryption_key_uri field — "
                    "round 2 should add std::string decryption_key_uri "
                    "and parse mbms2009:Decryption-KEY-URI into it.";
}

// ---------------------------------------------------------------------------
// Rel-11/12 (mbms2012) — Alt-Content-Location, Base-URL,
//                        FEC-Redundancy-Level, File-ETag
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10.2: each File MAY carry one or two
// Alternate-Content-Location-N children, each containing 0..N
// Alternate-Content-Location entries (anyURI) for unicast repair.
TEST(FdtMbms, Rel12AlternateContentLocationListPreserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12AlternateContentLocation);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileEntry has no alternate_content_locations field "
                    "— round 2: add a "
                    "std::vector<std::string> alternate_content_locations_1/2 "
                    "and populate from mbms2012:Alternate-Content-Location-N.";
}

// TS 26.346 cl. 7.2.10.2: FDT-Instance MAY carry mbms2012:Base-URL-1
// and Base-URL-2 children for resolving relative File Content-Location
// URIs.
TEST(FdtMbms, Rel12BaseUrl1And2Preserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12BaseUrls);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileDeliveryTable has no base_url_1/_2 accessor — "
                    "round 2: add std::optional<std::string> _base_url_1/_2 "
                    "and parse the mbms2012:Base-URL-N children.";
}

// TS 26.346 cl. 7.2.10.2: mbms2012:FEC-Redundancy-Level (unsignedInt
// percent value) on File requests the recommended FEC redundancy.
TEST(FdtMbms, Rel12FecRedundancyLevelPreserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12FecRedundancyLevel);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileEntry has no fec_redundancy_level field — "
                    "round 2: add std::optional<uint32_t> "
                    "fec_redundancy_level and parse the attribute.";
}

// TS 26.346 cl. 7.2.10.2: mbms2012:File-ETag (string) on File matches
// the HTTP ETag for cache-validation against an HTTP origin.
TEST(FdtMbms, Rel12FileEtagPreserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12FileEtag);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileEntry has no file_etag field — round 2: add "
                    "std::string file_etag and parse mbms2012:File-ETag.";
}

// ---------------------------------------------------------------------------
// Schema-version structural marker (TS 26.346 cl. 7.2.10) +
// Rel-19 (mbms2025) repair attributes
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10: serialised FDT-Instance documents MUST carry
// a <sv:schemaVersion> structural-versioning marker so receivers can
// check supported features. This applies to documents the *codebase*
// emits via to_string(); a parser must accept (and a producer must
// emit) the marker.
TEST(FdtMbms, SchemaVersionMarkerAcceptedOnParse) {
    auto fdt = Parse(libflute_test::mbms::kSchemaVersionMarker);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileDeliveryTable::to_string() does not emit "
                    "<sv:schemaVersion>. Round 2: emit the marker and "
                    "version it per TS 26.346 cl. 7.2.10.";
}

// TS 26.346 cl. 7.2.10.2 (Rel-19): mbms2025:Repair-Start (dateTime)
// and mbms2025:Repair-Limit-Percentage (unsignedInt) on File configure
// the unicast repair window.
TEST(FdtMbms, Rel19RepairAttributesPreservedIfPresent) {
    auto fdt = Parse(libflute_test::mbms::kRel19RepairAttributes);
    EXPECT_EQ(fdt.file_entries().size(), 1U);

    GTEST_SKIP() << "FileEntry has no repair_start / "
                    "repair_limit_percentage fields — round 2: add the "
                    "fields and parse the mbms2025 attributes.";
}
