// MBMS FDT XML fixtures — minimal <FDT-Instance> documents that
// exercise one TS 26.346 cl. 7.2.10 extension namespace at a time.
//
// XSD oracles:
//   TS26346_FLUTE-FDT_Extensions_Rel-6.xsd  → mbms2005 (Group / Session-Identity)
//   TS26346_FLUTE-FDT_Extensions_Rel-7.xsd  → mbms2007 (Cache-Control choice)
//   TS26346_FLUTE-FDT_Extensions_Rel-8.xsd  → mbms2008 (FullFDT attribute)
//   TS26346_FLUTE-FDT_Extensions_Rel-9.xsd  → mbms2009 (Decryption-KEY-URI)
//   TS26346_FLUTE-FDT_Extensions_Rel-11.xsd → mbms2012 (Alternate-Content-
//                                              Location, Base-URL,
//                                              FEC-Redundancy-Level, File-ETag)
//   TS26346_FLUTE-FDT_Extensions_Rel-13.xsd → mbms2015 (IndependentUnitPositions)
//   TS26346_FLUTE-FDT_Extensions_Rel-19.xsd → mbms2025 (Repair-Start,
//                                              Repair-Limit-Percentage)
//
// Each fixture is a complete, self-contained FDT-Instance with the
// xmlns:mbmsXXXX declarations the test needs. Tests stay declarative
// and the XML is grep-able against the XSD files above.

#pragma once

namespace libflute_test::mbms {

// Rel-7 mbms2007:Cache-Control choice = <Expires>NTP-seconds</Expires>.
// The parser at FileDeliveryTable.cpp:160-168 handles this variant.
inline constexpr const char* kRel7CacheControlExpires = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2007="urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r7-expires.bin" Content-Length="1024">
    <mbms2007:Cache-Control>
      <mbms2007:Expires>3600</mbms2007:Expires>
    </mbms2007:Cache-Control>
  </File>
</FDT-Instance>
)";

// Rel-7 mbms2007:Cache-Control choice = <no-cache>true</no-cache>.
inline constexpr const char* kRel7CacheControlNoCache = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2007="urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r7-nocache.bin" Content-Length="1024">
    <mbms2007:Cache-Control>
      <mbms2007:no-cache>true</mbms2007:no-cache>
    </mbms2007:Cache-Control>
  </File>
</FDT-Instance>
)";

// Rel-7 mbms2007:Cache-Control choice = <max-stale>true</max-stale>.
inline constexpr const char* kRel7CacheControlMaxStale = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2007="urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r7-maxstale.bin" Content-Length="1024">
    <mbms2007:Cache-Control>
      <mbms2007:max-stale>true</mbms2007:max-stale>
    </mbms2007:Cache-Control>
  </File>
</FDT-Instance>
)";

// Rel-7 illegal: XSD <xs:choice> means at most ONE child of
// Cache-Control. This fixture sets both no-cache AND Expires, which
// is XSD-invalid. A spec-conformant parser must reject (or at least
// warn-and-pick-one). Used by Mbms2007CacheControlChoiceMutualExclusion.
inline constexpr const char* kRel7CacheControlInvalidChoice = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2007="urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r7-invalid.bin" Content-Length="1024">
    <mbms2007:Cache-Control>
      <mbms2007:no-cache>true</mbms2007:no-cache>
      <mbms2007:Expires>3600</mbms2007:Expires>
    </mbms2007:Cache-Control>
  </File>
</FDT-Instance>
)";

// Rel-8 mbms2008:FullFDT="true" attribute on FDT-Instance.
inline constexpr const char* kRel8FullFdtTrue = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2008="urn:3GPP:metadata:2008:MBMS:FLUTE:FDT_ext"
              Expires="2208988800"
              mbms2008:FullFDT="true">
  <File TOI="1" Content-Location="r8.bin" Content-Length="1024"/>
</FDT-Instance>
)";

// Rel-9 mbms2009:Decryption-KEY-URI on a File element.
inline constexpr const char* kRel9DecryptionKeyUri = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2009="urn:3GPP:metadata:2009:MBMS:FLUTE:FDT_ext"
              Expires="2208988800">
  <File TOI="1" Content-Location="r9.bin" Content-Length="1024"
        mbms2009:Decryption-KEY-URI="https://kms.example.com/keys/abc123"/>
</FDT-Instance>
)";

// Rel-11/12 mbms2012:Alternate-Content-Location-1/-2 child elements
// on a File, each containing one or more Alternate-Content-Location
// entries (per the XSD's complex type with sequence).
inline constexpr const char* kRel12AlternateContentLocation = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2012="urn:3GPP:metadata:2012:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r12-acl.bin" Content-Length="1024">
    <mbms2012:Alternate-Content-Location-1>
      <mbms2012:Alternate-Content-Location>https://cdn1.example.com/r12-acl.bin</mbms2012:Alternate-Content-Location>
      <mbms2012:Alternate-Content-Location>https://cdn2.example.com/r12-acl.bin</mbms2012:Alternate-Content-Location>
    </mbms2012:Alternate-Content-Location-1>
    <mbms2012:Alternate-Content-Location-2>
      <mbms2012:Alternate-Content-Location>https://repair.example.com/r12-acl.bin</mbms2012:Alternate-Content-Location>
    </mbms2012:Alternate-Content-Location-2>
  </File>
</FDT-Instance>
)";

// Rel-11/12 mbms2012:Base-URL-1 and Base-URL-2 child elements on
// FDT-Instance carrying anyURI base URLs.
inline constexpr const char* kRel12BaseUrls = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2012="urn:3GPP:metadata:2012:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <mbms2012:Base-URL-1>https://primary.example.com/</mbms2012:Base-URL-1>
  <mbms2012:Base-URL-2>https://secondary.example.com/</mbms2012:Base-URL-2>
  <File TOI="1" Content-Location="r12-base.bin" Content-Length="1024"/>
</FDT-Instance>
)";

// Rel-11/12 mbms2012:FEC-Redundancy-Level (unsignedInt) attribute on
// File. Indicates the desired redundancy percentage for FEC repair.
inline constexpr const char* kRel12FecRedundancyLevel = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2012="urn:3GPP:metadata:2012:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r12-fec.bin" Content-Length="1024"
        mbms2012:FEC-Redundancy-Level="20"/>
</FDT-Instance>
)";

// Rel-11/12 mbms2012:File-ETag (string) attribute on File.
inline constexpr const char* kRel12FileEtag = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2012="urn:3GPP:metadata:2012:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r12-etag.bin" Content-Length="1024"
        mbms2012:File-ETag="W/&quot;abc123&quot;"/>
</FDT-Instance>
)";

// TS 26.346 cl. 7.2.10: a serialized FDT-Instance MUST carry the
// <sv:schemaVersion> structural-versioning element so receivers know
// which release set of extensions to expect. The xmlns:sv comes from
// TS26346_SchemaVersion.xsd.
inline constexpr const char* kSchemaVersionMarker = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:sv="urn:3GPP:metadata:2009:MBMS:schemaVersion"
              Expires="2208988800">
  <sv:schemaVersion>1</sv:schemaVersion>
  <File TOI="1" Content-Location="r-sv.bin" Content-Length="1024"/>
</FDT-Instance>
)";

// Rel-19 mbms2025:Repair-Start (dateTime) and
// mbms2025:Repair-Limit-Percentage (unsignedInt) attributes on File.
inline constexpr const char* kRel19RepairAttributes = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:mbms2025="urn:3GPP:metadata:2025:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <File TOI="1" Content-Location="r19.bin" Content-Length="1024"
        mbms2025:Repair-Start="2026-04-01T12:00:00Z"
        mbms2025:Repair-Limit-Percentage="35"/>
</FDT-Instance>
)";

// XML namespace prefixes are arbitrary labels that bind to URIs via
// xmlns declarations. A spec-conformant sender may bind the standard
// MBMS namespace URIs to ANY prefix — `n1`, `cc`, `sv`, etc. The
// parser MUST resolve qualified names via the xmlns map, not by
// pattern-matching the literal prefix string. This fixture binds the
// schemaVersion namespace to `n1` and the Rel-7 Cache-Control
// namespace to `cc` to exercise that resolution path.
inline constexpr const char* kCustomPrefixesSchemaVersionAndCacheControl = R"(<?xml version="1.0" encoding="UTF-8"?>
<FDT-Instance xmlns:n1="urn:3GPP:metadata:2009:MBMS:schemaVersion"
              xmlns:cc="urn:3GPP:metadata:2007:MBMS:FLUTE:FDT"
              Expires="2208988800">
  <n1:schemaVersion>1</n1:schemaVersion>
  <File TOI="1" Content-Location="custom-prefix.bin" Content-Length="1024">
    <cc:Cache-Control>
      <cc:Expires>9000</cc:Expires>
    </cc:Cache-Control>
  </File>
</FDT-Instance>
)";

}  // namespace libflute_test::mbms
