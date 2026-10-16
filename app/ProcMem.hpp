#pragma once
// 当前进程的**常驻**内存（不是 peak RSS）。
//
// 为什么单独要这个口径：`/usr/bin/time -l` 给的是**峰值**，而大谱的峰值落在**加载期**
// （`LevelData` 那 212 B/层），mesh / 渲染侧的数组是加载**之后**才建的 —— 于是"渲染侧
// 少了一支 4 B/砖 的数组"这种改动，峰值上**看不见**（2026-10-15 实测：MYC 删掉
// `m_drawOrder`（6.77M × 4 B = 27 MB）后峰值 1429.5 → 1428.9 MB，只在噪声内）。
// D1（内存与 n 解耦）要的恰恰是"跑起来常驻多少"，所以量这个。
//
// macOS 走 `task_info(MACH_TASK_BASIC_INFO).resident_size`；Linux 走 `/proc/self/statm`；
// 其它平台返回 0（调用方按"量不到"处理，别当 0 使用）。
#include <cstddef>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

namespace appmem {

inline size_t currentResidentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (size_t)info.resident_size;
    return 0;
#elif defined(__linux__)
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    if (std::fscanf(f, "%*d %ld", &pages) != 1) pages = 0;
    std::fclose(f);
    return (size_t)pages * (size_t)sysconf(_SC_PAGESIZE);
#else
    return 0;   // 量不到
#endif
}

}  // namespace appmem
