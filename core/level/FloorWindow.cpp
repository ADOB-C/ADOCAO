// ③ 共享层：checkpoint 计算（从 archive/AdocaoCheckpoints.cpp 搬来，**算式一字未改**——
// "从快照重算 = 全谱一次算"的逐位对拍就是靠它与 `calculateTilePositions`/`precalculateTiming`
// 逐字同序；搬动时只把两处**数据来源**换成访问器/参数：
//   1. `stepAngleDeg(level, i)` 原来读 `level.tiles[i].direction` → 改 `level.tileDirectionAt(i)`
//      （`tiles` 将来要按块物化，只能走访问器）；
//   2. 时间/BPM 原来在函数内建一次 Timeline → 改成可选参数（加载路径已经建过，别重复建）。

#include "core/level/FloorWindow.hpp"

#include <algorithm>
#include <cmath>

#include "core/level/LevelData.hpp"
#include "core/timeline/Timeline.hpp"

namespace adofai {
namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

bool computeCheckpoints(const LevelData& level, uint32_t stride,
                        const std::vector<double>& times, const std::vector<float>& bpms,
                        std::vector<FloorCheckpoint>& out, std::string& err) {
    out.clear();
    err.clear();
    if (stride == 0) { err = "checkpoint stride 不能为 0"; return false; }

    const size_t nData = (size_t)level.angleCount();  // 真层数；tiles 还会多一块 extra 砖
    if ((size_t)level.tileCount() < nData) {
        err = "LevelData 还没有算位置（tiles < angleData）——checkpoint 必须在 finishLoad 之后算";
        return false;
    }

    // 与 applyPositionTrackOffsets 同一套语义：非 justThisTile 的偏移**累进**（本层及之后都吃到），
    // justThisTile 是**点加**（只作用于本层）。这里按层号升序推进，所以只需要按 floor 排序的事件。
    std::vector<std::pair<int64_t, LevelData::TilePositionOffset>> offs(
        level.tilePositionOffsets.begin(), level.tilePositionOffsets.end());
    std::sort(offs.begin(), offs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t oi = 0;

    std::vector<std::pair<int64_t, const LevelData::FastAction*>> acts;
    acts.reserve(level.actions.size());
    for (const LevelData::FastAction& a : level.actions)
        if (a.floor >= 0) acts.push_back({(int64_t)a.floor, &a});
    std::stable_sort(acts.begin(), acts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t ai = 0;
    // ⚠️ 类型必须与 `Timeline::precalculateTiming` **完全一致**：那边 currentBPM/angleDir 是
    // **float**（`currentBPM *= a.val1` 每步都舍入回 float）。这里用 double 累加会在多次
    // SetSpeed 之后与生产路径差出 ULP —— 而"从快照重算 = 全谱一次算"要求逐位相同。
    bool isCW = true;
    float currentBPM = level.settings.bpm;
    float angleDir = 180.0f;

    double baseX = 0.0, baseY = 0.0;    // angleData 前缀和（**不含** PositionTrack 偏移）
    double cumX = 0.0, cumY = 0.0;     // PositionTrack 继承量（floor < i0 的全部事件处理完）
    double lastReal = 0.0;             // 最后一个真 angleData 值
    bool haveLast = false;
    bool lastWasSentinel = false;
    size_t sentinelRun = 0;            // 紧跟当前位置之前连续 999 的个数

    const size_t nCk = (nData + stride) / stride;    // i0 = 0, stride, 2*stride, ... < nData
    out.reserve(nCk ? nCk : 1);

    for (size_t i = 0; i < nData; ++i) {
        if (i % stride == 0) {
            FloorCheckpoint ck;
            ck.floor = (uint64_t)i;
            ck.baseX = baseX; ck.baseY = baseY;
            ck.cumX = cumX;   ck.cumY = cumY;
            ck.flags = kCkBase | kCkCum;
            ck.cw = isCW ? 1 : 0;
            ck.flags |= kCkCw;
            ck.bpm = (double)currentBPM;
            ck.flags |= kCkBpm;
            ck.angleDirDeg = (double)angleDir;      // 存的就是那个 float 值（别用 double 中间量）
            ck.flags |= kCkAngleDir;
            if (haveLast) { ck.lastAngleDeg = lastReal; ck.flags |= kCkLastAngle; }
            if (lastWasSentinel) ck.flags |= kCkLastSentinel;
            ck.sentinelRun = (uint32_t)sentinelRun;
            ck.flags |= kCkSentinelRun;
            if (i < times.size()) { ck.timeSec = times[i]; ck.flags |= kCkTime; }
            out.push_back(ck);
        }

        // 事件：floor < i0 的部分在快照里；i0 那一层的事件在**窗口重算**里处理（§8.8 不变量 3），
        // 所以这里推进事件游标只用于维护 cum 的"下一个快照值"。
        while (oi < offs.size() && offs[oi].first == (int64_t)i) {
            const LevelData::TilePositionOffset& o = offs[oi].second;
            if (!o.justThisTile) { cumX += o.offsetX; cumY += o.offsetY; }
            ++oi;   // justThisTile 不进 cum（点加，不跨层）
        }
        // Timeline Phase 1 的同序递推（Twirl / SetSpeed；Pause 是逐层的，不进快照）
        while (ai < acts.size() && acts[ai].first == (int64_t)i) {
            const LevelData::FastAction& a = *acts[ai].second;
            switch (a.type) {
            case LevelData::FastAction::Twirl: isCW = !isCW; break;
            case LevelData::FastAction::SetSpeed:
                if (a.flag) currentBPM *= a.val1; else currentBPM = a.val1;
                break;
            default: break;
            }
            ++ai;
        }
        // 交叉自检：Timeline 的 `m_tileBPM[i]` 是**处理完第 i 层事件之后**的值，而快照存的是
        // "处理之前"（窗口要自己再处理一遍，§8.8 不变量 3）。两者都要对得上，所以这里比的是
        // "本层事件处理完之后"的 currentBPM —— 它能抓住任何实现漂移。
        if (i < bpms.size() && bpms[i] != currentBPM) {
            err = "checkpoint 的 bpm 与 Timeline 不一致（实现漂移）@floor " + std::to_string(i);
            return false;
        }

        const double a = level.angleData[i];
        if (a == 999.0) { lastWasSentinel = true; ++sentinelRun; }
        else { lastReal = a; haveLast = true; lastWasSentinel = false; sentinelRun = 0; }
        // angleDir = "进下一层的方向"（ADOFAI-JS 语义，与 Phase 1 的收尾更新逐字同序）
        if (a == 999.0) {
            int minus = 1;
            while ((int64_t)i - minus >= 0 && level.angleData[i - minus] == 999.0) ++minus;
            const double realAngle = ((int64_t)i - minus >= 0) ? level.angleData[i - minus] : 0.0;
            angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
        } else {
            angleDir = (float)std::fmod(a + 180.0, 360.0);
        }
        if (angleDir < 0) angleDir += 360.0f;

        const double rad = (double)level.tileDirectionAt((int64_t)i) * kPi / 180.0;
        baseX += std::cos(rad);
        baseY += std::sin(rad);
    }
    return true;
}

bool computeCheckpoints(const LevelData& level, uint32_t stride,
                        std::vector<FloorCheckpoint>& out, std::string& err) {
    // 时间/BPM 只有 Timeline 才是权威实现（SetSpeed 继承 + duration 前缀和）——不在这里重抄一份
    // 状态机（重抄必然与生产路径漂移）。代价：要在 pack 期建一次 Timeline（约 32 B/层）。
    std::vector<double> times;
    std::vector<float> bpms;
    {
        Timeline tl;
        tl.build(level, /*exportOnly=*/true);
        times = tl.tileStartTimes();
        bpms = tl.tileBPMs();
    }
    return computeCheckpoints(level, stride, times, bpms, out, err);
}

namespace {
FloorRecomputeFn g_floorRecompute = nullptr;
}  // namespace

// ---------------- ③ 切片 4：LevelData 侧的访问器与缓存管理 ----------------

std::unique_ptr<FloorWindowCache> FloorWindowCache::clone() const {
    auto c = std::make_unique<FloorWindowCache>();
    c->m_level = m_level;            // 只读：指向同一份 LevelData
    c->m_ck = m_ck;                  // 只读：checkpoint 数组（96 B/4096 层）
    c->m_isCW = m_isCW;              // 只读：cw 位
    c->m_extra = m_extra;            // 只读：末尾合成砖
    c->m_hasExtra = m_hasExtra;
    c->m_tileCount = m_tileCount;
    c->m_ready = m_ready;
    c->m_windowed = m_windowed;
    c->m_why = m_why;
    c->m_block = -1;                 // 可变：全新窗口
    c->m_blocks = 0;
    return c;
}

int64_t LevelData::tileCount() const {
    if (m_winCache && m_winCache->windowed()) return m_winCache->tileCount();
    return (int64_t)tiles.size();
}

std::array<double, 2> LevelData::tilePositionAt(int64_t i, FloorWindowCache* wc) const {
    FloorWindowCache* c = wc ? wc : m_winCache;
    if (c && c->windowed()) {
        const Tile* t = c->tileAt(i);
        if (t) return t->position;
    }
    return (i >= 0 && (size_t)i < tiles.size()) ? tiles[(size_t)i].position
                                                : std::array<double, 2>{0.0, 0.0};
}

float LevelData::tileDirectionAt(int64_t i, FloorWindowCache* wc) const {
    FloorWindowCache* c = wc ? wc : m_winCache;
    if (c && c->windowed()) {
        const Tile* t = c->tileAt(i);
        if (t) return t->direction;
    }
    return (i >= 0 && (size_t)i < tiles.size()) ? tiles[(size_t)i].direction : 0.0f;
}

FloorWindowCache* LevelData::threadWindowCache() const {
    if (!m_winCache || !m_winCache->windowed()) return nullptr;
    struct TL {
        const LevelData* owner = nullptr;
        std::unique_ptr<FloorWindowCache> cache;
    };
    static thread_local TL tl;                 // 每线程一份（1 个窗口 = 65 KB）
    if (tl.owner != this || !tl.cache) {       // 换谱/首次 → 克隆一份只读状态
        tl.owner = this;
        tl.cache = m_winCache->clone();
    }
    return tl.cache.get();
}

bool LevelData::buildFloorWindowCache(const std::vector<double>& startTimes,
                                      const std::vector<float>& bpms,
                                      const std::vector<bool>& isCW, std::string* err) {
    m_winCache = new FloorWindowCache();
    std::string e;
    if (m_winCache->init(*this, startTimes, bpms, isCW, e)) return true;
    if (err) *err = e;
    delete m_winCache;
    m_winCache = nullptr;
    return false;
}

bool LevelData::dropTilesArray() {
    if (!m_winCache) return false;
    return m_winCache->dropTiles(*this);
}

bool LevelData::tilesWindowed() const { return m_winCache && m_winCache->windowed(); }
void LevelData::invalidateFloorWindowForTest() {
    if (m_winCache) m_winCache->invalidateWindowForTest();
}
size_t LevelData::windowBlocksMaterialized() const {
    return m_winCache ? m_winCache->blocksMaterialized() : 0;
}

// ---------------- ③ 切片 4：按块物化缓存 ----------------

FloorWindowCache::FloorWindowCache() = default;
FloorWindowCache::~FloorWindowCache() = default;

bool FloorWindowCache::init(LevelData& level, const std::vector<double>& startTimes,
                            const std::vector<float>& bpms, const std::vector<bool>& isCW,
                            std::string& err) {
    m_level = &level;
    m_ready = false;
    m_windowed = false;
    m_block = -1;
    m_blocks = 0;

    const size_t nData = (size_t)level.angleCount();
    m_tileCount = level.tileCount();          // 丢数组之前的权威砖数（含末尾合成砖）
    if (nData == 0 || m_tileCount < (int64_t)(nData + 1)) {
        m_why = "tiles 还没算好（需要在 finishLoad 之后调用）";
        err = m_why;
        return false;
    }
    if (level.hasAtStates) {   // 有 AnimateTrack：atStates 需要事件本体，本条路不适用
        m_why = "有 AnimateTrack（需要事件本体）";
        err = m_why;
        return false;
    }
    if (isCW.size() < nData) {
        m_why = "缺 tileIsCW";
        err = m_why;
        return false;
    }
    if (!computeCheckpoints(level, kCheckpointStride, startTimes, bpms, m_ck, err)) {
        m_why = err.c_str();   // 让日志说清楚为什么没接
        return false;
    }

    m_isCW.assign(isCW.begin(), isCW.end());     // 位图，0.125 B/层
    m_extra = level.tiles.back();                // 末尾那块合成砖不在任何窗口里，单独留一份
    m_hasExtra = (level.tileCount() > (int64_t)nData);
    m_ready = true;
    m_why = "可窗口化";
    return true;
}

bool FloorWindowCache::materialize(int64_t block) const {
    if (!m_ready || !m_level) return false;
    FloorRecomputeFn fn = floorRecompute();
    if (!fn) return false;                        // 忘记 archive::install()
    const int64_t nData = m_level->angleCount();
    const int64_t i0 = block * kCheckpointStride;
    const int64_t i1 = std::min(i0 + (int64_t)kCheckpointStride, nData);
    if (i0 < 0 || i1 <= i0) return false;
    const size_t bidx = (size_t)(i0 / (int64_t)kCheckpointStride);
    if (bidx >= m_ck.size() || m_ck[bidx].floor != (uint64_t)i0) return false;

    // 角度窗口：`computeFloorWindow` 以 `angles->size()` 作为窗口长度，所以这里切一块出来
    // （4096 × 8 B = 32 KB，只在物化那一刻存在）
    std::vector<double> angles((size_t)(i1 - i0));
    for (int64_t i = i0; i < i1; ++i) angles[(size_t)(i - i0)] = m_level->angleAt(i);

    FloorWindowInput in;
    in.checkpoint = m_ck[bidx];
    in.angles = &angles;
    in.strings = &m_level->actionStrTable;

    // **合成块内事件**：只用已经保留的每层数组 + 稀疏偏移表（不碰已被释放的 actions）
    std::vector<LevelData::FastAction> ev;
    ev.reserve(16);
    for (int64_t i = i0; i < i1; ++i) {
        if (i > 0 && m_isCW[(size_t)i] != m_isCW[(size_t)(i - 1)]) {
            LevelData::FastAction a;
            a.floor = (int)i;
            a.type = LevelData::FastAction::Twirl;
            ev.push_back(a);
        }
        if (i > 0) {
            const float b = m_level->tileBPMs[(size_t)i];
            const float pb = m_level->tileBPMs[(size_t)(i - 1)];
            if (b != pb) {
                LevelData::FastAction a;
                a.floor = (int)i;
                a.type = LevelData::FastAction::SetSpeed;
                a.flag = false;                   // 绝对赋值：数组里已经是"结果"
                a.val1 = b;
                ev.push_back(a);
            }
        }
        auto it = m_level->tilePositionOffsets.find((int)i);
        if (it != m_level->tilePositionOffsets.end()) {
            LevelData::FastAction a;
            a.floor = (int)i;
            a.type = LevelData::FastAction::PositionTrack;
            a.val1 = it->second.offsetX;
            a.val2 = it->second.offsetY;
            a.flag = it->second.justThisTile;     // true = 点加（只本层）
            ev.push_back(a);
        }
    }
    in.events = &ev;

    FloorWindow& w = m_scratch;          // 复用缓冲（容量保留）
    std::string err;
    if (!fn(m_level->settings, in, w, err)) return false;
    if ((int64_t)w.tiles.size() != (i1 - i0)) return false;
    m_block = block;
    ++m_blocks;
    return true;
}

const LevelData::Tile* FloorWindowCache::tileAt(int64_t i) const {
    if (!m_windowed) return nullptr;
    const int64_t nData = m_level->angleCount();
    if (i == nData) return m_hasExtra ? &m_extra : nullptr;
    if (i < 0 || i >= nData) return nullptr;
    const int64_t block = i / kCheckpointStride;
    if (m_block != block && !materialize(block)) return nullptr;
    const size_t k = (size_t)(i - block * kCheckpointStride);
    return k < m_scratch.tiles.size() ? &m_scratch.tiles[k] : nullptr;
}

bool FloorWindowCache::dropTiles(LevelData& level) {
    if (!m_ready || &level != m_level) return false;
    std::vector<LevelData::Tile>().swap(level.tiles);
    m_windowed = true;
    m_block = -1;
    return true;
}

void installFloorRecompute(FloorRecomputeFn fn) { g_floorRecompute = fn; }
FloorRecomputeFn floorRecompute() { return g_floorRecompute; }

}  // namespace adofai
