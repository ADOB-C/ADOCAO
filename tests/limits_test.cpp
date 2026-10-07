// #0：2^31−1 层的边界算术。
//
// 为什么要有这个测试：本地**造不出** 21 亿层的谱（最大的 primer 只有 6.84e7 层 = 目标的 3%），
// 所以"类型层面不坏"只能钉在纯函数上。这里的每一条对应一个真实会炸的写法：
//   * `(int)angleData.size() + 1`  → 2^31−1 时有符号溢出，n 变负 → resize(负) 炸；
//   * `(int)v` 收窄            → parseInt("2147483648") 得到 −2147483648，事件被静默丢弃；
//   * `int` 承接逐层字节数        → 51 GB 在 int 里早就溢出了。
#include "core/util/CountLimits.hpp"

#include <cstdint>
#include <cstdio>

using namespace adofai;

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("  FAIL %s\n", what); ++fails; }
        else     { std::printf("  ok   %s\n", what); }
    };

    // ① 目标规模的计数：新的写法不溢出，而且**明确知道**自己超出 int32。
    check(countFromAngleCount(kInt32Max) == (int64_t)kInt32Max + 1,
          "2^31−1 个角度 → 2^31 层（int64 不溢出）");
    check(!fitsInt32(countFromAngleCount(kInt32Max)),
          "2^31 已超 int32（收窄必须显式失败，不许截断）");
    check(countFromTilesOrAngles(0, kInt32Max) == (int64_t)kInt32Max + 1,
          "导出模式（tiles 空）由角度数推层数");
    check(countFromTilesOrAngles(1000, kInt32Max) == 1000,
          "有 tiles 时以 tiles 为准");

    // ② 收窄检查：边界两侧各一格，负数也要拦。
    int out = -1;
    check(toInt32Checked(kInt32Max, out) && out == kInt32Max, "int32 上限本身可以通过");
    check(!toInt32Checked(kInt32Max + 1, out), "int32 上限 +1 必须被拒");
    check(!toInt32Checked(-1, out), "负数必须被拒（id 语义下没有负层号）");
    out = -1;
    check(toInt32Checked(0, out) && out == 0, "0 可以通过");

    // ③ 逐层预算的算术必须全程 int64：这三个数就是 2^31−1 层的三笔大头。
    const int64_t goal = kTileGoal;
    check(goal * 24 == 51539607528LL, "Tile 24 B/层 × 2^31−1 = 51.54 GB 不溢出");
    check(goal * 16 == 34359738352LL, "FastAction 16 B/事件 × 2^31−1 = 34.36 GB 不溢出");
    check(goal * 8  == 17179869176LL, "angleData 8 B/层 × 2^31−1 = 17.18 GB 不溢出");
    check((goal * 8) / 8 == goal, "int64 下取回层数一致");

    // ④ 反面对照：老的 `int` 写法在同一组数上**确实**会坏（这条是在证明测试有效，
    //    不是在证明代码坏 —— 一旦哪天有人把类型改回 int，①②会先红）。
    const int oldLike = (int)(kInt32Max + 1);
    check(oldLike < 0, "对照：(int)(2^31) 变负（这正是老写法的行为）");

    std::printf(fails ? "limits: **%d 处失败**\n" : "limits: 全部通过（2^31−1 边界）\n", fails);
    return fails ? 1 : 0;
}
