# libflute — RFC 6726 FLUTE / RFC 5775 ALC for C++

A small, transport-agnostic implementation of the FLUTE/ALC file
delivery stack (RFC 6726 over RFC 5775 over RFC 5651), with the 3GPP
TS 26.346 MBMS extensions surfaced on the `FileEntry` structure and
optional Raptor (RFC 5053) / RaptorQ (RFC 6330) FEC integration via
the `bitstem-fec` codec, shipped as a binary distribution under
`lib/bitstem-fec/`.

The library produces and consumes ALC packet *bytes*. Wire transport
— UDP, IP multicast, broadcast-pipeline RLC SDUs, pcap replay — is
the consumer's job. There are no socket dependencies in the library:
no Boost.Asio, no libpcap, no event loop, and no internal threads.

## Public surface

Two classes carry the day:

- **`LibFlute::Encoder`** (TX). Construct with `(tsi, mtu,
  rate_limit_kbps, packet_cb)`. Each call to `send()` queues an
  object; `send_next_packet()` / `flush()` push packets into your
  `packet_cb`. The library hands you bytes — you call `sendto()`
  (or RLC-dispatch, or whatever) yourself.
- **`LibFlute::Decoder`** (RX). Construct with `(tsi)`. Feed every
  ALC payload byte block through `feed_packet()`; register a
  completion callback to receive assembled `LibFlute::File` objects
  once they're done.

Lower-level primitives (`AlcPacket`, `EncodingSymbol`,
`FileDeliveryTable`) are public for embedders that need to reach
under the API.

## Build

```sh
sudo apt install ninja-build build-essential libspdlog-dev \
    pkgconf libssl-dev libtinyxml2-dev clang-tidy

git clone <repo-url>
cd libflute
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Build options:

| Option | Default | Purpose |
|--------|---------|---------|
| `-DENABLE_RAPTOR=ON` | OFF | Link in the `bitstem-fec` codec (RFC 5053 R10 + RFC 6330 RaptorQ). Requires the matching binary tarball under `lib/bitstem-fec/<platform>/` — see [bitstem-fec integration](#bitstem-fec-integration) below. |
| `-DLIBFLUTE_BUILD_TESTING=ON` | ON | GoogleTest unit tests (fetches v1.15.2 via FetchContent). |
| `-DLIBFLUTE_BUILD_EXAMPLES=ON` | ON | The plain POSIX-socket demo apps in `examples/`. |
| `-DBSF_DISTRO=<codename>` | auto | Override the Linux distro codename used to select the bitstem-fec tarball (default: read from `/etc/os-release VERSION_CODENAME`). |
| `-DBSF_MICROARCH=v3` | `v3` | x86-64 microarchitecture level the bitstem-fec tarball is built for (`v1`/`v2`/`v3`/`v4`). |

`-DENABLE_RAPTOR=OFF` (the default) gives a CompactNoCode-only build
with no dependency on `bitstem-fec` — the library is fully usable
without the binary lib being present at all.

Run the test suite:

```sh
cd build && ctest --output-on-failure
```

See `tests/COVERAGE.md` for the full test/spec/skip map.

## Examples

`examples/` contains three small demo apps that show how a consumer
wires the library up to real transports. None of this code is part of
the library:

- `flute-transmitter` — opens a UDP socket and pumps `Encoder`
  output via `sendto` to a multicast (or unicast) target.
- `flute-receiver` — opens a UDP socket, joins the multicast group
  if applicable, and feeds every received datagram into a
  `Decoder` via `feed_packet()`.
- `flute-pcap-receiver` — same as `flute-receiver` but reads ALC
  packets from a `.pcap` capture file via libpcap (built only if
  libpcap is available).

Quick loopback demo:

```sh
# in one terminal:
./build/examples/flute-receiver --target=238.1.1.95

# in another:
./build/examples/flute-transmitter --target=238.1.1.95 -r 100000 some_file.bin
```

`-r` is the rate cap in kbit/s; pass `-r 0` for unlimited. `-f 1`
enables Raptor on the TX side (only meaningful when the library was
built with `ENABLE_RAPTOR=ON`).

## bitstem-fec integration

`bitstem-fec` (RFC 5053 R10 + RFC 6330 RaptorQ codec) is shipped as
platform-specific binary tarballs vendored in-tree under
`lib/bitstem-fec/`. The exact version is pinned by a one-line
`lib/bitstem-fec/VERSION` file; `cmake/BitstemFEC.cmake` reads that
pin, builds the expected tarball name from the host triple
(`<distro>-<arch>-<microarch>`), and extracts the matching archive
into the build tree on the first configure pass. Subsequent runs
hit a stamp file and skip.

The integration declares an imported target `bitstem::fec` against
the extracted shared library, so `libflute` dynamically links
`libfec.so.0`. CMake-managed rpath wires development builds to the
in-tree extraction directory; install rules are the consumer's
responsibility (typical 5gbc-rx pattern: ship `libfec.so.*` next to
the binary, use `INSTALL_RPATH=$ORIGIN`).

If the expected tarball is missing for the host triple, configure
hard-fails with a pointer to drop one in or override `BSF_DISTRO` /
`BSF_MICROARCH`. Stale checkouts (binaries that don't match
`VERSION`) fail fast at configure time rather than at runtime.

### Layout

```
lib/bitstem-fec/
├── VERSION                              # one-line pin (e.g. "0.9.0+main.99a6ce0")
├── README.md                            # detailed update notes
├── linux/
│   ├── bitstem-fec-shared-<VERSION>-noble-x86_64-v3.tar.gz
│   └── bitstem-fec-shared-<VERSION>-jammy-x86_64-v3.tar.gz
├── windows/                             # when ready
└── macos/                               # when ready
```

Each tarball expands to `include/bitstem/fec/fec.hpp`,
`lib/libfec.so` (+ soname symlinks), and `share/bitstem/fec/LICENSE`.

### Updating to a new bitstem-fec version

The update workflow is a single atomic commit:

1. Drop the new tarballs into `lib/bitstem-fec/<platform>/`. Keep
   the names exactly as produced by the bitstem-fec release
   pipeline:
   `bitstem-fec-shared-<version>-<distro>-<arch>-<microarch>.tar.gz`.
2. Bump the string in `lib/bitstem-fec/VERSION` to match.
3. Optionally remove the prior version's tarballs (kept around for
   bisection / rollback if disk-space and git-history budget allow).
4. Reconfigure (`cmake -S . -B build ...`) and rebuild.

Reviewers see the binaries plus the pin in the same diff; rolling
back is a `git revert` of that one commit.

## Wire-format conformance

Round-1..5 of the project's TDD-after-the-fact pass added ~85 unit
tests against the spec, not the implementation, and fixed every
real bug they surfaced. The fixed-bug ledger is in
`tests/COVERAGE.md`. Highlights:

- RFC 5052 §9.1 partial-last-symbol length
- RFC 5651 §3.2.5 EXT_NOP / EXT_AUTH / EXT_TIME variable-length skip
- RFC 6726 §3.4.1 EXT_FDT FLUTE-version 2 acceptance
- RFC 6726 §3.3 FDT-Instance monotonicity (incl. 20-bit circular ID
  comparison and Expires-time-based rejection)
- Namespace-URI-aware FDT parser (XML Namespaces 1.0 §6.1)
- `mbms2007:Cache-Control` `xs:choice` enforcement
- TX-side Raptor source-block padding + `FEC-OTI-Scheme-Specific-Info`
  serialisation per RFC 5053 §3.2.

## TS 26.346 conformance (R10 file delivery)

The TS 26.346 §B.3.4.1 derivation algorithm is normative for MBMS file
delivery senders. libflute matches each input parameter:

| Param | TS 26.346 | libflute |
|-------|-----------|----------|
| Al    | 4         | 4 (`RaptorFEC.h`) |
| KMIN  | 1024      | 1024 (`G = ceil(P·1024/F)` literal in `calculate_partitioning`) |
| GMAX  | 10        | 10 (the `fmin(..., 10.0f)` cap) |
| W     | 256 KB    | caller-supplied via `FileTransmissionConfig::sub_block_size_target`; default 16 MB |
| Kmax  | 8192      | 8192 |

The G/T/Z/N derivation formula matches RFC 5053 §4.2 byte-for-byte and
the per-block split agrees with TS 26.346 Table B.3.4.2-1's worked
examples. For full TS 26.346-conformant emission, the xMB / SDP-side
caller sets `sub_block_size_target = 256 KB` per file (as the bench
does by default — see `tests/bench/flute_bench.cpp`). RaptorQ is
out-of-scope for TS 26.346 and uses the same W default; RFC 6330 §4.3
leaves WS as a deployment knob.

## Documentation

`tests/COVERAGE.md` — what the test suite proves, organised by
test file and spec citation.

`doc/rfc6726.txt` — the canonical RFC, kept checked-in so tests can
cite specific sections without external lookups.

Source-level Doxygen: `doxygen` in the project root, then open
`html/index.html`.
