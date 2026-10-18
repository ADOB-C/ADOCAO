#include "AudioEngine.hpp"
#include "core/util/Logger.hpp"

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>  // enables MA_HAS_VORBIS in miniaudio

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif


namespace adofai {

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    m_device = new ma_device;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate        = AUDIO_SAMPLE_RATE;
    config.dataCallback      = dataCallback;
    config.pUserData         = this;

    if (ma_device_init(nullptr, &config, m_device) != MA_SUCCESS) {
        config.sampleRate = 0;
        if (ma_device_init(nullptr, &config, m_device) != MA_SUCCESS) {
            LOG_E("AudioEngine: Failed to init audio device");
            delete m_device;
            m_device = nullptr;
            return false;
        }
    }

    // **回调率**才是我们该跟的：miniaudio 在 internal 率与回调率不一致时**内部重采样**，
    // dataCallback 永远按 `pDevice->sampleRate` 被调用（本机实测 callback=48000、internal=44100，
    // 而音乐的 ffprobe 也是 48000 —— 拿 internal 率去配解码器会把音乐弄慢 8.8%）。
    // 只有在 `ma_device_init` 第一次失败、我们重试 sampleRate=0 时，回调率才会等于硬件率。
    m_deviceRate = (int)m_device->sampleRate;
    if (m_deviceRate <= 0) m_deviceRate = AUDIO_SAMPLE_RATE;
    LOG_I("AudioEngine: Initialized (callback=%dHz, internal=%dHz, backend=%s)%s",
          (int)m_device->sampleRate,
          (int)m_device->playback.internalSampleRate,
          ma_get_backend_name(m_device->pContext->backend),
          m_deviceRate == AUDIO_SAMPLE_RATE ? ""
              : " —— 回调率不是 48k：解码器与音色按回调率输出（miniaudio 已内部重采样）");
    return true;
}

void AudioEngine::shutdown() {
    if (m_device) {
        if (m_device->pContext) ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
    }
    if (m_decoder) {
        ma_decoder_uninit(m_decoder);
        delete m_decoder;
        m_decoder = nullptr;
    }
    m_fileData.clear();
    m_hasMusic = false;
    m_playing = false;
}

bool AudioEngine::loadMusic(const std::string& filepath) {
    if (!m_device) return false;

    if (m_decoder) {
        ma_decoder_uninit(m_decoder);
        delete m_decoder;
        m_decoder = nullptr;
        m_hasMusic = false;
    }
    m_fileData.clear();

    // Read file into memory (handles UTF-8 paths on Windows)
    std::vector<uint8_t> data;
    {
#ifdef _WIN32
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, nullptr, 0);
        if (wlen <= 0) { LOG_E("AudioEngine: Invalid path"); return false; }
        std::wstring wpath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, &wpath[0], wlen);

        FILE* f = _wfopen(wpath.c_str(), L"rb");
        if (!f) { LOG_E("AudioEngine: Cannot open file"); return false; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return false; }
        data.resize((size_t)sz);
        fread(data.data(), 1, (size_t)sz, f);
        fclose(f);
#else
        std::ifstream f(filepath, std::ios::binary | std::ios::ate);
        if (!f) { LOG_E("AudioEngine: Cannot open file"); return false; }
        auto sz = f.tellg();
        f.seekg(0);
        data.resize((size_t)sz);
        f.read((char*)data.data(), sz);
#endif
    }

    m_decoder = new ma_decoder;
    // 解码器按**设备率**输出（miniaudio 在解码时重采样）—— 否则回调 1:1 拷贝会让
    // 音乐以 设备率/48000 的速度播放，`position()` 也会按同样的比例跑偏（关卡时钟跟着错）。
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, (ma_uint32)m_deviceRate);
    if (ma_decoder_init_memory(data.data(), data.size(), &dc, m_decoder) != MA_SUCCESS) {
        LOG_E("AudioEngine: Unsupported format: %s", filepath.c_str());
        delete m_decoder;
        m_decoder = nullptr;
        return false;
    }

    ma_uint64 total = 0;
    ma_decoder_get_length_in_pcm_frames(m_decoder, &total);
    m_sampleRate = m_deviceRate;
    m_readCursor = 0;
    m_duration = (m_sampleRate > 0) ? (float)total / (float)m_sampleRate : 0.0f;
    m_fileData = std::move(data);  // keep alive for memory-backed decoder

    m_hasMusic = true;
    buildEnvelope();
    LOG_I("AudioEngine: Loaded (%dHz, stereo, %.1fs): %s",
          m_sampleRate, m_duration, filepath.c_str());
    return true;
}

void AudioEngine::buildEnvelope() {
    m_envelope.clear();
    if (m_fileData.empty() || m_sampleRate <= 0) return;
    // 单独一个单声道解码器扫一遍（不碰播放用的 m_decoder）；只留 RMS，
    // 内存 = envHz 个 float/秒（50 Hz × 10 分钟 = 120 KB）。
    ma_decoder dec;
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)m_sampleRate);
    if (ma_decoder_init_memory(m_fileData.data(), m_fileData.size(), &dc, &dec) != MA_SUCCESS) return;
    const int win = std::max(1, m_sampleRate / m_envHz);
    std::vector<float> buf(8192);
    m_envelope.reserve((size_t)(m_duration * (float)m_envHz) + 2);
    double acc = 0.0;
    int n = 0;
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), buf.size(), &got) != MA_SUCCESS || got == 0) break;
        for (ma_uint64 i = 0; i < got; i++) {
            const double v = buf[(size_t)i];
            acc += v * v;
            if (++n == win) {
                m_envelope.push_back((float)std::sqrt(acc / (double)win));
                acc = 0.0; n = 0;
            }
        }
    }
    if (n > 0) m_envelope.push_back((float)std::sqrt(acc / (double)n));
    ma_decoder_uninit(&dec);
    LOG_D("AudioEngine: 幅度包络 %zu 点 @ %d Hz（%.1fs）", m_envelope.size(), m_envHz, (double)m_duration);
}

void AudioEngine::play() {
    if (!m_device) return;
    m_playing = true;
    if (ma_device_is_started(m_device) == MA_FALSE)
        ma_device_start(m_device);
}

void AudioEngine::pause() {
    m_playing = false;
    if (m_device && ma_device_is_started(m_device) != MA_FALSE)
        ma_device_stop(m_device);
}

void AudioEngine::seek(float seconds) {
    if (!m_decoder || m_sampleRate <= 0) return;
    ma_uint64 frame = (ma_uint64)(seconds * (float)m_sampleRate);
    ma_decoder_seek_to_pcm_frame(m_decoder, frame);
    m_readCursor = frame;
}

float AudioEngine::position() const {
    if (!m_decoder || m_sampleRate <= 0) return 0.0f;
    return (float)m_readCursor / (float)m_sampleRate;
}

float AudioEngine::duration() const {
    return m_duration;
}

void AudioEngine::attachExternal(const float* buffer, size_t totalFrames, int channels, int sampleRate,
                                  size_t* cursor, bool* playing) {
    m_extBuffer = buffer;
    m_extTotalFrames = totalFrames;
    m_extChannels = channels;
    if (sampleRate != m_deviceRate)
        LOG_W("AudioEngine: 外部音源 %dHz ≠ 设备 %dHz —— 回调不做重采样，播放速度会偏",
              sampleRate, m_deviceRate);
    m_extCursor = cursor;
    m_extPlaying = playing;
}

void AudioEngine::detachExternal() {
    m_extBuffer = nullptr;
    m_extCursor = nullptr;
    m_extPlaying = nullptr;
}

void AudioEngine::dataCallback(ma_device* pDevice, void* pOutput, const void*, unsigned int frameCount) {
    auto* self = static_cast<AudioEngine*>(pDevice->pUserData);
    float* out = (float*)pOutput;
    unsigned int devCh = pDevice->playback.channels;
    unsigned int total = frameCount * devCh;

    // Fill with silence first
    for (unsigned int i = 0; i < total; i++) out[i] = 0.0f;

    // --- Music source (ma_decoder: stereo f32 @ AUDIO_SAMPLE_RATE) ---
    if (self->m_decoder && self->m_playing) {
        float* musicBuf = (float*)alloca(frameCount * 2 * sizeof(float));
        ma_uint64 decoded = 0;
        ma_decoder_read_pcm_frames(self->m_decoder, musicBuf, frameCount, &decoded);
        self->m_readCursor += decoded;

        if (decoded > 0) {
            for (ma_uint64 i = 0; i < decoded; i++) {
                out[i * devCh]     += musicBuf[i * 2];
                out[i * devCh + 1] += musicBuf[i * 2 + 1];
            }
        }
        if (decoded == 0) self->m_playing = false;
    }

    // --- External source (hitsounds) ---
    if (self->m_extBuffer && self->m_extPlaying && *self->m_extPlaying && self->m_extCursor) {
        size_t cursor = *self->m_extCursor;
        size_t extTotal = self->m_extTotalFrames;
        int extCh = self->m_extChannels;

        for (unsigned int i = 0; i < frameCount && cursor < extTotal; i++, cursor++) {
            for (int c = 0; c < extCh && c < (int)devCh; c++) {
                out[i * devCh + c] += self->m_extBuffer[cursor * extCh + c];
            }
        }
        *self->m_extCursor = cursor;
        if (cursor >= extTotal) *self->m_extPlaying = false;
    }
}

}  // namespace adofai
