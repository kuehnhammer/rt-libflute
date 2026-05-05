# Test coverage map

Living document. Lists every test in `tests/unit/`, the spec clause it
encodes, and its state (active / skip / planned). Survives context
compaction by being checked in. Every change to the test surface must
update this file; that's the rule.

## Spec oracles (read-only)

| Source | Path / location | Role |
|--------|-----------------|------|
| RFC 6726 | `doc/rfc6726.txt` | FLUTE v2 — primary protocol spec |
| RFC 5651 | (IETF) | LCT base header |
| RFC 5775 | (IETF) | ALC framing |
| RFC 5052 | (IETF) | FEC building blocks (FEC OTI, FEC payload ID, source-block partitioning §9.1) |
| RFC 5445 | (IETF) | Compact No-Code FEC scheme |
| RFC 3926 | (IETF) | FLUTE v1 — accepted for back-compat in EXT_FDT |
| 3GPP TS 26.346 | `/home/klaus/workspace/3gpp/26346-j30/` | MBMS user-services file delivery; profiles RFC 6726 + adds release-versioned FDT XSD overlays |

MBMS XSD release map:

| Release | Namespace prefix | XSD file | Adds |
|---------|------------------|----------|------|
| Rel-6  | `mbms2005` | `TS26346_FLUTE-FDT_Extensions_Rel-6.xsd`  | Group, MBMS-Session-Identity (out of scope) |
| Rel-7  | `mbms2007` | `TS26346_FLUTE-FDT_Extensions_Rel-7.xsd`  | `<Cache-Control>` choice (no-cache \| max-stale \| Expires) |
| Rel-8  | `mbms2008` | `TS26346_FLUTE-FDT_Extensions_Rel-8.xsd`  | `FullFDT` boolean attribute on `FDT-Instance` |
| Rel-9  | `mbms2009` | `TS26346_FLUTE-FDT_Extensions_Rel-9.xsd`  | `Decryption-KEY-URI` anyURI attribute on `File` |
| Rel-11/12 | `mbms2012` | `TS26346_FLUTE-FDT_Extensions_Rel-11.xsd` | `Alternate-Content-Location-1/-2`, `Base-URL-1/-2`, `FEC-Redundancy-Level`, `File-ETag` |
| Rel-13 | `mbms2015` | `TS26346_FLUTE-FDT_Extensions_Rel-13.xsd` | `IndependentUnitPositions` (out of scope) |
| Rel-19 | `mbms2025` | `TS26346_FLUTE-FDT_Extensions_Rel-19.xsd` | `Repair-Start` dateTime, `Repair-Limit-Percentage` |

Schema versioning marker: `<sv:schemaVersion>` from
`TS26346_SchemaVersion.xsd`, namespace
`urn:3GPP:metadata:2009:MBMS:schemaVersion`.

## Test files

States: **active** (runs, must pass), **skip** (compiled, GTEST_SKIP
with rationale), **planned** (not yet written; documented here so we
remember).

### `tests/unit/alc_packet_test.cpp` — RFC 5651/5775/6726/5052 wire format

| Test | Spec | State |
|------|------|-------|
| `RejectsPacketsShorterThan4Bytes` | RFC 5651 §5.1 (LCT base ≥ 4 bytes) | active |
| `RejectsLctVersionNotEqualOne` | RFC 5651 §5.1 (V field) | active |
| `RejectsLctHeaderLengthOfZero` | RFC 5651 §5.1 (HDR_LEN ≥ 1) | active |
| `RejectsLctHeaderLengthGreaterThanPacket` | RFC 5651 §5.1 + parse safety | active |
| `ParsesShortTsiHalfWordOnly` | RFC 5651 §5.1 (TSI half-word) | active |
| `ParsesFullTsi48Bits` | RFC 5651 §5.1 (TSI 48 bits) | active |
| `ParsesToiFlagValueZero` | RFC 6726 §3.1 (TOI half only) | active |
| `ParsesToiFlagValueOne32BitExtension` | RFC 6726 §3.1 (TOI 48 bits) | active |
| `ParsesToiFlagValueTwo64BitExtension` | RFC 6726 §3.1 (TOI 80 bits) | active |
| `MapsCodepointZeroToCompactNoCode` | RFC 6726 §5 + RFC 5445 | active |
| `MapsCodepointOneToRaptor` | RFC 6726 §5 + RFC 5053 | active |
| `ParsesExtFdtVersionOneAndExtractsInstanceId` | RFC 3926 (FLUTE v1) | active |
| `AcceptsExtFdtVersionTwoPerRfc6726` | RFC 6726 §3.4.1 (FLUTE v2) | active (was skip; round-2 fix) |
| `RejectsExtFdtVersionThreeAndAbove` | RFC 6726 §3.4.1 (negative case) | active |
| `ParsesExtCencAlgorithmGzip` | RFC 6726 §3.4.3 (EXT_CENC HET=193) | active |
| `ParsesExtFtiCompactNoCodeFields` | RFC 5052 §3.4.3 (EXT_FTI HET=64) | active |
| `RejectsExtensionRunningPastLctHeaderEnd` | RFC 5651 parse safety | active |
| `ParsesExtNopHelOneFollowedByExtFdt` | RFC 5651 §3.2.5.1 (EXT_NOP variable HEL) | active (round-2 fix) |
| `ParsesTwoConsecutiveExtNopHelOne` | RFC 5651 §3.2.5.1 (alignment between extensions) | active (round-2 fix) |
| `ParsesExtNopHelTwoFollowedByExtFdt` | RFC 5651 §3.2.5.1 (HEL=2 case) | active (round-2 fix) |
| `ParsesExtAuthAndExtTimeFollowedByExtFdt` | RFC 5651 §3.2.5.3/.4 | active (round-2 fix) |
| `BuildAndParseRoundTripPreservesAllFields` | integration | active |

### `tests/unit/encoding_symbol_test.cpp` — RFC 5052 §3.5 SBN/ESI framing

| Test | Spec | State |
|------|------|-------|
| `ExtractsSbnAndEsiInNetworkByteOrder` | RFC 5052 §3.5 | active |
| `RejectsContentEncodingOtherThanNone` | RFC 6726 §3.4.3 (CENC scope) | active |
| `RejectsPayloadShorterThanSbnEsiHeader` | RFC 5052 §3.5 (4-byte FEC payload ID required) | active |
| `MultiSymbolPayloadEmitsSequentialEsi` | RFC 5052 §3.5 + RFC 5053 §3.4 | active |
| `LastPartialSymbolHasCorrectLength` | RFC 5052 §9.1 (residual symbol bytes < T) | active (was skip; round-2 fix) |
| `ToPayloadFromPayloadPreservesEsiSequence` | integration | active |

### `tests/unit/fdt_test.cpp` — RFC 6726 §3.4.2 / §5 FDT XML schema

| Test | Spec | State |
|------|------|-------|
| `RejectsFdtInstanceMissingExpires` | RFC 6726 §3.4.2 (Expires REQUIRED) | active |
| `RejectsXmlMissingFdtInstanceRoot` | RFC 6726 §5 (root MUST be FDT-Instance) | active |
| `ParsesFdtInstanceWithMinimalFileEntry` | RFC 6726 §3.4.2 | active |
| `RejectsFileEntryMissingToi` | RFC 6726 §3.4.2 (TOI REQUIRED) | active |
| `RejectsFileEntryMissingContentLocation` | RFC 6726 §3.4.2 (Content-Location REQUIRED) | active |
| `FdtLevelFecOtiInheritedByFileEntries` | RFC 6726 §3.4.2 (FEC-OTI inheritance) | active |
| `FileLevelFecOtiOverridesFdtLevel` | RFC 6726 §3.4.2 (per-File override) | active |
| `Mbms2007CacheControlExpiresParsedIntoFileEntry` | TS 26.346 cl. 7.2.10.2 (mbms2007) | active |
| `RoundTripFromFileEntryToStringAndBack` | integration | active |

### `tests/unit/fdt_mbms_test.cpp` — TS 26.346 cl. 7.2.10 MBMS profile

| Test | Spec | State |
|------|------|-------|
| `Rel7CacheControlExpiresAttachedToFileEntry` | TS 26.346 cl. 7.2.10.2 (Rel-7 Expires) | active |
| `Rel7CacheControlNoCacheRecognised` | TS 26.346 cl. 7.2.10.2 (Rel-7 no-cache) | active (round-3) |
| `Rel7CacheControlMaxStaleRecognised` | TS 26.346 cl. 7.2.10.2 (Rel-7 max-stale) | active (round-3) |
| `Rel7CacheControlChoiceMutualExclusion` | Rel-7 XSD `<xs:choice>` enforcement | active (round-3) |
| `Rel8FullFdtBooleanRoundTrip` | Rel-8 (mbms2008:FullFDT) | active (round-3) |
| `Rel9DecryptionKeyUriRoundTrip` | Rel-9 (mbms2009:Decryption-KEY-URI) | active (round-3) |
| `Rel12AlternateContentLocationListPreserved` | Rel-11/12 (mbms2012:Alternate-Content-Location-1/-2) | active (round-3) |
| `Rel12BaseUrl1And2Preserved` | Rel-11/12 (mbms2012:Base-URL-1/-2) | active (round-3) |
| `Rel12FecRedundancyLevelPreserved` | Rel-11/12 (mbms2012:FEC-Redundancy-Level) | active (round-3) |
| `Rel12FileEtagPreserved` | Rel-11/12 (mbms2012:File-ETag) | active (round-3) |
| `SchemaVersionMarkerAcceptedOnParse` | TS 26.346 cl. 7.2.10 (sv:schemaVersion) | active (round-3) |
| `ToStringEmitsSchemaVersionMarker` | TS 26.346 cl. 7.2.10 (sv:schemaVersion emit) | active (round-3) |
| `Rel19RepairAttributesPreservedIfPresent` | Rel-19 (mbms2025:Repair-Start, Repair-Limit-Percentage) | active (round-3) |
| `ParserResolvesXmlNamespacesViaXmlnsMapNotPrefixString` | XML namespaces 1.0 §6.1 — prefix is arbitrary, namespace URI is normative | active (round-3) |

### `tests/unit/file_partitioning_test.cpp` — RFC 5052 §9.1

| Test | Spec | State |
|------|------|-------|
| `NofSourceSymbolsCeilTransferLengthDivT` | RFC 5052 §9.1 (N_S = ceil(F/T)) | active |
| `MatchesRfc5052Section91WhenLargeAndSmallDiffer` | RFC 5052 §9.1 (A_large/A_small worked example) | active |
| `GetNextSymbolsReturnsCorrectCountForGivenMaxSize` | integration (encoder API) | active |
| `CheckFileCompletionFiresWhenAllBlocksCompleteNoFec` | RFC 6726 §3.2 (no-FEC completion) | active |

### `tests/unit/file_test.cpp` — File class + calculate_md5

| Test | Spec | State |
|------|------|-------|
| `CalculateMd5.ReturnsNegativeSentinelOnNullInput` | (libflute internal contract) | active (round-2 fix) |
| `CalculateMd5.ReturnsNegativeSentinelOnZeroLength` | (libflute internal contract) | active (round-2 fix) |
| `CalculateMd5.ReturnsDigestLengthOnValidInput` | (libflute internal contract) | active |
| `FileConstruction.RejectsZeroLengthDataBuffer` | RFC 6726 §3.4.2 (Content-MD5 must be correct when emitted) | active (round-2 fix) |

### `tests/unit/decoder_test.cpp` — Decoder consumer entry path

(File renamed from `direct_receiver_test.cpp` in round 5 alongside the
DirectReceiver→Decoder API rename.)

| Test | Spec | State |
|------|------|-------|
| `Decoder.RejectsPacketsWithMismatchedTsi` | RFC 5651 §3.1 (session TSI filter) | active |
| `Decoder.FdtPacketWithToi0AndExtFdtTriggersFdtParse` | RFC 6726 §3.4.1 + App. A | active |
| `Decoder.FilePacketRoutedToFileByToi` | RFC 6726 App. A step 4 | active |
| `Decoder.CompletionCallbackFiresWhenFileFullyReceived` | RFC 6726 App. A step 7 | active |

### `tests/unit/stats_test.cpp` — Encoder/Decoder operational counters

Counters are advertised via `LibFlute::EncoderStats` and
`LibFlute::DecoderStats`; consumers pick which to surface to
operators.

| Test | What it pins | State |
|------|--------------|-------|
| `EncoderStats.FreshEncoderHasAllZeroCounters` | All counters start at 0 | active (round-6) |
| `DecoderStats.FreshDecoderHasAllZeroCounters` | All counters start at 0 | active (round-6) |
| `StatsIntegration.OneFileRoundTripCountersMatch` | Encoder.bytes_emitted == Decoder.bytes_received; one file flows end-to-end | active (round-6) |
| `DecoderStats.WrongTsiPacketIncrementsDroppedCounter` | dropped_wrong_tsi semantics | active (round-6) |
| `DecoderStats.MalformedPacketIncrementsDroppedCounter` | dropped_malformed semantics | active (round-6) |
| `DecoderStats.StaleFdtPacketIncrementsDroppedCounter` | dropped_stale_fdt semantics | active (round-6) |
| `DecoderStats.EvictedIncompleteFileCountsAsDiscarded` | files/bytes_discarded_incomplete bumped on eviction | active (round-6) |
| `DecoderStats.RaptorLossyRoundTripDistinguishesSourceVsRepair` | source_symbols_received vs repair_symbols_received split correctly under loss | active (round-6, gated on RAPTOR_ENABLED) |

### `tests/bench/flute_bench.cpp` — end-to-end perf benchmark

Not a gtest target; built only with `-DLIBFLUTE_BUILD_BENCH=ON`.
Round-trips 10 / 100 / 500 MB buffers through Encoder→Decoder
in-process and prints throughput, packet counts, and FEC overhead.
Round-6 baseline numbers (Release, bitstem-r10 fast decoder, mtu=1500):

| F | FEC | Throughput | FEC overhead |
|---|---|---|---|
| 10 MB  | None   | ~305 MB/s | 0 |
| 10 MB  | Raptor | ~2.4 MB/s | 15.0 % |
| 100 MB | None   | ~105 MB/s | 0 |
| 100 MB | Raptor | ~2.1 MB/s | 13.5 % |
| 500 MB | None   | ~10.7 MB/s | 0 |
| 500 MB | Raptor | ~2.1 MB/s | 14.9 % |

The Raptor numbers are dramatically below the bare bitstem-r10
codec's measured throughput; the FLUTE-layer glue is the bottleneck.
Investigation is round-7+ work.

### `tests/unit/integration_test.cpp` — TX→RX round-trip

End-to-end. Encoder emits ALC packet bytes via its PacketCallback;
the test pipes them straight into Decoder.feed_packet and asserts the
received File buffer equals the sent buffer. No sockets, no asio.

| Test | Spec | State |
|------|------|-------|
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F1_mtu1500` | RFC 5052 §9.1 (tiny F < T) | active (round-5) |
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F64_mtu1500` | RFC 5052 §9.1 (small F < T) | active (round-5) |
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F1456_mtu1500` | RFC 5052 §9.1 (single full symbol) | active (round-5) |
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F1457_mtu1500` | RFC 5052 §9.1 (T+1 partial residue) | active (round-5) |
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F8192_mtu576` | RFC 5052 §9.1 (small MTU multi-block) | active (round-5) |
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F65536_mtu1500` | RFC 5052 §9.1 (64 KiB) | active (round-5) |
| `CompactNoCodeRoundTrip.FileBytesMatchAfterTransport/F262144_mtu1500` | RFC 5052 §9.1 (256 KiB deep multi-block) | active (round-5) |
| `RaptorRoundTrip.FileBytesMatchAfterTransport/F4096_mtu1500` | RFC 5053 + lib/raptor (small block) | active (round-5, gated on RAPTOR_ENABLED) |
| `RaptorRoundTrip.FileBytesMatchAfterTransport/F65536_mtu1500` | RFC 5053 + lib/raptor | active (round-5, gated on RAPTOR_ENABLED) |
| `RaptorRoundTrip.FileBytesMatchAfterTransport/F200000_mtu1500` | RFC 5053 + lib/raptor (multi-block) | active (round-5, gated on RAPTOR_ENABLED) |
| `RaptorLossyRoundTrip.FileBytesMatchAfterRaptorRepairsLoss/F65536_mtu1500_drop1in20` | RFC 5053 inactivation decoding | active (round-5, gated on RAPTOR_ENABLED) |
| `RaptorLossyRoundTrip.FileBytesMatchAfterRaptorRepairsLoss/F200000_mtu1500_drop1in10` | RFC 5053 inactivation decoding | active (round-5, gated on RAPTOR_ENABLED) |
| `RaptorLossyRoundTrip.FileBytesMatchAfterRaptorRepairsLoss/F200000_mtu1500_drop1in20` | RFC 5053 inactivation decoding | active (round-5, gated on RAPTOR_ENABLED) |

### `tests/unit/fdt_lifecycle_test.cpp` — RFC 6726 §3.3 FDT updates

| Test | Spec | State |
|------|------|-------|
| `NewerFdtAcceptedAndAddsFileEntries` | RFC 6726 §3.3 + App. A step 6 | active (round-3) |
| `OlderFdtIsRejectedAfterNewerSeen` | RFC 6726 §3.3 monotonicity | active (round-3, surfaces+fixes ReceiverBase `!=` bug) |
| `RemovedFileEntryFromNewFdtDoesNotEvictExistingFile` | RFC 6726 §3.3 ¶5 | active (round-3) |
| `RepeatedFdtWithSameInstanceIdIsIdempotent` | RFC 6726 §3.3 | active (round-3) |
| `FdtInstanceIdComparator.ForwardSmallStepIsNewer` | RFC 1982 + RFC 6726 §3.3 | active (round-4) |
| `FdtInstanceIdComparator.BackwardSmallStepIsNotNewer` | RFC 1982 + RFC 6726 §3.3 | active (round-4) |
| `FdtInstanceIdComparator.EqualIsNotNewer` | RFC 1982 + RFC 6726 §3.3 | active (round-4) |
| `FdtInstanceIdComparator.WraparoundForwardAcrossZero` | RFC 1982 + RFC 6726 §3.3 (20-bit wrap) | active (round-4) |
| `FdtInstanceIdComparator.MidpointResolvesAsNotNewer` | RFC 1982 ambiguity resolution | active (round-4) |
| `InstanceIdWraparoundAt2Pow20IsCircular` | RFC 6726 §3.3 (integration via DirectReceiver) | active (round-4) |
| `FdtExpires.IsExpiredReturnsTrueWhenNowExceedsExpires` | RFC 6726 §3.3 Expires semantics | active (round-4) |
| `ExpiredFdtInstanceIsRejectedAtParseTime` | RFC 6726 §3.3 (integration via DirectReceiver + injectable clock) | active (round-4) |
| `NonExpiredFdtInstanceIsAccepted` | RFC 6726 §3.3 contrapositive | active (round-4) |
| `InFlightFdtCollisionWhenNewerInstanceArrivesMidReceive` | RFC 6726 §3.3 + ReceiverBase routing | active (round-4, surfaces+fixes the latent in-flight collision) |
| `InFlightFdtIgnoresOlderInstancePackets` | RFC 6726 §3.3 monotonicity (in-flight phase) | active (round-4) |

## Bugs fixed

| Round | Bug | Spec affected | Commit |
|-------|-----|---------------|--------|
| 2 | EncodingSymbol::from_payload last-symbol len() == T | RFC 5052 §9.1 | `324953a` |
| 2 | AlcPacket EXT_NOP/AUTH/TIME `+= 3` over/under-advance | RFC 5651 §3.2.5 | `324953a` |
| 2 | calculate_md5 broken `< 0` error path (unsigned return) | (libflute internal) | `324953a` |
| 2 | EXT_FDT rejected FLUTE v2 | RFC 6726 §3.4.1 | `324953a` |
| 2 | AlcPacket producer: redundant codepoint overwrite (latent) | RFC 6726 §5 | `324953a` |
| 3 | FDT parser pattern-matched literal prefix instead of resolving xmlns:* declarations to URIs | XML Namespaces 1.0 §6.1 | `b608e7a` |
| 3 | mbms2007:Cache-Control `<xs:choice>` not enforced (multi-child documents accepted silently) | TS 26.346 cl. 7.2.10.2 + Rel-7 XSD | `b608e7a` |
| 3 | ReceiverBase used `!=` for FDT instance-ID comparison; older FDTs could supersede newer ones | RFC 6726 §3.3 monotonicity | `b608e7a` |
| 4 | ReceiverBase linear `>` comparison didn't handle 20-bit instance-ID wraparound | RFC 6726 §3.3 + RFC 1982 | `f2c9a81` |
| 4 | Receiver never checked FDT-Instance Expires; expired FDTs were applied indefinitely | RFC 6726 §3.3 Expires | `f2c9a81` |
| 4 | In-flight TOI=0 File (sized for v=N) accepted v=N+1 packet bytes, corrupting both | RFC 6726 §3.3 + ReceiverBase routing | `f2c9a81` |
| 5 | RaptorFEC encoder passed a non-`K*T`-sized source span to bitstem-r10's Encoder::Create when F was not a multiple of T; the codec rejected it | RFC 5053 §4.2 (zero-pad last source symbol to T) | `27b6866` |
| 5 | RaptorFEC `add_fdt_info` wrote per-attribute Z/N/Al fields but `parse_fdt_info` reads a base64'd `FEC-OTI-Scheme-Specific-Info` blob; sender and receiver disagreed on wire format | RFC 6726 §3.4.2 + RFC 5053 §3.2 | `27b6866` |
| 6 | Sender-side `File` set `max_source_block_length = K*T` (bytes) for Raptor, breaking source/repair classification on both sides (CompactNoCode correctly used K-in-symbols) | RFC 5052 §3.4.2 | `36bbf17` |
| 7 | RaptorFEC used `K = min(Kt, 8192) + remainder-in-last-block` partitioning; for Kt where Kt mod 8192 < 4 the last block's K fell below `kJKMinK = 4` and `bitstem::r10::Encoder::Create` rejected it (e.g. F = 11.4 MB at mtu=1500 → Kt=8195 → last block K=3, send fails). Replaced with proper §4.4.1.2 KL/KS/ZL/ZS distribution. | RFC 5053 §4.4.1.2 | (round-7 commit) |

## Out of scope (future rounds)

| Area | Why out of scope |
|------|------------------|
| `mbms2005:Group` / `MBMS-Session-Identity` | Rel-6 group subscription not used by current embedder |
| `mbms2015:IndependentUnitPositions` | Specialised incremental FEC repair |
| XSD-validation pass on serialised FDT | Would need libxml2 for schema validation; structural tests cover drift adequately |
| TSI-scope multi-source filtering | Needs source-IP awareness which Decoder doesn't expose |
| Default-namespace (`xmlns="..."` without prefix) handling in FDT parser | All MBMS FDT-Instance children are in the no-namespace bucket by spec; default-ns rebinding is not used in TS 26.346 examples |
| Mid-element xmlns redeclaration | Pathological; not used in MBMS FDTs in practice |

### Round-6 candidates

| Area | Spec | Notes |
|------|------|-------|
| Proper RFC 5053 §4.4.1.2 source-block partitioning (K_L / K_S split, Z_L / Z_S blocks) | RFC 5053 §4.4.1.2 | DONE in round 7 — see fix in `src/fec/RaptorFEC.cpp` and the `F11931920_mtu1500` regression test in `tests/unit/integration_test.cpp`. |
| `Encoder::send` 16-bit TSI/TOI cap | RFC 5651 §5.1 (TSI/TOI may be up to 48 bits) | Encoder/AlcPacket producer hardcodes `half_word_flag=1, toi_flag=0`; supporting wider IDs needs producer changes. |
| Raptor decoder's repair tolerance | RFC 5053 / lib/raptor | Round-5 lossy tests deliberately stay within FEC budget; characterising the codec's actual loss limit and adding a "loss right at the edge" stress test would tighten coverage. |
| Decoder/Encoder reception statistics | (operational, not RFC-required) | DONE in round 6 — see EncoderStats / DecoderStats and tests/unit/stats_test.cpp. |
| Raptor FLUTE-glue throughput investigation | bench/flute_bench output | Round-6 benchmark surfaces ~2 MB/s Raptor throughput vs bare bitstem-r10's much higher numbers. See "Raptor FLUTE-glue perf hypotheses" section below for the diagnostic checklist. |

## Raptor FLUTE-glue perf hypotheses (round-7 starting points)

Round-6 `tests/bench/flute_bench` shows Raptor end-to-end at ~2 MB/s
across all sizes (10 / 100 / 500 MB), versus the bare `lib/raptor`
codec's measured throughput which is much higher. The gap is in the
FLUTE wrapper, not the codec itself. Hypotheses to check first when
investigation resumes:

1. **Per-packet decode invocation.** If the FLUTE layer calls the R10
   decoder once per *received FEC packet* rather than batching all
   received symbols into one decode call per source block, every
   call rebuilds the constraint matrix and re-runs schedule
   construction. R10 is not designed for incremental operation;
   single decode call per source block is the intended shape.
   Inspection target: `RaptorFEC::process_symbol` in
   `src/fec/RaptorFEC.cpp`.
2. **Memory allocation in the inner path.** If symbol buffers are
   allocated inside the receive loop rather than once per block,
   every decode call allocates and frees ~K×T bytes (≈7 MB at
   K=8192, T=900). Profile signal: kernel time in
   `do_anonymous_page` / `__alloc_pages`. The same pattern bit the
   r10 codec earlier in the optimization arc.
3. **Symbol copy overhead.** If FLUTE hands the decoder a
   vector-of-vectors / list-of-(ESI, symbol) pairs and the decoder
   wants contiguous symbols, the conversion is an O(K×T) copy per
   decode. Bandwidth-bound; ~50–100 ms at K=8192, not seconds, but
   worth measuring.
4. **LT row regeneration / pre-coding parameter recomputation.**
   The decoder needs ESIs to construct LT rows. If the FLUTE
   wrapper triggers full DerivePreCodingParameters per packet
   instead of going through the constexpr LUT path that round-10
   of bitstem-r10 optimized, the cost regressses. Check whether
   the FLUTE wrapper hits the same parameter-derivation path the
   bare benchmark exercises.
5. **Source-block reconstruction concurrency.** A 100 MB file
   produces ~12 source blocks; if they decode serially when they
   could parallelize, that's a multiplier (not 100× but real). Less
   important than (1)/(2) but easy to measure once those are out
   of the way.
6. **FLUTE bookkeeping in the per-packet path.** Header parsing,
   ALC reassembly, FDT-instance routing — none should dominate, but
   a poorly-built FLUTE receiver can spend significant time in
   per-packet state management. Check `Decoder::feed_packet` /
   `File::put_symbol` hot paths.

**Diagnostic narrowing question:** of the ~3340 ms decoder time at
F = 10 MB / Raptor in the bench, how much is *inside*
`bitstem::r10::fast::Decoder::TryDecode` (i.e.
`BuildDecodingScheduleInactivation` + `ApplyDecodingSchedule`) vs.
the surrounding FLUTE-layer wrapping? If the inner-codec time is at
or near the bare-benchmark expectation (~40 ms), the entire gap is
glue overhead and the work is on libflute's side. If the inner time
is much larger, something is calling the decoder pathologically
often (hypothesis 1).

Method: `perf record -g` on `flute_bench` with
`FLUTE_BENCH_SIZES_MB=10`, then `perf report` with call-graph and
look for the hot stacks under `Decoder::feed_packet`.

## Test-writing rules

1. **Test the spec, not the code.** If a parser quirk would fail any
   other RFC-conformant implementation, the test should fail too.
2. **Build wire bytes from `fixtures.hpp`, never from the producing
   constructor of the class under test.** Round-trip-with-itself
   coverage holes are the failure mode we're guarding against.
3. **Skip with rationale, not silence.** Every `GTEST_SKIP()` carries
   a one-line explanation pointing at the spec gap and the FileEntry
   field / parser path that needs to change to un-skip it.
4. **Update this file when you touch the test surface.** No drift.
