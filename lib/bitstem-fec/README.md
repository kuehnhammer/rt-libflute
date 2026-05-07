# bitstem-fec binary distribution

This directory ships the `bitstem-fec` codec (RFC 5053 R10 + RFC 6330
RaptorQ) as platform-specific tarballs, plus a `VERSION` pin that
selects which one libflute's CMake glue picks up at configure time.

## Layout

```
lib/bitstem-fec/
├── VERSION                          # one-line pin (e.g. "0.9.0+main.99a6ce0")
├── linux/
│   ├── bitstem-fec-shared-<VERSION>-noble-x86_64-v3.tar.gz
│   └── bitstem-fec-shared-<VERSION>-jammy-x86_64-v3.tar.gz
├── windows/                          # later
└── macos/                            # later
```

Each tarball expands to:

```
include/bitstem/fec/fec.hpp
lib/libfec.so + soname symlinks
share/bitstem/fec/LICENSE
```

## How CMake selects the tarball

`cmake/BitstemFEC.cmake` reads `VERSION` and the host triple
(distro codename + arch + microarch) and builds the expected
filename:

```
bitstem-fec-shared-${VERSION}-${distro}-${arch}-${microarch}.tar.gz
```

It then extracts that exact file from the matching `lib/bitstem-fec/<platform>/`
directory into the build tree (`${CMAKE_BINARY_DIR}/_deps/bitstem-fec/`)
and exposes the result as the imported target `bitstem::fec`.

If the expected tarball isn't present, CMake fails at configure time
with a clear message — the build never silently picks a different
version than the `VERSION` pin declares.

## Updating the version

Atomic three-step commit:

1. Drop the new tarballs into `lib/bitstem-fec/linux/` (and later
   `windows/` / `macos/`). Keep names as produced by the bitstem-fec
   release pipeline.
2. Bump the string in `VERSION` to match.
3. Optionally delete the prior version's tarballs (kept around for
   rollback / bisection if disk-space and repo-history budget allow).

Reviewers see the binaries plus the pin in the same diff. Stale
checkouts (binaries don't match the pin) fail fast at configure time.

## Microarchitecture

Tarballs are stamped with the x86-64 microarch level they were built
for: `v3` (AVX2 baseline) is the current default. The `BSF_MICROARCH`
CMake cache variable lets a build pin a different level if matching
tarballs ship.
