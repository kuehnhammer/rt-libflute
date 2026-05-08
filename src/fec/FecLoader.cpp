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

#include <dlfcn.h>

#include "spdlog/spdlog.h"

namespace LibFlute {

namespace {

// SONAME of the codec library. The bitstem-fec build sets
// SOVERSION=0 and exports the C ABI under linker version tag
// BITSTEM_FEC_0; consumers MUST dlopen via the SONAME (libfec.so.0)
// rather than the unversioned libfec.so symlink, so a major-version
// bump on the codec side surfaces as a clean dlopen failure here
// rather than silently picking up an ABI-incompatible build.
constexpr const char* kSoname = "libfec.so.0";

// Helper: resolve `name` from `handle`, returning nullptr (and
// logging) on failure. dlerror() must be cleared before each dlsym
// call per POSIX — failures are signalled by dlsym returning NULL,
// which is itself a valid symbol address, so we use the dlerror-
// based check.
template <typename Fn>
Fn dlsym_or_null(void* handle, const char* name) {
  ::dlerror();  // clear
  void* sym = ::dlsym(handle, name);
  if (const char* err = ::dlerror(); err != nullptr) {
    spdlog::error("FecLoader: dlsym(\"{}\") failed: {}", name, err);
    return nullptr;
  }
  return reinterpret_cast<Fn>(sym);
}

}  // namespace

FecLoader& FecLoader::instance() {
  static FecLoader loader;  // C++11 magic static: thread-safe lazy init
  return loader;
}

FecLoader::FecLoader() {
  _handle = ::dlopen(kSoname, RTLD_NOW | RTLD_LOCAL);
  if (_handle == nullptr) {
    // Expected on production receivers without an FEC license; not
    // an error per se. Warn so the operator notices if FEC files
    // were expected. Subsequent FDT entries declaring Raptor /
    // RaptorQ will surface "FEC unavailable" via FileDeliveryTable.
    spdlog::warn("FecLoader: dlopen({}) failed: {} — Raptor / RaptorQ unavailable",
                 kSoname, ::dlerror());
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
    ::dlclose(_handle);
    _handle = nullptr;
    return;
  }
  const unsigned abi = c_abi_version();
  if (abi != BITSTEM_FEC_C_ABI_VERSION) {
    spdlog::error("FecLoader: {} reports ABI v{}; libflute compiled for v{}",
                  kSoname, abi, BITSTEM_FEC_C_ABI_VERSION);
    ::dlclose(_handle);
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
    ::dlclose(_handle);
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
