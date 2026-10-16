#include "archive/AdocaoWindow.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "archive/AdocaoBlocked.hpp"
#include "archive/AdocaoColumns.hpp"   // legacy 段的裸列解码（窗口化的退回路径）
#include "core/util/Logger.hpp"

namespace adofai {
namespace adocao {
namespace {

constexpr double kPiD = 3.14159265358979323846;
constexpr float  kPiF = 3.14159265f;

// `Timeline::totalAngleAt` + `durationAt` 的**生产路径**公式（逐字照抄，包括浮点类型与
// 常量 3.14159265f；`precalcTileRange` 是另一条 A/B 参考实现，别照抄那一份）。
// 第 i 层的 duration 由 tileIsCW / tileBPM / preAngleDir / preExtraRot 决定。
float rotationDuration(double rawAngleData, float preAngleDir, bool isCW, float currentBPM,
                       float extraRotation) {
    double relAngle;
    if (rawAngleData == 999.0) {
        relAngle = 0.0;
    } else {
        double delta = std::fmod((double)preAngleDir - rawAngleData, 360.0);
        if (delta < 0) delta += 360.0;
        if (!isCW) {
            relAngle = 360.0 - delta;
            if (relAngle >= 360.0) relAngle -= 360.0;
        } else {
            relAngle = delta;
        }
        if (delta < 0.0001) relAngle = 360.0;
    }
    float totalAngle = (float)relAngle * kPiF / 180.0f;
    if (isCW) totalAngle = -totalAngle;
    if (isCW) totalAngle -= extraRotation * 2.0f * kPiF;
    else      totalAngle += extraRotation * 2.0f * kPiF;
    const float rotationAmount = std::abs(totalAngle) / (2.0f * kPiF);
    return rotationAmount * 2.0f * (60.0f / currentBPM);
}

}  // namespace


// ---------------- FloorWindowSource：从容器里按块取数据 ----------------

namespace {

uint32_t rdU32w(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
uint64_t rdU64w(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
inline bool haveW(size_t n, size_t off, size_t k) { return off <= n && k <= n - off; }

// 从分块列里取出 [from, to) 这一段值（只解压覆盖到的那些帧）
template <typename T>
bool gatherTyped(const ColumnView& v, uint64_t from, uint64_t to, std::vector<T>& out,
                 uint64_t* bytesDecoded) {
    out.clear();
    if (to < from || to > v.total) return false;
    if (to == from) return true;
    if (!v.blocked) {
        std::vector<T> all;
        if (!decodeColumnV3(v.container, v.containerSize, all)) return false;
        if (all.size() < to) return false;
        out.assign(all.begin() + (size_t)from, all.begin() + (size_t)to);
        if (bytesDecoded) *bytesDecoded += v.containerSize;
        return true;
    }
    const uint64_t firstBlock = from / v.blockElems;
    const uint64_t lastBlock = (to - 1) / v.blockElems;
    out.reserve((size_t)(to - from));
    for (uint64_t b = firstBlock; b <= lastBlock; ++b) {
        std::vector<T> blk;
        if (!decodeColumnBlock(v, b, blk)) return false;
        const uint64_t blockFirst = b * v.blockElems;
        const uint64_t lo = (from > blockFirst) ? from - blockFirst : 0;
        const uint64_t hi = std::min<uint64_t>(blk.size(), to - blockFirst);
        if (lo > hi || hi > blk.size()) return false;
        out.insert(out.end(), blk.begin() + (size_t)lo, blk.begin() + (size_t)hi);
        // 记账：这一块所在帧的存储字节（近似"碰了多少"）
        if (bytesDecoded) {
            const uint8_t* e = v.frameTable + b / 0 * 0;   // 占位，下面用 locateBlock 拿真实帧
            (void)e;
            uint64_t frame = 0, inFrame = 0;
            if (v.locateBlock(b, frame, inFrame)) {
                const uint8_t* fe = v.frameTable + frame * kFrameEntry;
                *bytesDecoded += rdU64w(fe + 24);          // compSize
            }
        }
    }
    return out.size() == (size_t)(to - from);
}

bool gatherD(const ColumnView& v, uint64_t from, uint64_t to, std::vector<double>& out,
             uint64_t* bd) { return gatherTyped<double>(v, from, to, out, bd); }
bool gatherI(const ColumnView& v, uint64_t from, uint64_t to, std::vector<int64_t>& out,
             uint64_t* bd) { return gatherTyped<int64_t>(v, from, to, out, bd); }
bool gatherU(const ColumnView& v, uint64_t from, uint64_t to, std::vector<uint32_t>& out,
             uint64_t* bd) { return gatherTyped<uint32_t>(v, from, to, out, bd); }

}  // namespace

bool FloorWindowSource::open(const uint8_t* data, size_t length, std::string& err) {
    m_file = data;
    m_fileLen = length;
    std::string e;
    if (!getSectionView(data, length, (uint8_t)SectionId::AngleData, m_angleSec, e)) {
        err = "窗口化：取 AngleData 段失败：" + e;
        return false;
    }
    if (!getSectionView(data, length, (uint8_t)SectionId::Actions, m_actionsSec, e)) {
        err = "窗口化：取 Actions 段失败：" + e;
        return false;
    }
    (void)getSectionView(data, length, (uint8_t)SectionId::Derived, m_derivedSec, e);
    (void)getSectionView(data, length, (uint8_t)SectionId::ActionIndex, m_indexSec, e);
    // 字符串池：`atStates` 的动画名要用（格式 = u32 count + 每项 u32 len + bytes）
    if (getSectionView(data, length, (uint8_t)SectionId::StringPool, m_poolSec, e)) {
        const uint8_t* p = m_poolSec.p;
        const size_t n = m_poolSec.size;
        if (haveW(n, 0, 4)) {
            const uint32_t cnt = rdU32w(p);
            size_t off = 4;
            bool ok = cnt > 0 && cnt <= 1000000u;
            for (uint32_t i = 0; ok && i < cnt; ++i) {
                if (!haveW(n, off, 4)) { ok = false; break; }
                const uint32_t len = rdU32w(p + off);
                off += 4;
                if (!haveW(n, off, len)) { ok = false; break; }
                m_strings.emplace_back((const char*)p + off, len);
                off += len;
            }
            if (!ok) { err = "窗口化：字符串池解析失败"; return false; }
        }
    }

    // v3 的列容器（不是 legacy 裸列）才谈得上"按块寻址"
    const bool angleV3 = (m_angleSec.flags & kFlagLegacyColumns) == 0;
    if (!angleV3 || !openColumnV3(m_angleSec.p, m_angleSec.size, m_angleCol)) {
        m_why = "AngleData 段是 legacy 裸列（不是分块容器）——只能整列解出来";
        m_windowable = false;
    } else if (!m_angleCol.blocked) {
        m_why = "AngleData 列选了单块布局（分块它更大）——只能整列解出来";
        m_windowable = false;
    } else {
        m_windowable = true;
    }
    m_angleBlocked = m_angleCol.blocked;
    m_stride = m_angleBlocked ? m_angleCol.blockElems : kCheckpointStride;
    m_floors = m_angleCol.total;
    m_blocks = (m_floors + m_stride - 1) / m_stride;

    // Actions 段：u32 列数 + 每列 u64 长度 + 载荷（稀疏列前缀 = u32 mask + u64 used）
    m_actionsV3 = (m_actionsSec.flags & kFlagLegacyColumns) == 0;
    {
        const uint8_t* p = m_actionsSec.p;
        const size_t n = m_actionsSec.size;
        if (!haveW(n, 0, 4)) { err = "窗口化：Actions 段太短"; return false; }
        const uint32_t cols = rdU32w(p);
        if (cols != 6) { err = "窗口化：Actions 列数不是 6"; return false; }
        size_t off = 4;
        for (uint32_t i = 0; i < 6; ++i) {
            if (!haveW(n, off, 8)) { err = "窗口化：Actions 列长度越界"; return false; }
            const uint64_t len = rdU64w(p + off);
            off += 8;
            if (!haveW(n, off, (size_t)len)) { err = "窗口化：Actions 列越界"; return false; }
            const uint8_t* col = p + off;
            size_t colN = (size_t)len;
            off += (size_t)len;
            ActionColumn& c = m_cols[i];
            if (i == 2 || i == 4 || i == 5) {          // strId / val1 / val2 是稀疏列
                if (!haveW(colN, 0, 12)) { err = "窗口化：稀疏列前缀越界"; return false; }
                c.sparse = true;
                c.mask = rdU32w(col);
                c.used = rdU64w(col + 4);
                col += 12;
                colN -= 12;
            }
            if (m_actionsV3) {
                if (!openColumnV3(col, colN, c.view)) {
                    err = "窗口化：actions 第 " + std::to_string(i) + " 列容器解析失败";
                    return false;
                }
                c.blocked = c.view.blocked;
            } else {
                // legacy 段：整段解一次（退回路径）。仍然正确，只是少了这一侧的内存收益。
                c.whole.assign(col, col + colN);
                if (i == 0) {
                    if (!decodeIntColumn(c.whole.data(), c.whole.size(), c.wholeI)) {
                        err = "窗口化：legacy actions.floor 解码失败";
                        return false;
                    }
                } else {
                    if (!decodeU32Column(c.whole.data(), c.whole.size(), c.wholeU)) {
                        err = "窗口化：legacy actions 列 " + std::to_string(i) + " 解码失败";
                        return false;
                    }
                }
            }
        }
    }

    // Derived（checkpoint）与 ActionIndex
    if (m_derivedSec.size) {
        if (!decodeCheckpoints(m_derivedSec.p, m_derivedSec.size, m_ckStride, m_ck)) {
            err = "窗口化：Derived 段解析失败";
            return false;
        }
    }
    if (m_indexSec.size) {
        uint32_t st = 0;
        if (!decodeActionIndex(m_indexSec.p, m_indexSec.size, st, m_index)) {
            err = "窗口化：ActionIndex 段解析失败";
            return false;
        }
    }
    return true;
}

const FloorCheckpoint* FloorWindowSource::checkpointForBlock(uint64_t block) const {
    if (m_ck.empty() || m_ckStride == 0) return nullptr;
    const uint64_t i0 = block * m_stride;
    const uint64_t want = (i0 / m_ckStride) * m_ckStride;
    // m_ck 按 floor 递增（decodeCheckpoints 已校验），二分找 <= i0 的最后一条
    size_t lo = 0, hi = m_ck.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (m_ck[mid].floor <= want) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return nullptr;
    const FloorCheckpoint& c = m_ck[lo - 1];
    return (c.floor == want) ? &c : nullptr;      // 只接受正好对齐的那一条
}

bool FloorWindowSource::readBlock(uint64_t block, FloorBlock& out, std::string& err) {
    out = FloorBlock{};
    err.clear();
    if (block >= m_blocks) { err = "块号越界"; return false; }
    const uint64_t i0 = block * m_stride;
    const uint64_t i1 = std::min<uint64_t>(i0 + m_stride, m_floors);
    out.first = i0;
    out.count = (size_t)(i1 - i0);

    // ---- angleData：分块布局只解这一块的帧 ----
    if (m_angleBlocked) {
        if (!decodeColumnBlock(m_angleCol, block, out.angles)) {
            err = "窗口化：angleData 第 " + std::to_string(block) + " 块解码失败";
            return false;
        }
        uint64_t frame = 0, inFrame = 0;
        if (m_angleCol.locateBlock(block, frame, inFrame)) {
            m_bytesDecoded += rdU64w(m_angleCol.frameTable + frame * kFrameEntry + 24);
        }
    } else {
        if (m_angleWhole.empty() &&
            !decodeColumnV3(m_angleCol.container, m_angleCol.containerSize, m_angleWhole)) {
            err = "窗口化：angleData 整列解码失败";
            return false;
        }
        if (m_angleWhole.size() < i1) { err = "窗口化：angleData 长度不足"; return false; }
        out.angles.assign(m_angleWhole.begin() + (size_t)i0, m_angleWhole.begin() + (size_t)i1);
        m_bytesDecoded += m_angleCol.containerSize;
    }

    // ---- 事件：靠按层索引取 action 区间 ----
    uint64_t a0 = 0, a1 = 0;
    uint64_t uStr0 = 0, uStr1 = 0, uV10 = 0, uV11 = 0, uV20 = 0, uV21 = 0;
    if (!m_index.empty()) {
        if (block + 1 >= m_index.size()) { err = "窗口化：按层索引条数不足"; return false; }
        a0 = m_index[block].firstAction;
        a1 = m_index[block + 1].firstAction;
        uStr0 = m_index[block].usedStrId;  uStr1 = m_index[block + 1].usedStrId;
        uV10 = m_index[block].usedVal1;    uV11 = m_index[block + 1].usedVal1;
        uV20 = m_index[block].usedVal2;    uV21 = m_index[block + 1].usedVal2;
    } else {
        err = "窗口化：没有按层索引（写入方没写 or actions 不是按 floor 有序）";
        return false;
    }

    std::vector<int64_t> floors;
    std::vector<uint32_t> types, flags, strs, v1, v2;
    auto need = [&](int i) -> const ActionColumn& { return m_cols[i]; };
    const bool dbg = std::getenv("ADOCAO_WINDOW_DBG") != nullptr;
    auto diag = [&](int col, uint64_t f, uint64_t t, bool ok) {
        if (!dbg || ok) return;
        const ActionColumn& c = m_cols[col];
        std::fprintf(stderr, "[win] 取区间失败 col=%d [%llu,%llu) blocked=%d total=%llu "
                             "blocks=%llu\n", col, (unsigned long long)f, (unsigned long long)t,
                     (int)c.view.blocked, (unsigned long long)c.view.total,
                     (unsigned long long)(c.view.blocked
                         ? (c.view.total + c.view.blockElems - 1) / c.view.blockElems : 1));
    };
    // 稠密列：v3 容器按块取；legacy 段从"整段解出来的那份"里切（仍然正确）
    auto takeI = [&](int i, uint64_t f, uint64_t t, std::vector<int64_t>& out) {
        const ActionColumn& c = need(i);
        if (m_actionsV3) return gatherI(c.view, f, t, out, &m_bytesDecoded);
        if (t > c.wholeI.size() || t < f) return false;
        out.assign(c.wholeI.begin() + (size_t)f, c.wholeI.begin() + (size_t)t);
        return true;
    };
    auto takeU = [&](int i, uint64_t f, uint64_t t, std::vector<uint32_t>& out) {
        const ActionColumn& c = need(i);
        if (m_actionsV3) return gatherU(c.view, f, t, out, &m_bytesDecoded);
        if (t > c.wholeU.size() || t < f) return false;
        out.assign(c.wholeU.begin() + (size_t)f, c.wholeU.begin() + (size_t)t);
        return true;
    };
    if (!takeI(0, a0, a1, floors)) {
        diag(0, a0, a1, false); err = "窗口化：actions.floor 取区间失败"; return false;
    }
    if (!takeU(1, a0, a1, types)) {
        diag(1, a0, a1, false); err = "窗口化：actions.type 取区间失败"; return false;
    }
    if (!takeU(3, a0, a1, flags)) {
        diag(3, a0, a1, false); err = "窗口化：actions.flag 取区间失败"; return false;
    }
    if (need(2).sparse && !takeU(2, uStr0, uStr1, strs)) {
        err = "窗口化：actions.strId 取区间失败"; return false;
    }
    if (need(4).sparse && !takeU(4, uV10, uV11, v1)) {
        err = "窗口化：actions.val1 取区间失败"; return false;
    }
    if (need(5).sparse && !takeU(5, uV20, uV21, v2)) {
        err = "窗口化：actions.val2 取区间失败"; return false;
    }
    const size_t m = floors.size();
    if (types.size() != m || flags.size() != m) { err = "窗口化：actions 列长度不一致"; return false; }

    out.actions.resize(m);
    size_t ks = 0, k1 = 0, k2 = 0;
    for (size_t i = 0; i < m; ++i) {
        LevelData::FastAction& a = out.actions[i];
        const uint32_t t = types[i];
        const uint32_t bit = 1u << (t & 31u);
        a.floor = (int)floors[i];
        a.type = (LevelData::FastAction::Type)(uint8_t)t;
        a.strId = (need(2).mask & bit) ? (uint16_t)strs[ks++] : 0;
        a.flag = (flags[i] != 0);
        const uint32_t x1 = (need(4).mask & bit) ? v1[k1++] : 0u;
        const uint32_t x2 = (need(5).mask & bit) ? v2[k2++] : 0u;
        std::memcpy(&a.val1, &x1, 4);
        std::memcpy(&a.val2, &x2, 4);
    }
    if (ks != strs.size() || k1 != v1.size() || k2 != v2.size()) {
        err = "窗口化：actions 稀疏列条数与掩码不符";
        return false;
    }

    if (const FloorCheckpoint* ck = checkpointForBlock(block)) {
        out.checkpoint = *ck;
        out.hasCheckpoint = true;
    }
    return true;
}

float checkpointPrevDirection(const FloorCheckpoint& ck) {
    // `calculateTilePositions`：999 → floats[i] = floats[i-1] + 180（沿用上一个"有效"方向）。
    // 所以 i0-1 的有效方向 = 最后一个真角度 + 180 × 紧跟其后的 999 个数。
    return (float)(ck.lastAngleDeg + 180.0 * (double)ck.sentinelRun);
}

bool computeFloorWindow(const LevelData::Settings& settings, const FloorWindowInput& in,
                        FloorWindow& out, std::string& err) {
    // ③ 切片 4：**不要** `out = FloorWindow{}` —— 那会把 vector 的容量扔掉，而"按块物化"
    // 会反复调用本函数（MYC 1654 块 + 运行期），每次重新分配就是纯粹的 churn（实测 footprint
    // 反涨到 1.1 GB 而 live 只有 150 MB）。改成显式复位 + `assign`（容量保留）。
    out.first = 0;
    out.hasAtStates = false;
    out.atStates.clear();
    err.clear();
    if (!in.angles || !in.events) { err = "computeFloorWindow 缺输入（angles/events）"; return false; }
    const FloorCheckpoint& ck = in.checkpoint;
    const uint32_t need = kCkBase | kCkCum | kCkBpm | kCkTime | kCkCw | kCkAngleDir;
    if ((ck.flags & need) != need) {
        err = "checkpoint 缺分量（窗口续算需要的 base/cum/bpm/time/cw/angleDir 不齐）";
        return false;
    }
    const size_t n = in.angles->size();
    if (n == 0) return true;
    const uint64_t i0 = ck.floor;
    if (n > kCheckpointStride) { err = "一个窗口不能超过一个 stride（分块边界就是窗口边界）"; return false; }

    out.first = i0;
    out.tiles.assign(n, LevelData::Tile{});
    out.tileBPMs.assign(n, 0.0f);
    out.tileStartTimes.assign(n, 0.0);
    out.tileHasTwirl.assign(n, false);
    out.tileHasSetSpeed.assign(n, false);
    out.atStates.clear();
    out.hasAtStates = false;

    // 事件按 floor 稳定排序（同层保持文件顺序 —— 与 processActions/Timeline 的迭代顺序一致）。
    // PositionTrack 是"同层最后一个胜出"（`tilePositionOffsets[floor] = {...}` 覆盖），
    // 而 Twirl/SetSpeed/Pause 是逐个生效 —— 所以下面按类型分开处理，别混成"同层累加"。
    std::vector<const LevelData::FastAction*> ev;
    ev.reserve(in.events->size());
    for (const LevelData::FastAction& a : *in.events) {
        if (a.floor < 0) continue;
        const uint64_t f = (uint64_t)a.floor;
        if (f >= i0 && f < i0 + n) ev.push_back(&a);
    }
    std::stable_sort(ev.begin(), ev.end(),
                     [](const LevelData::FastAction* a, const LevelData::FastAction* b) {
                         return a->floor < b->floor;
                     });

    // ---- 起点状态（全部来自快照；位置**从快照重新生成**，绝不原地累加）----
    double baseX = ck.baseX, baseY = ck.baseY;
    double cumX = ck.cumX, cumY = ck.cumY;
    bool isCW = ck.cw != 0;
    float currentBPM = (float)ck.bpm;
    float angleDir = (float)ck.angleDirDeg;      // "进第 i0 层"的方向（ADOFAI-JS 语义）
    double totalTime = ck.timeSec;

    // 上一层（i0-1）的**有效**方向：999 连跑时由 lastAngleDeg + sentinelRun 推回来
    float prevDir = (ck.flags & kCkLastAngle) ? checkpointPrevDirection(ck) : 0.0f;

    size_t ei = 0;
    for (size_t k = 0; k < n; ++k) {
        const uint64_t i = i0 + k;
        const double raw = (*in.angles)[k];

        // ---- ① 本层事件（floor == i）：位置 / 转向 / BPM / Pause ----
        double px = 0.0, py = 0.0;               // 点加（justThisTile，只作用于本层）
        float extraRotation = 0.0f;              // Pause：val1/2
        // Twirl/SetSpeed/Pause 逐个生效；**PositionTrack 同层最后一个胜出**
        // （processActions 里是 `tilePositionOffsets[floor] = {…}`，同层会互相覆盖）。
        const LevelData::FastAction* lastPos = nullptr;
        while (ei < ev.size() && (uint64_t)ev[ei]->floor == i) {
            const LevelData::FastAction& a = *ev[ei];
            switch (a.type) {
            case LevelData::FastAction::Twirl:
                isCW = !isCW;
                out.tileHasTwirl[k] = true;
                break;
            case LevelData::FastAction::SetSpeed:
                out.tileHasSetSpeed[k] = true;
                if (a.flag) currentBPM *= a.val1; else currentBPM = a.val1;
                break;
            case LevelData::FastAction::Pause:   extraRotation += a.val1 / 2.0f; break;
            case LevelData::FastAction::PositionTrack: lastPos = &a; break;
            case LevelData::FastAction::AnimateTrack: {
                // 与 `LevelData::processActions` 逐字同一条表达式（缺省值全来自 settings）。
                // 键用**相对窗口起点**的层号 —— 窗口的消费方按窗口索引寻址。
                const std::vector<std::string>* strs = in.strings;
                const std::string& s0 = (strs && a.strId < strs->size()) ? (*strs)[a.strId]
                                                                        : std::string();
                out.hasAtStates = true;
                out.atStates[(int)k] = {s0.empty() ? settings.trackDisappearAnimation : s0,
                                        settings.trackAnimation,
                                        a.val1 >= 0 ? a.val1 : settings.beatsBehind,
                                        a.val2 >= 0 ? a.val2 : settings.beatsAhead,
                                        a.flag};
                break;
            }
            default: break;
            }
            ++ei;
        }
        if (lastPos) {
            if (lastPos->flag) { px += lastPos->val1; py += lastPos->val2; }   // 点加：只本层
            else { cumX += lastPos->val1; cumY += lastPos->val2; }            // 继承：本层及之后
        }

        // ---- ② 有效方向（999 → 上一层 + 180）----
        const float dir = (raw == 999.0) ? (prevDir + 180.0f) : (float)raw;

        // ---- ③ 位置：base（从快照续走）+ cum + 点加 ----
        // 结合顺序必须与 `applyPositionTrackOffsets` 的 `position += cumX + px` 完全一致。
        out.tiles[k].position = {baseX + (cumX + px), baseY + (cumY + py)};
        out.tiles[k].direction = dir;

        // ---- ④ BPM / 时间（Timeline Phase 1 的顺序：本层事件处理完之后再取状态）----
        out.tileBPMs[k] = currentBPM;
        out.tileStartTimes[k] = totalTime;
        totalTime += (double)rotationDuration(raw, angleDir, isCW, currentBPM, extraRotation);

        // ---- ⑤ 推进：angleDir（下一层的"角度方向"）、prevDir、base ----
        if (raw == 999.0) {
            // 与 Phase 1 的收尾更新同序：往回找最后一个非 999，angleDir = real + (minus-1)*180
            // 这里 k 是窗口内的下标，往回找只可能落在窗口内（窗口起点由 prevDir 兜住）。
            int minus = 1;
            while ((int64_t)k - minus >= 0 && (*in.angles)[k - minus] == 999.0) ++minus;
            if ((int64_t)k - minus >= 0) {
                const double realAngle = (*in.angles)[k - minus];
                angleDir = (float)std::fmod(realAngle + (minus - 1) * 180.0, 360.0);
            } else {
                angleDir = (float)std::fmod((double)ck.lastAngleDeg + (double)ck.sentinelRun * 180.0
                                            + (minus - 1) * 180.0, 360.0);
            }
        } else {
            angleDir = (float)std::fmod(raw + 180.0, 360.0);
        }
        if (angleDir < 0) angleDir += 360.0f;

        prevDir = dir;
        const double rad = (double)dir * kPiD / 180.0;
        baseX += std::cos(rad);
        baseY += std::sin(rad);
    }
    (void)settings;
    return true;
}

}  // namespace adocao
}  // namespace adofai
