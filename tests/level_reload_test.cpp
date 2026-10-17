// 关卡对象**所有权 / 幂等**的回归用例 —— 对应 2026-10-17 外部评审的三条真问题：
//
//   ① `LevelData` 有自定义析构（删 `m_winCache`）却没有禁拷贝 → `LevelData b = a;` 会让两个
//      对象各持同一个缓存，析构 double free。现在拷贝被禁用、移动安全（缓存是 move-only 的
//      unique_ptr + 自定义删除器）。这一条**编译期**就守住：下面两个 static_assert。
//   ② `loadFromBuffer` 不幂等：同一个对象连调两次，`actions` 会被追加（两条解析路径都是
//      `if (actions.empty()) … else insert(...)`）而 `angleData` 是替换 → "角度新的、事件两份"。
//      现在开头回到"刚构造"的状态（`settings` 例外，它是"文件没写的字段沿用调用方给的默认值"）。
//   ③ `computeCheckpoints` 直接读 `level.angleData[i]`：`releaseAngleData()` 之后是空数组 → UB。
//      现在走 `angleAt()` 访问器；这条由"压缩后仍然能建 checkpoint"的用例守住。
//
// 用法：adocao_level_reload_test <谱面.adofai>   （不给参数则用 tests/charts/angles360.adofai）

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

std::vector<char> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool load(const std::vector<char>& bytes, LevelData& lv) {
    return !bytes.empty() && lv.loadFromBuffer(bytes.data(), bytes.size(), nullptr, false);
}

// 加载一次时 actions/settings/角度应当长什么样；两次加载必须完全一样
struct Snapshot {
    int64_t tiles = 0;
    size_t actions = 0;
    float bpm = 0, offset = 0;
    std::string hitsound;
    bool ok = false;
};

Snapshot snap(const LevelData& lv) {
    Snapshot s;
    s.tiles = lv.tileCount();
    s.actions = lv.actions.size();
    s.bpm = lv.settings.bpm;
    s.offset = lv.settings.offset;
    s.hitsound = lv.settings.hitsound;
    s.ok = true;
    return s;
}

bool same(const Snapshot& a, const Snapshot& b) {
    return a.tiles == b.tiles && a.actions == b.actions && a.bpm == b.bpm && a.offset == b.offset
        && a.hitsound == b.hitsound;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : std::string("tests/charts/angles360.adofai");
    const auto bytes = readFile(path);
    if (bytes.empty()) {
        std::printf("SKIP: 读不到 %s\n", path.c_str());
        return 0;
    }

    // ---- ① 编译期：不可拷贝、可移动 ----
    static_assert(!std::is_copy_constructible_v<LevelData>, "LevelData 必须不可拷贝（缓存所有权）");
    static_assert(!std::is_copy_assignable_v<LevelData>, "LevelData 必须不可拷贝赋值");
    static_assert(std::is_move_constructible_v<LevelData>, "LevelData 必须可移动（测试/容器都要用）");
    static_assert(std::is_move_assignable_v<LevelData>, "LevelData 必须可移动赋值");
    check(true, "编译期：LevelData 不可拷贝、可移动（①所有权）");

    // ---- ② 幂等：同一个对象加载两次 == 新建一个加载一次 ----
    LevelData fresh;
    check(load(bytes, fresh), "首次加载（新建对象）");
    const Snapshot one = snap(fresh);

    LevelData reuse;
    check(load(bytes, reuse), "同一个对象第一次加载");
    check(load(bytes, reuse), "同一个对象**第二次**加载（幂等）");
    const Snapshot twice = snap(reuse);
    std::printf("一次加载: %lld 层 / %zu 事件；两次加载: %lld 层 / %zu 事件\n",
                (long long)one.tiles, one.actions, (long long)twice.tiles, twice.actions);
    check(same(one, twice), "两次加载与一次加载完全一致（actions 没有被追加）");

    // settings 是"调用方给的默认值"的载体：加载前设进去、加载后必须还在（文件里没有的字段）
    {
        LevelData keep;
        keep.settings.hitsound = "Snare";       // 文件里通常有 hitsound，这条只验证不被清空
        keep.settings.beatsAhead = 7.5f;        // 文件里没有 → 必须沿用
        check(load(bytes, keep), "预置 settings 后加载");
        check(keep.settings.beatsAhead == 7.5f, "① 的例外：settings 里「文件没写的字段」沿用预置值");
    }

    // ---- ① 移动：源对象被置空，接管方析构安全（能跑完不 double free 就是这条的验收）----
    {
        LevelData src;
        check(load(bytes, src), "移动源对象加载");
        const Snapshot before = snap(src);
        LevelData dst = std::move(src);         // 默认移动：缓存指针交接、源置空
        check(same(snap(dst), before), "移动后目标对象内容完整");
        check(src.tileCount() == 0 && src.actions.empty(), "移动后源对象被置空（不会二次 delete）");
        LevelData back;
        back = std::move(dst);                  // 移动赋值同样要安全
        check(same(snap(back), before), "移动赋值后内容完整");
    }

    // ---- ③ 释放角度数组（紧凑存储）之后仍然能建 checkpoint ----
    {
        LevelData lv;
        check(load(bytes, lv), "checkpoint 用例：加载");
        Timeline tl;
        tl.build(lv);                           // void（错误自己 LOG_E）
        check(tl.tileCount() > 0, "checkpoint 用例：Timeline 建起来了");
        std::string err;
        lv.releaseAngleData();                  // 此后 `angleData` 是空的，只剩紧凑存储
        const bool ok = lv.buildFloorWindowCache(tl.tileStartTimes(), tl.tileBPMs(),
                                                 tl.tileIsCW(), &err);
        check(ok, "releaseAngleData 之后 buildFloorWindowCache 仍然成功（③ 走 angleAt）");
    }

    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
