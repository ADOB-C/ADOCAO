#pragma once

#include <cstddef>

// CLI 进度行：常驻一行、\r 原地刷新、自带速率与 ETA。
// 写 stderr（不污染 stdout 的机器可读输出），多线程可安全调用（内部加锁 + 限频）。

namespace adofai {

namespace progress {

void enable(bool on);            // 由 app 决定：--progress / --no-progress / isatty(stderr)
bool enabled();
void reset();                    // 开始新阶段（重置计时与速率基线）
void update(long long done, long long total, const char* unit, const char* extra = nullptr);
void finish();                   // 阶段结束：换行

}  // namespace progress

}  // namespace adofai
