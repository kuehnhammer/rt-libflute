/*  Copyright (C) Bitstem GmbH
 *  All rights reserved.
 *
 *  This source code is proprietary and confidential.
 *  Unauthorized copying, distribution, or disclosure of this file,
 *  in whole or in part, is strictly prohibited unless explicitly
 *  permitted in writing by Bitstem GmbH.
 *
 *  Author: Klaus Kuehnhammer <klaus@bitstem.com>
 */

#include "fec/FecLoader.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include "spdlog/spdlog.h"

namespace LibFlute {

namespace {

// Codec library to load. On POSIX we dlopen the SONAME (libfec.so.0)
// rather than the unversioned libfec.so symlink so a major-version
// bump on the codec side surfaces as a clean load failure here
// rather than silently picking up an ABI-incompatible build. On
// Windows DLLs aren't soname-versioned — LoadLibrary takes the bare
// filename — and the FetchContent-style packaging staged by
// BitstemFEC.cmake ships exactly one fec.dll per zip.
#ifdef _WIN32
constexpr const char* kSoname = "fec.dll";
#else
constexpr const char* kSoname = "libfec.so.0";
#endif

#ifdef _WIN32
// On Windows the loader API is GetLastError() based — GetProcAddress
// returning NULL is itself the failure signal (no symbol may legally
// resolve to NULL). Format the Win32 error so log messages match the
// dlerror() shape used on POSIX.
std::string LastErrorString() {
  const DWORD err = ::GetLastError();
  char* buf = nullptr;
  const DWORD len = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
  std::string msg = (len > 0 && buf != nullptr)
                        ? std::string(buf, len)
                        : ("error " + std::to_string(err));
  if (buf != nullptr) {
    ::LocalFree(buf);
  }
  // Strip the trailing CRLF that FormatMessage tacks on.
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
    msg.pop_back();
  }
  return msg;
}
#endif

// Helper: resolve `name` from `handle`, returning nullptr (and
// logging) on failure.
template <typename Fn>
Fn dlsym_or_null(void* handle, const char* name) {
#ifdef _WIN32
  FARPROC sym = ::GetProcAddress(static_cast<HMODULE>(handle), name);
  if (sym == nullptr) {
    spdlog::error("FecLoader: GetProcAddress(\"{}\") failed: {}", name,
                  LastErrorString());
    return nullptr;
  }
  return reinterpret_cast<Fn>(sym);
#else
  ::dlerror();  // clear
  void* sym = ::dlsym(handle, name);
  if (const char* err = ::dlerror(); err != nullptr) {
    spdlog::error("FecLoader: dlsym(\"{}\") failed: {}", name, err);
    return nullptr;
  }
  return reinterpret_cast<Fn>(sym);
#endif
}

}  // namespace

FecLoader& FecLoader::instance() {
  static FecLoader loader;  // C++11 magic static: thread-safe lazy init
  return loader;
}

FecLoader::FecLoader() {
#ifdef _WIN32
  _handle = ::LoadLibraryA(kSoname);
#else
  _handle = ::dlopen(kSoname, RTLD_NOW | RTLD_LOCAL);
#endif
  if (_handle == nullptr) {
    // Expected on production receivers without an FEC license; not
    // an error per se. Warn so the operator notices if FEC files
    // were expected. Subsequent FDT entries declaring Raptor /
    // RaptorQ will surface "FEC unavailable" via FileDeliveryTable.
#ifdef _WIN32
    spdlog::warn("FecLoader: LoadLibrary({}) failed: {} — Raptor / RaptorQ unavailable",
                 kSoname, LastErrorString());
#else
    spdlog::warn("FecLoader: dlopen({}) failed: {} — Raptor / RaptorQ unavailable",
                 kSoname, ::dlerror());
#endif
    return;
  }

  // ABI-version probe first. The dlsym signature for this one is
  // frozen by contract; resolving it before the rest lets us refuse
  // a mismatched .so before pulling in the rest of the symbol
  // table.
  c_abi_version = dlsym_or_null<decltype(c_abi_version)>(
      _handle, "bitstem_fec_c_abi_version");
  if (c_abi_version == nullptr) {
    spdlog::error("FecLoader: {} missing bitstem_fec_c_abi_version", kSoname);
#ifdef _WIN32
    ::FreeLibrary(static_cast<HMODULE>(_handle));
#else
    ::dlclose(_handle);
#endif
    _handle = nullptr;
    return;
  }
  const unsigned abi = c_abi_version();
  if (abi != BITSTEM_FEC_C_ABI_VERSION) {
    spdlog::error("FecLoader: {} reports ABI v{}; libflute compiled for v{}",
                  kSoname, abi, BITSTEM_FEC_C_ABI_VERSION);
#ifdef _WIN32
    ::FreeLibrary(static_cast<HMODULE>(_handle));
#else
    ::dlclose(_handle);
#endif
    _handle = nullptr;
    return;
  }

  // Resolve the rest of the surface. Any missing symbol → mark the
  // loader unavailable; we don't try to support partial ABIs.
  has_scheme_fn = dlsym_or_null<decltype(has_scheme_fn)>(
      _handle, "bitstem_fec_has_scheme");

  encoder_create = dlsym_or_null<decltype(encoder_create)>(
      _handle, "bitstem_fec_encoder_create");
  encoder_destroy = dlsym_or_null<decltype(encoder_destroy)>(
      _handle, "bitstem_fec_encoder_destroy");
  encoder_reset = dlsym_or_null<decltype(encoder_reset)>(
      _handle, "bitstem_fec_encoder_reset");
  encoder_encode_symbol = dlsym_or_null<decltype(encoder_encode_symbol)>(
      _handle, "bitstem_fec_encoder_encode_symbol");

  decoder_create = dlsym_or_null<decltype(decoder_create)>(
      _handle, "bitstem_fec_decoder_create");
  decoder_destroy = dlsym_or_null<decltype(decoder_destroy)>(
      _handle, "bitstem_fec_decoder_destroy");
  decoder_reset_span = dlsym_or_null<decltype(decoder_reset_span)>(
      _handle, "bitstem_fec_decoder_reset_span");
  decoder_add_received_symbol = dlsym_or_null<decltype(decoder_add_received_symbol)>(
      _handle, "bitstem_fec_decoder_add_received_symbol");
  decoder_try_decode = dlsym_or_null<decltype(decoder_try_decode)>(
      _handle, "bitstem_fec_decoder_try_decode");
  decoder_is_decoded = dlsym_or_null<decltype(decoder_is_decoded)>(
      _handle, "bitstem_fec_decoder_is_decoded");

  const bool all_resolved =
      has_scheme_fn && encoder_create && encoder_destroy && encoder_reset &&
      encoder_encode_symbol && decoder_create && decoder_destroy &&
      decoder_reset_span && decoder_add_received_symbol &&
      decoder_try_decode && decoder_is_decoded;
  if (!all_resolved) {
    spdlog::error("FecLoader: {} missing one or more bitstem_fec_* symbols", kSoname);
#ifdef _WIN32
    ::FreeLibrary(static_cast<HMODULE>(_handle));
#else
    ::dlclose(_handle);
#endif
    _handle = nullptr;
    return;
  }

  _available = true;
  spdlog::info("FecLoader: {} loaded successfully (ABI v{})", kSoname, abi);
}

FecLoader::~FecLoader() {
  // Deliberately do NOT dlclose at process shutdown. The codec
  // owns a process-wide schedule cache + thread-local scratch
  // pools (see bitstem-fec internals); their destruction order
  // relative to libflute's own statics is unconstrained, and any
  // codec-internal atexit handler firing after dlclose would crash.
  // dlclose at process exit is a hint anyway — the OS reclaims the
  // mapping when the address space dies.
  _handle = nullptr;
}

bool FecLoader::has_scheme(bitstem_fec_scheme_t scheme) const noexcept {
  if (!_available) {
    return false;
  }
  return has_scheme_fn(scheme) != 0;
}

}  // namespace LibFlute
