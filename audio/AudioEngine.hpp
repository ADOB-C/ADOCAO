#pragma once

#include <string>
#include <vector>
#include <cstdint>


// miniaudio 的类型属于**全局**命名空间：前置声明必须在 adofai 之外，
// 否则声明出来的是 adofai::ma_device，跟真类型成了两个东西（编译期 incomplete type）。
struct ma_device;
struct ma_decoder;

namespace adofai {

constexpr int AUDIO_SAMPLE_RATE = 48000;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();
    void shutdown();

    bool loadMusic(const std::string& filepath);
    void play();
    void pause();
    void seek(float seconds);

    float position() const;
    float duration() const;
    bool isPlaying() const { return m_playing; }
    bool hasMusic() const { return m_hasMusic; }


    // **回调率**（`ma_device::sampleRate`）—— dataCallback 就是按它被调用的，解码器与音色
    // 缓冲必须按它来。注意不是 internalSampleRate：那是硬件率，miniaudio 会在两者之间
    // **内部重采样**（本机 callback=48000 / internal=44100，见 docs/audio.md）。
    int deviceSampleRate() const { return m_deviceRate; }

    // 音乐**幅度包络**（RMS，0..1，按 envHz 采样）。Volume 砖色类型用它：
    // Re_ADOJAS 用 WebAudio 的时域 RMS（2048 样本窗口），这里离线算成一条包络，
    // 于是**抓帧也确定**（不依赖当时有没有在放音）。
    float amplitudeAt(float seconds) const {
        if (m_envelope.empty() || seconds < 0.0f) return 0.0f;
        const size_t i = (size_t)(seconds * (float)m_envHz);
        return i < m_envelope.size() ? m_envelope[i] : 0.0f;
    }

    // External audio source for mixing (hitsounds)
    void attachExternal(const float* buffer, size_t totalFrames, int channels, int sampleRate,
                        size_t* cursor, bool* playing);
    void detachExternal();

private:
    ma_device* m_device = nullptr;
    ma_decoder* m_decoder = nullptr;
    std::vector<uint8_t> m_fileData;  // keep file memory alive for decoder
    int m_sampleRate = 44100;
    int m_deviceRate = AUDIO_SAMPLE_RATE;
    std::vector<float> m_envelope;   // 音乐 RMS 包络（见 amplitudeAt）
    int m_envHz = 50;
    void buildEnvelope();
    uint64_t m_readCursor = 0;  // current decoder position in frames
    float m_duration = 0.0f;
    bool m_hasMusic = false;
    bool m_playing = false;

    // External source (hitsounds)
    const float* m_extBuffer = nullptr;
    size_t m_extTotalFrames = 0;
    int m_extChannels = 0;
    size_t* m_extCursor = nullptr;
    bool* m_extPlaying = nullptr;

    static void dataCallback(ma_device* pDevice, void* pOutput, const void*, unsigned int frameCount);
};

}  // namespace adofai
