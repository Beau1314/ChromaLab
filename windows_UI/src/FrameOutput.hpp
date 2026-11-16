#pragma once

#include "Layer2.hpp"
#include "desktop/preview_host.hpp"
#include <QImage>
#include <QRectF>
#include <filesystem>

// Geometry belongs after the engine. Every output surface uses these same
// pixel edges; cropping never changes the emulsion or re-renders its grain.
// The Layer 2 grade belongs before geometry: it is the print's, laid over the
// full original exactly as the canvas shader grades the displayed frame.
namespace spk::desktop::output {

enum class Format { TIFF16, PNG8, PNG16, JPEG8 };
// The file's colour space: the ICC profile the pixels are tagged with. The
// names are the engine's (`spk_output_transform`'s destinations); the Mac's
// export sheet offers the same four (`ExportRecipe.Colour`).
enum class ColourSpace { Srgb, DisplayP3, AdobeRgb, ProPhotoRgb };
struct ExportOptions {
    Format format = Format::TIFF16;
    int jpegQuality = 95;
    int maxLongEdge = 0; // zero preserves the cropped size; never upscale
    ColourSpace colourSpace = ColourSpace::Srgb;
    // The conversion out of the session's working space into this space, as
    // fetched per destination from the engine (`PreviewHost::outputTransform`)
    // and applied by the CPU core (`ColourTransform`). Null means the pixels
    // are already encoded in this space -- the session's own space, whose
    // fetch would be an identity -- and the pass is skipped, keeping the
    // default export byte-identical to the picture.
    const desktop::OutputTransformData* transform = nullptr;
};

struct Geometry {
    // Crop coordinates always refer to the original, unturned source.
    QRectF crop{0,0,1,1};
    int quarterTurns = 0; // clockwise, 0 through 3
    bool flipHorizontal = false; // in the final output axes
    bool flipVertical = false;
    double straightenDegrees = 0; // clockwise, -45 through +45
    bool operator==(const Geometry&) const = default;
};

// Normalized top-left crop in [0,1]. All fields must be finite, its area
// positive, and it must fit inside the full frame. Both edges round to the
// nearest pixel, with a minimum extent of one pixel for a valid small crop.
// Invalid input throws std::runtime_error; an empty rectangle is not "full".
QRect cropPixels(QSize fullSize, QRectF normalizedCrop);
void validateGeometry(QSize fullSize, const Geometry& geometry);
QImage displayImage(const QImage& full8, QRectF normalizedCrop);
QImage displayImage(const QImage& full8, const Geometry& geometry);
// Use this for transformed rendered previews: the grade is applied to the
// original RGBA16 and geometry is applied to the graded print before
// quantization, as it is for an 8-bit exported file.
QImage displayFrame(const FramePtr& frame, const Geometry& geometry,
                    const layer2::Adjustments& adjustments = {});

// The source is opaque RGBA16 encoded in the session's working space. The
// processing order is the Layer 2 grade on the full original (neutral
// adjustments skip the pass entirely, leaving the pixels byte-identical) ->
// the output transform into the export's colour space, still on the full
// picture, exactly as the Mac's export transforms the whole render before it
// turns or crops anything -> original-space crop -> bilinear straighten with
// zoom-to-fill (fixed crop dimensions) -> clockwise quarter turns ->
// output-axis flips -> optional downsize -> output depth. Identity and
// orthogonal geometry are lossless; straightening interpolates 16-bit
// samples without a further colour transform.
QImage exportImage(const QImage& fullRgba64, QRectF normalizedCrop,
                   const ExportOptions& options,
                   const layer2::Adjustments& adjustments = {});
QImage exportImage(const QImage& fullRgba64, const Geometry& geometry,
                   const ExportOptions& options,
                   const layer2::Adjustments& adjustments = {});
bool formatAvailable(Format format);

// Uses the explicitly bundled resources/io/sRGB.icc. Writes a complete file
// in a private temporary directory beside the destination, then publishes it
// atomically with no replacement, including if another writer wins a race.
// The caller's frame/image and any existing destination stay untouched.
// The final suffix must match the format (case insensitive); a missing suffix
// is rejected rather than creating an ambiguously typed image file.
void exportImageFile(const QImage& fullRgba64,
                     const std::filesystem::path& destination,
                     QRectF normalizedCrop, const ExportOptions& options,
                     const std::filesystem::path& resources,
                     const layer2::Adjustments& adjustments = {});
void exportImageFile(const QImage& fullRgba64,
                     const std::filesystem::path& destination,
                     const Geometry& geometry, const ExportOptions& options,
                     const std::filesystem::path& resources,
                     const layer2::Adjustments& adjustments = {});
void exportFrame(const FramePtr& frame, const std::filesystem::path& destination,
                 QRectF normalizedCrop, const ExportOptions& options,
                 const std::filesystem::path& resources,
                 const layer2::Adjustments& adjustments = {});
void exportFrame(const FramePtr& frame, const std::filesystem::path& destination,
                 const Geometry& geometry, const ExportOptions& options,
                 const std::filesystem::path& resources,
                 const layer2::Adjustments& adjustments = {});

} // namespace spk::desktop::output
