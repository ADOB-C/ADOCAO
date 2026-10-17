#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <rapidjson/document.h>

#include "core/level/AngleStore.hpp"   // ③ 切片 4：angleData 的紧凑存储

// Parsed .adofai level file
// 驻留实现：互斥量与 thread-local 缓存都在 .cpp 文件作用域，按驻留表地址做缓存键；
// LevelData 里因此没有 mutex / thread_local 数据成员（保持可拷贝，MSVC 也接受）。

namespace adofai {

uint16_t internActionStrIn(std::vector<std::string>& table, const std::string& v);

struct LevelData;
class FloorWindowCache;

// 缓存句柄的删除器：`operator()` 定义在 LevelData.cpp（那里 FloorWindowCache 是完整类型）。
// 这样头里可以安全地持有 `unique_ptr<FloorWindowCache, …>` —— 见下面 m_winCache 的注释。
struct WinCacheDeleter { void operator()(FloorWindowCache* p) const; };

struct LevelData {
    struct Settings {
        int    version = 15;
        float  bpm = 100.0f;
        float  offset = 0.0f;        // ms
        int    countdownTicks = 4;
        float  zoom = 100.0f;
        float  rotation = 0.0f;
        std::string relativeTo = "Player";
        std::array<float, 2> position = {0.0f, 0.0f};
        std::string songFilename;
        std::string hitsound = "Kick";
        float  hitsoundVolume = 100.0f;
        std::string trackColor = "debb7b";
        std::string secondaryTrackColor = "ffffff";
        std::string backgroundColor = "000000";
        bool   stickToFloors = true;
        std::string planetEase = "Linear";
        std::string trackDisappearAnimation = "None";
        std::string trackAnimation = "None";
        float  beatsBehind = 4.0f;
        float  beatsAhead  = 3.0f;
        // ... more fields as needed
    };

    // 32 B → 24 B：`index` 从来只被写入（`= i`），没有任何读者 —— 层号就是它在 `tiles` 里的位置。
    struct Tile {
        float angle = 180.0f;
        float direction = 0.0f;
        std::array<double, 2> position = {0.0, 0.0};
    };
    // 内存预算护栏：1e9 层时每 8 B 就是 8 GB。加字段前先问"这个值能不能推导/能不能窗口化"。
    static_assert(sizeof(Tile) == 24, "Tile 膨胀了：先确认这 8 B/层（1e9 层 = 8 GB）是必要的");

    Settings settings;
    std::vector<double> angleData;
    std::string        pathData;       // raw pathData string (alternative to angleData)
    std::vector<Tile>  tiles;
    // Lightweight action (avoids nlohmann DOM allocation for millions of actions)
    // 事件里的字符串（hitsound 名、trackDisappear 动画名）绝大多数是重复的少数几个值，
    // 而又只有这两类事件用到。以前每个 action 内联一个 std::string（32 B）：1.18 GB / 912 万
    // action 的谱面上"结构本身"就是 417 MB，其中 292 MB 是这个缓冲。现在只存 16 位 id，
    // 字符串驻留到 actionStrTable，结构 48 B -> 16 B（417 MB -> 146 MB）。
    struct FastAction {
        int floor = 0;
        uint16_t strId = 0;                 // 0 = 空串；索引进 actionStrTable
        enum Type : uint8_t { Twirl, SetSpeed, PositionTrack, SetHitsound, Bookmark, Pause, AnimateTrack, Other } type = Other;
        bool flag = false;
        float val1 = 0, val2 = 0;
    };
    std::vector<FastAction> actions;
    std::vector<std::string> actionStrTable{std::string()};   // 驻留表（id 0 = 空串）
    // 事件字符串入表：按内容去重。热路径（同一类型重复几百万次）走 thread-local 缓存、不加锁；
    // 未命中才加锁查/插，所以分块并行的 worker 也是安全的。
    uint16_t internActionStr(const std::string& v) { return internActionStrIn(actionStrTable, v); }
    const std::string& actionStr(const FastAction& a) const {
        return a.strId < actionStrTable.size() ? actionStrTable[a.strId] : actionStrTable[0];
    }

    // ③ 切片 4（第 1 步）：**运行期读者不要再直接碰 `tiles[i]`** —— `tiles` 现在会按块物化，
    // 这两个访问器就是那层接口。未窗口化时直读密集数组（行为逐位不变）。
    //
    // ⚠️ **按值**返回（不是引用）：物化下一块会替换窗口缓存，引用会失效；按值返回也顺带
    // 让"同时持有两块里的位置"（`PositionSolver` 就同时读 tileIdx 与第 0 层）天然安全。
    // ⚠️ **不能**只写 `tiles.size()`：丢过密集数组之后它是 0，而全运行期（mesh 装配、capture、
    // jump、最近砖搜索）都拿它当"谱面砖数"。窗口化时由 provider 记着那个数。
    int64_t tileCount() const;
    // 可选 `wc`：并行剔除时传**本线程自己的克隆**（见 `FloorWindowCache::clone()`）；
    // 不传 = 用共享缓存（单线程路径，语义与以前逐位相同）。
    std::array<double, 2> tilePositionAt(int64_t i, FloorWindowCache* wc = nullptr) const;
    float tileDirectionAt(int64_t i, FloorWindowCache* wc = nullptr) const;
    // 本线程的私有窗口克隆（首次调用克隆一次，随线程存活；level 换了会自动重建）
    FloorWindowCache* threadWindowCache() const;
    // ③ 切片 4：运行期读角度一律走 `angleAt()` —— 加载收尾会把 `angleData` 换成**位精确紧凑存储**
    // （字典 + 位打包下标；MYC 实测 45 个取值 → 0.75 B/层，54 MB → 4.8 MB）。
    // 加载期（解析 / Timeline::build / computeCheckpoints / TileMesh::build）仍读原数组。
    double angleAt(int64_t i) const {
        if (m_angleStore.valid()) return m_angleStore.at(i);
        return (i >= 0 && (size_t)i < angleData.size()) ? angleData[(size_t)i] : 180.0;
    }
    int64_t angleCount() const {
        return m_angleStore.valid() ? m_angleStore.size() : (int64_t)angleData.size();
    }
    // 把 `angleData` 换紧凑存储（取值集过大则失败、原数组保留）
    bool releaseAngleData(std::string* err = nullptr);
    bool anglesCompacted() const { return m_angleStore.valid(); }
    size_t angleStoreBytes() const { return m_angleStore.bytes(); }
    int64_t angleStoreUnique() const { return m_angleStore.uniqueCount(); }
    int angleStoreBitsPerIndex() const { return m_angleStore.bitsPerIndex(); }
    void setAngleForTest(int64_t i, double v) { m_angleStore.setForTest(i, v); }

    // 建"按块物化"缓存（加载收尾调用；失败就保持密集数组）
    bool buildFloorWindowCache(const std::vector<double>& startTimes,
                               const std::vector<float>& bpms,
                               const std::vector<bool>& isCW, std::string* err = nullptr);
    bool dropTilesArray();          // 丢掉密集 tiles（成功 = 之后走窗口）
    bool tilesWindowed() const;
    size_t windowBlocksMaterialized() const;
    void invalidateFloorWindowForTest();   // 仅测试用（见 FloorWindowCache::invalidateWindowForTest）

    struct TilePositionOffset {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        // 官方语义（对照 Re_ADOJAS 的移植确认）：false = 偏移**继承**到之后所有层；
        // true = **只作用于本层**。消费方见 applyPositionTrackOffsets()。
        bool  justThisTile = false;
    };

    // Per-tile event data (computed from actions)
    //
    // ⚠️ **存活契约**（2026-10-17 补，之前只散在 app/GameWindow.cpp 的注释里）：
    //   `TileMesh` 在**绘制期**（而不是只在 build 期）读这三张表 —— 每砖图标位是剔除时按窗口
    //   现派生的（`TileMesh::deriveRange`），而 provider 合成 SetSpeed 事件也要 `tileBPMs`
    //   （`FloorWindow::computeFloorWindow`）。所以：
    //     * `releaseMemory()` **必须留着它们**（4 B/层 + 2 bit/层），谁清谁负责；
    //     * 窗口化（`dropTilesArray()`）之后 `tileHasTwirl/tileHasSetSpeed` **绝对不能清**：
    //       清了就会派生出 icon=0（实测块 0 有 2148 处不一致、出图 858 个边缘散点）；
    //     * 用 `refreshDerivedCaches()` 重建，别手搓。
    std::vector<float> tileBPMs;      // BPM for each tile (after SetSpeed events)
    std::vector<bool>  tileHasTwirl;  // true if tile has a Twirl event
    std::vector<bool>  tileHasSetSpeed; // true if tile has a SetSpeed event
    std::unordered_map<int, std::string> tileHitsounds;      // per-tile hitsound override (sparse)
    // per-tile hitsound volume override. 稠密：有覆盖时长度 = 层数，"无覆盖"用 NaN 表示；
    // 整张谱没有覆盖时为空 vector。以前是 unordered_map<int,float>：在"每层都有 SetHitsound"
    // 的 audio-as-chart 谱上（实测 TNR 1.18 GB / 915 万层）有 457 万条 ≈ 209 MB，
    // 稠密后只要 37 MB。
    std::vector<float> tileHitsoundVolumes;
    bool hasHitsoundVolume(int floor) const {
        return floor >= 0 && (size_t)floor < tileHitsoundVolumes.size()
               && !std::isnan(tileHitsoundVolumes[(size_t)floor]);
    }
    std::unordered_map<int, TilePositionOffset> tilePositionOffsets; // sparse
    std::vector<int> bookmarkFloors;  // Bookmark event floors

    // AnimateTrack state overrides (sparse, floor → state)
    struct ATState { std::string da, aa; float bb=4, ba=3; bool hasAA=false; };
    std::unordered_map<int, ATState> atStates;
    // 粘性标志：releaseMemory() 会清掉 atStates 的实体（每条 101 B，巨谱上是几十 MB~几百 MB），
    // 但 Timeline 与 LevelScene 都要按"有没有 AnimateTrack"来判定 TrackVis —— 那个判定发生在
    // 释放之后，所以不能再看 .empty()。三处判据（Timeline.cpp、app/LevelScene.cpp、
    // tests/level_parse_test.cpp 的自检）必须一起用它。
    bool hasAtStates = false;
    // 粘性标志（同 hasAtStates 的理由）：`releaseMemory()` 会清掉每砖 hitsound 覆盖与音量
    // （它们只服务加载期的打拍音合成）。清完之后 `Timeline::getHitsoundTimestampGroups()`
    // 再算就是**静默错值**（类型退化成 settings.hitsound、音量退化成 settings.hitsoundVolume），
    // 所以用这个标志让它**明确报错**而不是给一份看起来正常的错表。
    bool hitsoundCachesReleased = false;

    // ---- 所有权与拷贝/移动（2026-10-17 补）----
    // 这里原来只有析构、没有禁拷贝：`LevelData a; LevelData b = a;` 会让两个对象各持同一个
    // `m_winCache`，析构时 **double free**（缓存自己删了拷贝，但裸指针绕过了那层保护）。
    // 现在缓存是 move-only 的 unique_ptr（自定义删除器，见下），于是拷贝自动禁用、移动安全。
    LevelData() = default;
    ~LevelData();   // 定义在 LevelData.cpp（FloorWindowCache 在那里才是完整类型）
    LevelData(const LevelData&) = delete;
    LevelData& operator=(const LevelData&) = delete;
    LevelData(LevelData&&) = default;              // unique_ptr 的 move 会把源置空 ✓
    LevelData& operator=(LevelData&&) = default;
    void releaseMemory();  // free data no longer needed after loading
    // ③ 切片 4：按块物化缓存（未建/未窗口化时为 null，访问器走密集数组）。
    // 用 unique_ptr + **自定义删除器**（`WinCacheDeleter::operator()` 在 .cpp 里定义）：
    // libc++ 里 `unique_ptr<不完整类型>` 的默认删除器只要被本 TU 构造/析构就会实例化
    // "delete an incomplete type"（原来因此写成裸指针），而 `LevelData.hpp` 不能包含
    // `FloorWindow.hpp`（后者要 LevelData 的嵌套类型）。自定义删除器的 `operator()`
    // 只声明不定义，就没有那个实例化问题 —— 同时白拿 move-only 语义。
    std::unique_ptr<FloorWindowCache, WinCacheDeleter> m_winCache;
    AngleStore m_angleStore;   // ③ 切片 4：`angleData` 的紧凑存储（releaseAngleData 之后生效）

    using ProgressCb = std::function<void(float pct, const char* stage)>;

    // ---- 派生病重与写回（ADOCAO-E 编辑器要用的公开入口；2026-10-17）----
    // 幂等地从 `actions` 重建**全部每砖派生缓存**：tileBPMs / tileHasTwirl / tileHasSetSpeed /
    // tileHitsounds / tileHitsoundVolumes / tilePositionOffsets / bookmarkFloors / atStates。
    // 每次调用都先清空再重建，所以可以反复调（加载收尾就是调它一次）；**不碰 `tiles`**。
    // 层数取自 `angleCount()` → `releaseAngleData()` 之后也能用。
    void refreshDerivedCaches();

    // 角度写回。`releaseAngleData()` 之后角度只剩位打包下标（AngleStore 只允许改成字典里
    // **已有**的取值），所以编辑任意角度前要先 `expandAngles()` 回到密集数组。
    // 改完谁改了谁负责重建：`refreshDerivedCaches()`（事件派生）+ `rebuildPositions()`（几何）。
    bool expandAngles();                          // 紧凑 → 密集；已是密集时无操作
    bool setAngles(std::vector<double> angles);   // 整体替换（顺带展开）
    bool setAngle(int64_t i, double v);           // 单点替换（需要时自动展开）
    // 从当前 `angleData` 重建 `tiles` 的位置/方向 + PositionTrack 偏移。
    // 窗口化（`dropTilesArray()` 之后）不可用 —— 那时几何只能由容器窗口重算。
    bool rebuildPositions(std::string* err = nullptr);

    bool loadFromFile(const std::string& filepath, ProgressCb onProgress = nullptr, bool exportOnly = false);
    bool loadFromString(const std::string& jsonStr, ProgressCb onProgress = nullptr, bool exportOnly = false);
    // Same thing on a raw buffer (used by loadFromFile's mmap and by tests)
    bool loadFromBuffer(const char* data, size_t length, ProgressCb onProgress = nullptr, bool exportOnly = false);

private:
    // Fast path: scan angleData/actions straight out of the buffer, reproducing what
    // cleanJson() does to the file. Returns false (and leaves the object untouched) as
    // soon as it meets something it cannot reproduce, so the caller can fall back to
    // parseLegacy(). Set ADOCAO_FORCE_DOM_PARSE=1 to force the legacy path (A/B tests).
    bool tryFastParse(const char* data, size_t length, ProgressCb onProgress);
    // Legacy path: cleanJson + RapidJSON DOM. Also the fallback and the A/B reference.
    bool parseLegacy(const std::string& cleanedJson, ProgressCb onProgress, bool exportOnly);
    // Shared tail: pathData → angleData, tile positions, actions, position offsets
    bool finishLoad(ProgressCb onProgress, bool exportOnly);

    void calculateTilePositions();
    void convertPathToAngles();
    void applyPositionTrackOffsets();
    static float pathCharToAngle(char c);
};

}  // namespace adofai
