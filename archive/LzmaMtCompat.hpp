#pragma once

#include <cstdint>
#include <limits>

// lzma_mt 的内存上限字段跨版本改过名：5.4.0 把 `memlimit` 拆成了
// `memlimit_threading` + `memlimit_stop`（和 MT 解码器本身同一版引入）。
//
// **别用 LZMA_VERSION 判断**：写成 5.6.0 那种阈值时，5.4/5.5 的系统库会走进旧分支、
// 然后报 "struct lzma_mt has no member named 'memlimit'" —— ADOFAI.Lib 的 linux-system-deps
// 那一格就是这么红的（内嵌的 5.6.3 永远编得过，所以本地看不出来）。按字段是否存在来选，
// 对任何版本都对。discarded 分支必须留在模板里才不会被检查（非模板的 if constexpr 两边都查）。
namespace adofai::archive_detail {

template <class Mt>
concept HasMemlimitThreading = requires(Mt& m) { m.memlimit_threading; };

template <class Mt>
void setMtMemlimit(Mt& mt) {
    if constexpr (HasMemlimitThreading<Mt>) {
        // 线程缓冲上限。../Song.adofai 那边给 UINT64_MAX（只要最快），但那是 CLI，
        // 而这里解完还要在同一个进程里放下解析结构，峰值内存更值钱。同机 10 核、
        // 64 MiB 字典 / 19 blocks 实测（1.18 GB 输出）：不限 475 ms / 1.67 GB 峰值，
        // 1 GiB 上限 585 ms / 1.04 GB —— +110 ms 换掉 0.6 GB，值。
        mt.memlimit_threading = (uint64_t)1 << 30;
        mt.memlimit_stop = std::numeric_limits<uint64_t>::max();   // 别让 lzma_code 因上限直接失败
    } else {
        mt.memlimit = std::numeric_limits<uint64_t>::max();        // 5.4 之前的唯一字段
    }
}

}  // namespace adofai::archive_detail
