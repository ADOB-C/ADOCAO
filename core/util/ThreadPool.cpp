#include "ThreadPool.hpp"
#include <algorithm>


namespace adofai {

ThreadPool::ThreadPool(unsigned count) {
    if (count == 0) { count = std::thread::hardware_concurrency(); if (count == 0) count = 2; }
    m_workers.reserve(count);
    for (unsigned i = 0; i < count; i++) m_workers.emplace_back(&ThreadPool::workerLoop, this);
}

ThreadPool::~ThreadPool() {
    { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; }
    m_cv.notify_all();
    for (auto& w : m_workers) if (w.joinable()) w.join();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        { std::unique_lock<std::mutex> lock(m_mutex); m_cv.wait(lock, [this]{return m_stop||!m_tasks.empty();}); if(m_stop&&m_tasks.empty())return; task=std::move(m_tasks.front());m_tasks.pop(); }
        task();
    }
}

void ThreadPool::parallelFor(size_t start, size_t end,
                              const std::function<void(size_t, size_t)>& func,
                              size_t minChunk) {
    size_t total = end - start; if (total == 0) return;
    size_t numWorkers = m_workers.size() + 1;
    size_t chunkSize = std::max(minChunk, (total + numWorkers - 1) / numWorkers);
    std::atomic<size_t> remaining{total};
    { std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t cs = start; cs < end; ) {
            size_t ce = std::min(cs + chunkSize, end);
            m_tasks.emplace([&func, cs, ce, &remaining]() { func(cs, ce); remaining.fetch_sub(ce - cs); });
            cs = ce;
        }
    }
    m_cv.notify_all();
    while (remaining.load() > 0) {
        std::function<void()> task;
        { std::unique_lock<std::mutex> lock(m_mutex); if (!m_tasks.empty()) { task = std::move(m_tasks.front()); m_tasks.pop(); } }
        if (task) task(); else std::this_thread::yield();
    }
}

}  // namespace adofai
