#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace spk::io {

// Host-side export, outside the rendering C API. Pixels must already be
// encoded, bounded RGBA16 in the colour space the ICC profile describes,
// with opaque alpha; no transfer function or colour transform is performed
// here. Rows may include caller-owned padding. The profile's bytes are
// embedded unchanged in the TIFF: the file is tagged by the caller's choice
// of profile, and the writer checks only that the bytes are a structurally
// complete RGB-to-XYZ ICC profile (the same validation both entry points
// apply).
//
// Creates an uncompressed little-endian RGB16 classic TIFF with top-left
// orientation and square pixels. Existing paths are never overwritten.
// On failure only the file created by this call is removed, by its open
// Windows handle. An unsuccessful cleanup is included in the error.
//
// Two ways to hand over the profile:
//
// - `write_rgb16_tiff` takes the bytes directly. The frontend builds them
//   from a Qt named colour space (`QColorSpace::iccProfile`) or reads them
//   from a trusted bundled resource; no path is ever opened on their behalf.
// - `write_srgb_tiff` / `write_srgb_tiff_held` keep the original contract's
//   shape for the pinned sRGB resource: `srgb_icc_path` is an explicit
//   trusted profile resource, never a guessed system path.
bool write_rgb16_tiff(const std::filesystem::path& destination, const uint16_t* rgba,
                      uint32_t width, uint32_t height, uint32_t row_stride_px,
                      const uint8_t* icc_profile, size_t icc_size, std::string& error);
bool write_srgb_tiff(const std::filesystem::path& destination, const uint16_t* rgba,
                     uint32_t width, uint32_t height, uint32_t row_stride_px,
                     const std::filesystem::path& srgb_icc_path, std::string& error);

// Internal Windows host variant for transactional output publication. On
// success native_handle receives the still-open, exclusive Windows HANDLE,
// with GENERIC_READ | GENERIC_WRITE | DELETE access. The caller owns it and
// must close it, or mark it for deletion by handle before closing on rollback.
// The handle is positioned at EOF. On failure native_handle is nullptr and
// cleanup follows write_srgb_tiff's contract. Pass an empty output slot.
bool write_srgb_tiff_held(const std::filesystem::path& destination, const uint16_t* rgba,
                          uint32_t width, uint32_t height, uint32_t row_stride_px,
                          const std::filesystem::path& srgb_icc_path,
                          void*& native_handle, std::string& error);

}  // namespace spk::io
