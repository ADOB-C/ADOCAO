#pragma once
// 按段计时（Frame profiler 的那一半：**段**，另一半是整帧数）。
//
// 为什么需要：`--capture` 只给整帧 `render_ms`（而且默认是**首帧** —— 含着色器编译/纹理上传等
// 一次性开销），而 D3（帧预算）与 ④/③ 的收益都需要知道"这一帧里 cull / attr / 上传 / draw /
// 场景更新 各占多少"，还要能分开**首帧**与**稳态**。靠外部探针只能模拟，不能拆真路径。
//
// 用法：`ADOCAO_PROFILE=1`（**默认关**，关掉时只有一个 cached bool 的代价）：
//   profile::frameBegin();            // 每帧开头
//   { profile::Scoped s("cull"); ... }   // RAII 段，可嵌套
//   profile::frameEnd();              // 每帧结尾（自带 steady_clock 量这一帧）
// 然后 `report(profile::lastFrame())` / `report(profile::firstFrame())` 拿两条口径。
// **自洽校验**：段和 ≤ 该帧自量耗时（`other = frame - 段和` 就是没被段的：ImGui/swap/present）。
//
// 边界（别当通用 profiler）：
//   * **只在渲染线程**：累加器无锁；加载期/异步 mesh 建表在别的线程（那部分看 `app/ProcMem.hpp`
//     与日志里的建表耗时）。
//   * 段名是**字面量**（指针 + strcmp 匹配，段数很少）。
//   * 它回答"哪一段贵"，不回答"函数里哪一行贵" —— 后者用 `sample`/`xctrace`（见 `docs/profiling.md`）。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace adofai {
namespace profile {

inline bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("ADOCAO_PROFILE");
        return e && e[0] != '\0' && e[0] != '0';
    }();
    return on;
}

struct Entry {
    const char* name;
    double ms = 0.0;
    long long hits = 0;
};

// 一帧的快照：段 + 这一帧自量耗时
struct Frame {
    std::vector<Entry> sections;
    double ms = 0.0;
    bool valid = false;
};

inline std::vector<Entry>& current() { static std::vector<Entry> v; return v; }
inline Frame& firstFrame() { static Frame f; return f; }
inline Frame& lastFrame() { static Frame f; return f; }
inline long long& frameCount() { static long long n = 0; return n; }
inline std::chrono::steady_clock::time_point& frameT0() {
    static std::chrono::steady_clock::time_point t;
    return t;
}

inline void add(const char* name, double ms) {
    auto& v = current();
    for (Entry& e : v) {
        if (e.name == name || std::strcmp(e.name, name) == 0) {
            e.ms += ms;
            ++e.hits;
            return;
        }
    }
    v.push_back({name, ms, 1});
}

inline void frameBegin() {
    if (!enabled()) return;
    current().clear();
    frameT0() = std::chrono::steady_clock::now();
}

inline void frameEnd() {
    if (!enabled()) return;
    Frame f;
    f.sections = current();
    f.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameT0()).count();
    f.valid = true;
    if (frameCount() == 0) firstFrame() = f;
    lastFrame() = std::move(f);
    ++frameCount();
}

inline void reset() {
    current().clear();
    firstFrame() = Frame{};
    lastFrame() = Frame{};
    frameCount() = 0;
}

inline double sumMs(const Frame& f) {
    double s = 0;
    for (const Entry& e : f.sections) s += e.ms;
    return s;
}

// "cull=0.42 upload=0.11 draw=30.05 other=0.31 frame=30.89"
inline std::string report(const Frame& f) {
    char buf[64];
    std::string out;
    for (const Entry& e : f.sections) {
        std::snprintf(buf, sizeof buf, "%s=%.3f ", e.name, e.ms);
        out += buf;
    }
    std::snprintf(buf, sizeof buf, "other=%.3f frame=%.3f", f.ms - sumMs(f), f.ms);
    out += buf;
    return out;
}

// RAII 段（只累加到 current；帧边界由 frameBegin/frameEnd 管）
struct Scoped {
    const char* name;
    std::chrono::steady_clock::time_point t0;
    bool on;
    explicit Scoped(const char* n) : name(n), on(enabled()) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~Scoped() {
        if (!on) return;
        add(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;
};

}  // namespace profile
}  // namespace adofai
