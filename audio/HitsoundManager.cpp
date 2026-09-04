#include "HitsoundManager.hpp"
#include "AudioEngine.hpp"
#include "core/util/Logger.hpp"
#include "core/util/DataFile.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <unordered_map>

static std::unordered_map<std::string, std::vector<float>> s_wavCache;
static std::unordered_map<std::string, std::vector<int16_t>> s_wavRawCache;

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <vector>
#endif

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#include <vector>
#endif

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    return true;
}

static const char* hitsoundKey(const std::string& type) {
    // Case-insensitive matching (ADOFAI levels may use mixed case)
    if (type.empty()) return nullptr;
    if (iequals(type, "Kick"))              return "Kick.wav";
    if (iequals(type, "KickHouse"))         return "KickHouse.wav";
    if (iequals(type, "KickChroma"))        return "KickChroma.wav";
    if (iequals(type, "KickRupture"))       return "KickRupture.wav";
    if (iequals(type, "Snare"))             return "SnareAcoustic2.wav";
    if (iequals(type, "SnareHouse"))        return "SnareHouse.wav";
    if (iequals(type, "SnareVapor"))        return "SnareVapor.wav";
    if (iequals(type, "Clap"))              return "ClapHit.wav";
    if (iequals(type, "ClapHit"))           return "ClapHit.wav";
    if (iequals(type, "ClapHitEcho"))       return "ClapHitEcho.wav";
    if (iequals(type, "Hat"))               return "Hat.wav";
    if (iequals(type, "HatHouse"))          return "HatHouse.wav";
    if (iequals(type, "Chuck"))             return "Chuck.wav";
    if (iequals(type, "Hammer"))            return "Hammer.wav";
    if (iequals(type, "Shaker"))            return "Shaker.wav";
    if (iequals(type, "ShakerLoud"))        return "ShakerLoud.wav";
    if (iequals(type, "Sidestick"))         return "Sidestick.wav";
    if (iequals(type, "Stick"))             return "Stick.wav";
    if (iequals(type, "ReverbClack"))       return "ReverbClack.wav";
    if (iequals(type, "ReverbClap"))        return "ReverbClap.wav";
    if (iequals(type, "Squareshot"))        return "Squareshot.wav";
    if (iequals(type, "FireTile"))          return "FireTile.wav";
    if (iequals(type, "IceTile"))           return "IceTile.wav";
    if (iequals(type, "PowerUp"))           return "PowerUp.wav";
    if (iequals(type, "PowerDown"))         return "PowerDown.wav";
    if (iequals(type, "VehiclePositive"))   return "VehiclePositive.wav";
    if (iequals(type, "VehicleNegative"))   return "VehicleNegative.wav";
    if (iequals(type, "Sizzle"))            return "Sizzle.wav";
    return nullptr;
}

static std::string executableDirectory() {
#ifdef _WIN32
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string dir(exePath, len);
        auto pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos);
        return dir;
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size > 0 ? size : 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::string dir(buf.data());
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir;
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    std::string dir(buf);
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir;
#endif
    return {};
}

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static std::string findAssetsDir() {
    const std::string exeDir = executableDirectory();

    // Prefer CWD-relative hitsounds for command-line usage from the repo/build
    // root, then fall back to the executable directory (Finder / desktop launches).
    // Assets live in "assets/hitsounds/" (see docs/project-structure.md §2).
    std::vector<std::string> candidates;
#ifdef _WIN32
    if (!exeDir.empty()) candidates.push_back(exeDir + "/assets/hitsounds/");
#else
    candidates.push_back("assets/hitsounds/");
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + "/assets/hitsounds/");

        // If the binary is in a nested build subdirectory (e.g. build/app or
        // build/ADOCAO.app/Contents/MacOS), also look above it for assets
        // copied next to the executable / .app bundle.
        auto dir = exeDir;
        for (int i = 0; i < 3 && !dir.empty(); i++) {
            const auto slash = dir.find_last_of("/\\");
            if (slash == std::string::npos) { dir.clear(); break; }
            dir = dir.substr(0, slash);
        }
        if (!dir.empty())
            candidates.push_back(dir + "/assets/hitsounds/");
    }
#endif

    for (const auto& dir : candidates) {
        if (fileExists(dir + "Kick.wav"))
            return dir;
    }

    return candidates.empty() ? "assets/hitsounds/" : candidates.front();
}

HitsoundManager::HitsoundManager() = default;
HitsoundManager::~HitsoundManager() { m_buffer.clear(); }

void HitsoundManager::init(const std::string& assetsDir) {
    m_assetsDir = assetsDir.empty() ? findAssetsDir() : assetsDir;
}

std::string HitsoundManager::hitsoundPath(const std::string& type) const {
    const char* fn = hitsoundKey(type);
    return fn ? (m_assetsDir + fn) : std::string();
}

void HitsoundManager::setHitsoundType(const std::string& type) {
    if (m_hitsoundType == type) return;
    m_hitsoundType = type;
    m_synthesized = false;
}

void HitsoundManager::setVolume(float vol) {
    m_volume = std::max(0.0f, std::min(100.0f, vol)) / 100.0f;
}

void HitsoundManager::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!enabled) stop();
}

bool HitsoundManager::readWav(const std::string& filepath,
                               std::vector<float>& samples,
                               int& sampleRate, int& channels) {
    // Check cache first
    auto it = s_wavCache.find(filepath);
    if (it != s_wavCache.end()) {
        sampleRate = AUDIO_SAMPLE_RATE; channels = 1;  // cached data is always mono AUDIO_SAMPLE_RATE
        samples = it->second;
        return true;
    }

    auto wavData = readDataFile(filepath);
    if (wavData.empty()) { LOG_W("Hitsound: Cannot open %s", filepath.c_str()); return false; }
    const uint8_t* p = wavData.data();
    const uint8_t* end = p + wavData.size();
    auto read32 = [&]() { if(p+4>end)return(uint32_t)0; uint32_t v; memcpy(&v,p,4); p+=4; return v; };
    auto read16 = [&]() { if(p+2>end)return(uint16_t)0; uint16_t v; memcpy(&v,p,2); p+=2; return v; };

    if (memcmp(p, "RIFF", 4)) return false; p += 4;
    uint32_t fs = read32();
    if (memcmp(p, "WAVE", 4)) return false; p += 4;

    uint16_t bits=0, nch=0; uint32_t sr=0, dsize=0;
    const uint8_t* dataPtr = nullptr;
    while (p + 8 <= end) {
        char id[4]; memcpy(id, p, 4); p += 4;
        uint32_t cs = read32();
        if (!memcmp(id, "fmt ", 4) && cs >= 16) {
            read16(); // audio format
            nch = read16(); sr = read32();
            p += 6; bits = read16();
            if (cs > 16) p += cs - 16;
        } else if (!memcmp(id, "data", 4)) {
            dsize = cs; dataPtr = p; p += cs;
        } else {
            p += cs;
        }
    }
    if (bits!=16 || dsize==0 || !dataPtr) return false;

    sampleRate=(int)sr; channels=(int)nch;
    int nf=(int)dsize/((int)bits/8)/channels;
    std::vector<int16_t> raw((size_t)nf*channels);
    memcpy(raw.data(), dataPtr, raw.size() * sizeof(int16_t));

    samples.resize(raw.size());
    for (size_t i=0;i<raw.size();i++) samples[i]=(float)raw[i]/32768.0f;
    s_wavCache[filepath] = samples;     // cache for later reuse
    s_wavRawCache[filepath] = raw;     // cache raw int16 for hard-clip mixing
    return true;
}

bool HitsoundManager::preSynthesize(const std::vector<HitsoundTimestampGroup>& groups,
                                     float totalDuration,
                                     HitsoundProgressCb onProgress) {
    if (!m_enabled) return false;
    if (groups.empty()) {
        LOG_I("Hitsound: No groups, skipping");
        return false;
    }

    // Load all WAV files per group (cached). We keep a pointer to the raw
    // int16 data so the mixing loop can use 16-bit hard-clip (matching the
    // original HitSoundGenerator.exe: clamp(sum, -32768, 32767) each step).
    struct GroupData { const std::vector<int16_t>* rawSamples; int lenFrames; int sr; int ch; };
    std::unordered_map<std::string, GroupData> wavData;
    float maxHitSec = 0.0f;

    for (auto& g : groups) {
        if (g.type == "None" || g.type.empty()) continue;
        auto it = wavData.find(g.type);
        if (it != wavData.end()) continue;

        std::string hp = hitsoundPath(g.type);
        if (hp.empty()) {
            LOG_W("Hitsound: Unknown type '%s', redirecting to '%s'", g.type.c_str(), m_hitsoundType.c_str());
            hp = hitsoundPath(m_hitsoundType);
            if (hp.empty()) continue;
        }
        {
            std::vector<float> tmpSamples;
            GroupData gd;
            if (!readWav(hp, tmpSamples, gd.sr, gd.ch)) {
                LOG_W("Hitsound: Failed to read WAV for '%s'", g.type.c_str());
                continue;
            }
            gd.rawSamples = &s_wavRawCache[hp];  // guaranteed populated by readWav
            gd.lenFrames = (int)gd.rawSamples->size() / gd.ch;
            float dur = (float)gd.lenFrames / (float)gd.sr;
            if (dur > maxHitSec) maxHitSec = dur;
            wavData[g.type] = gd;
        }
    }
    if (wavData.empty()) return false;
    if (onProgress) onProgress(5.0f);

    int sr = AUDIO_SAMPLE_RATE;
    m_sampleRate = sr;
    int totalFrames = (int)((totalDuration + maxHitSec + 1.0f) * sr);
    size_t bufSize = (size_t)totalFrames * 2;

    // 16-bit hard-clip mixing buffer — matches HitSoundGenerator.exe
    std::vector<int16_t> mixBuf(bufSize, 0);

    int totalHits = 0;
    for (auto& g : groups) totalHits += (int)g.timestamps.size();
    int processed = 0;

    for (auto& g : groups) {
        if (g.type == "None" || g.type.empty()) continue;
        auto it = wavData.find(g.type);
        if (it == wavData.end()) continue;

        auto& gd = it->second;
        float volScale = g.volume / 100.0f;
        auto& raw  = *gd.rawSamples;

        // Timestamps are already in tile order from getHitsoundTimestampGroups()
        for (double ts : g.timestamps) {
            if (ts < 0.0) continue;
            int sf = (int)(ts * (double)sr);
            int cl = gd.lenFrames;
            if (sf + cl > totalFrames) cl = totalFrames - sf;
            if (cl <= 0) continue;
            for (int i = 0; i < cl; i++) {
                int add = (int)(raw[(size_t)i * (size_t)gd.ch] * volScale);
                int idx = (sf + i) * 2;
                // 16-bit hard-clip: clamp each addition individually
                int sumL = (int)mixBuf[idx]     + add;
                int sumR = (int)mixBuf[idx + 1] + add;
                if (sumL > 32767) sumL = 32767; else if (sumL < -32768) sumL = -32768;
                if (sumR > 32767) sumR = 32767; else if (sumR < -32768) sumR = -32768;
                mixBuf[idx]     = (int16_t)sumL;
                mixBuf[idx + 1] = (int16_t)sumR;
            }
            processed++;
        }
    }

    // Convert mixed int16 buffer back to float for playback
    m_buffer.resize(bufSize);
    for (size_t i = 0; i < bufSize; i++)
        m_buffer[i] = (float)mixBuf[i] / 32768.0f;

    if (onProgress) onProgress(100.0f);
    LOG_D("Hitsound: Synthesized %d hits from %zu groups into %.1fs buffer",
          processed, groups.size(), totalDuration);
    m_synthesized = true;
    return true;
}

void HitsoundManager::reset() {
    m_readCursor = 0;
    m_playing = true;
}

void HitsoundManager::resetAt(float audioPosSec) {
    m_readCursor = (size_t)(audioPosSec * (float)m_sampleRate);
    if (m_readCursor >= m_buffer.size() / 2) m_readCursor = 0;
    m_playing = true;
}

void HitsoundManager::stop() {
    m_playing = false;
}

bool HitsoundManager::writeWav(const std::string& filepath) {
    if (m_buffer.empty()) return false;
    size_t n = m_buffer.size() / 2;  // stereo frames
    // 16-bit stereo WAV
    std::vector<int16_t> raw(m_buffer.size());
    for (size_t i = 0; i < m_buffer.size(); i++) {
        float v = m_buffer[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        raw[i] = (int16_t)(v * 32767.0f);
    }
    FILE* f = fopen(filepath.c_str(), "wb");
    if (!f) return false;
    uint32_t dataSize = (uint32_t)(raw.size() * sizeof(int16_t));
    uint32_t riffSize = 36 + dataSize;
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(riffSize); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2); w32(AUDIO_SAMPLE_RATE); w32(AUDIO_SAMPLE_RATE * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(dataSize);
    fwrite(raw.data(), sizeof(int16_t), raw.size(), f);
    fclose(f);
    LOG_D("Hitsound: Exported %zu frames to %s", n, filepath.c_str());
    return true;
}
