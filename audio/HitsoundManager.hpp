#pragma once

#include <string>
#include <vector>
#include <functional>

#include "core/timeline/HitsoundTimestampGroup.hpp"

using HitsoundProgressCb = std::function<void(float percent)>;

class HitsoundManager {
public:
    HitsoundManager();
    ~HitsoundManager();

    HitsoundManager(const HitsoundManager&) = delete;
    HitsoundManager& operator=(const HitsoundManager&) = delete;

    void init(const std::string& assetsDir = "");

    void setHitsoundType(const std::string& type);
    void setVolume(float vol);  // 0-100
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Optional Nyquist-style de-duplication: drop hits landing within minGapSec of
    // the previously kept hit (ADOFAI_HitSound filters at 1/(sr/2) = 41.7 us, i.e.
    // hits less than two samples apart at 48 kHz). Off by default.
    //
    // Measured: this is NOT transparent. On Tempest (3.15% of hits dropped) 92.8%
    // of output samples change, max |diff| 1.4x full scale; on a 6.77M-hit chart
    // (91.5% dropped) 95.0% change and the peak-normalised RMS rises 6.5 dB. The
    // dropped hits are summed energy, not noise.
    void setNyquistDedup(bool enabled, double minGapSec = 1.0 / 24000.0);

    // Mix semantics. Default (false) = float accumulation + one final gain, which
    // is what ADOFAI_HitSound does. true = the legacy int16 accumulation with a
    // clamp on every addition, bit-exact with the pre-2026-10 ADOCAO code and with
    // HitSoundGenerator.exe (hard-clips 0.33% of samples on a dense chart, but that
    // saturation is the authentic sound). Both paths are parallelised identically.
    void setHardClipMix(bool on) { m_hardClipMix = on; }

    // Soft-limiter drive applied after peak normalisation (tanh(drive * x)).
    // Peak normalisation alone cannot control loudness — any gain is divided back
    // out — so a peaky impulse mix stays quiet (RMS -16 dBFS on Tempest, while the
    // old hard-clipped mix measured -7 dBFS). 1.0 disables the limiter; ~4 lands
    // at the old loudness with DR 9.1 dB instead of 5.96 dB and no hard clipping.
    // Ignored by the hard-clip path, which is loud by construction.
    void setLimiterDrive(double drive) { m_limiterDrive = drive > 1.0 ? drive : 1.0; }

    // Hits actually mixed by the last preSynthesize() call.
    int lastMixedHits() const { return m_lastMixedHits; }

    bool preSynthesize(const std::vector<HitsoundTimestampGroup>& groups, float totalDuration,
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
    bool m_nyquistDedup = false;
    double m_nyquistGap = 1.0 / 24000.0;
    double m_limiterDrive = 1.0;
    bool m_hardClipMix = false;
    int m_lastMixedHits = 0;

    std::string hitsoundPath(const std::string& type) const;
    bool readWav(const std::string& filepath,
                 std::vector<float>& samples,
                 int& sampleRate, int& channels);
};
