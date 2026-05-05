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
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include "flute_types.h"
#include "fec/FecTransformer.h"

namespace LibFlute {
  /**
   *  A class for parsing and creating FLUTE FDTs (File Delivery Tables).
   */
  class FileDeliveryTable {
    public:
     /**
      *  Create an empty FDT
      *
      *  @param instance_id FDT instance ID to set
      *  @param fec_oti Global FEC OTI parameters
      */
      FileDeliveryTable(uint32_t instance_id, FecOti fec_oti);

     /**
      *  Parse an XML string and create a FDT class from it
      *
      *  @param instance_id FDT instance ID (from ALC headers)
      *  @param buffer String containing the FDT XML
      *  @param len Length of the buffer
      */
      FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len);

     /**
      *  Default destructor.
      */
      virtual ~FileDeliveryTable() = default;

     /**
      *  Get the FDT instance ID
      */
      uint32_t instance_id() { return _instance_id; };

     /**
      *  TS 26.346 cl. 7.2.10.2 (Rel-7 mbms2007): the <Cache-Control>
      *  element is an XSD <xs:choice> of three alternatives. Senders
      *  MUST emit at most one; receivers MUST reject documents that
      *  carry more than one. The default (when no Cache-Control
      *  element is present) is treated as Expires with the value
      *  carried in `expires` below — that's the path the round-1
      *  tests already exercised before MBMS variants were added.
      */
      enum class CacheControl {
        Expires,    // <mbms2007:Expires>NTP-seconds</...>
        NoCache,    // <mbms2007:no-cache>true</...>
        MaxStale,   // <mbms2007:max-stale>true</...>
      };

     /**
      *  An entry for a file in the FDT
      */
      struct FileEntry {
        uint32_t toi;
        std::string content_location;
        uint64_t content_length;
        std::string content_md5;
        std::string content_type;
        uint64_t expires;
        CacheControl cache_control = CacheControl::Expires;
        FecOti fec_oti;
        std::shared_ptr<FecTransformer> fec_transformer;

        // TS 26.346 cl. 7.2.10.2 MBMS extension fields. Empty / nullopt
        // means "not present in the FDT XML".
        std::string decryption_key_uri;                          // mbms2009 (Rel-9)
        std::vector<std::string> alternate_content_locations_1;  // mbms2012 (Rel-11/12)
        std::vector<std::string> alternate_content_locations_2;  // mbms2012
        std::optional<uint32_t> fec_redundancy_level;            // mbms2012
        std::string file_etag;                                    // mbms2012
        std::string repair_start;                                 // mbms2025 (Rel-19) xs:dateTime
        std::optional<uint32_t> repair_limit_percentage;          // mbms2025
      };

     /**
      *  Set the expiry value
      */
      void set_expires(uint64_t exp) { _expires = exp; };

     /**
      *  Add a file entry
      */
      void add(FileEntry& entry);

     /**
      *  Remove a file entry
      */
      void remove(uint32_t toi);

     /**
      *  Serialize the FDT to an XML string
      */
      std::string to_string() const;

     /**
      *  Get all current file entries
      */
      std::vector<FileEntry> file_entries() { return _file_entries; };

      // TS 26.346 cl. 7.2.10.2 FDT-Instance-level MBMS extension fields.
      // nullopt means "not present in the FDT XML".
      std::optional<bool> full_fdt() const { return _full_fdt; }            // mbms2008 (Rel-8)
      std::optional<std::string> base_url_1() const { return _base_url_1; } // mbms2012 (Rel-11/12)
      std::optional<std::string> base_url_2() const { return _base_url_2; }
      // TS 26.346 cl. 7.2.10 + TS26346_SchemaVersion.xsd: a serialised
      // FDT-Instance MUST carry an <sv:schemaVersion> marker. Defaults
      // to 1 (matches the marker emitted by libflute today and the
      // value tagged by the schemaVersion XSD).
      int schema_version() const { return _schema_version; }

      void set_full_fdt(bool v) { _full_fdt = v; }
      void set_base_url_1(std::string v) { _base_url_1 = std::move(v); }
      void set_base_url_2(std::string v) { _base_url_2 = std::move(v); }
      void set_schema_version(int v) { _schema_version = v; }

    private:
      uint32_t _instance_id;

      std::vector<FileEntry> _file_entries;
      FecOti _global_fec_oti;
      std::unique_ptr<FecTransformer> _fdt_fec_transformer = nullptr;

      uint64_t _expires;

      std::optional<bool> _full_fdt;
      std::optional<std::string> _base_url_1;
      std::optional<std::string> _base_url_2;
      int _schema_version = 1;
  };
};
