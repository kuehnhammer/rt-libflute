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

#pragma once

#include "bitstem/fec/fec_c.h"

namespace LibFlute {

// Runtime loader for the bitstem-fec C ABI (libfec.so.0).
//
// libflute is built without a link-time dependency on libfec.so —
// the ELF has no NEEDED entry for it — so binaries that consume
// libflute start cleanly even when the codec is not present on the
// system. The codec is only required for files declaring an FEC
// scheme (Raptor / RaptorQ) in their FDT entry; CompactNoCode
// delivery never touches it.
//
// At first use of a Raptor / RaptorQ FEC path, RaptorFEC obtains
// FecLoader::instance(). The first call dlopens "libfec.so.0",
// resolves the C ABI (see <bitstem/fec/fec_c.h>), and caches the
// function pointers. Subsequent calls return the same singleton.
//
// Failure modes the loader handles cleanly (available() == false):
//   * libfec.so.0 not present on the dynamic-linker search path
//   * ABI version mismatch (libfec built against a different
//     BITSTEM_FEC_C_ABI_VERSION than libflute)
//   * Missing symbols (incomplete / corrupt build)
//
// Failures are logged once at warn level and never throw; callers
// branch on available() and surface a clean "FEC unavailable" error
// to the application.
class FecLoader {
 public:
  // Acquire the singleton. Thread-safe (C++11 magic statics).
  static FecLoader& instance();

  FecLoader(const FecLoader&) = delete;
  FecLoader& operator=(const FecLoader&) = delete;
  FecLoader(FecLoader&&) = delete;
  FecLoader& operator=(FecLoader&&) = delete;

  // True iff the dlopen + dlsym + ABI-version check all succeeded.
  bool available() const noexcept { return _available; }

  // Probe whether the loaded libfec.so was built with support for
  // the given scheme. Returns false when !available() OR when the
  // codec was disabled at build time via -DFEC_ENABLE_<X>=OFF.
  bool has_scheme(bitstem_fec_scheme_t scheme) const noexcept;

  // ---- function-pointer table ----
  // Only valid when available(). Calling through a null pointer is
  // UB; gate on available() / has_scheme() first.

  unsigned (*c_abi_version)() noexcept = nullptr;
  int      (*has_scheme_fn)(bitstem_fec_scheme_t) noexcept = nullptr;

  bitstem_fec_encoder_t* (*encoder_create)(const bitstem_fec_params_t*) noexcept = nullptr;
  void (*encoder_destroy)(bitstem_fec_encoder_t*) noexcept = nullptr;
  void (*encoder_reset)(bitstem_fec_encoder_t*,
                         const uint8_t* src, size_t src_len) noexcept = nullptr;
  void (*encoder_encode_symbol)(bitstem_fec_encoder_t*,
                                  uint32_t esi,
                                  uint8_t* out, size_t out_len) noexcept = nullptr;

  bitstem_fec_decoder_t* (*decoder_create)(const bitstem_fec_params_t*,
                                              uint8_t* lent_buffer,
                                              size_t lent_len) noexcept = nullptr;
  void (*decoder_destroy)(bitstem_fec_decoder_t*) noexcept = nullptr;
  void (*decoder_reset_span)(bitstem_fec_decoder_t*,
                              uint8_t* lent_buffer,
                              size_t lent_len) noexcept = nullptr;
  void (*decoder_add_received_symbol)(bitstem_fec_decoder_t*,
                                        uint32_t esi,
                                        const uint8_t* sym,
                                        size_t sym_len) noexcept = nullptr;
  int  (*decoder_try_decode)(bitstem_fec_decoder_t*) noexcept = nullptr;
  int  (*decoder_is_decoded)(const bitstem_fec_decoder_t*) noexcept = nullptr;

 private:
  FecLoader();
  ~FecLoader();

  void* _handle    = nullptr;
  bool  _available = false;
};

}  // namespace LibFlute
