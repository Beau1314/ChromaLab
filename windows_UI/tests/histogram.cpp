// The output histogram's counting core: 256 bins over the published preview's
// pixels, R/G/B/luma, ported from desktop/src/canvas/histogram-core.ts. No RAW
// decoder, no GPU, no controller — the same pure tier as the Layer 2 test.
#include "Histogram.hpp"
#include <QCoreApplication>
#include <QImage>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace histogram = spk::desktop::histogram;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
// The gate's and the review's reference counter: one serial pass with the
// desktop's formula — Rec.709 luma rounded like histogram-core.ts Math.round.
histogram::Bins manualCount(const QImage& image) {
    histogram::Bins bins{};
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb p = line[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            ++bins[0][r];
            ++bins[1][g];
            ++bins[2][b];
            ++bins[3][static_cast<int>(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b))];
        }
    }
    return bins;
}
bool sameBins(const histogram::Bins& left, const histogram::Bins& right) {
    for (int channel = 0; channel < 4; ++channel)
        for (int bin = 0; bin < histogram::kBins; ++bin)
            if (left[channel][bin] != right[channel][bin]) return false;
    return true;
}
long long total(const histogram::Bins& bins) {
    long long sum = 0;
    for (const auto& channel : bins)
        for (const int bin : channel) sum += bin;
    return sum;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        // A null (no-frame) image counts to all zeros — the property's no-frame
        // contract comes straight from this branch.
        {
            const histogram::Bins bins = histogram::count(QImage());
            bool zero = true;
            for (const auto& channel : bins)
                for (const int bin : channel)
                    if (bin != 0) zero = false;
            check(zero, "a null image must count to all-zero bins");
        }

        // Crafted pixels: every channel lands in its own bin, the luma rounding
        // anchors match the desktop's Math.round semantics, and each channel's
        // total equals the pixel count.
        {
            QImage image(3, 2, QImage::Format_ARGB32);
            image.fill(0); // a fully transparent pixel still contributes rgb
            image.setPixel(0, 0, qRgba(10, 200, 30, 255));
            image.setPixel(1, 0, qRgba(0, 0, 0, 128)); // alpha is not counted
            image.setPixel(2, 0, qRgba(255, 255, 255, 255));
            image.setPixel(0, 1, qRgba(10, 200, 30, 255));
            const histogram::Bins bins = histogram::count(image);
            check(bins[0][10] == 2, "red bin 10 must hold both red-10 pixels");
            check(bins[1][200] == 2, "green bin 200 must hold both green-200 pixels");
            check(bins[2][30] == 2, "blue bin 30 must hold both blue-30 pixels");
            // 0.2126·10 + 0.7152·200 + 0.0722·30 = 147.332 → bin 147 twice.
            check(bins[3][147] == 2, "the (10,200,30) luma must round to bin 147");
            check(bins[3][0] == 3, "black pixels (two transparent, one alpha 128) luma-count into bin 0");
            // 0.2126+0.7152+0.0722 == 1.0, so white's luma rounds to bin 255.
            check(bins[3][255] == 1, "white must luma-count into bin 255");
            check(bins[0][0] == 3 && bins[1][0] == 3 && bins[2][0] == 3,
                  "black and transparent pixels count rgb as stored");
            for (int channel = 0; channel < 4; ++channel) {
                long long channelSum = 0;
                for (const int bin : bins[channel]) channelSum += bin;
                check(channelSum == 6, "each channel's bins must sum to the pixel count");
            }
            check(sameBins(bins, manualCount(image)),
                  "the parallel counter must match the serial reference on crafted pixels");
        }

        // The threaded path: 20 rows spawn parallel row blocks. Every bin must
        // equal the serial reference exactly — no lost or duplicated counts.
        {
            const int width = 5, height = 20;
            QImage image(width, height, QImage::Format_ARGB32);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const int r = (x * 53 + y * 97) % 256;
                    const int g = (x * 131 + y * 29) % 256;
                    const int b = (x * 211 + y * 7) % 256;
                    image.setPixel(x, y, qRgb(r, g, b));
                }
            check(sameBins(histogram::count(image), manualCount(image)),
                  "the parallel counter must match the serial reference across row blocks");
        }

        // A non-ARGB32 input is converted first, like Layer2's applyImage does.
        {
            QImage rgb32(4, 3, QImage::Format_RGB32);
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 4; ++x)
                    rgb32.setPixel(x, y, qRgb((x + y) * 20 % 256, 30 * x, 255 - y * 40));
            check(sameBins(histogram::count(rgb32), manualCount(rgb32.convertToFormat(QImage::Format_ARGB32))),
                  "an RGB32 image must count like its ARGB32 conversion");
        }

        // Empty dimensions guard against a zero-sized publication.
        {
            const QImage empty(0, 0, QImage::Format_ARGB32);
            check(total(histogram::count(empty)) == 0, "a zero-sized image must count to nothing");
        }

        std::printf("Output histogram counting: %d checks passed; no GPU, no RAW input.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Histogram counting FAILED after %d checks: %s\n", checks, error.what());
        return 1;
    }
}
