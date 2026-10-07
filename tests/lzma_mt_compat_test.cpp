// lzma_mt 内存字段的跨版本兼容：5.4.0 起 `memlimit` 拆成 memlimit_threading + memlimit_stop。
// 这个用例把两种结构都喂给生产代码里的 setMtMemlimit()，确认选中的分支**正好**是那个版本该走的
// —— 挡住"把阈值写成 5.6.0、于是 5.4/5.5 的系统库编译失败"这类回归
// （ADOFAI.Lib 的 linux-system-deps 真红过一次，内嵌的 5.6.3 本地看不出来）。
#include "archive/LzmaMtCompat.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace {
constexpr uint64_t kThreadingCap = (uint64_t)1 << 30;
constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

struct FakeOld { uint64_t memlimit = 0; };                       // liblzma < 5.4
struct FakeNew { uint64_t memlimit_threading = 0, memlimit_stop = 0; };   // liblzma >= 5.4

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("  ✗ %s\n", what); ++g_fail; }
    else     { std::printf("  ✓ %s\n", what); }
}
}  // namespace

int main() {
    std::printf("lzma_mt 字段兼容自测\n");
    // 编译期选择本身也要对：新结构必须走新分支、旧结构必须走旧分支
    static_assert(adofai::archive_detail::HasMemlimitThreading<FakeNew>);
    static_assert(!adofai::archive_detail::HasMemlimitThreading<FakeOld>);

    FakeNew nw;
    adofai::archive_detail::setMtMemlimit(nw);
    check(nw.memlimit_threading == kThreadingCap, "5.4+：memlimit_threading = 1 GiB");
    check(nw.memlimit_stop == kMax, "5.4+：memlimit_stop = UINT64_MAX");

    FakeOld od;
    adofai::archive_detail::setMtMemlimit(od);
    check(od.memlimit == kMax, "5.4 之前：memlimit = UINT64_MAX");

    std::printf(g_fail ? "lzma_mt 兼容自测失败（%d 处）\n" : "lzma_mt 兼容自测通过\n", g_fail);
    return g_fail ? 1 : 0;
}
