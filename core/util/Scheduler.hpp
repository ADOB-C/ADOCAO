#pragma once
// 全库唯一的调度器（设计见 docs/parallelism.md）。
//
// 现状（改造前）有五套各管各的机制：`core/util/ThreadPool` 静态单例、`LevelData` 每次调用新建池、
// `MapExport`/`PngBand` 自造线程、`std::async`、裸 `std::thread` —— 互相不知情 → 过订阅。
// 这里给一个**小**模型（明确不建 fiber / 工作窃取 / 全局 DAG / 自研无锁队列）：
//   * 一个池，`clamp(hw-1,1,8)` 个 worker，**调用线程永远参与**（`Config::workers` 可改）；
//   * 两条**优先级车道**（`Frame` > `Background`）：帧关键的活不被后台饿死；
//   * `parallelFor`：**确定性分块**（按 index 连续切片、按序派发），调用线程也干活；
//   * **嵌套护栏**：并行区里再调 `parallelFor` → 就地串行（防过订阅，且确定）；
//   * `ADOCAO_JOBS=1` → 全部串行执行（全库统一的串行 A/B 钩子）。
//
// 四条不变式（比 API 重要，见文档 §3.2）：并行区内不写共享缓存 / 输出按 index 写 /
// GL 归上下文所有者 / 确定性优先于速度。调用方负责前两条。
//
// 只用 `std::thread`（不碰平台头 → 不违反 core-purity）；平台策略（macOS QoS 等）由 app 侧注册回调。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace adofai {

enum class Lane { Frame = 0, Background = 1 };
constexpr int kLaneCount = 2;

class Scheduler {
public:
    static Scheduler& instance();

    unsigned workers() const { return (unsigned)m_workers.size(); }
    // 全局串行（`ADOCAO_JOBS=1`）—— 测试/对照用，进程级只读一次
    static bool serialForced();
    // 当前线程是否在并行区内（嵌套护栏用）
    static bool inParallel();

    // 确定性并行 for：把 [0,n) 按 index 切块，每块只写自己的输出段（不变式 2）。
    // `minGrain` = 每块至少多少元素；总量不够就就地串行（省唤醒+分配）。
    template <class F>
    void parallelFor(size_t n, size_t minGrain, Lane lane, F&& fn) {
        if (n == 0) return;
        if (serialForced() || inParallel() || n <= minGrain) {
            fn((size_t)0, n);
            return;
        }
        const size_t per = (n + targetChunks() - 1) / targetChunks();
        const size_t grain = per > minGrain ? per : minGrain;
        const size_t chunks = (n + grain - 1) / grain;
        // 一个任务 = 一个块（确定性：块按 index 连续切、写自己的输出段）
        auto done = std::make_shared<std::atomic<size_t>>(0);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (size_t c = 0; c < chunks; ++c) {
                const size_t lo = c * grain;
                const size_t hi = (lo + grain < n) ? (lo + grain) : n;
                m_queue[(int)lane].push_back([&fn, lo, hi, done] {
                    fn(lo, hi);
                    done->fetch_add(1, std::memory_order_release);
                });
            }
        }
        m_cv.notify_all();
        // 调用线程也干活（不变式：不干活就可能让 worker 等着、自己空转）
        std::function<void()> task;
        while (done->load(std::memory_order_acquire) < chunks) {
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (!popTask(task)) { task = nullptr; }
            }
            if (task) {
                InParallelGuard g;
                task();
                m_doneCv.notify_all();
            } else {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_doneCv.wait_for(lk, std::chrono::microseconds(200), [&] {
                    return done->load(std::memory_order_acquire) >= chunks;
                });
            }
        }
    }

    // 取消令牌：任务在**块边界**自查（协作式）。不可抢占 —— 文档 §3.4 的"取消"那格。
    class CancelToken {
    public:
        CancelToken() : m_flag(std::make_shared<std::atomic<bool>>(false)) {}
        bool cancelled() const { return m_flag && m_flag->load(std::memory_order_acquire); }
        void cancel() const { if (m_flag) m_flag->store(true, std::memory_order_release); }
    private:
        std::shared_ptr<std::atomic<bool>> m_flag;
    };

    // 一次性后台任务（可等待、可取消）。`Handle` 目前只有 ready/wait/cancel；
    // 将来要加 `after()` 依赖链（DAG）时不动调用方 —— 这是预留的缝之一。
    class Handle {
    public:
        Handle() = default;
        explicit Handle(std::shared_ptr<std::atomic<bool>> done, CancelToken tok = {})
            : m_done(std::move(done)), m_tok(std::move(tok)) {}
        void wait() const;
        bool valid() const { return m_done != nullptr; }
        void clear() { m_done.reset(); m_tok = CancelToken(); }
        bool ready() const { return !m_done || m_done->load(std::memory_order_acquire); }
        void cancel() const { m_tok.cancel(); }
    private:
        std::shared_ptr<std::atomic<bool>> m_done;
        CancelToken m_tok;
    };
    Handle submit(Lane lane, std::function<void(CancelToken)> fn);

    ~Scheduler();

private:
    Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    size_t targetChunks() const;   // worker 数的几倍（让负载均衡 vs 分块成本平衡）
public:
    // 并行区标记（RAII）：嵌套护栏与"worker 里再进并行区"都靠它
    struct InParallelGuard {
        InParallelGuard();
        ~InParallelGuard();
    };
private:
    void workerLoop();
    bool popTask(std::function<void()>& out);

    std::vector<std::thread> m_workers;
    std::deque<std::function<void()>> m_queue[kLaneCount];
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_doneCv;
    std::atomic<bool> m_stop{false};
};

}  // namespace adofai
