# libflute — RFC 6726 FLUTE / RFC 5775 ALC for C++

A small, transport-agnostic implementation of the FLUTE/ALC file
delivery stack (RFC 6726 over RFC 5775 over RFC 5651), with the 3GPP
TS 26.346 MBMS extensions surfaced on the `FileEntry` structure and
optional Raptor (RFC 5053) FEC integration via the `bitstem-r10`
codec.

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

git clone --recurse-submodules <repo-url>
cd libflute
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Build options:

| Option | Default | Purpose |
|--------|---------|---------|
| `-DENABLE_RAPTOR=ON` | OFF | Compile in Raptor (RFC 5053) FEC support via `lib/raptor` (bitstem-r10). |
| `-DLIBFLUTE_BUILD_TESTING=ON` | ON | GoogleTest unit tests (fetches v1.15.2 via FetchContent). |
| `-DLIBFLUTE_BUILD_EXAMPLES=ON` | ON | The plain POSIX-socket demo apps in `examples/`. |

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

## Documentation

`tests/COVERAGE.md` — what the test suite proves, organised by
test file and spec citation.

`doc/rfc6726.txt` — the canonical RFC, kept checked-in so tests can
cite specific sections without external lookups.

Source-level Doxygen: `doxygen` in the project root, then open
`html/index.html`.
