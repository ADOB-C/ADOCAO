#pragma once
// 按段计时（Frame profiler 的那一半：**段**，另一半是整帧数）。
//
// 为什么需要：`--capture` 只给整帧 `render_ms`（而且默认是**首帧** —— 含着色器编译/纹理上传等
// 一次性开销），而 D3（帧预算）与 ④/③ 的收益都需要知道"这一帧里 cull / attr / 上传 / draw /
// 场景更新 各占多少"，还要能分开**首帧**与**稳态**。靠外部探针只能模拟，不能拆真路径。
//
// 时钟：用 `std::chrono::steady_clock`。**Windows/MSVC 上它就是 `QueryPerformanceCounter`**
// （macOS/libc++ 是 `clock_gettime(CLOCK_MONOTONIC)`、Linux 同），所以平台侧不需要再包一层；
// 实测读钟成本（Apple M5）`mach_absolute_time` 5.0 ns/次、`clock_gettime` 16.9 ns/次 ——
// 一次 RAII 段 = 2 次读钟 = 10~34 ns，一帧 7 段 ≈ 0.07~0.24 µs，相对稳态帧 0.3~0.7 ms 是
// **0.02~0.07%**。→ **精度瓶颈不在时钟，在采样**：所以下面按"首帧 / 稳态多帧统计"给。
//
// 用法：`ADOCAO_PROFILE=1`（**默认关**，关掉时只有一个 cached bool 的代价）：
//   profile::frameBegin();              // 每帧开头
//   { profile::Scoped s("cull"); ... }  // RAII 段，可嵌套
//   profile::frameEnd();                // 每帧结尾（自量这一帧）
// 抓帧结束用 `reportFirst()` / `reportSteady()` 打两条口径：
//   * 首帧：单次快照（含着色器编译/纹理上传等一次性开销）；
//   * 稳态：**第 2..N 帧**的 min / 中位 / max（n 次），首帧不混进去。
// **自洽校验**：稳态"段中位和" ≤ 帧中位 × 1.05 + 0.5 ms（超出就 LOG_W，防仪表盘自己错）。
//
// 边界（别当通用 profiler）：
//   * **只在渲染线程**：累加器无锁；加载期/异步 mesh 建表在别的线程（那部分看 `app/ProcMem.hpp`
//     与日志里的建表耗时）。
//   * 段名是**字面量**（指针 + strcmp 匹配，段数很少）。
//   * `draw` 段量的是 **CPU 提交时间**，不是 GPU 时间（要看 GPU 得上 `GL_TIMER_QUERY`）。
//   * 它回答"哪一段贵"，不回答"函数里哪一行贵" —— 后者用 `sample`/`xctrace`（`docs/profiling.md`）。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "core/util/Env.hpp"
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace adofai {
namespace profile {

inline bool enabled() {
    static const bool on = env::flag("ADOCAO_PROFILE");
    return on;
}

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Entry {
    const char* name;
    double ms = 0.0;
};

// 一帧的快照（段 + 这一帧自量耗时）
struct Frame {
    std::vector<Entry> sections;
    double ms = 0.0;
    bool valid = false;
};

// 某段在"稳态帧"上的样本
struct Section {
    const char* name;
    std::vector<double> samples;   // 第 2..N 帧各一次
};

inline std::vector<Entry>& current() { static std::vector<Entry> v; return v; }
inline Frame& firstFrame() { static Frame f; return f; }
inline std::vector<Section>& sections() { static std::vector<Section> v; return v; }
inline std::vector<double>& frameSamples() { static std::vector<double> v; return v; }
inline long long& frameCount() { static long long n = 0; return n; }
inline Clock::time_point& frameT0() { static Clock::time_point t; return t; }

inline Section& sectionFor(const char* name) {
    auto& v = sections();
    for (Section& s : v) {
        if (s.name == name || std::strcmp(s.name, name) == 0) return s;
    }
    v.push_back({name, {}});
    return v.back();
}

inline void add(const char* name, double ms) {
    auto& v = current();
    for (Entry& e : v) {
        if (e.name == name || std::strcmp(e.name, name) == 0) {
            e.ms += ms;
            return;
        }
    }
    v.push_back({name, ms});
}

inline void frameBegin() {
    if (!enabled()) return;
    current().clear();
    frameT0() = Clock::now();
}

inline void frameEnd() {
    if (!enabled()) return;
    const double ms = msSince(frameT0());
    if (frameCount() == 0) {
        firstFrame() = Frame{current(), ms, true};      // 首帧单次快照
    } else {
        for (const Entry& e : current()) sectionFor(e.name).samples.push_back(e.ms);
        frameSamples().push_back(ms);
    }
    ++frameCount();
}

inline void reset() {
    current().clear();
    sections().clear();
    frameSamples().clear();
    firstFrame() = Frame{};
    frameCount() = 0;
}

inline double sumMs(const Frame& f) {
    double s = 0;
    for (const Entry& e : f.sections) s += e.ms;
    return s;
}

namespace detail {
struct Basic { double min, med, max; };
inline Basic basicOf(std::vector<double> v) {
    if (v.empty()) return {0, 0, 0};
    std::sort(v.begin(), v.end());
    return {v.front(), v[v.size() / 2], v.back()};
}
}  // namespace detail

// 首帧：单次快照 + 未归段的余量
inline std::string reportFirst() {
    const Frame& f = firstFrame();
    if (!f.valid) return "（无首帧样本）";
    char buf[96];
    std::string out;
    for (const Entry& e : f.sections) {
        std::snprintf(buf, sizeof buf, "%s=%.3f ", e.name, e.ms);
        out += buf;
    }
    std::snprintf(buf, sizeof buf, "other=%.3f frame=%.3f", f.ms - sumMs(f), f.ms);
    out += buf;
    return out;
}

// 稳态：第 2..N 帧的 min/中位/max（n 次）+ 帧本身的三分位；`other` 用中位数相减估算
inline std::string reportSteady() {
    if (frameSamples().empty()) return "（只有一帧，无稳态样本；用 ADOCAO_CAPTURE_FRAMES=N 取 N>1）";
    char buf[160];
    std::string out;
    double medSum = 0;
    for (const Section& s : sections()) {
        const detail::Basic b = detail::basicOf(s.samples);
        medSum += b.med;
        std::snprintf(buf, sizeof buf, "%s=%.3f[%.3f..%.3f] ", s.name, b.med, b.min, b.max);
        out += buf;
    }
    const detail::Basic fb = detail::basicOf(frameSamples());
    std::snprintf(buf, sizeof buf, "other~=%.3f frame=%.3f[%.3f..%.3f] n=%zu",
                  fb.med - medSum, fb.med, fb.min, fb.max, frameSamples().size());
    out += buf;
    return out;
}

inline double steadySectionSumMed() {
    double s = 0;
    for (const Section& sec : sections()) s += detail::basicOf(sec.samples).med;
    return s;
}
inline double steadyFrameMed() {
    return frameSamples().empty() ? 0.0 : detail::basicOf(frameSamples()).med;
}

// RAII 段（只累加到 current；帧边界由 frameBegin/frameEnd 管）
struct Scoped {
    const char* name;
    Clock::time_point t0;
    bool on;
    explicit Scoped(const char* n) : name(n), on(enabled()) {
        if (on) t0 = Clock::now();
    }
    ~Scoped() {
        if (!on) return;
        add(name, msSince(t0));
    }
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;
};

}  // namespace profile
}  // namespace adofai
