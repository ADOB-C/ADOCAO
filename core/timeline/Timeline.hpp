#pragma once

#include "core/level/LevelData.hpp"
#include "core/timeline/HitsoundTimestampGroup.hpp"
#include <string>
#include <vector>

// `--force-hitsound raw-pcm`：命中它时不做 hitsound 混音，直接把逐层音量当 PCM 播。

namespace adofai {

inline bool isRawPcmType(const std::string& t) {
    if (t.size() != 7) return false;
    const char* k = "raw-pcm";
    for (size_t i = 0; i < 7; i++) {
        char c = t[i]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != k[i]) return false;
    }
    return true;
}

// Pure timeline data + precalculation for an .adofai level. This type has no
// GL/audio/window dependencies and is the part that may be exported/reused by
// other engines (ADOCAV etc).
class Timeline {
public:
    Timeline() = default;

    void build(const LevelData& level, bool exportOnly = false);
    void setForceHitsoundType(const std::string& type) { m_forceHitsoundType = type; }

    const LevelData* level() const { return m_level; }

    // Precalculated timing arrays (size = n tiles including extra)
    const std::vector<double>& tileStartTimes() const { return m_tileStartTimes; }
    float durationAt(int64_t i) const;
    float startAngleAt(int64_t i) const;
    float totalAngleAt(int64_t i) const;

    // A/B 参考实现（和 TileGeometryReference / parseLegacy 同一套路）：把 Phase 2 的三个数组
    // **用同一批输入、同一套算式、原样再算一遍**，写进调用方给的三个向量。
    // 用途只有一个：量出"换个代码位置重算"会把值改变多少（浮点的位模式依赖编译上下文，
    // 见 docs/scale-1e8-to-2e9.md §6.1 / TODO 的 2^31 任务 ②(b)）。它**不参与生产路径**。
    void recomputeForTest(std::vector<float>& outStartAngles,
                          std::vector<float>& outTotalAngles,
                          std::vector<float>& outDurations) const;
    // ③ 切片 1：**每 4096 层一个锚点**。目标是"按块重算"替代 3 支 double 常驻
    // （MYC 原版实测：3×8 B/层 = 162 MB，占 live 530 MB 的 31%）。
    // 本切片只**记录**锚点并做"锚点 == 全量数组在该层的值"的逐位对拍，
    // 不改任何消费方（时钟/PositionSolver/TrackVis 仍读全量数组）。
    struct Anchor {
        int64_t floor = 0;
        double startTime = 0.0;        // == m_tileStartTimes[floor]
        float  bpm = 0.0f;             // == m_tileBPM[floor]
        bool   cw = false;             // == m_tileIsCW[floor]
        double disappearTime = 0.0;    // 仅 needsTrackVis 时有意义
        double appearTime = 0.0;
        bool   hasTrackVis = false;
    };
    static constexpr int64_t kAnchorStride = 4096;   // 与 ② 分块 / ④ 块索引同粒度
    const std::vector<Anchor>& anchors() const { return m_anchors; }
    // ③ 切片 2：**按块重算**。给块号与块内 [i0,i1)，用"锚点 + 每层时长（float）+ 每层 BPM +
    // TrackVis 恒定态"重算出该块的三支 double（若 needsTrackVis 则含 disappear/appear）。
    // 逐位口径与 precalculateTiming 相同：duration 转 double 后按层序累加（同一顺序、同一操作）。
    struct BlockResult {
        std::vector<double> startTimes;     // [i0, i1]（多一层：TrackVis 要用 start[i+1]）
        std::vector<double> disappearTimes; // 空 = 这张谱不需要 / 不可重算 TrackVis
        std::vector<double> appearTimes;
    };
    // 能否按块重算：需要 TrackVis 时，只有**块内没有 AnimateTrack 事件**才严格等价
    // （FastAction 没留 trackAnimation 的值，见 LevelData.cpp 的 AnimateTrack 组装）。
    bool blocksRecomputable() const { return m_blocksRecomputable; }
    const char* blocksRecomputableWhy() const { return m_blocksWhy; }
    bool recomputeBlock(int64_t i0, int64_t i1, BlockResult& out) const;
    // ③ 切片 3：TrackVis 两支的**窗口化访问**。粗索引（每 kAnchorStride 层一份，26 KB）
    // 供消费方的二分查找用；逐层值由"物化所在块"给出（单块缓存）。
    bool  hasTrackVisTimes() const { return m_needsTrackVis; }
    bool  trackVisWindowed() const { return m_visWindowed; }   // 密集两支已丢
    // 粗索引是否单调（消费方的二分沿用原实现的前提；不单调就不敢窗口化）
    bool  trackVisCoarseMonotone() const { return m_visCoarseMonotone; }
    int64_t tileCount() const { return (int64_t)m_tileBPM.size(); }
    const std::vector<double>& coarseDisappearTimes() const { return m_coarseDA; }
    // 逐层 TrackVis 时间（未窗口化时直接读密集数组；窗口化时物化所在块）
    const BlockResult* windowFor(int64_t i) const;   // 单块缓存（物化 i 所在的块）
    // 起始时间：未窗口化时直读密集数组，窗口化时物化所在块（单块缓存）
    double startTimeAt(int64_t i) const;
    // 丢掉 `m_tileStartTimes`（只依赖"锚点 + 每层时长"，**与 TrackVis 能否重算无关**，
    // 所以有 AnimateTrack 的谱也能省这 54 MB）。导出路径也走 startTimeAt()。
    bool dropStartTimesArray();
    double disappearTimeAt(int64_t i) const;
    double appearTimeAt(int64_t i) const;
    // 丢掉两支密集 double（只有 blocksRecomputable() 时才允许；丢之前先建粗索引）
    bool dropTrackVisArrays();
    // **仅测试用**：给"按块重算"做反面对照 —— 直接把某一层的 BPM 改掉（时长随之变，
    // 1 ULP 也必须被抓到）。以前这里改的是 `m_durations`，那个数组 2026-10-16 已删除
    // （时长由 `durationAt(i)` 现算，见下）。
    void setBpmForTest(int64_t i, float v) {
        if (i >= 0 && (size_t)i < m_tileBPM.size()) m_tileBPM[(size_t)i] = v;
    }
    const std::vector<double>& tileDisappearTimes() const { return m_tileDisappearTimes; }
    const std::vector<double>& tileAppearTimes() const { return m_tileAppearTimes; }
    const std::vector<float>& tileBPMs() const { return m_tileBPM; }
    const std::vector<bool>& tileIsCW() const { return m_tileIsCW; }
    // 每层距离：0..n-2 上恒为 1.0f，只有**最后一层**可能是实测距离（Timeline.cpp 的 Phase 4）。
    // 所以不存整条数组（那要 8 B/层 —— 2^31 层就是 17 GB），只存最后一层的标量 + 层数。
    float tileStartDist(size_t i) const {
        return (i + 1 == m_distCount) ? m_lastStartDist : 1.0f;
    }
    float tileEndDist(size_t i) const {
        return (i + 1 == m_distCount) ? m_lastEndDist : 1.0f;
    }

    // 第 0 层的起始时间（= 倒计时起点，通常为负）。**别在逐采样路径里调
    // `startTimeAt(0)`**：那是单块缓存，会把当前块的缓存撕回 block 0（The Moon 实测
    // 每次多两次 4096 层重算 → 拖尾采样 128µs/点、1 秒/帧）。这里存一个标量。
    double firstStartTime() const { return m_firstStartTime; }

    float preRoll() const { return m_preRoll; }
    float audioStartOffset() const { return m_audioStartOffset; }

    int findTileIndex(double timeInLevel) const;
    double totalDuration() const;

    // 谱面里的**真实**砖数（排除 `calculateTilePositions()` 追加的合成 extra 砖）。
    // 打拍音/逐层音量都按它来排；直接数 `tileStartTimes().size()` 会多出末尾那一击。
    int realTileCount() const;

    // audio-as-chart 直通：把"每层音量"当 PCM 采样值导出（1 层 = 1 采样，采样率 = bpm/60）。
    // 映射与 Song.adofai 的编码严格互逆：编码是 volume = int16 / 655.36 → 采样 = volume * 655.36。
    // 返回 false = 这张谱不适合直通（没有逐层音量 / BPM 不恒定），调用方应回退到 hitsound 混音。
    bool buildRawPcm(std::vector<float>& out, double& sampleRateOut) const;

    std::vector<double> getHitsoundTimestamps() const;
    // 按 (音色, 音量) 分组的时间戳。**必须在 `LevelData::releaseMemory()` 之前调**：
    // 那之后每砖覆盖/音量已被释放，函数会 LOG_E 并返回空表（不再静默给错值）。
    std::vector<HitsoundTimestampGroup> getHitsoundTimestampGroups() const;

private:
    const LevelData* m_level = nullptr;
    bool m_exportOnly = false;
    std::string m_forceHitsoundType;

    std::vector<double> m_tileStartTimes;
    std::vector<double> m_tileDisappearTimes;
    std::vector<double> m_tileAppearTimes;
    std::vector<std::pair<int64_t, float>> m_pauseRot;
    float angleDirBefore(int64_t i) const;
    float extraRotAt(int64_t i) const;
    float m_lastTotalAngle = 0.0f;
    std::vector<float> m_tileBPM;
    std::vector<bool>  m_tileIsCW;
    std::vector<Anchor> m_anchors;   // ③ 切片 1：见 anchors()
    // ③ 切片 2 起时长是"按块重算"的前缀来源；2026-10-16 起**不再存**（27 MB）：`durationAt(i)`
    // 由 `angleAt(i)` + `m_tileBPM[i]` + `m_tileIsCW[i]` 现算，口径与 `precalculateTiming` 逐字相同。
    bool   m_blocksRecomputable = false;
    const char* m_blocksWhy = "尚未记录";
    // TrackVis 恒定态（无 AnimateTrack 事件时全程不变）
    std::string m_visDA, m_visAA;
    float  m_visBB = 0.0f, m_visBA = 0.0f;
    float  m_visNum5 = 0.0f;         // = bpm[0]（i==0 时的 num5/num6 初值）
    bool   m_needsTrackVis = false;
    // 切片 3
    bool   m_startWindowed = false;
    bool   m_visWindowed = false;
    bool   m_visCoarseMonotone = false;
    std::vector<double> m_coarseDA, m_coarseAA;   // 每锚点一份
    mutable int64_t m_winBlock = -1;              // 单块缓存（物化过的块号）
    mutable BlockResult m_winCache;
    size_t m_distCount = 0;          // 有效层数（= n）；配合下面两个标量实现 tileStartDist/tileEndDist
    float  m_lastStartDist = 1.0f;   // 只有最后一层用得上（其余恒 1.0f）
    float  m_lastEndDist   = 1.0f;

    double m_firstStartTime = 0.0;
    float m_preRoll = 0.0f;
    float m_audioStartOffset = 0.0f;

    void precalculateTiming();
};

}  // namespace adofai
