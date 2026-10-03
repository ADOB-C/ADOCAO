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

// Saturating-add kernels for the hitsound mixer (the clamp semantics map exactly
// onto these instructions).
#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
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
    // A float path with a soft limiter was tried and deleted (see AGENTS.md): it
    // changed 94.5% of the samples and sounded wrong. Only WHERE the work happens
    // changes here, never the result.
    //
    // Three output-neutral restructurings, all verified byte-identical:
    //  1. The per-sample scale `(int)(s * volume)` depends only on (group, sample
    //     index), so it is computed ONCE per group instead of once per hit — the
    //     old loop re-did it for every one of millions of hits.
    //  2. The accumulation buffer is MONO. In the authentic mix both channels always
    //     receive the same value (both start at 0 and every add is identical), so
    //     accumulating one channel and duplicating at the end is exact — and halves
    //     the read-modify-write traffic.
    //  3. The output is swept in blocks that fit in L2, and a block only visits the
    //     hits overlapping it, so source data stays hot; blocks are independent and
    //     are handed to worker threads (no locking, hits visited in timestamp order
    //     inside a block).
    constexpr int kBlockFrames = 1 << 16;      // 65536 frames = 128 KB mono int16
    const size_t numBlocks = ((size_t)totalFrames + kBlockFrames - 1) / kBlockFrames;

    // Prepared per-group scaled source (see 1). `wide` is only used when a scaled
    // sample does not fit in int16 (volume > 100); then the scalar int path keeps
    // the original semantics exactly.
    struct Prepared {
        const int16_t* buf = nullptr;          // scaled samples, first channel only
        const int32_t* wide = nullptr;         // used when !fits16
        int len = 0;
        bool fits16 = true;
        std::vector<int16_t> own;
        std::vector<int32_t> ownWide;
        std::vector<double> ts;
    };
    std::unordered_map<std::string, Prepared> prepared;
    for (auto& g : groups) {
        if (g.type == "None" || g.type.empty()) continue;
        auto it = wavData.find(g.type);
        if (it == wavData.end()) continue;
        if (prepared.count(g.type)) continue;

        auto& gd = it->second;
        const float vol = g.volume / 100.0f;
        Prepared pr;
        pr.len = gd.lenFrames;
        pr.own.resize((size_t)pr.len);
        pr.ownWide.resize((size_t)pr.len);
        for (int i = 0; i < pr.len; i++) {
            const int add = (int)(gd.rawSamples->data()[(size_t)i * (size_t)gd.ch] * vol);
            pr.own[(size_t)i] = (int16_t)add;
            pr.ownWide[(size_t)i] = add;
            if (add > 32767 || add < -32768) pr.fits16 = false;
        }
        pr.buf = pr.own.data();
        pr.wide = pr.ownWide.data();
        pr.ts = g.timestamps;
        prepared.emplace(g.type, std::move(pr));
    }

    std::vector<int16_t> mixBuf((size_t)totalFrames, 0);

    auto mixBlock = [&](size_t block, int& hits) {
        const int lo = (int)(block * (size_t)kBlockFrames);
        const int hi = (int)std::min<size_t>((block + 1) * (size_t)kBlockFrames, (size_t)totalFrames);

        for (auto& [type, pr] : prepared) {
            const auto& ts = pr.ts;
            const double minTs = (double)(lo - pr.len) / (double)sr;
            for (auto hit = std::upper_bound(ts.begin(), ts.end(), minTs); hit != ts.end(); ++hit) {
                const long sf = (long)(*hit * (double)sr);
                if (sf >= hi) break;
                if (sf >= lo) hits++;
                const int i0 = std::max(0, lo - (int)sf);
                const int i1 = std::min(pr.len, hi - (int)sf);
                if (i1 <= i0) continue;

                int16_t* d = mixBuf.data() + (size_t)sf + (size_t)i0;
                if (pr.fits16) {
                    const int16_t* sAdd = pr.buf + i0;
                    const int n = i1 - i0;
                    int i = 0;
#if defined(__ARM_NEON)
                    for (; i + 8 <= n; i += 8) {
                        int16x8_t a = vld1q_s16(d + i);
                        int16x8_t b = vld1q_s16(sAdd + i);
                        vst1q_s16(d + i, vqaddq_s16(a, b));   // saturating add == clamp
                    }
#elif defined(__SSE2__)
                    for (; i + 8 <= n; i += 8) {
                        __m128i a = _mm_loadu_si128((const __m128i*)(d + i));
                        __m128i b = _mm_loadu_si128((const __m128i*)(sAdd + i));
                        _mm_storeu_si128((__m128i*)(d + i), _mm_adds_epi16(a, b));
                    }
#endif
                    for (; i < n; i++) {
                        int v = (int)d[i] + (int)sAdd[i];
                        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                        d[i] = (int16_t)v;
                    }
                } else {
                    const int32_t* sAdd = pr.wide + i0;
                    for (int i = 0, k = i0; k < i1; ++i, ++k) {
                        int v = (int)d[i] + (int)sAdd[i];
                        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                        d[i] = (int16_t)v;
                    }
                }
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
        pool.parallelFor(0, numBlocks, [&](size_t b0, size_t b1) {
            int local = 0;
            for (size_t b = b0; b < b1; ++b) mixBlock(b, local);
            mixed.fetch_add(local, std::memory_order_relaxed);
        }, 1);
        processed = mixed.load();
    }

    m_lastMixedHits = processed;
    m_buffer.resize(bufSize);
    for (int f = 0; f < totalFrames; f++) {           // mono mix -> L == R
        const float v = (float)mixBuf[(size_t)f] / 32768.0f;
        m_buffer[(size_t)f * 2]     = v;
        m_buffer[(size_t)f * 2 + 1] = v;
    }

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
