// line_pool.hpp -- `for_each_line(lines, fn)`, the cross-core line walk that
// contrast_mask.cpp's print-side blurs share.
//
// The contract (contrast_mask.cpp's comment, kept here as the single copy):
//
//   `fn(line)` for every line, across the cores. Each line writes only its own
//   outputs and reads only shared inputs, and each output is still accumulated
//   in the same order by one thread -- so the result is bit-identical to the
//   serial loop; only the wall time changes. Measured on a 45 MP frame at the
//   live tier: the serial Gaussian was ~260 ms of a 275 ms reprint at scale
//   0.03 and ~600 of 631 at 0.093.
//
// macOS keeps GCD's `dispatch_apply_f` unchanged; everywhere else (the
// Windows port) a pool of `std::thread::hardware_concurrency` workers with an
// atomic-counter line assignment stands in. The contract never names an order
// *across* lines -- a line's accumulator is private to the one worker that
// owns it -- so which worker takes which line, or how many workers there are,
// cannot move a bit of any output: threaded, single-worker and GCD runs are
// the same grid.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

namespace spk {

template <class F>
void for_each_line(uint32_t lines, F&& fn) {
#ifdef __APPLE__
    struct Ctx { F* fn; } ctx{&fn};
    dispatch_apply_f(lines, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &ctx,
                     [](void* c, size_t line) { (*static_cast<Ctx*>(c)->fn)(uint32_t(line)); });
#else
    if (lines == 0) return;
    // One worker per core, capped at the line count: a grid's height can sit
    // far below it, and spawning more workers than lines is pure overhead.
    // A single line runs inline, which also gives every caller the serial
    // reference the bit-identity claim is stated against.
    unsigned workers = std::thread::hardware_concurrency();
    if (workers == 0) workers = 1;
    workers = std::min<unsigned>(workers, lines);
    if (workers <= 1) {
        for (uint32_t line = 0; line < lines; ++line) fn(line);
        return;
    }
    std::atomic<uint32_t> next{0};
    auto worker = [&next, lines, &fn]() {
        for (;;) {
            const uint32_t line = next.fetch_add(1, std::memory_order_relaxed);
            if (line >= lines) return;
            fn(line);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(size_t(workers) - 1);
    for (unsigned i = 1; i < workers; ++i) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
#endif
}

}  // namespace spk
