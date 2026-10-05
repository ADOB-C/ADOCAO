#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>


namespace adofai {

class ThreadPool {
public:
    explicit ThreadPool(unsigned count = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void parallelFor(size_t start, size_t end,
                     const std::function<void(size_t, size_t)>& func,
                     size_t minChunk = 1);
    unsigned threadCount() const { return (unsigned)m_workers.size(); }

private:
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    void workerLoop();
};

}  // namespace adofai
