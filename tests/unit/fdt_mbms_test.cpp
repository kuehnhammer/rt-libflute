// FDT MBMS extensions — TS 26.346 cl. 7.2.10 + the per-release XSD
// overlays at /home/klaus/workspace/3gpp/26346-j30/.
//
// XSD oracles (one per release):
//   Rel-7 (mbms2007): Cache-Control choice (no-cache | max-stale | Expires)
//   Rel-8 (mbms2008): FullFDT (boolean attribute on FDT-Instance)
//   Rel-9 (mbms2009): Decryption-KEY-URI (anyURI attribute on File)
//   Rel-11/12 (mbms2012): Alternate-Content-Location-1/-2 (children),
//                          Base-URL-1/-2 (children), FEC-Redundancy-Level
//                          (unsignedInt attr), File-ETag (string attr)
//   Rel-19 (mbms2025): Repair-Start (dateTime), Repair-Limit-Percentage
//
// All tests parse hand-built FDT XML fixtures (fixtures_mbms.hpp) and
// assert that the parsed FileDeliveryTable / FileEntry surfaces the
// extension fields per the XSD types.

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
    EXPECT_EQ(entries[0].cache_control,
              LibFlute::FileDeliveryTable::CacheControl::Expires);
    EXPECT_EQ(entries[0].expires, 3600U);
}

// Rel-7 XSD lists <no-cache>true</...> as one of the three Cache-Control
// choices.
TEST(FdtMbms, Rel7CacheControlNoCacheRecognised) {
    auto fdt = Parse(libflute_test::mbms::kRel7CacheControlNoCache);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].toi, 1U);
    EXPECT_EQ(entries[0].cache_control,
              LibFlute::FileDeliveryTable::CacheControl::NoCache);
}

// Same as above for max-stale (third Cache-Control choice).
TEST(FdtMbms, Rel7CacheControlMaxStaleRecognised) {
    auto fdt = Parse(libflute_test::mbms::kRel7CacheControlMaxStale);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].cache_control,
              LibFlute::FileDeliveryTable::CacheControl::MaxStale);
}

// Rel-7 XSD declares Cache-Control as <xs:choice> — the three options
// are MUTUALLY EXCLUSIVE. A document that combines no-cache and
// Expires is XSD-invalid; the parser MUST reject it.
TEST(FdtMbms, Rel7CacheControlChoiceMutualExclusion) {
    EXPECT_THROW(Parse(libflute_test::mbms::kRel7CacheControlInvalidChoice),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// Rel-8 (mbms2008) — FullFDT boolean attribute on FDT-Instance
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10.2: mbms2008:FullFDT="true" on FDT-Instance
// signals "this FDT-Instance supersedes all prior partial FDTs". The
// parser must surface the boolean value via the FullFDT accessor.
TEST(FdtMbms, Rel8FullFdtBooleanRoundTrip) {
    auto fdt = Parse(libflute_test::mbms::kRel8FullFdtTrue);
    EXPECT_EQ(fdt.file_entries().size(), 1U);
    ASSERT_TRUE(fdt.full_fdt().has_value());
    EXPECT_TRUE(*fdt.full_fdt());
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
    EXPECT_EQ(entries[0].decryption_key_uri,
              "https://kms.example.com/keys/abc123");
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
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    ASSERT_EQ(entries[0].alternate_content_locations_1.size(), 2U);
    EXPECT_EQ(entries[0].alternate_content_locations_1[0],
              "https://cdn1.example.com/r12-acl.bin");
    EXPECT_EQ(entries[0].alternate_content_locations_1[1],
              "https://cdn2.example.com/r12-acl.bin");
    ASSERT_EQ(entries[0].alternate_content_locations_2.size(), 1U);
    EXPECT_EQ(entries[0].alternate_content_locations_2[0],
              "https://repair.example.com/r12-acl.bin");
}

// TS 26.346 cl. 7.2.10.2: FDT-Instance MAY carry mbms2012:Base-URL-1
// and Base-URL-2 children for resolving relative File Content-Location
// URIs.
TEST(FdtMbms, Rel12BaseUrl1And2Preserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12BaseUrls);
    EXPECT_EQ(fdt.file_entries().size(), 1U);
    ASSERT_TRUE(fdt.base_url_1().has_value());
    EXPECT_EQ(*fdt.base_url_1(), "https://primary.example.com/");
    ASSERT_TRUE(fdt.base_url_2().has_value());
    EXPECT_EQ(*fdt.base_url_2(), "https://secondary.example.com/");
}

// TS 26.346 cl. 7.2.10.2: mbms2012:FEC-Redundancy-Level (unsignedInt
// percent value) on File requests the recommended FEC redundancy.
TEST(FdtMbms, Rel12FecRedundancyLevelPreserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12FecRedundancyLevel);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    ASSERT_TRUE(entries[0].fec_redundancy_level.has_value());
    EXPECT_EQ(*entries[0].fec_redundancy_level, 20U);
}

// TS 26.346 cl. 7.2.10.2: mbms2012:File-ETag (string) on File matches
// the HTTP ETag for cache-validation against an HTTP origin.
TEST(FdtMbms, Rel12FileEtagPreserved) {
    auto fdt = Parse(libflute_test::mbms::kRel12FileEtag);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].file_etag, "W/\"abc123\"");
}

// ---------------------------------------------------------------------------
// Schema-version structural marker (TS 26.346 cl. 7.2.10) +
// Rel-19 (mbms2025) repair attributes
// ---------------------------------------------------------------------------

// TS 26.346 cl. 7.2.10: serialised FDT-Instance documents MUST carry
// a <sv:schemaVersion> structural-versioning marker. Parser must read
// the version, emitter must output it on to_string().
TEST(FdtMbms, SchemaVersionMarkerAcceptedOnParse) {
    auto fdt = Parse(libflute_test::mbms::kSchemaVersionMarker);
    EXPECT_EQ(fdt.file_entries().size(), 1U);
    EXPECT_EQ(fdt.schema_version(), 1);
}

// to_string() must emit the schemaVersion marker per TS 26.346 cl.
// 7.2.10 — receivers depending on it for feature-set negotiation will
// otherwise fail.
TEST(FdtMbms, ToStringEmitsSchemaVersionMarker) {
    LibFlute::FecOti oti{LibFlute::FecScheme::CompactNoCode, 0, 1024, 64, ""};
    LibFlute::FileDeliveryTable fdt(/*instance_id=*/1, oti);
    fdt.set_expires(2208988800ULL);
    fdt.set_schema_version(1);
    auto xml = fdt.to_string();
    EXPECT_NE(xml.find("schemaVersion"), std::string::npos)
        << "to_string() output should carry an sv:schemaVersion marker:\n" << xml;
}

// TS 26.346 cl. 7.2.10.2 (Rel-19): mbms2025:Repair-Start (dateTime)
// and mbms2025:Repair-Limit-Percentage (unsignedInt) on File configure
// the unicast repair window.
TEST(FdtMbms, Rel19RepairAttributesPreservedIfPresent) {
    auto fdt = Parse(libflute_test::mbms::kRel19RepairAttributes);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].repair_start, "2026-04-01T12:00:00Z");
    ASSERT_TRUE(entries[0].repair_limit_percentage.has_value());
    EXPECT_EQ(*entries[0].repair_limit_percentage, 35U);
}

// ---------------------------------------------------------------------------
// XML namespace handling — prefix variation
// ---------------------------------------------------------------------------

// XML namespace prefixes are arbitrary labels chosen by the document
// author. The XSDs pin namespace URIs (e.g.
// `urn:3GPP:metadata:2009:MBMS:schemaVersion`) but a sender may bind
// that URI to any prefix it likes. A receiver that pattern-matches
// against the literal prefix string (e.g. `sv:schemaVersion`) silently
// drops payloads from senders that chose a different prefix and is
// not XML-namespace conformant.
//
// This test parses an FDT-Instance that binds the schemaVersion
// namespace to `n1` and the Rel-7 Cache-Control namespace to `cc`,
// and verifies that both fields are surfaced exactly as if the
// canonical `sv` / `mbms2007` prefixes had been used.
TEST(FdtMbms, ParserResolvesXmlNamespacesViaXmlnsMapNotPrefixString) {
    auto fdt = Parse(
        libflute_test::mbms::kCustomPrefixesSchemaVersionAndCacheControl);

    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].cache_control,
              LibFlute::FileDeliveryTable::CacheControl::Expires);
    EXPECT_EQ(entries[0].expires, 9000U);
    EXPECT_EQ(fdt.schema_version(), 1);
}
