#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <rapidjson/document.h>

// Parsed .adofai level file
// 驻留实现：互斥量与 thread-local 缓存都在 .cpp 文件作用域，按驻留表地址做缓存键；
// LevelData 里因此没有 mutex / thread_local 数据成员（保持可拷贝，MSVC 也接受）。

namespace adofai {

uint16_t internActionStrIn(std::vector<std::string>& table, const std::string& v);

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

    struct TilePositionOffset {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        bool  justThisTile = false;
    };

    // Per-tile event data (computed from actions)
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

    void releaseMemory();  // free data no longer needed after loading

    using ProgressCb = std::function<void(float pct, const char* stage)>;

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
    void processActions();
    void applyPositionTrackOffsets();
    static float pathCharToAngle(char c);
};

}  // namespace adofai
