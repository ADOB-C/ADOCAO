// 关卡对象**所有权 / 幂等**的回归用例 —— 对应 2026-10-17 外部评审的三条真问题：
//
//   ① `LevelData` 有自定义析构（删 `m_winCache`）却没有禁拷贝 → `LevelData b = a;` 会让两个
//      对象各持同一个缓存，析构 double free。现在拷贝被禁用、移动安全（缓存是 move-only 的
//      unique_ptr + 自定义删除器）。这一条**编译期**就守住：下面两个 static_assert。
//   ② `loadFromBuffer` 不幂等：同一个对象连调两次，`actions` 会被追加（两条解析路径都是
//      `if (actions.empty()) … else insert(...)`）而 `angleData` 是替换 → "角度新的、事件两份"。
//      现在开头回到"刚构造"的状态（`settings` 例外，它是"文件没写的字段沿用调用方给的默认值"）。
//   ③ `computeCheckpoints` 直接读 `level.angleData[i]`：`releaseAngleData()` 之后是空数组 → UB。
//
// 另外这一版还钉住 **ADOCAO-E（编辑器）要的公开能力**（2026-10-17）：
//   * `refreshDerivedCaches()` 幂等（连调两次结果相同，且不碰 tiles）；
//   * `expandAngles()`/`setAngle()`/`setAngles()` + `rebuildPositions()`（压缩之后还能改角度）；
//   * `Timeline::realTileCount()` 在压缩之后仍然少一块（走 angleCount()）；
//   * `Settings::songFilename` 从 `"song"` 解析出来；
//   * `releaseMemory()` 之后 `getHitsoundTimestampGroups()` **明确失败**（空表）而不是静默错值。
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

    // ---- ADOCAO-E：幂等的派生缓存重建（不碰 tiles）----
    {
        LevelData lv;
        check(load(bytes, lv), "派生缓存用例：加载");
        const auto tilesBefore = lv.tileCount();
        const auto actionsBefore = lv.actions.size();
        const size_t bpm0 = lv.tileBPMs.size();
        lv.refreshDerivedCaches();
        lv.refreshDerivedCaches();      // 第二次必须与第一次等价（幂等）
        check(lv.tileCount() == tilesBefore, "refreshDerivedCaches 不碰 tiles（层数不变）");
        check(lv.actions.size() == actionsBefore, "refreshDerivedCaches 不动 actions");
        check(lv.tileBPMs.size() == bpm0, "两次重建的 tileBPMs 大小一致（没有叠加）");
        // 稀疏表也不许留下上次的条目：bookmarkFloors 末尾固定补 0 与 n-1，数量应当稳定
        const size_t bm = lv.bookmarkFloors.size();
        lv.refreshDerivedCaches();
        check(lv.bookmarkFloors.size() == bm, "bookmarkFloors 幂等（稀疏表被清过再重建）");
    }

    // ---- ADOCAO-E：压缩之后仍然能改角度并重建几何 ----
    {
        LevelData lv;
        check(load(bytes, lv), "角度写回用例：加载");
        const int64_t n = lv.angleCount();
        const double a10 = lv.angleAt(10);
        check(lv.releaseAngleData(), "releaseAngleData 成功（角度换成位打包存储）");
        check(lv.anglesCompacted(), "确实压缩了");
        check(lv.angleCount() == n, "压缩后 angleCount() 仍报真实层数");
        check(lv.expandAngles() && !lv.anglesCompacted(), "expandAngles() 展开回密集数组");
        check(lv.angleAt(10) == a10, "展开后逐位不变");
        check(lv.setAngle(10, a10 + 90.0) && lv.angleAt(10) == a10 + 90.0, "setAngle 单点写回");
        std::vector<double> all((size_t)n, 45.0);
        check(lv.setAngles(std::move(all)) && lv.angleAt(0) == 45.0 && lv.angleAt(n - 1) == 45.0,
              "setAngles 整体替换");
        std::string err;
        check(lv.rebuildPositions(&err), "rebuildPositions 从当前 angleData 重建几何");
        check(lv.tileCount() == n + 1, "重建后层数 = 角度数 + 末尾那块合成砖");
        check(lv.rebuildPositions(&err), "rebuildPositions 可以再调一次（幂等）");
    }

    // ---- ADOCAO-E：realTileCount 在压缩之后仍然要少那一块合成砖 ----
    {
        LevelData lv;
        check(load(bytes, lv), "realTileCount 用例：加载");
        Timeline tl;
        tl.build(lv);
        const int before = tl.realTileCount();
        lv.releaseAngleData();
        Timeline tl2;
        tl2.build(lv);
        std::printf("realTileCount：压缩前 %d，压缩后 %d（tileCount=%lld）\n", before,
                    tl2.realTileCount(), (long long)tl2.tileCount());
        check(before == tl2.realTileCount(), "realTileCount 压缩前后一致（不会多算末尾合成砖）");
        check((int64_t)tl2.realTileCount() == lv.angleCount(),
              "realTileCount == angleCount()（正是 ③ 修的那条）");
    }

    // ---- ADOCAO-E：songFilename 从 `"song"` 解析 ----
    {
        // 真实 .adofai（v12+）把 bpm/song 放在**嵌套的 `"settings"` 对象**里
        std::string text = R"({"settings":{"song":"my song.ogg","bpm":120,"offset":0},)"
                           R"("angleData":[0,90,180,270]})";
        LevelData lv;
        check(lv.loadFromString(text), "songFilename 用例：加载最小谱");
        check(lv.settings.songFilename == "my song.ogg", "Settings.songFilename 解析自 settings.song");
    }

    // ---- ADOCAO-E：释放之后 hitsound 分组必须**明确失败**，不能静默错值 ----
    {
        LevelData lv;
        check(load(bytes, lv), "hitsound 分组用例：加载");
        Timeline tl;
        tl.build(lv);
        const size_t groupsBefore = tl.getHitsoundTimestampGroups().size();
        lv.releaseMemory();
        check(lv.hitsoundCachesReleased, "releaseMemory 置了 hitsoundCachesReleased 粘性标志");
        check(tl.getHitsoundTimestampGroups().empty(),
              "释放之后 getHitsoundTimestampGroups() 返回空表（配合日志 = 明确失败）");
        check(groupsBefore > 0, "释放之前本来是有分组的（所以上面那条不是空对空）");
    }

    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
