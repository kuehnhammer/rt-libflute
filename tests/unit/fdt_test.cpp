// FileDeliveryTable — RFC 6726 §3.4.2 (FDT XML), §3.3 (FDT semantics),
// §5 (FDT Schema), with one MBMS Rel-7 attribute (Cache-Control/Expires
// from TS 26.346 cl. 7.2.10) that the codebase already exercises.
//
// FDT-Instance shape per RFC 6726 §3.4.2:
//
//   <FDT-Instance Expires="<NTP seconds>"
//                 [FEC-OTI-FEC-Encoding-ID="..."]
//                 [FEC-OTI-Maximum-Source-Block-Length="..."]
//                 [FEC-OTI-Encoding-Symbol-Length="..."]
//                 [...]>
//     <File TOI="..." Content-Location="..." [Content-Length="..."]
//           [Content-MD5="..."] [Content-Type="..."]
//           [FEC-OTI-FEC-Encoding-ID="..."]
//           [FEC-OTI-Maximum-Source-Block-Length="..."]
//           [FEC-OTI-Encoding-Symbol-Length="..."]/>
//     ...
//   </FDT-Instance>
//
// The "Expires" attribute is REQUIRED. TOI and Content-Location are
// REQUIRED on every File entry (§3.4.2 + Schema in §5).
//
// FEC-OTI parameters MAY be set on FDT-Instance (defaults for all
// files) and MAY be overridden per-File. Per-File values override.

#include "FileDeliveryTable.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "flute_types.h"

namespace {

// FileDeliveryTable's parsing constructor takes a non-const buffer.
// Wrap the convention into a helper that owns the writable bytes.
std::vector<char> XmlBuf(std::string_view xml) {
    return std::vector<char>(xml.begin(), xml.end());
}

constexpr const char* kMinimalFdt = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance Expires="2208988800">
  <File TOI="1" Content-Location="file1.bin" Content-Length="1024"/>
</FDT-Instance>
)";

}  // namespace

// RFC 6726 §3.4.2: Expires is REQUIRED; a parser that accepts an
// FDT-Instance without it is non-conformant.
TEST(Fdt, RejectsFdtInstanceMissingExpires) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance>
  <File TOI="1" Content-Location="file1.bin" Content-Length="0"/>
</FDT-Instance>
)";
    auto buf = XmlBuf(kXml);

    EXPECT_THROW(
        LibFlute::FileDeliveryTable(/*instance_id=*/1, buf.data(), buf.size()),
        std::runtime_error);
}

// RFC 6726 §5: the root element of the FDT XML MUST be FDT-Instance.
// XML that's well-formed but uses a different root must be rejected.
TEST(Fdt, RejectsXmlMissingFdtInstanceRoot) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<NotAnFdtInstance Expires="2208988800"/>
)";
    auto buf = XmlBuf(kXml);

    EXPECT_THROW(
        LibFlute::FileDeliveryTable(/*instance_id=*/1, buf.data(), buf.size()),
        std::runtime_error);
}

// RFC 6726 §3.4.2: a minimal FDT-Instance with one File entry parses
// and surfaces TOI + Content-Location to the caller.
TEST(Fdt, ParsesFdtInstanceWithMinimalFileEntry) {
    auto buf = XmlBuf(kMinimalFdt);

    LibFlute::FileDeliveryTable fdt(/*instance_id=*/42, buf.data(), buf.size());

    EXPECT_EQ(fdt.instance_id(), 42U);
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].toi, 1U);
    EXPECT_EQ(entries[0].content_location, "file1.bin");
    EXPECT_EQ(entries[0].content_length, 1024U);
}

// RFC 6726 §3.4.2 and §5 schema: TOI is REQUIRED on every File element.
// A File missing TOI must be rejected, not silently skipped.
TEST(Fdt, RejectsFileEntryMissingToi) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance Expires="2208988800">
  <File Content-Location="file1.bin" Content-Length="1024"/>
</FDT-Instance>
)";
    auto buf = XmlBuf(kXml);

    EXPECT_THROW(
        LibFlute::FileDeliveryTable(/*instance_id=*/1, buf.data(), buf.size()),
        std::runtime_error);
}

// Same as above but for Content-Location (also REQUIRED per §3.4.2).
TEST(Fdt, RejectsFileEntryMissingContentLocation) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance Expires="2208988800">
  <File TOI="1" Content-Length="1024"/>
</FDT-Instance>
)";
    auto buf = XmlBuf(kXml);

    EXPECT_THROW(
        LibFlute::FileDeliveryTable(/*instance_id=*/1, buf.data(), buf.size()),
        std::runtime_error);
}

// RFC 6726 §3.4.2: FEC-OTI parameters set on FDT-Instance act as
// defaults for every File entry that doesn't override them.
TEST(Fdt, FdtLevelFecOtiInheritedByFileEntries) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance Expires="2208988800"
              FEC-OTI-FEC-Encoding-ID="0"
              FEC-OTI-Maximum-Source-Block-Length="64"
              FEC-OTI-Encoding-Symbol-Length="1024">
  <File TOI="1" Content-Location="file1.bin" Content-Length="65536"/>
  <File TOI="2" Content-Location="file2.bin" Content-Length="2048"/>
</FDT-Instance>
)";
    auto buf = XmlBuf(kXml);

    LibFlute::FileDeliveryTable fdt(/*instance_id=*/1, buf.data(), buf.size());
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 2U);
    for (const auto& fe : entries) {
        EXPECT_EQ(fe.fec_oti.encoding_id, LibFlute::FecScheme::CompactNoCode);
        EXPECT_EQ(fe.fec_oti.max_source_block_length, 64U);
        EXPECT_EQ(fe.fec_oti.encoding_symbol_length, 1024U);
    }
}

// RFC 6726 §3.4.2: per-File FEC-OTI overrides FDT-Instance defaults.
// Both files take their own value when set; otherwise the default.
TEST(Fdt, FileLevelFecOtiOverridesFdtLevel) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance Expires="2208988800"
              FEC-OTI-FEC-Encoding-ID="0"
              FEC-OTI-Maximum-Source-Block-Length="64"
              FEC-OTI-Encoding-Symbol-Length="1024">
  <File TOI="1" Content-Location="file1.bin" Content-Length="65536"
        FEC-OTI-Encoding-Symbol-Length="512"/>
  <File TOI="2" Content-Location="file2.bin" Content-Length="2048"
        FEC-OTI-Maximum-Source-Block-Length="32"
        FEC-OTI-Encoding-Symbol-Length="256"/>
</FDT-Instance>
)";
    auto buf = XmlBuf(kXml);

    LibFlute::FileDeliveryTable fdt(/*instance_id=*/1, buf.data(), buf.size());
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 2U);

    // file 1: T overridden, max_sbl inherited.
    EXPECT_EQ(entries[0].toi, 1U);
    EXPECT_EQ(entries[0].fec_oti.max_source_block_length, 64U);
    EXPECT_EQ(entries[0].fec_oti.encoding_symbol_length, 512U);

    // file 2: both overridden.
    EXPECT_EQ(entries[1].toi, 2U);
    EXPECT_EQ(entries[1].fec_oti.max_source_block_length, 32U);
    EXPECT_EQ(entries[1].fec_oti.encoding_symbol_length, 256U);
}

// TS 26.346 cl. 7.2.10 (Rel-7 mbms2007 namespace): a File entry may
// carry a <mbms2007:Cache-Control> child holding an <mbms2007:Expires>
// element with a per-file expiry. This must be parsed into the
// FileEntry.expires field.
TEST(Fdt, Mbms2007CacheControlExpiresParsedIntoFileEntry) {
    constexpr const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2007="urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="file1.bin" Content-Length="1024">
    <mbms2007:Cache-Control>
      <mbms2007:Expires>3600</mbms2007:Expires>
    </mbms2007:Cache-Control>
  </File>
</FDT-Instance>
)";
    auto buf = XmlBuf(kXml);

    LibFlute::FileDeliveryTable fdt(/*instance_id=*/1, buf.data(), buf.size());
    auto entries = fdt.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].expires, 3600U);
}

// RFC 6726 §3.4.2 + §5: an FDT round-tripped through to_string() then
// re-parsed must preserve the File entries the embedder cares about
// (TOI, Content-Location, Content-Length, FEC-OTI, MBMS expires).
// Whitespace/attribute-order differences are tolerated; field
// equivalence is asserted explicitly.
TEST(Fdt, RoundTripFromFileEntryToStringAndBack) {
    LibFlute::FecOti global_oti{
        LibFlute::FecScheme::CompactNoCode,
        /*transfer_length*/ 0,
        /*encoding_symbol_length*/ 1024,
        /*max_source_block_length*/ 64,
        /*scheme_specific_info*/ ""
    };
    LibFlute::FileDeliveryTable fdt(/*instance_id=*/7, global_oti);
    fdt.set_expires(2208988800ULL);

    LibFlute::FileDeliveryTable::FileEntry fe{};
    fe.toi = 9;
    fe.content_location = "audio/clip.aac";
    fe.content_length = 4096;
    fe.content_md5 = "";
    fe.content_type = "audio/aac";
    fe.expires = 7200;
    fe.fec_oti = LibFlute::FecOti{
        LibFlute::FecScheme::CompactNoCode,
        /*transfer_length*/ 4096,
        /*encoding_symbol_length*/ 1024,
        /*max_source_block_length*/ 64,
        ""
    };
    fe.fec_transformer = nullptr;
    fdt.add(fe);

    auto xml = fdt.to_string();
    ASSERT_FALSE(xml.empty());

    std::vector<char> reparse_buf(xml.begin(), xml.end());
    LibFlute::FileDeliveryTable parsed(/*instance_id=*/7, reparse_buf.data(),
                                        reparse_buf.size());
    auto entries = parsed.file_entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].toi, 9U);
    EXPECT_EQ(entries[0].content_location, "audio/clip.aac");
    EXPECT_EQ(entries[0].content_length, 4096U);
    EXPECT_EQ(entries[0].content_type, "audio/aac");
    EXPECT_EQ(entries[0].expires, 7200U);
    EXPECT_EQ(entries[0].fec_oti.encoding_symbol_length, 1024U);
    EXPECT_EQ(entries[0].fec_oti.max_source_block_length, 64U);
}
