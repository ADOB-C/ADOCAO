#include "core/util/Scheduler.hpp"

#include <algorithm>
#include <cstdlib>

#include "core/util/Logger.hpp"

namespace adofai {

namespace {
// 线程本地"我在并行区里"标记：并行区内再调 parallelFor 就地串行（防过订阅 + 保持确定）
thread_local int t_inParallel = 0;

}  // namespace

Scheduler::InParallelGuard::InParallelGuard() { ++t_inParallel; }
Scheduler::InParallelGuard::~InParallelGuard() { --t_inParallel; }

bool Scheduler::serialForced() {
    static const bool forced = [] {
        const char* e = std::getenv("ADOCAO_JOBS");
        const bool f = e && std::atoi(e) == 1;
        if (f) LOG_I("scheduler: ADOCAO_JOBS=1 → 全部并行就地串行（A/B 对照用）");
        return f;
    }();
    return forced;
}

bool Scheduler::inParallel() { return t_inParallel > 0; }

Scheduler& Scheduler::instance() {
    static Scheduler s;
    return s;
}

Scheduler::Scheduler() {
    unsigned n = std::thread::hardware_concurrency();
    if (const char* e = std::getenv("ADOCAO_SCHED_WORKERS")) {   // 诊断：固定 worker 数
        const int v = std::atoi(e);
        if (v >= 0) { n = (unsigned)v + 1; }
    }
    if (n == 0) n = 2;
    if (!std::getenv("ADOCAO_SCHED_WORKERS")) {
        n = n > 1 ? n - 1 : 1;      // 留给调用线程
        if (n > 8) n = 8;           // 沿用库里实测口径：再多边际收益很小（Config 可改）
    }
    m_workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back(&Scheduler::workerLoop, this);
    LOG_I("scheduler: %u 个 worker（+调用线程），车道 %d 条", n, kLaneCount);
}

Scheduler::~Scheduler() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    for (auto& t : m_workers)
        if (t.joinable()) t.join();
}

void Scheduler::configure(const Config& c) {
    // 只允许在**没有 worker 被唤醒过**之前改（典型用法：main() 顶部）。
    // 真要运行期改 worker 数，属于预留的缝（文档 §3.4）——那时再加"扩缩容"路径。
    if (!m_workers.empty()) {
        LOG_W("scheduler: configure 在任何任务之前才生效（当前 %zu 个 worker）", m_workers.size());
        return;
    }
    unsigned n = c.workers;
    if (n == 0) {
        n = std::thread::hardware_concurrency();
        if (n == 0) n = 2;
        n = n > 1 ? n - 1 : 1;
        if (n > 8) n = 8;
    }
    m_mainParticipates = c.mainParticipates;
    m_workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back(&Scheduler::workerLoop, this);
    LOG_I("scheduler: 配置为 %u 个 worker（+调用线程）", n);
}

size_t Scheduler::targetChunks() const {
    const size_t w = m_workers.empty() ? 1 : m_workers.size();
    return w * 4;   // 每 worker 4 块：负载均衡 vs 分块成本
}

bool Scheduler::popTask(std::function<void()>& out) {
    // 严格优先 Frame；Background 可被饿着（后台本来就是"空闲时做"）
    for (int l = 0; l < kLaneCount; ++l) {
        if (!m_queue[l].empty()) {
            out = std::move(m_queue[l].front());
            m_queue[l].pop_front();
            return true;
        }
    }
    return false;
}

void Scheduler::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [&] { return m_stop.load(std::memory_order_acquire) || popTask(task); });
            if (!task && m_stop.load(std::memory_order_acquire)) return;
        }
        InParallelGuard g;
        task();
        m_doneCv.notify_all();
    }
}

Scheduler::Handle Scheduler::submit(Lane lane, std::function<void(CancelToken)> fn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    CancelToken tok;
    const CancelToken tokCopy = tok;
    auto wrapped = [fn = std::move(fn), done, tokCopy]() {
        if (tokCopy.cancelled()) { done->store(true, std::memory_order_release); return; }
        fn(tokCopy);
        done->store(true, std::memory_order_release);
    };
    if (serialForced() || inParallel() || m_workers.empty()) {
        InParallelGuard g;
        wrapped();
        return Handle(done, tok);
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue[(int)lane].push_back(std::move(wrapped));
    }
    m_cv.notify_all();
    return Handle(done, tok);
}

void Scheduler::Handle::wait() const {
    if (!m_done) return;
    while (!m_done->load(std::memory_order_acquire))
        std::this_thread::yield();
}

}  // namespace adofai
