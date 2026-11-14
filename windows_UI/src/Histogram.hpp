#pragma once

// The output histogram's counting core — a port of
// desktop/src/canvas/histogram-core.ts: 256 bins over the *published* pixels
// (the canvas's frame: geometry and Layer 2 applied — AGENTS.md trap 33), one
// bins array per channel, ordered [0..3] = R/G/B/luma. Luma carries the
// desktop's Rec.709 weights, rounded exactly like its Math.round (identical to
// lround for non-negative sums). Rows are counted in parallel blocks like
// Layer2's applyImage; each worker owns private bins so the merge is exact.

#include <QImage>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace spk::desktop::histogram {

inline constexpr int kBins = 256;

// bins[0..3] = R/G/B/luma; all zeros for a null or empty image (the no-frame
// property state).
using Bins = std::array<std::array<int, kBins>, 4>;

[[nodiscard]] inline Bins count(const QImage& image) {
    Bins bins{};
    if (image.isNull()) return bins;
    // The published preview is Format_ARGB32; any other format is converted
    // first so the row walk can read QRgb directly.
    const QImage pixels = image.format() == QImage::Format_ARGB32
                              ? image
                              : image.convertToFormat(QImage::Format_ARGB32);
    const int width = pixels.width();
    const int height = pixels.height();
    if (width <= 0 || height <= 0) return bins;
    const uchar* const base = pixels.constBits();
    const qsizetype bytesPerLine = pixels.bytesPerLine();
    const auto countRow = [&](int y, Bins& into) {
        const QRgb* line = reinterpret_cast<const QRgb*>(base + y * bytesPerLine);
        for (int x = 0; x < width; ++x) {
            const QRgb p = line[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            ++into[0][static_cast<std::size_t>(r)];
            ++into[1][static_cast<std::size_t>(g)];
            ++into[2][static_cast<std::size_t>(b)];
            // histogram-core.ts: Math.round(0.2126r + 0.7152g + 0.0722b).
            ++into[3][static_cast<std::size_t>(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b))];
        }
    };
    int threads = QThread::idealThreadCount();
    threads = std::clamp(threads, 1, 16);
    if (height < 16 || threads <= 1) {
        for (int y = 0; y < height; ++y) countRow(y, bins);
        return bins;
    }
    // Parallel blocks, each with private bins merged below — increments on the
    // shared bins would race, unlike Layer2's rows which write disjoint pixels.
    const int block = (height + threads - 1) / threads;
    std::vector<std::thread> workers;
    std::vector<Bins> partials(static_cast<std::size_t>(threads));
    std::mutex guard;
    std::exception_ptr failure;
    const auto spawn = [&](int begin, int end, std::size_t slot) {
        workers.emplace_back([&, begin, end, slot] {
            try {
                for (int y = begin; y < end; ++y) countRow(y, partials[slot]);
            } catch (...) {
                const std::lock_guard<std::mutex> lock(guard);
                if (!failure) failure = std::current_exception();
            }
        });
    };
    try {
        for (int t = 0; t < threads; ++t) {
            const int begin = t * block;
            if (begin >= height) break;
            spawn(begin, std::min(begin + block, height), static_cast<std::size_t>(t));
        }
    } catch (...) {
        for (std::thread& worker : workers)
            if (worker.joinable()) worker.join();
        throw;
    }
    for (std::thread& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
    for (std::size_t slot = 0; slot < workers.size(); ++slot)
        for (int channel = 0; channel < 4; ++channel)
            for (int bin = 0; bin < kBins; ++bin)
                bins[channel][bin] += partials[slot][channel][bin];
    return bins;
}

} // namespace spk::desktop::histogram
