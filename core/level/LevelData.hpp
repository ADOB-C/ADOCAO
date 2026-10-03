#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <rapidjson/document.h>

// Parsed .adofai level file
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

    struct Tile {
        int   index = 0;
        float angle = 180.0f;
        float direction = 0.0f;
        std::array<double, 2> position = {0.0, 0.0};
    };

    Settings settings;
    std::vector<double> angleData;
    std::string        pathData;       // raw pathData string (alternative to angleData)
    std::vector<Tile>  tiles;
    // Lightweight action (avoids nlohmann DOM allocation for millions of actions)
    struct FastAction {
        int floor = 0;
        enum Type : uint8_t { Twirl, SetSpeed, PositionTrack, SetHitsound, Bookmark, Pause, AnimateTrack, Other } type = Other;
        float val1 = 0, val2 = 0;
        bool flag = false;
        std::string str;
    };
    std::vector<FastAction> actions;

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
