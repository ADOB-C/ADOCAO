#include "core/util/Progress.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>


namespace adofai {

namespace progress {
namespace {

bool g_on = false;
std::mutex g_m;
double g_t0 = 0.0, g_lastT = 0.0;
long long g_lastDone = 0;

double nowSec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

}  // namespace

void enable(bool on) { g_on = on; }
bool enabled() { return g_on; }

void reset() {
    std::lock_guard<std::mutex> lk(g_m);
    g_t0 = g_lastT = nowSec();
    g_lastDone = 0;
}

void update(long long done, long long total, const char* unit, const char* extra) {
    if (!g_on) return;
    std::lock_guard<std::mutex> lk(g_m);
    const double t = nowSec();
    if (g_t0 == 0.0) { g_t0 = g_lastT = t; g_lastDone = 0; }
    // 限频：每 100ms 或每 0.5% 才重画一次（多线程下也不会刷屏）
    const bool bigStep = total > 0 && (double)(done - g_lastDone) >= (double)total * 0.005;
    if (!bigStep && (t - g_lastT) < 0.1 && done < total) return;
    g_lastT = t;
    g_lastDone = done;
    const double el = t - g_t0;
    const double pct = total > 0 ? 100.0 * (double)done / (double)total : 0.0;
    const double rate = el > 0.001 ? (double)done / el : 0.0;
    const double eta = (rate > 0.0 && total > done) ? (double)(total - done) / rate : 0.0;
    std::fprintf(stderr, "\r  %5.1f%%  %lld/%lld %s  %.0f/s  ETA %02d:%02d%s%s\x1b[K",
                 pct, done, total, unit, rate, (int)eta / 60, (int)eta % 60,
                 (extra && extra[0]) ? "  " : "", extra ? extra : "");
    std::fflush(stderr);
}

void finish() {
    if (!g_on) return;
    std::lock_guard<std::mutex> lk(g_m);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    g_t0 = g_lastT = 0.0;
    g_lastDone = 0;
}

}  // namespace progress

}  // namespace adofai
