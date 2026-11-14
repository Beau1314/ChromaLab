// The worker-pool `for_each_line` behind the contrast mask's print-side
// blurs (line_pool.hpp). The contract it must hold is the one the GCD path
// kept on macOS (contrast_mask.cpp's former comment, now the header's):
//
//   each line is written by one thread and accumulated in the same order, so
//   the result is bit-identical to the serial loop; only the wall time
//   changes.
//
// These checks pin exactly that, on synthetic data:
//   1. every line is processed exactly once (a visited counter),
//   2. each line's order-sensitive double accumulation equals the serial
//      loop's *bit for bit* (operator==, not a tolerance),
//   3. repeated threaded runs are byte-identical to each other -- the
//      scheduler may interleave the workers any way it likes, but no output
//      bit may move,
//   4. the degenerate shapes (0 lines, 1 line, fewer lines than workers,
//      more) all hold 1-3, and `lines == 1` exercises the inline serial path
//      the template keeps as the reference implementation.
//
// The grid-shaped case mirrors gaussian()'s real use: one line per grid row,
// a kernel-weighted sum across the row accumulated left to right, compared
// against the same loop written serially.
#include "pipeline/line_pool.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "for_each_line: %s\n", message.c_str());
        ++failures;
    }
}

// An order-sensitive sum: addends of wildly differing magnitude make the
// running total's rounding depend on the order, so any reordering inside a
// line would show as a bit difference.
double order_sensitive_sum(uint32_t line, uint32_t terms) {
    double acc = 0.0;
    for (uint32_t k = 0; k < terms; ++k) {
        const double big = 1.0 + std::sin(double(line * 31 + k)) * 1e3;
        const double tiny = 1e-14 + 1e-16 * double(k % 7);
        acc += big;      // the big step first...
        acc += tiny;     // ...then the one a reordered loop could swallow
    }
    return acc;
}

// The grid-shaped case: `blur_line(line)` writes one row of a w-wide, h-tall
// double grid, accumulating across the row in order -- the same write shape
// as gaussian()'s passes and box_1d's lines.
void grid_case(uint32_t w, uint32_t h) {
    std::vector<double> threaded(size_t(w) * h, 0.0), serial(size_t(w) * h, 0.0);
    std::vector<uint32_t> visited(h, 0);
    auto blur_line = [&](uint32_t line) {
        visited[line] += 1;
        double acc = 0.0;
        for (uint32_t c = 0; c < w; ++c) {
            const double wt = 1.0 + std::cos(double((line + c) % 9)) * 0.25;
            acc += wt * order_sensitive_sum(line * w + c, 24);
            threaded[size_t(line) * w + c] = acc;
        }
    };
    spk::for_each_line(h, blur_line);
    for (uint32_t line = 0; line < h; ++line) {
        double acc = 0.0;
        for (uint32_t c = 0; c < w; ++c) {
            const double wt = 1.0 + std::cos(double((line + c) % 9)) * 0.25;
            acc += wt * order_sensitive_sum(line * w + c, 24);
            serial[size_t(line) * w + c] = acc;
        }
    }
    require(visited == std::vector<uint32_t>(h, 1),
            "a grid line was processed " + std::to_string(visited[0]) + " times or skipped");
    require(threaded == serial,
            "threaded grid output differs from the serial loop (w=" + std::to_string(w) +
                " h=" + std::to_string(h) + ")");
    // The scheduler is free to interleave the workers differently on every
    // run; no bit of the grid may depend on that.
    for (int run = 0; run < 64; ++run) {
        std::vector<double> again(size_t(w) * h, 0.0);
        std::vector<uint32_t> seen(h, 0);
        spk::for_each_line(h, [&](uint32_t line) {
            seen[line] += 1;
            double acc = 0.0;
            for (uint32_t c = 0; c < w; ++c) {
                const double wt = 1.0 + std::cos(double((line + c) % 9)) * 0.25;
                acc += wt * order_sensitive_sum(line * w + c, 24);
                again[size_t(line) * w + c] = acc;
            }
        });
        require(seen == std::vector<uint32_t>(h, 1), "a rerun visited a line twice");
        require(std::memcmp(again.data(), threaded.data(), again.size() * sizeof(double)) == 0,
                "a rerun of the threaded grid produced different bits");
    }
}

void shape_case(uint32_t lines) {
    std::vector<double> threaded(lines, 0.0), serial(lines, 0.0);
    std::vector<uint32_t> visited(std::max<uint32_t>(lines, 1), 0);
    auto line_body = [&](uint32_t line) {
        visited[line] += 1;
        threaded[line] = order_sensitive_sum(line, 512);
    };
    spk::for_each_line(lines, line_body);
    for (uint32_t line = 0; line < lines; ++line) serial[line] = order_sensitive_sum(line, 512);
    require(threaded == serial,
            "threaded output differs from the serial loop (" + std::to_string(lines) + " lines)");
    for (uint32_t line = 0; line < lines; ++line)
        require(visited[line] == 1, "a line was processed twice or skipped");
    for (int run = 0; run < 64; ++run) {
        std::vector<double> again(lines, 0.0);
        spk::for_each_line(lines, [&](uint32_t line) { again[line] = order_sensitive_sum(line, 512); });
        require(std::memcmp(again.data(), threaded.data(), again.size() * sizeof(double)) == 0,
                "a rerun produced different bits");
    }
}

}  // namespace

int main() {
    // The shapes the blurs actually meet, including every branch of the
    // template: 0 lines, the inline serial path (1 line), fewer lines than
    // workers, roughly one per worker, and far more.
    shape_case(0);
    shape_case(1);
    shape_case(3);
    const unsigned hw = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1;
    shape_case(uint32_t(hw));
    shape_case(uint32_t(hw) * 7 + 3);
    // The grid case, uneven both ways: taller and wider than the worker pool
    // in turn.
    grid_case(97, 16);
    grid_case(16, 257);
    if (failures) {
        std::fprintf(stderr, "for_each_line checks failed (%d)\n", failures);
        return 1;
    }
    std::printf("for_each_line checks passed (serial == threaded bit for bit on %u cores; "
                "64 reruns byte-identical)\n", hw);
    return 0;
}
