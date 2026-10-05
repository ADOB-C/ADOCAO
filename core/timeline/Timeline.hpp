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
    const std::string& forceHitsoundType() const { return m_forceHitsoundType; }

    const LevelData* level() const { return m_level; }

    // Precalculated timing arrays (size = n tiles including extra)
    const std::vector<double>& tileStartTimes() const { return m_tileStartTimes; }
    const std::vector<float>& tileDurations() const { return m_tileDurations; }
    const std::vector<double>& tileDisappearTimes() const { return m_tileDisappearTimes; }
    const std::vector<double>& tileAppearTimes() const { return m_tileAppearTimes; }
    const std::vector<float>& tileTotalAngles() const { return m_tileTotalAngles; }
    const std::vector<float>& tileStartAngles() const { return m_tileStartAngles; }
    const std::vector<float>& tileBPMs() const { return m_tileBPM; }
    const std::vector<bool>& tileIsCW() const { return m_tileIsCW; }
    const std::vector<float>& tileStartDist() const { return m_tileStartDist; }
    const std::vector<float>& tileEndDist() const { return m_tileEndDist; }

    float preRoll() const { return m_preRoll; }
    float audioStartOffset() const { return m_audioStartOffset; }

    int findTileIndex(double timeInLevel) const;
    double totalDuration() const;

    // audio-as-chart 直通：把"每层音量"当 PCM 采样值导出（1 层 = 1 采样，采样率 = bpm/60）。
    // 映射与 Song.adofai 的编码严格互逆：编码是 volume = int16 / 655.36 → 采样 = volume * 655.36。
    // 返回 false = 这张谱不适合直通（没有逐层音量 / BPM 不恒定），调用方应回退到 hitsound 混音。
    bool buildRawPcm(std::vector<float>& out, double& sampleRateOut) const;

    std::vector<double> getHitsoundTimestamps() const;
    std::vector<HitsoundTimestampGroup> getHitsoundTimestampGroups() const;

private:
    const LevelData* m_level = nullptr;
    bool m_exportOnly = false;
    std::string m_forceHitsoundType;

    std::vector<double> m_tileStartTimes;
    std::vector<double> m_tileDisappearTimes;
    std::vector<double> m_tileAppearTimes;
    std::vector<float> m_tileDurations;
    std::vector<float> m_tileTotalAngles;
    std::vector<float> m_tileStartAngles;
    std::vector<float> m_tileBPM;
    std::vector<bool>  m_tileIsCW;
    std::vector<float> m_tileStartDist;
    std::vector<float> m_tileEndDist;

    float m_preRoll = 0.0f;
    float m_audioStartOffset = 0.0f;

    void precalculateTiming();
};

}  // namespace adofai
