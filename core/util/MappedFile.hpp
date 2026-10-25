#pragma once

// 只读文件映射（POSIX mmap / Windows 回退读进内存）。
//
// 为什么需要它（2026-10-25，用户："你这序列化器有大问题"）：
//   `LevelData::loadFromFile()` 原来是"整份 read 进 std::string" —— 6.5 GB 的谱面就是
//   6.5 GB 堆内存，加上解析时的角度/砖/事件数组，16 GB 机器直接被挤到 swap
//   （实测 footprint 44 GB、换出 ~139 GB）。mmap 之后文件页由内核按需调页：
//   解析器仍然看到连续内存，但**不再复制一份**；pack 需要的原始切片也能直接指向映射，
//   不必再为每条 action 复制一个 std::string。

#include <cstddef>
#include <string>

namespace adofai {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // 成功返回 true；失败时 data() 为 nullptr、size() 为 0。
    bool open(const std::string& path);

    const char* data() const { return m_data; }
    size_t size() const { return m_size; }
    // true = 真的 mmap 出来的（不可写、不拥有堆内存）；false = 回退读进 vector
    bool mapped() const { return m_mapped; }

private:
    void close();

    const char* m_data = nullptr;
    size_t m_size = 0;
    bool m_mapped = false;
    std::string m_fallback;   // Windows/失败时的堆副本
};

}  // namespace adofai
