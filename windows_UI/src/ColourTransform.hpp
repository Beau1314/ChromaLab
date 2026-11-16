#pragma once

// The output transform's CPU core: the one conversion out of the session's
// working space, per destination, with the engine's own numbers
// (`spk_output_transform`, RFC-018 §5.2, carried by
// `desktop::OutputTransformData` from the host). The arithmetic is ported
// statement for statement from the Mac app's runtime Metal kernel
// (`app/SpektraLab/Sources/Engine/OutputTransform.swift` :207-464, itself a
// port of the engine's `nodes.metal`/`gamut.metal`): a second implementation
// of ProPhoto's linear toe or of CAM16's inverse would be a second chance to
// get a breakpoint wrong, and the disagreement would look like a grading
// decision. Handing the picture to a system colour engine instead (a tagged
// profile, converted elsewhere) would clip what this rolls off: a different
// picture.
//
//     working (encoded) -> its curve, off -> 3x3 linear, chromatically
//     adapted -> CAM16-UCS gamut compression into the target -> the target's
//     curve, on
//
// Two things differ from the kernel, neither in the arithmetic:
//
// - **The math is double, not float32.** Windows has no Metal here, and the
//   engine's own colour core computes in double; the port keeps that
//   precision. `tests/colour_transform.cpp` holds the transcription to the
//   kernel's formulas in double (the Mac measured the same kernel against
//   lcms at 0.6 % MAE; this port's gate is the transcription itself plus the
//   engine fetch, recorded as such in the task report).
// - **Depth happens after, in two roundings.** The kernel writes 8 bits
//   itself, straight from its floats; this core produces the 16-bit picture
//   and the export's existing 16-to-8 conversion quantizes it. The extra
//   rounding is at most half a 16-bit LSB, far under one 8-bit step.
//
// The export's CAM16 setting is the Mac's `withoutLightness`: the chroma
// knee, without the lightness stage the engine already applied before it
// encoded the working space. The gate travels in the data
// (`lightness_compression_active`), because a kernel that merely read the
// neutralised `k[15..17]` would not be applying an identity.
#include "desktop/preview_host.hpp"

#include <QImage>
#include <array>
#include <cstdint>
#include <vector>

namespace spk::desktop::colour {

// Signed power: |v|**p with v's sign kept, where the reference uses it.
double spow(double v, double p);

// The transfer functions, numbered exactly as the engine's kernels number
// them (`pipeline.cpp`'s `cctf_mode_for`): 0 sRGB (and Display P3), 1
// ProPhoto RGB, 2 Adobe RGB (1998), 3 BT.709/BT.2020, 4 identity. The
// breakpoints are the reference's exact ones, not the spec's printed
// roundings.
double cctf_decode(double v, std::uint32_t mode);
double cctf_encode(double v, std::uint32_t mode);

// CAM16-UCS gamut compression into the target, one pixel, in linear RGB: the
// engine's `gamut.metal`, its uniform and `cmax` reads becoming the setup's
// fields. Three of the reference's traps are reproduced deliberately: the
// sign-preserving power for J, the 460/1403 family of normalisation factors
// in the inverse (a, b) solve, and a hue index wrapped the way Python's `%`
// wraps it.
std::array<double, 3> cam16ucs_into(const std::array<double, 3>& rgb_linear,
                                    const OutputTransformData& setup);

// One pixel, out of the working space and into the target's own encoding:
// decode the source curve, the chromatically adapted matrix, the CAM16 knee
// when the fetch said it is active, encode the target curve.
std::array<double, 3> transform_pixel(const std::array<double, 3>& encoded,
                                      const OutputTransformData& setup);

// What a 16-bit container holds of an encoded value: its limits, and a
// number where there was none (a NaN is not a colour; it is written as black
// rather than as whatever the cast gives). Round to nearest, half up.
std::uint16_t to16(double v);
std::uint8_t to8(double v);

// Whole-frame passes. Alpha rides untouched; the transform is the last
// colour step, and these run on the full graded print before any geometry,
// exactly as the Mac's export does (`EngineHost.deliver` transforms the
// whole render, then turns and crops the picture).
void apply_rgba16(std::vector<std::uint16_t>& rgba, int width, int height,
                  const OutputTransformData& setup);
// Format_RGBA64 in and out.
QImage apply_rgba64(const QImage& frame, const OutputTransformData& setup);
// The 8-bit variant for a file that holds 8: Format_RGB888 in and out, one
// rounding through to8 (above).
QImage apply_rgb888(const QImage& frame, const OutputTransformData& setup);

}  // namespace spk::desktop::colour
