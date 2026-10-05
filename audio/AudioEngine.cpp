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

    m_initialized = true;
    LOG_I("AudioEngine: Initialized (%dHz, backend=%s)",
          (int)m_device->playback.internalSampleRate,
          ma_get_backend_name(m_device->pContext->backend));
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
    m_initialized = false;
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
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, AUDIO_SAMPLE_RATE);
    if (ma_decoder_init_memory(data.data(), data.size(), &dc, m_decoder) != MA_SUCCESS) {
        LOG_E("AudioEngine: Unsupported format: %s", filepath.c_str());
        delete m_decoder;
        m_decoder = nullptr;
        return false;
    }

    ma_uint64 total = 0;
    ma_decoder_get_length_in_pcm_frames(m_decoder, &total);
    m_sampleRate = AUDIO_SAMPLE_RATE;
    m_readCursor = 0;
    m_duration = (m_sampleRate > 0) ? (float)total / (float)m_sampleRate : 0.0f;
    m_fileData = std::move(data);  // keep alive for memory-backed decoder

    m_hasMusic = true;
    LOG_I("AudioEngine: Loaded (%dHz, stereo, %.1fs): %s",
          m_sampleRate, m_duration, filepath.c_str());
    return true;
}

void AudioEngine::play() {
    if (!m_device) return;
    m_playing = true;
    if (ma_device_is_started(m_device) == MA_FALSE)
        ma_device_start(m_device);
}

void AudioEngine::resume() {
    if (!m_device || !m_hasMusic) return;
    m_playing = true;
    if (ma_device_is_started(m_device) == MA_FALSE)
        ma_device_start(m_device);
}

void AudioEngine::pause() {
    m_playing = false;
    if (m_device && ma_device_is_started(m_device) != MA_FALSE)
        ma_device_stop(m_device);
}

void AudioEngine::stop() {
    m_playing = false;
    if (m_decoder) { ma_decoder_seek_to_pcm_frame(m_decoder, 0); m_readCursor = 0; }
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

void AudioEngine::setVolume(float v) {
    m_volume = std::max(0.0f, std::min(1.0f, v));
}

void AudioEngine::attachExternal(const float* buffer, size_t totalFrames, int channels, int sampleRate,
                                  size_t* cursor, bool* playing) {
    m_extBuffer = buffer;
    m_extTotalFrames = totalFrames;
    m_extChannels = channels;
    m_extSampleRate = sampleRate;
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

    if (!self) return;

    // --- Music source (ma_decoder: stereo f32 @ AUDIO_SAMPLE_RATE) ---
    if (self->m_decoder && self->m_playing) {
        float* musicBuf = (float*)alloca(frameCount * 2 * sizeof(float));
        ma_uint64 decoded = 0;
        ma_decoder_read_pcm_frames(self->m_decoder, musicBuf, frameCount, &decoded);
        self->m_readCursor += decoded;

        if (decoded > 0) {
            for (ma_uint64 i = 0; i < decoded; i++) {
                out[i * devCh]     += musicBuf[i * 2]     * self->m_volume;
                out[i * devCh + 1] += musicBuf[i * 2 + 1] * self->m_volume;
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
