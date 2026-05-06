# RaptorQ integration plan

Branch: `rq` off of `development`. Goal: add RFC 6330 RaptorQ
encoding/decoding to libflute alongside the existing R10 (RFC 5053)
support, while keeping bitstem-r10 untainted by LGPL code and
preserving libflute's usability inside the closed-source 5gbc-rx /
5gr parent.

## Survey: libRaptorQ (lucafulchir/libRaptorQ)

| | |
|---|---|
| License | **LGPL v3**, no linking exception (also dual-licensed GPL v3) |
| Language | C++11; header-only OR linked variants |
| Build | CMake 2.8.12 minimum; produces `RaptorQ` (shared) + `RaptorQ_Static` |
| External deps | **Eigen3** (required), pthreads (required), LZ4 (optional, default ON) |
| API | namespace `RaptorQ__v1` (alias `RaptorQ`); RAW + RFC6330 surface, plus a C wrapper |
| Wire compat | Claims RFC 6330 from v0.1.10 onwards (older versions are NOT RFC compliant) |
| Maturity | v1.0.0-rc2 most recent tag; ~262 commits; pre-1.0 status |

**Key API shape** (RAW/header-only path):

```cpp
namespace RaptorQ = RaptorQ__v1;

// Encoder:
RaptorQ::Encoder<uint8_t*, uint8_t*> enc(K_block_size, symbol_size);
enc.set_data(begin, end);
enc.compute_sync();
for (auto it = enc.begin_source(); it != enc.end_source(); ++it) { ... }
for (auto it = enc.begin_repair(); it != enc.end_repair(max_repair); ++it) { ... }

// Decoder:
RaptorQ::Decoder<uint8_t*, uint8_t*> dec(K_block_size, symbol_size,
                                           Decoder::Report::COMPLETE);
dec.add_symbol(begin, end, esi);
dec.end_of_input(RaptorQ::Fill_With_Zeros::NO);
auto res = dec.wait_sync();
auto bytes_out = dec.decode_bytes(begin_out, end_out, 0, 0);
```

Iterator-based; not a great match for libflute's per-symbol-by-ESI
push model. Will need a small adapter inside RaptorQFEC.

## License strategy

The constraint is that 5gbc-rx (closed-source) consumes libflute,
and bitstem-r10 (closed-source) must stay LGPL-clean. The chosen
shape:

```
                              ┌──────────────────────┐
                              │  bitstem-r10         │  proprietary
                              │  (R10 codec, .a)     │  closed source
                              └──────────┬───────────┘  no LGPL anywhere
                                         │ static link
                                         ▼
   ┌────────────────────┐     ┌──────────────────────┐
   │  libRaptorQ        │     │  libflute            │  OSS (kept OSS)
   │  (RFC 6330, .so)   │◀════│  (FLUTE/ALC)         │  ENABLE_RAPTORQ
   │  LGPL v3           │ DYN │                      │  build option
   └────────────────────┘     └──────────┬───────────┘  off by default
                                         │ static link
                                         ▼
                              ┌──────────────────────┐
                              │  5gbc-rx → 5gr       │  proprietary
                              │  (bcast core)        │  closed source
                              └──────────────────────┘  uses libRaptorQ.so
                                                         dynamically
```

### Rules of the road

1. **bitstem-r10 sees no LGPL.** It is built without ENABLE_RAPTORQ
   ever being relevant to it — RaptorQ is libflute's concern, not
   r10's. r10 stays a pure R10 codec.

2. **libflute → libRaptorQ is dynamically linked.** LGPL v3 §4
   permits convey-as-Combined-Work when the user can replace the
   library. Dynamic linking via `libRaptorQ.so` satisfies that
   ("uses at run time a copy of the Library already present on the
   user's computer system"). Static linking would require shipping
   libflute object code separately so a user could re-link against
   a different libRaptorQ — workable but more ceremony.

3. **5gbc-rx ships libRaptorQ.so alongside its binary, not bundled
   into the executable.** LGPL compliance for the closed-source
   parent then comes down to:
   - Mark the libRaptorQ.so file as LGPL'd in the deployment
     manifest.
   - Ship libRaptorQ source (or a written offer) with the
     distribution.
   - Don't statically embed any libRaptorQ code in the closed-
     source binary.

4. **Build-time switch.** `cmake -DENABLE_RAPTORQ=ON` opts in;
   default is OFF. Production 5gr builds that don't need RaptorQ
   simply don't enable it — zero LGPL exposure, zero new
   dependencies (no Eigen3, no LZ4) on the closed-source side.

5. **Header isolation.** Any libflute TU that includes a
   `<RaptorQ/...>` header MUST NOT also be included by 5gbc-rx
   public headers. Put the libRaptorQ-touching code behind a
   PIMPL'd `RaptorQFEC::Impl` (same pattern bitstem-r10 just got)
   so libflute's public headers stay LGPL-free at compile time
   too. This protects against accidental inline-instantiation of
   LGPL-licensed templates inside the closed-source consumer's
   compile.

## Integration touchpoints in libflute

| Component | Change | Effort |
|-----------|--------|-------:|
| `CMakeLists.txt` | Add `option(ENABLE_RAPTORQ ...)` + `find_package(RaptorQ)` (or `pkg_check_modules`); guard new files behind it; link `RaptorQ` (the .so target). | ½ day |
| `flute_types.h` | `FecScheme::RaptorQ` already exists (enum value 6, RFC 5052). No change. | — |
| `AlcPacket` parsing | RaptorQ FEC payload ID is **SBN(8) + ESI(24)** (RFC 6330 §3.2), vs R10's SBN(16) + ESI(16). The wire-format parse path needs a scheme dispatch on the `Codepoint` LCT field (or the FEC OTI from the FDT). | ½ day |
| `EncodingSymbol` | Same SBN/ESI split adjustment on the producer side. | ½ day |
| `FileDeliveryTable` parse/serialize | RaptorQ FEC OTI scheme-specific info per RFC 6330 §3.4.2 is 4 bytes of (Z, N, Al), distinct from R10's. Add an MBMS-friendly base64 round-trip. | ½ day |
| `RaptorQFEC` (new) | New `FecTransformer` impl behind PIMPL. Wraps libRaptorQ's iterator API into libflute's per-(SBN, ESI) push model. Encoder reuse pattern (one `RaptorQ::Encoder` per K, repurposed across blocks) modelled on the R10 `Encoder::Reset` work. | 2–3 days |
| `Encoder::send()` | Already accepts `FecScheme`; flesh out the RaptorQ branch (FEC OTI population, Z/N/Al partitioning per RFC 6330 §4.4). | ½ day |
| `Encoder::EstimateOverhead` | Add RaptorQ to the FecScheme switch (same redundancy formula as R10; RaptorQ's 4-byte FEC payload ID is the same size as R10's, so per-packet headers don't change). | trivial |
| `tests/unit/integration_test.cpp` | Round-trip + lossy round-trip parametric cases for RaptorQ. | 1 day |
| `tests/bench/flute_bench.cpp` | Add RaptorQ alongside Raptor in the scenario sweep. | trivial |
| `doc/COVERAGE.md` | Round-N writeup. | — |
| **Total** | | **5–6 days** |

## Open questions / risks

1. **libRaptorQ packaging.** Most distros don't ship it; 5gbc-rx's
   build environment will need an explicit install step (or a
   Conan / vcpkg recipe, or a vendored libRaptorQ source tree
   built as ExternalProject). Decide upfront: do we (a) require
   `apt install libraptorq` on build hosts, (b) vendor it into
   `lib/raptorq/` like we did for bitstem-r10 (but as a regular
   submodule, not pulled into the closed-source build by default),
   or (c) build it via ExternalProject_Add? My read: **(b)** is
   cleanest — a `lib/raptorq` git submodule, ExternalProject on
   ENABLE_RAPTORQ, dynamic-link output ends up in the build dir,
   reproducible across hosts.

2. **Eigen3 transitive dep.** libRaptorQ pulls Eigen3 (header-only
   linear-algebra). Stays in the libRaptorQ.so build but doesn't
   leak into libflute's headers — assuming we keep RaptorQFEC
   PIMPL'd. If find_package surfaces Eigen3 via an INTERFACE link
   on the `RaptorQ` target, we'll need to strip it from libflute's
   PUBLIC link list.

3. **libRaptorQ pre-1.0.** v1.0.0-rc2 is the most recent tag.
   Maintenance is sporadic. Pin a specific commit or tag at
   integration time and audit it before bumps.

4. **Wire-format interop.** RFC 6330 compliance is claimed from
   v0.1.10. We'll need at least one independent test vector
   (broadcast standard test stream, or a known-good RaptorQ peer)
   to verify libflute + libRaptorQ produces wire bytes a
   third-party RaptorQ receiver can decode. Without that, "passes
   our round-trip tests" only proves the lib is internally
   consistent.

5. **Performance baseline.** R10 sits at 247 MB/s lossless / 140
   MB/s lossy on 500 MB Raptor end-to-end after round 10.
   libRaptorQ is unoptimised by comparison (Eigen3 GE for the
   matrix solve, no schedule cache, no lossless short-circuit).
   Expect first numbers to be 10–30× slower than R10 at K=8000.
   Acceptable for "the feature works" but a real perf round may
   be needed if the broadcast workload picks RaptorQ as default.

6. **K bounds.** R10 supports K ∈ [4, 8192]. RaptorQ supports K
   ∈ [1, 56403] — much wider. The libflute partitioning code
   currently caps at R10's K_max; for RaptorQ we can let blocks
   run larger (fewer Z, less FDT churn). Handled in
   RaptorQFEC::calculate_partitioning.

7. **FEC redundancy at the wire level.** R10 ships 1.15K (15 %).
   RaptorQ has near-MDS rank performance (~0.99% rank deficiency
   at K, ~10⁻⁶ at K+2), so production deployments often run as low
   as 1.05× redundancy. Default RaptorQFEC redundancy should
   probably be 0.10 (vs R10's 0.15) — but plumb through
   `FecOti.scheme_specific_info` rather than hardcoding.

## Recommended next steps

1. **Land an ENABLE_RAPTORQ skeleton** that does nothing except
   gate compilation. Build green with the option both ON and OFF.
2. **Add libRaptorQ as `lib/raptorq` git submodule** at a pinned
   commit, plus the ExternalProject_Add wiring that produces
   `libRaptorQ.so` in the build dir. Verify Eigen3 stays out of
   libflute's public link.
3. **Wire format first**: implement RaptorQ FEC payload ID parsing
   in AlcPacket / EncodingSymbol, gated by `FecScheme::RaptorQ`.
   Pin against hand-crafted byte sequences in unit tests before
   touching the codec at all.
4. **PIMPL'd RaptorQFEC**: encode-only first (using libRaptorQ's
   `begin_source` / `begin_repair` iterators), then decode. Each
   path validated by an in-process round-trip.
5. **FDT round-trip** of RaptorQ FEC OTI scheme-specific info
   (Z, N, Al) with base64 wire encoding per TS 26.346.
6. **Cross-implementation interop**: identify a known-good
   RaptorQ peer (5G-MAG reference receiver, or an offline
   reference encoder) and verify libflute's emitted packets
   decode there.
7. **Update COVERAGE.md** with the round writeup + bench numbers.

After step 7 the rq branch is mergeable into development.
