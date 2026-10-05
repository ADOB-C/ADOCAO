#pragma once

#include <string>
#include <vector>
#include <functional>

#include "core/timeline/HitsoundTimestampGroup.hpp"


namespace adofai {

using HitsoundProgressCb = std::function<void(float percent)>;

class HitsoundManager {
public:
    HitsoundManager();
    ~HitsoundManager();

    HitsoundManager(const HitsoundManager&) = delete;
    HitsoundManager& operator=(const HitsoundManager&) = delete;

    void init(const std::string& assetsDir = "");

    // hitsounds 的相对目录（产品设一次，例如 "assets/hitsounds"）。不设 = 相对当前目录。
    static void setDefaultHitsoundSubdir(const std::string& subdir);

    void setHitsoundType(const std::string& type);
    void setVolume(float vol);  // 0-100
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }




    // Hits actually mixed by the last preSynthesize() call.
    int lastMixedHits() const { return m_lastMixedHits; }

    bool preSynthesize(const std::vector<HitsoundTimestampGroup>& groups, float totalDuration,
                       HitsoundProgressCb onProgress = nullptr);

    // raw-PCM 直通（audio-as-chart）：不做 hitsound 混音，把逐采样值线性重采样到设备采样率
    // 并复制成双声道。sampleRate 来自谱面（bpm/60），通常 44.1k / 48k / 96k / 192k。
    bool preSynthesizeRawPcm(const std::vector<float>& samples, double sourceRate,
                             HitsoundProgressCb onProgress = nullptr);

    // Read-only access for mixer
    const float* buffer() const { return m_buffer.data(); }
    size_t totalFrames() const { return m_buffer.size() / 2; }
    int channels() const { return 2; }
    int sampleRate() const { return m_sampleRate; }
    size_t* cursor() { return &m_readCursor; }
    bool* playing() { return &m_playing; }

    void reset();
    void resetAt(float audioPosSec);  // seek cursor to position
    void stop();
    bool isSynthesized() const { return m_synthesized; }
    bool writeWav(const std::string& filepath);  // export pre-mixed buffer to WAV

private:
    std::string m_assetsDir;

    std::vector<float> m_buffer;
    int m_sampleRate = 44100;
    size_t m_readCursor = 0;

    std::string m_hitsoundType = "Kick";
    float m_volume = 1.0f;
    bool m_enabled = true;
    bool m_synthesized = false;
    bool m_playing = false;
    int m_lastMixedHits = 0;

    std::string hitsoundPath(const std::string& type) const;
    bool readWav(const std::string& filepath,
                 std::vector<float>& samples,
                 int& sampleRate, int& channels);
};

}  // namespace adofai
