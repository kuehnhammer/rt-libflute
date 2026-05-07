// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
// 
// See the License for the specific language governing permissions and limitations
// under the License.
//
#include "FileDeliveryTable.h"
#include <cstdlib>         // for strtoul, strtoull
#include <stdexcept>        // for runtime_error
#include <string>           // for string, to_string, stoull
#include <string_view>
#include <unordered_map>
#include <utility>          // for move
#include "spdlog/spdlog.h"  // for debug
#include "tinyxml2.h"       // for XMLElement, XMLDocument, XMLPrinter, COLL...
#ifdef RAPTOR_ENABLED
#include "fec/RaptorFEC.h"
#endif

namespace {

// MBMS / FLUTE FDT namespace URIs (TS 26.346 cl. 7.2.10 + per-release
// XSD overlays). Senders may bind these URIs to any prefix they like
// (`mbms2007:`, `cc:`, `n1:`, …) so the parser MUST resolve qualified
// names against the document's xmlns declarations rather than
// pattern-matching on the literal prefix.
constexpr std::string_view kNsMbms2007 = "urn:3GPP:metadata:2007:MBMS:FLUTE:FDT";
constexpr std::string_view kNsMbms2008 = "urn:3GPP:metadata:2008:MBMS:FLUTE:FDT_ext";
constexpr std::string_view kNsMbms2009 = "urn:3GPP:metadata:2009:MBMS:FLUTE:FDT_ext";
constexpr std::string_view kNsMbms2012 = "urn:3GPP:metadata:2012:MBMS:FLUTE:FDT";
constexpr std::string_view kNsMbms2025 = "urn:3GPP:metadata:2025:MBMS:FLUTE:FDT";
constexpr std::string_view kNsSchemaVersion =
    "urn:3GPP:metadata:2009:MBMS:schemaVersion";

// Map xmlns prefix → namespace URI as declared on the FDT-Instance
// root. Real XML allows xmlns to be redefined in nested elements; we
// don't (MBMS FDTs don't do that in practice), and keep this scoped to
// the document root for simplicity. The empty key represents the
// no-namespace bucket, where unqualified attribute / element names
// like `Expires`, `File`, `TOI` live.
using NsMap = std::unordered_map<std::string, std::string>;

NsMap ReadXmlnsDeclarations(const tinyxml2::XMLElement* e) {
    NsMap m;
    m[std::string{}] = std::string{};  // unprefixed attrs are in no-namespace
    for (const tinyxml2::XMLAttribute* a = e->FirstAttribute();
         a != nullptr; a = a->Next()) {
        const std::string name = a->Name() != nullptr ? a->Name() : "";
        const std::string val  = a->Value() != nullptr ? a->Value() : "";
        if (name.rfind("xmlns:", 0) == 0) {
            m[name.substr(6)] = val;
        }
    }
    return m;
}

// Split "prefix:local" into (prefix, local). If no colon, prefix == "".
std::pair<std::string_view, std::string_view> SplitQName(std::string_view qn) {
    auto pos = qn.find(':');
    if (pos == std::string_view::npos) return {std::string_view{}, qn};
    return {qn.substr(0, pos), qn.substr(pos + 1)};
}

// True when `qn` (a qualified name from the document) resolves to
// (target_uri, target_local) under the supplied prefix→URI map.
bool QNameMatches(std::string_view qn, const NsMap& ns,
                  std::string_view target_uri, std::string_view target_local) {
    auto [prefix, local] = SplitQName(qn);
    if (local != target_local) return false;
    auto it = ns.find(std::string(prefix));
    if (it == ns.end()) return false;
    return it->second == target_uri;
}

// Find the first child of `parent` whose qualified name resolves to
// (uri, local-name). Returns nullptr if no match.
tinyxml2::XMLElement*
FindChildByNs(tinyxml2::XMLElement* parent, const NsMap& ns,
              std::string_view target_uri, std::string_view target_local) {
    for (auto* c = parent->FirstChildElement(); c != nullptr;
         c = c->NextSiblingElement()) {
        if (c->Name() != nullptr &&
            QNameMatches(c->Name(), ns, target_uri, target_local)) {
            return c;
        }
    }
    return nullptr;
}

// Look up an attribute on `e` by namespace URI + local name. Skips the
// xmlns:* declarations themselves.
const char* FindAttrByNs(const tinyxml2::XMLElement* e, const NsMap& ns,
                          std::string_view target_uri,
                          std::string_view target_local) {
    for (const auto* a = e->FirstAttribute(); a != nullptr; a = a->Next()) {
        const std::string n = a->Name() != nullptr ? a->Name() : "";
        if (n == "xmlns" || n.rfind("xmlns:", 0) == 0) continue;
        if (QNameMatches(n, ns, target_uri, target_local)) {
            return a->Value();
        }
    }
    return nullptr;
}

}  // namespace

// RFC 6726 §3.3 instance-ID space is 20 bits (the EXT_FDT field
// reserves 20 bits). RFC 1982 serial-number arithmetic over a 2^20
// circle: half the space (2^19) is "ahead", the other half is
// "behind". We compute the forward distance modulo 2^20 and call
// the candidate "newer" iff the forward distance is in (0, 2^19).
bool LibFlute::FileDeliveryTable::IsNewerInstanceId(uint32_t candidate,
                                                     uint32_t current) {
  constexpr uint32_t kModulus  = 1u << 20;
  constexpr uint32_t kHalf     = 1u << 19;
  const uint32_t cand = candidate & (kModulus - 1u);
  const uint32_t cur  = current   & (kModulus - 1u);
  const uint32_t fwd  = (cand - cur) & (kModulus - 1u);
  return fwd != 0u && fwd < kHalf;
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, FecOti fec_oti)
  : _instance_id( instance_id )
  , _global_fec_oti( std::move(fec_oti) )
{
  switch (_global_fec_oti.encoding_id){
#ifdef RAPTOR_ENABLED
    case FecScheme::Raptor:
    case FecScheme::RaptorQ:
    _fdt_fec_transformer = std::make_unique<RaptorFEC>(_global_fec_oti.encoding_id);
    break;
#endif
    default:
    _fdt_fec_transformer = nullptr;
    break;
  }
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len)
  : _instance_id( instance_id )
{
  tinyxml2::XMLDocument doc(true, tinyxml2::COLLAPSE_WHITESPACE);
  if (doc.Parse(buffer, len) != tinyxml2::XML_SUCCESS) {
    throw std::runtime_error(std::string("Failed to parse FDT XML: ") +
                             (doc.ErrorStr() != nullptr ? doc.ErrorStr() : "unknown"));
  }
  auto* fdt_instance = doc.FirstChildElement("FDT-Instance");
  if (fdt_instance == nullptr) {
    throw std::runtime_error("FDT XML missing FDT-Instance root element");
  }
  const auto* expires_attr = fdt_instance->Attribute("Expires");
  if (expires_attr == nullptr) {
    throw std::runtime_error("FDT-Instance missing required Expires attribute");
  }
  try {
    _expires = std::stoull(expires_attr);
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Invalid Expires attribute on FDT-Instance: ") + ex.what());
  }

  spdlog::debug("Received new FDT with instance ID {}: {}", instance_id, buffer);

  // Resolve xmlns prefixes once at the FDT-Instance root. Every MBMS
  // qualified-name lookup below goes through this map so senders are
  // free to bind the standard URIs to whatever prefix they like.
  const NsMap ns = ReadXmlnsDeclarations(fdt_instance);

  // TS 26.346 cl. 7.2.10.2 (Rel-8 mbms2008): FullFDT boolean attribute
  // on FDT-Instance signals "this FDT supersedes prior partial ones".
  if (const char* full_fdt_attr =
          FindAttrByNs(fdt_instance, ns, kNsMbms2008, "FullFDT");
      full_fdt_attr != nullptr) {
    const std::string v(full_fdt_attr);
    _full_fdt = (v == "true" || v == "1");
  }

  // TS 26.346 cl. 7.2.10 (sv:schemaVersion): if a marker is present,
  // pick it up. Default stays at 1.
  if (auto* sv = FindChildByNs(fdt_instance, ns, kNsSchemaVersion,
                                "schemaVersion");
      sv != nullptr) {
    if (const char* text = sv->GetText(); text != nullptr) {
      _schema_version = static_cast<int>(strtol(text, nullptr, 0));
    }
  }

  // TS 26.346 cl. 7.2.10.2 (Rel-11/12 mbms2012): Base-URL-1 / Base-URL-2
  // children of FDT-Instance carry anyURI base URLs for resolving
  // relative File Content-Location references.
  if (auto* b1 = FindChildByNs(fdt_instance, ns, kNsMbms2012, "Base-URL-1");
      b1 != nullptr && b1->GetText() != nullptr) {
    _base_url_1 = b1->GetText();
  }
  if (auto* b2 = FindChildByNs(fdt_instance, ns, kNsMbms2012, "Base-URL-2");
      b2 != nullptr && b2->GetText() != nullptr) {
    _base_url_2 = b2->GetText();
  }

  uint8_t def_fec_encoding_id = 0;
  const auto* val = fdt_instance->Attribute("FEC-OTI-FEC-Encoding-ID");
  if (val != nullptr) {
    def_fec_encoding_id = strtoul(val, nullptr, 0);
  }

  uint32_t def_fec_max_source_block_length = 0;
  val = fdt_instance->Attribute("FEC-OTI-Maximum-Source-Block-Length");
  if (val != nullptr) {
    def_fec_max_source_block_length = strtoul(val, nullptr, 0);
  }

  uint32_t def_fec_encoding_symbol_length = 0;
  val = fdt_instance->Attribute("FEC-OTI-Encoding-Symbol-Length");
  if (val != nullptr) {
    def_fec_encoding_symbol_length = strtoul(val, nullptr, 0);
  }

  // TS 26.346 §7.2.10.1: the FEC-OTI-* attribute set on FDT-Instance
  // also includes Instance-ID, Max-Number-of-Encoding-Symbols and
  // Scheme-Specific-Info. All three serve as defaults for any File
  // that omits its own override.
  uint8_t def_fec_instance_id = 0;
  val = fdt_instance->Attribute("FEC-OTI-FEC-Instance-ID");
  if (val != nullptr) {
    def_fec_instance_id = static_cast<uint8_t>(strtoul(val, nullptr, 0));
  }

  uint32_t def_fec_max_number_of_encoding_symbols = 0;
  val = fdt_instance->Attribute("FEC-OTI-Max-Number-of-Encoding-Symbols");
  if (val != nullptr) {
    def_fec_max_number_of_encoding_symbols = strtoul(val, nullptr, 0);
  }

  std::string def_fec_scheme_specific_info;
  val = fdt_instance->Attribute("FEC-OTI-Scheme-Specific-Info");
  if (val != nullptr) {
    def_fec_scheme_specific_info = val;
  }

  for (auto* file = fdt_instance->FirstChildElement("File");
      file != nullptr; file = file->NextSiblingElement("File")) {

    // required attributes
    const auto* toi_str = file->Attribute("TOI");
    if (toi_str == nullptr) {
      throw std::runtime_error("Missing TOI attribute on File element");
    }
    uint32_t toi = strtoull(toi_str, nullptr, 0);

    const auto* content_location = file->Attribute("Content-Location");
    if (content_location == nullptr) {
      throw std::runtime_error("Missing Content-Location attribute on File element");
    }

    uint64_t content_length = 0;
    val = file->Attribute("Content-Length");
    if (val != nullptr) {
      content_length = strtoull(val, nullptr, 0);
    }

    uint64_t transfer_length = 0;
    val = file->Attribute("Transfer-Length");
    if (val != nullptr) {
      transfer_length = strtoull(val, nullptr, 0);
    } else {
      transfer_length = content_length;
    }

    const auto* content_md5 = file->Attribute("Content-MD5");
    if (content_md5 == nullptr) {
      content_md5 = "";
    }

    const auto* content_type = file->Attribute("Content-Type");
    if (content_type == nullptr) {
      content_type = "";
    }

    auto encoding_id = def_fec_encoding_id;
    val = file->Attribute("FEC-OTI-FEC-Encoding-ID");
    if (val != nullptr) {
      encoding_id = strtoul(val, nullptr, 0);
    }

    std::shared_ptr<FecTransformer> fec_transformer = nullptr;

    switch (encoding_id){
#ifdef RAPTOR_ENABLED
      case (int) FecScheme::Raptor:
        fec_transformer = std::make_shared<RaptorFEC>(FecScheme::Raptor);
        spdlog::debug("Received FDT entry for an R10 (Raptor) encoded file");
        break;
      case (int) FecScheme::RaptorQ:
        fec_transformer = std::make_shared<RaptorFEC>(FecScheme::RaptorQ);
        spdlog::debug("Received FDT entry for a RaptorQ encoded file");
        break;
#endif
      default:
        break;
    }


    auto max_source_block_length = def_fec_max_source_block_length;
    val = file->Attribute("FEC-OTI-Maximum-Source-Block-Length");
    if (val != nullptr) {
      max_source_block_length = strtoul(val, nullptr, 0);
    }

    auto encoding_symbol_length = def_fec_encoding_symbol_length;
    val = file->Attribute("FEC-OTI-Encoding-Symbol-Length");
    if (val != nullptr) {
      encoding_symbol_length = strtoul(val, nullptr, 0);
    }

    auto fec_instance_id = def_fec_instance_id;
    val = file->Attribute("FEC-OTI-FEC-Instance-ID");
    if (val != nullptr) {
      fec_instance_id = static_cast<uint8_t>(strtoul(val, nullptr, 0));
    }

    auto fec_max_number_of_encoding_symbols =
        def_fec_max_number_of_encoding_symbols;
    val = file->Attribute("FEC-OTI-Max-Number-of-Encoding-Symbols");
    if (val != nullptr) {
      fec_max_number_of_encoding_symbols = strtoul(val, nullptr, 0);
    }

    auto fec_scheme_specific_info = def_fec_scheme_specific_info;
    val = file->Attribute("FEC-OTI-Scheme-Specific-Info");
    if (val != nullptr) {
      fec_scheme_specific_info = val;
    }

    if (fec_transformer && !fec_transformer->parse_fdt_info(file)) {
      throw std::runtime_error("Failed to parse fdt info for specific FEC data");
    }

    // TS 26.346 cl. 7.2.10.2 (Rel-7 mbms2007): <Cache-Control> is an
    // <xs:choice> of three alternatives — exactly one of <Expires>,
    // <no-cache> or <max-stale>. Documents carrying more than one are
    // XSD-invalid; reject them rather than picking arbitrarily.
    uint64_t expires = 0;
    FileDeliveryTable::CacheControl cache_control =
        FileDeliveryTable::CacheControl::Expires;
    if (auto* cc = FindChildByNs(file, ns, kNsMbms2007, "Cache-Control");
        cc != nullptr) {
      int child_count = 0;
      bool have_expires  = false;
      bool have_nocache  = false;
      bool have_maxstale = false;
      for (auto* c = cc->FirstChildElement(); c != nullptr;
           c = c->NextSiblingElement()) {
        ++child_count;
        if (c->Name() == nullptr) continue;
        const std::string_view qn = c->Name();
        if (QNameMatches(qn, ns, kNsMbms2007, "Expires")) {
          have_expires = true;
          if (const char* t = c->GetText(); t != nullptr) {
            expires = strtoull(t, nullptr, 0);
          }
        } else if (QNameMatches(qn, ns, kNsMbms2007, "no-cache")) {
          have_nocache = true;
        } else if (QNameMatches(qn, ns, kNsMbms2007, "max-stale")) {
          have_maxstale = true;
        }
      }
      if (child_count > 1) {
        throw std::runtime_error(
            "mbms2007:Cache-Control violates xs:choice — multiple children present");
      }
      if (have_nocache)       cache_control = FileDeliveryTable::CacheControl::NoCache;
      else if (have_maxstale) cache_control = FileDeliveryTable::CacheControl::MaxStale;
      else if (have_expires)  cache_control = FileDeliveryTable::CacheControl::Expires;
    }

    FecOti fec_oti{
      (FecScheme)encoding_id,
        transfer_length,
        encoding_symbol_length,
        max_source_block_length,
        fec_scheme_specific_info,
        fec_instance_id,
        fec_max_number_of_encoding_symbols,
    };

    FileEntry fe{};
    fe.toi               = toi;
    fe.content_location  = std::string(content_location);
    fe.content_length    = content_length;
    fe.content_md5       = std::string(content_md5);
    fe.content_type      = std::string(content_type);
    fe.expires           = expires;
    fe.cache_control     = cache_control;
    fe.fec_oti           = fec_oti;
    fe.fec_transformer   = fec_transformer;

    // TS 26.346 cl. 7.2.10.2 (Rel-9 mbms2009): per-File decryption key URI.
    if (const char* dku =
            FindAttrByNs(file, ns, kNsMbms2009, "Decryption-KEY-URI");
        dku != nullptr) {
      fe.decryption_key_uri = dku;
    }

    // TS 26.346 cl. 7.2.10.2 (Rel-11/12 mbms2012): per-File MBMS attrs.
    if (const char* etag = FindAttrByNs(file, ns, kNsMbms2012, "File-ETag");
        etag != nullptr) {
      fe.file_etag = etag;
    }
    if (const char* frl =
            FindAttrByNs(file, ns, kNsMbms2012, "FEC-Redundancy-Level");
        frl != nullptr) {
      fe.fec_redundancy_level =
          static_cast<uint32_t>(strtoul(frl, nullptr, 0));
    }
    auto collect_alt_cl = [&](tinyxml2::XMLElement* parent,
                                std::vector<std::string>& out) {
      for (auto* e = parent->FirstChildElement(); e != nullptr;
           e = e->NextSiblingElement()) {
        if (e->Name() != nullptr &&
            QNameMatches(e->Name(), ns, kNsMbms2012,
                         "Alternate-Content-Location")) {
          if (const char* t = e->GetText(); t != nullptr) {
            out.emplace_back(t);
          }
        }
      }
    };
    if (auto* acl1 = FindChildByNs(file, ns, kNsMbms2012,
                                    "Alternate-Content-Location-1");
        acl1 != nullptr) {
      collect_alt_cl(acl1, fe.alternate_content_locations_1);
    }
    if (auto* acl2 = FindChildByNs(file, ns, kNsMbms2012,
                                    "Alternate-Content-Location-2");
        acl2 != nullptr) {
      collect_alt_cl(acl2, fe.alternate_content_locations_2);
    }

    // TS 26.346 cl. 7.2.10.2 (Rel-19 mbms2025): repair attributes.
    if (const char* rs = FindAttrByNs(file, ns, kNsMbms2025, "Repair-Start");
        rs != nullptr) {
      fe.repair_start = rs;
    }
    if (const char* rlp =
            FindAttrByNs(file, ns, kNsMbms2025, "Repair-Limit-Percentage");
        rlp != nullptr) {
      fe.repair_limit_percentage =
          static_cast<uint32_t>(strtoul(rlp, nullptr, 0));
    }

    _file_entries.push_back(fe);
  }
}

auto LibFlute::FileDeliveryTable::add(FileEntry& entry) -> void
{
  _instance_id++;
  _file_entries.push_back(entry);
}

auto LibFlute::FileDeliveryTable::remove(uint32_t toi) -> void
{
  for (auto it = _file_entries.begin(); it != _file_entries.end();) {
    if (it->toi == toi) {
      it = _file_entries.erase(it);
    } else {
      ++it;
    }
  }
  _instance_id++;
}

auto LibFlute::FileDeliveryTable::to_string() const -> std::string {
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild( doc.NewDeclaration() );
  auto* root = doc.NewElement("FDT-Instance");
  root->SetAttribute("Expires", std::to_string(_expires).c_str());
  root->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned)_global_fec_oti.encoding_id);
  root->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", (unsigned)_global_fec_oti.max_source_block_length);
  root->SetAttribute("FEC-OTI-Encoding-Symbol-Length", (unsigned)_global_fec_oti.encoding_symbol_length);
  // TS 26.346 §7.2.10.1: emit Instance-ID / Max-Number-of-Encoding-Symbols
  // / Scheme-Specific-Info on the FDT-Instance root only if the encoder's
  // FDT-Instance defaults set them. Sentinel (0 / empty) ⇒ omit, matching
  // the spec's "absent attribute = no default applies" semantics.
  if (_global_fec_oti.instance_id != 0) {
    root->SetAttribute("FEC-OTI-FEC-Instance-ID",
                        (unsigned)_global_fec_oti.instance_id);
  }
  if (_global_fec_oti.max_number_of_encoding_symbols != 0) {
    root->SetAttribute("FEC-OTI-Max-Number-of-Encoding-Symbols",
                        (unsigned)_global_fec_oti.max_number_of_encoding_symbols);
  }
  if (!_global_fec_oti.scheme_specific_info.empty()) {
    root->SetAttribute("FEC-OTI-Scheme-Specific-Info",
                        _global_fec_oti.scheme_specific_info.c_str());
  }
  // TS 26.346 cl. 7.2.10 + per-release XSDs: declare all the MBMS
  // namespaces this implementation may emit, plus the schema-version
  // namespace. Receivers expect these prefixes when reading the
  // qualified attributes/elements below.
  root->SetAttribute("xmlns:mbms2007", "urn:3GPP:metadata:2007:MBMS:FLUTE:FDT");
  root->SetAttribute("xmlns:mbms2008", "urn:3GPP:metadata:2008:MBMS:FLUTE:FDT_ext");
  root->SetAttribute("xmlns:mbms2009", "urn:3GPP:metadata:2009:MBMS:FLUTE:FDT_ext");
  root->SetAttribute("xmlns:mbms2012", "urn:3GPP:metadata:2012:MBMS:FLUTE:FDT");
  root->SetAttribute("xmlns:mbms2025", "urn:3GPP:metadata:2025:MBMS:FLUTE:FDT");
  root->SetAttribute("xmlns:sv",       "urn:3GPP:metadata:2009:MBMS:schemaVersion");

  if (_full_fdt.has_value()) {
    root->SetAttribute("mbms2008:FullFDT", *_full_fdt ? "true" : "false");
  }
  doc.InsertEndChild(root);

  // TS 26.346 cl. 7.2.10: emit the schema-version structural marker
  // before any Base-URL or File children. The Rel-9 schemaVersion XSD
  // declares it as a top-level child of FDT-Instance.
  {
    auto* sv = doc.NewElement("sv:schemaVersion");
    sv->SetText(std::to_string(_schema_version).c_str());
    root->InsertEndChild(sv);
  }

  if (_base_url_1.has_value()) {
    auto* b1 = doc.NewElement("mbms2012:Base-URL-1");
    b1->SetText(_base_url_1->c_str());
    root->InsertEndChild(b1);
  }
  if (_base_url_2.has_value()) {
    auto* b2 = doc.NewElement("mbms2012:Base-URL-2");
    b2->SetText(_base_url_2->c_str());
    root->InsertEndChild(b2);
  }

  for (const auto& file : _file_entries) {
    auto* f = doc.NewElement("File");
    f->SetAttribute("TOI", file.toi);
    f->SetAttribute("Content-Location", file.content_location.c_str());
    f->SetAttribute("Content-Length", static_cast<uint64_t>(file.content_length));
    f->SetAttribute("Transfer-Length", static_cast<uint64_t>(file.fec_oti.transfer_length));
    f->SetAttribute("Content-MD5", file.content_md5.c_str());
    f->SetAttribute("Content-Type", file.content_type.c_str());
    if (!file.decryption_key_uri.empty()) {
      f->SetAttribute("mbms2009:Decryption-KEY-URI", file.decryption_key_uri.c_str());
    }
    if (!file.file_etag.empty()) {
      f->SetAttribute("mbms2012:File-ETag", file.file_etag.c_str());
    }
    if (file.fec_redundancy_level.has_value()) {
      f->SetAttribute("mbms2012:FEC-Redundancy-Level", *file.fec_redundancy_level);
    }
    if (!file.repair_start.empty()) {
      f->SetAttribute("mbms2025:Repair-Start", file.repair_start.c_str());
    }
    if (file.repair_limit_percentage.has_value()) {
      f->SetAttribute("mbms2025:Repair-Limit-Percentage",
                      *file.repair_limit_percentage);
    }
    // TS 26.346 §7.2.10.1: per-File FEC-OTI-* attributes override the
    // FDT-Instance defaults. Emit only the fields that differ from the
    // FDT-Instance default (sentinel 0 / empty inherits) so the FDT
    // stays compact when every File shares the session defaults — the
    // common case for homogeneous broadcasts.
    if (file.fec_oti.encoding_id != _global_fec_oti.encoding_id) {
      f->SetAttribute("FEC-OTI-FEC-Encoding-ID",
                       (unsigned)file.fec_oti.encoding_id);
    }
    if (file.fec_oti.encoding_symbol_length != 0 &&
        file.fec_oti.encoding_symbol_length !=
            _global_fec_oti.encoding_symbol_length) {
      f->SetAttribute("FEC-OTI-Encoding-Symbol-Length",
                       (unsigned)file.fec_oti.encoding_symbol_length);
    }
    if (file.fec_oti.max_source_block_length != 0 &&
        file.fec_oti.max_source_block_length !=
            _global_fec_oti.max_source_block_length) {
      f->SetAttribute("FEC-OTI-Maximum-Source-Block-Length",
                       (unsigned)file.fec_oti.max_source_block_length);
    }
    if (file.fec_oti.instance_id != 0 &&
        file.fec_oti.instance_id != _global_fec_oti.instance_id) {
      f->SetAttribute("FEC-OTI-FEC-Instance-ID",
                       (unsigned)file.fec_oti.instance_id);
    }
    if (file.fec_oti.max_number_of_encoding_symbols != 0 &&
        file.fec_oti.max_number_of_encoding_symbols !=
            _global_fec_oti.max_number_of_encoding_symbols) {
      f->SetAttribute("FEC-OTI-Max-Number-of-Encoding-Symbols",
                       (unsigned)file.fec_oti.max_number_of_encoding_symbols);
    }
    if (!file.fec_oti.scheme_specific_info.empty() &&
        file.fec_oti.scheme_specific_info !=
            _global_fec_oti.scheme_specific_info) {
      f->SetAttribute("FEC-OTI-Scheme-Specific-Info",
                       file.fec_oti.scheme_specific_info.c_str());
    }
    if (file.fec_transformer) {
      file.fec_transformer->add_fdt_info(f);
    }

    auto emit_alt_cl = [&](const char* tag,
                            const std::vector<std::string>& urls) {
      if (urls.empty()) return;
      auto* parent = doc.NewElement(tag);
      for (const auto& url : urls) {
        auto* e = doc.NewElement("mbms2012:Alternate-Content-Location");
        e->SetText(url.c_str());
        parent->InsertEndChild(e);
      }
      f->InsertEndChild(parent);
    };
    emit_alt_cl("mbms2012:Alternate-Content-Location-1",
                file.alternate_content_locations_1);
    emit_alt_cl("mbms2012:Alternate-Content-Location-2",
                file.alternate_content_locations_2);

    // TS 26.346 cl. 7.2.10.2 (Rel-7): emit the Cache-Control choice
    // matching the FileEntry's discriminator. NoCache / MaxStale skip
    // the Expires value.
    auto* cc = doc.NewElement("mbms2007:Cache-Control");
    switch (file.cache_control) {
      case FileDeliveryTable::CacheControl::NoCache: {
        auto* nc = doc.NewElement("mbms2007:no-cache");
        nc->SetText("true");
        cc->InsertEndChild(nc);
        break;
      }
      case FileDeliveryTable::CacheControl::MaxStale: {
        auto* ms = doc.NewElement("mbms2007:max-stale");
        ms->SetText("true");
        cc->InsertEndChild(ms);
        break;
      }
      case FileDeliveryTable::CacheControl::Expires:
      default: {
        auto* exp = doc.NewElement("mbms2007:Expires");
        exp->SetText(std::to_string(file.expires).c_str());
        cc->InsertEndChild(exp);
        break;
      }
    }
    f->InsertEndChild(cc);
    root->InsertEndChild(f);
  }


  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return {printer.CStr()};
}
