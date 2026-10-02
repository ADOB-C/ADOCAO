#include "HitsoundManager.hpp"
#include "AudioEngine.hpp"
#include "core/util/Logger.hpp"
#include "core/util/DataFile.hpp"
#include "core/util/ThreadPool.hpp"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <thread>
#include <unordered_map>

static std::unordered_map<std::string, std::vector<float>> s_wavCache;
static std::unordered_map<std::string, std::vector<int16_t>> s_wavRawCache;
// Real (sampleRate, channels) of each cached file. The caches hold the file's own
// layout, so a cache hit must report these instead of assuming mono/48k: assuming
// mono made a cached STEREO hit twice as long and mixed L/R as consecutive frames,
// so any second synthesis in the same process produced wrong audio.
static std::unordered_map<std::string, std::pair<int,int>> s_wavMeta;

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdlib.h>
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
    // The reported path is the one used to launch, which may go through a
    // symlink (e.g. /Applications/ADOCAO.app). Resolve it so the search roots
    // below actually point into the bundle.
    char resolved[PATH_MAX];
    if (realpath(dir.c_str(), resolved)) dir = resolved;
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
        // Standard macOS bundle layout (see scripts/make-app.sh).
        candidates.push_back(exeDir + "/../Resources/assets/hitsounds/");

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
        const auto meta = s_wavMeta.find(filepath);
        sampleRate = meta != s_wavMeta.end() ? meta->second.first  : AUDIO_SAMPLE_RATE;
        channels   = meta != s_wavMeta.end() ? meta->second.second : 1;
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
    s_wavMeta[filepath] = { (int)sr, (int)nch };
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

    // --- Mixing -------------------------------------------------------------
    // ONE mixing path, and it is the authentic one: 16-bit accumulation with the
    // sum clamped on every addition, exactly like the original HitSoundGenerator.
    // A float path with a soft limiter was tried and removed: measured against this
    // one it changed 94.5% of the samples, and in listening the loud sections turned
    // into a distorted plateau while quiet sections lost level. Faithfulness wins;
    // the only thing that changed here is WHERE the work happens, never the result.
    //
    // Parallelism splits the OUTPUT sample range, not the hits: a task owns [lo, hi)
    // and only writes inside it (no locking), and inside a task the hits are still
    // visited in timestamp order — so every output sample gets the same contributions
    // in the same order as the old serial loop and the buffer is bit-identical.
    // Verified: coarse single-thread chunking vs multi-threaded, and a repeat
    // synthesis in one process, all produce byte-identical WAVs.
    std::vector<int16_t> mixBuf(bufSize, 0);

    auto visitHits = [&](int lo, int hi, int& hits, auto&& emit) {
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            auto& g = groups[gi];
            if (g.type == "None" || g.type.empty()) continue;
            auto it = wavData.find(g.type);
            if (it == wavData.end()) continue;

            auto& gd = it->second;
            const float volScale = g.volume / 100.0f;
            const int16_t* src = gd.rawSamples->data();
            const int ch = gd.ch;
            const int len = gd.lenFrames;
            const auto& ts = g.timestamps;

            // Hits are sorted by time, so binary-search the first one whose tail can
            // still reach sample lo.
            const double minTs = (double)(lo - len) / (double)sr;
            for (auto hit = std::upper_bound(ts.begin(), ts.end(), minTs); hit != ts.end(); ++hit) {
                const long sf = (long)(*hit * (double)sr);
                if (sf >= hi) break;
                if (sf >= lo) hits++;                 // counted by the chunk it starts in
                const int i0 = std::max(0, lo - (int)sf);
                const int i1 = std::min(len, hi - (int)sf);
                if (i1 <= i0) continue;
                emit((size_t)sf + (size_t)i0, src, i0, i1 - i0, ch, volScale);
            }
        }
    };

    int processed = 0;
    {
        // ADOCAO_MIX_THREADS is a test hook: a small value makes the chunking coarse
        // so the parallel result can be diffed against the multi-chunk one.
        unsigned hw = std::thread::hardware_concurrency();
        if (const char* env = std::getenv("ADOCAO_MIX_THREADS")) {
            const int n = std::atoi(env);
            if (n > 0) hw = (unsigned)n;
        }
        ThreadPool pool(hw > 0 ? hw : 4u);
        std::atomic<int> mixed{0};
        pool.parallelFor(0, (size_t)totalFrames, [&](size_t lo, size_t hi) {
            int local = 0;
            visitHits((int)lo, (int)hi, local, [&](size_t at, const int16_t* s, int i0, int n, int ch, float vol) {
                int16_t* d = mixBuf.data() + at * 2;
                for (int i = 0; i < n; i++) {
                    const int add = (int)(s[(size_t)(i0 + i) * (size_t)ch] * vol);
                    // Both channels receive the same value, so clamp once.
                    int v = (int)d[i * 2] + add;
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    d[i * 2]     = (int16_t)v;
                    d[i * 2 + 1] = (int16_t)v;
                }
            });
            mixed.fetch_add(local, std::memory_order_relaxed);
        }, 4096);
        processed = mixed.load();
    }

    m_lastMixedHits = processed;
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
