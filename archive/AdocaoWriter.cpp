#include "archive/AdocaoWriter.hpp"

#include <cstring>

#include "archive/AdocaoBlocked.hpp"
#include "core/util/Logger.hpp"
#include "archive/AdocaoCheckpoints.hpp"
#include "archive/AdocaoColumns.hpp"
#include "core/level/LevelData.hpp"

#include <zstd.h>   // 段级压缩（archive 的 PRIVATE 依赖；实测真实文件上 zstd 比 xz 更小）

namespace adofai {
namespace adocao {
namespace {

void putU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void putBytes(std::vector<uint8_t>& b, const void* p, size_t n) {
    const uint8_t* q = (const uint8_t*)p;
    b.insert(b.end(), q, q + n);
}
void putF32(std::vector<uint8_t>& b, float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, 4);          // 位模式原样：float 列必须逐位无损
    putU32(b, x);
}

// 载荷列按类型稀疏：只写"该类型真的用到这一列"的事件，其余读侧取默认 0。
// 判据是**自描述的**：某类型在这一列出现过非默认值 → 该类型进掩码（不靠列名/语义硬编码）。
uint32_t sparseMask(const std::vector<uint32_t>& types, const std::vector<uint32_t>& col) {
    uint32_t mask = 0;
    for (size_t i = 0; i < types.size(); ++i)
        if (col[i] != 0u) mask |= (1u << (types[i] & 31u));
    return mask;
}

// 内容指纹：FNV-1a-64（算法写进 docs/adocao-format.md §2.1，别的实现可逐字节复现）。
// 覆盖**未压缩**的各段内容 + 段 id，所以与压缩器、与构建都无关（可跨实现比对）。
uint64_t fnv1a64(const void* data, size_t n, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// 构建期注入的 git commit（40 位十六进制）→ 20 原始字节；没有/不合法就写全 0。
// 读取方**不看**这个字段，它只作溯源 —— 绝不能当兼容闸门。
void writerCommitBytes(uint8_t out[20]) {
    std::memset(out, 0, 20);
#ifdef ADOCAO_GIT_COMMIT
    const char* s = ADOCAO_GIT_COMMIT;
    if (std::strlen(s) != 40) return;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 20; ++i) {
        const int hi = hex(s[i * 2]);
        const int lo = hex(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) { std::memset(out, 0, 20); return; }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
#endif
}

// 段级 zstd 的**决策缓存**：决策阶段（plain 是否比 packed 小）与组装阶段用的是同一份
// blob —— 之前会各压一次 zstd-19（小谱走 plain 是常态，Actions 段直接双倍 CPU）。
// 缓存按 (data,size) 命中；thread_local + packLevel 开头 reset，避免跨调用/跨线程。
namespace {
struct ZEntry {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool smaller = false;          // zstd 确实更小 → z 有效
    std::vector<uint8_t> z;
};
struct ZCache {
    std::vector<ZEntry> v;
    void reset() { v.clear(); }
    const ZEntry* find(const std::vector<uint8_t>& b) const {
        for (const ZEntry& e : v)
            if (e.data == b.data() && e.size == b.size()) return &e;
        return nullptr;
    }
    size_t storedBytes(const std::vector<uint8_t>& b) {
        if (b.size() < 256) return b.size();
        if (const ZEntry* e = find(b)) return e->smaller ? e->z.size() : b.size();
        ZEntry e;
        e.data = b.data(); e.size = b.size();
        e.z.resize(ZSTD_compressBound(b.size()));
        const size_t got = ZSTD_compress(e.z.data(), e.z.size(), b.data(), b.size(), 19);
        if (!ZSTD_isError(got) && got < b.size()) { e.z.resize(got); e.smaller = true; }
        else e.z.clear();
        v.push_back(std::move(e));
        return v.back().smaller ? v.back().z.size() : b.size();
    }
    // true = 决策已覆盖该 blob（out 非空表示用压缩结果，空表示明确存原样）
    bool take(const std::vector<uint8_t>& b, std::vector<uint8_t>& out) const {
        const ZEntry* e = find(b);
        if (!e) return false;
        if (e->smaller) out = e->z;
        return true;
    }
};
thread_local ZCache g_zcache;
}  // namespace

// 段级压缩后的实际存储字节（与下面的段级 zstd 决策同一口径）
size_t sectionStoredBytes(const std::vector<uint8_t>& blob) {
    return g_zcache.storedBytes(blob);
}

// 报告里的布局名（"为什么这列大/小"必须一眼看得出来）
std::string layoutName(const ColumnLayoutStats& st, bool sectionPacked) {
    if (!sectionPacked) return "LegacyV2";              // 与 v2 逐字节等价：裸列字节 + 段级 zstd
    if (st.forced) return "Blocked(forced)";
    return st.blocked ? "Blocked" : "Single+zstd";
}

struct Section {
    uint8_t id = 0;
    uint16_t flags = 0;
    uint64_t elements = 0;
    std::vector<uint8_t> blob;
    // true = blob 已经是**最终存储字节**（列容器自己压过：帧就是 seek 的单位，§8.3），
    // 段级不再包一层 zstd。
    bool preStored = false;
};

// 字符串池：v[0] 必须是空串（strId = 0 的约定）。actionStrTable 的**原序**必须保留
// （strId 就是它的下标），settings 用到的字符串按固定顺序追加到末尾。
struct Pool {
    std::vector<std::string> v;
    uint16_t add(const std::string& s) {
        if (s.empty()) return 0;
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i] == s) return (uint16_t)i;
        v.push_back(s);
        return (uint16_t)(v.size() - 1);
    }
};

// settings 的定长记录（字段顺序固定 → 打包确定性）。字符串走池下标；未知键与今天的行为一致
// （两条解析路径本来就只读已知字段），原样保留原始字节的 `preserved` 段留给后续版本。
void writeSettings(const LevelData::Settings& s, Pool& pool, std::vector<uint8_t>& b) {
    putU32(b, (uint32_t)s.version);
    putF32(b, s.bpm);
    putF32(b, s.offset);
    putU32(b, (uint32_t)s.countdownTicks);
    putF32(b, s.zoom);
    putF32(b, s.rotation);
    putU16(b, pool.add(s.relativeTo));
    putF32(b, s.position[0]);
    putF32(b, s.position[1]);
    putU16(b, pool.add(s.hitsound));
    putF32(b, s.hitsoundVolume);
    putU16(b, pool.add(s.trackColor));
    putU16(b, pool.add(s.secondaryTrackColor));
    putU16(b, pool.add(s.backgroundColor));
    putU8(b, s.stickToFloors ? 1 : 0);
    putU16(b, pool.add(s.planetEase));
    putU16(b, pool.add(s.trackDisappearAnimation));
    putU16(b, pool.add(s.trackAnimation));
    putF32(b, s.beatsBehind);
    putF32(b, s.beatsAhead);
    // 2026-10-17 追加（**末尾**，所以读侧既能读新文件也能读旧文件；旧读者会忽略尾部多余字节）：
    // `.adofai` 的 `"song"`，ADOCAO-E 的"另存/重打包"需要它。
    putU16(b, pool.add(s.songFilename));
    // 运镜要用的两个 settings 字段（同样在末尾追加，缺省 = 旧文件 → 保留调用方默认值）：
    // pitch 同时缩放事件时长与跟随速度；legacyCamRelativeTo 是老语义开关。
    putF32(b, s.pitch);
    putU8(b, s.legacyCamRelativeTo ? 1 : 0);
    // 2026-10-17 砖色：样式/颜色类型/脉冲 + 三个数值（全部追加在末尾，旧文件读不到就保留默认）。
    putU16(b, pool.add(s.trackStyle));
    putU16(b, pool.add(s.trackColorType));
    putU16(b, pool.add(s.trackColorPulse));
    putF32(b, s.trackColorAnimDuration);
    putF32(b, s.trackPulseLength);
    putF32(b, s.trackGlowIntensity);
}

}  // namespace

bool packLevel(const LevelData& level, std::vector<uint8_t>& out, std::string& err,
               PackResult* result, const PackOptions& opts) {
    g_zcache.reset();   // 每次打包独立的决策缓存（同 blob 只压一次）

    out.clear();
    err.clear();
    PackResult rep;

    // ---- ① settings（先做：它会往字符串池里追加 settings 的字符串）----
    Pool pool;
    pool.v = level.actionStrTable;
    if (pool.v.empty()) pool.v.emplace_back();
    std::vector<uint8_t> settingsBlob;
    writeSettings(level.settings, pool, settingsBlob);

    // 相机/重复事件的 tag 也要进字符串池 —— **必须在 poolBlob 之前**加，
    // 否则池已经序列化完，段里的下标会越界。
    std::vector<uint16_t> camTagIdx(level.cameraEvents.size());
    for (size_t i = 0; i < level.cameraEvents.size(); ++i)
        camTagIdx[i] = pool.add(level.cameraEvents[i].tag);
    std::vector<uint16_t> mvTagIdx(level.moveTrackEvents.size());
    for (size_t i = 0; i < level.moveTrackEvents.size(); ++i)
        mvTagIdx[i] = pool.add(level.moveTrackEvents[i].tag);
    std::vector<uint16_t> repTagIdx(level.repeatEvents.size());
    for (size_t i = 0; i < level.repeatEvents.size(); ++i)
        repTagIdx[i] = pool.add(level.repeatEvents[i].tag);
    struct ColorIdx { uint16_t type, color, secondary, pulse, style; };
    std::vector<ColorIdx> colIdx(level.colorEvents.size());
    for (size_t i = 0; i < level.colorEvents.size(); ++i) {
        const auto& c = level.colorEvents[i].cfg;
        colIdx[i] = {pool.add(c.colorType), pool.add(c.color), pool.add(c.secondary),
                     pool.add(c.pulse), pool.add(c.style)};
    }

    // ---- ② 字符串池（原序保留 actionStrTable，末尾是 settings 追加的）----
    std::vector<uint8_t> poolBlob;
    putU32(poolBlob, (uint32_t)pool.v.size());
    for (const std::string& s : pool.v) {
        putU32(poolBlob, (uint32_t)s.size());
        putBytes(poolBlob, s.data(), s.size());
    }

    // ---- ③④ 列段：**两种候选都真编一遍，按"实际存储字节"取最小** ----
    //
    //   plain  = 各列 SingleRaw + **段级** zstd  —— 与 v2 逐字节等价（回归兜底）
    //   packed = 各列在 SingleZstd / Blocked 里取小，段级不再压（帧 = seek 单位，§8.3）
    //
    // 为什么决策放在**段级**而不是列级：v2 是把整个 Actions 段（6 列一起）压成一帧，
    // 跨列共享熵表、少 6 个帧头，实测比"每列各压一帧"更省（The Moon 3544 B vs 3992 B）。
    // 两个候选都真编出来取小的那个，于是 **v3 的文件永远 ≤ v2 的文件**（§8.0-2）。
    std::vector<uint8_t> angleBlob, actionsBlob;
    std::vector<ColumnLayoutStats> angleStats, actionStats;
    bool anglePacked = false, actionsPacked = false;
    // ---- ⑤' ActionIndex：按层索引（③）----
    // 每 4096 层一条 `firstAction` = 第一个 floor >= 块起点的 action 下标（末尾多一条哨兵 =
    // actions.size()）。于是"第 b 块里的事件" = actions[first[b] .. first[b+1])，**不用扫全谱**。
    // 前提：actions 按 floor **非递减**（写入方校验；不成立就不写这一段，读者必须明确知道）。
    // 声明在这里、计算在下面的 actions 块里（那里才有 `floors`）、组装段时用。
    std::vector<uint8_t> actionIndexBlob;
    bool actionIndexOk = false;
    {
        // ---- angleData：一列 double（字典是主编码）----
        std::vector<uint8_t> anglePlain, anglePackedBytes;
        ColumnLayoutStats stPlain{}, stPacked{};
        if (!encodeColumnV3(ColKind::Double, level.angleData.data(), level.angleData.size(),
                            ColumnStorage::LegacyV2, anglePlain, &stPlain) ||
            !encodeColumnV3(ColKind::Double, level.angleData.data(), level.angleData.size(),
                            ColumnStorage::BestSelf, anglePackedBytes, &stPacked)) {
            err = "angleData 列编码失败（内部错误）";
            return false;
        }
        (void)stPlain;
        const size_t plainStored = sectionStoredBytes(anglePlain);
        // 强制分块的列**必须**让整段走 packed：否则列级白算一通，段级又落回 LegacyV2，
        // ③ 拿到的还是"不可寻址"的段（2026-10-15 真踩过：文件大小一模一样，报告却说强制分块）
        if (stPacked.forced || anglePackedBytes.size() < plainStored) {
            angleBlob = std::move(anglePackedBytes);
            anglePacked = true;
        } else {
            angleBlob = std::move(anglePlain);
            anglePacked = false;
        }
        // 报告**一律用 packed 那次**的统计：它同时含"单块+自压"与"分块"两档数字，
        // 于是"分块到底赚了还是亏了"在任何情况下都看得见（LegacyV2 那次不尝试分块）。
        angleStats.push_back(stPacked);
    }
    {
        // ---- actions：列式（floor 差分、type/strId/flag 位打包、val1/val2 按 float 位模式）----
        const size_t n = level.actions.size();
        std::vector<int64_t> floors;
        std::vector<uint32_t> types, strIds, flags, v1, v2;
        floors.reserve(n); types.reserve(n); strIds.reserve(n); flags.reserve(n); v1.reserve(n); v2.reserve(n);
        for (const LevelData::FastAction& a : level.actions) {
            floors.push_back((int64_t)a.floor);
            types.push_back((uint32_t)a.type);
            strIds.push_back((uint32_t)a.strId);
            flags.push_back(a.flag ? 1u : 0u);
            uint32_t x = 0;
            std::memcpy(&x, &a.val1, 4); v1.push_back(x);
            std::memcpy(&x, &a.val2, 4); v2.push_back(x);
        }
        // 稀疏列（strId/val1/val2）：只写"该类型真的用到这一列"的事件；掩码自描述
        auto sparseUsed = [&](const std::vector<uint32_t>& col, uint32_t& mask) {
            mask = sparseMask(types, col);
            std::vector<uint32_t> used;
            used.reserve(col.size() / 4 + 16);
            for (size_t i = 0; i < col.size(); ++i)
                if (mask & (1u << (types[i] & 31u))) used.push_back(col[i]);
            return used;
        };
        uint32_t mStr = 0, mV1 = 0, mV2 = 0;
        const std::vector<uint32_t> usedStr = sparseUsed(strIds, mStr);
        const std::vector<uint32_t> usedV1  = sparseUsed(v1, mV1);
        const std::vector<uint32_t> usedV2  = sparseUsed(v2, mV2);

        struct ColPlan {
            const char* name;
            ColKind kind;
            const void* data;
            size_t count;
            size_t rawBytes;      // 朴素表示（报告用）
            bool sparse;
            uint32_t mask;
            uint64_t usedCount;
        };
        const ColPlan plans[] = {
            {"actions.floor", ColKind::Int64, floors.data(), floors.size(), floors.size() * 8, false, 0, 0},
            {"actions.type",  ColKind::U32,   types.data(),  types.size(),  types.size() * 4,  false, 0, 0},
            {"actions.strId", ColKind::U32,   usedStr.data(), usedStr.size(), strIds.size() * 4, true, mStr, usedStr.size()},
            {"actions.flag",  ColKind::U32,   flags.data(),  flags.size(),  flags.size() * 4,  false, 0, 0},
            {"actions.val1",  ColKind::U32,   usedV1.data(), usedV1.size(), v1.size() * 4,      true, mV1, usedV1.size()},
            {"actions.val2",  ColKind::U32,   usedV2.data(), usedV2.size(), v2.size() * 4,      true, mV2, usedV2.size()},
        };

        auto buildActions = [&](ColumnStorage mode, std::vector<uint8_t>& dst,
                                std::vector<ColumnLayoutStats>* stats) -> bool {
            dst.clear();
            putU32(dst, 6);                      // 列数
            for (const ColPlan& c : plans) {
                std::vector<uint8_t> enc; ColumnLayoutStats st;
                if (!encodeColumnV3(c.kind, c.data, c.count, mode, enc, &st)) return false;
                std::vector<uint8_t> col;
                if (c.sparse) { putU32(col, c.mask); putU64(col, c.usedCount); }
                putBytes(col, enc.data(), enc.size());
                putU64(dst, col.size());         // u64：允许单列 > 4 GB
                putBytes(dst, col.data(), col.size());
                if (stats) stats->push_back(st);
            }
            return true;
        };

        std::vector<uint8_t> actPlain, actPacked;
        std::vector<ColumnLayoutStats> statsPlain, statsPacked;
        if (!buildActions(ColumnStorage::LegacyV2, actPlain, &statsPlain) ||
            !buildActions(ColumnStorage::BestSelf, actPacked, &statsPacked)) {
            err = "actions 列编码失败（内部错误）";
            return false;
        }
        bool anyForced = false;
        for (const ColumnLayoutStats& cst : statsPacked) anyForced = anyForced || cst.forced;
        if (anyForced || actPacked.size() < sectionStoredBytes(actPlain)) {
            actionsBlob = std::move(actPacked);
            actionsPacked = true;
        } else {
            actionsBlob = std::move(actPlain);
            actionsPacked = false;
        }
        (void)statsPlain;
        actionStats = std::move(statsPacked);   // 同上：报告用 packed 那次的统计
        // 按层索引（③）：声明在外层，这里只填内容
        if (!floors.empty()) {
        bool sortedByFloor = true;
        for (size_t i = 1; i < floors.size(); ++i)
            if (floors[i] < floors[i - 1]) { sortedByFloor = false; break; }
        if (sortedByFloor) {
            const uint64_t nBlocks =
                (level.angleData.size() + kCheckpointStride - 1) / kCheckpointStride;
            std::vector<ActionIndexEntry> entries;
            entries.reserve((size_t)nBlocks + 1);
            size_t cur = 0, scan = 0;
            uint64_t uStr = 0, uV1 = 0, uV2 = 0;
            for (uint64_t b = 0; b <= nBlocks; ++b) {
                const int64_t start = (int64_t)(b * kCheckpointStride);
                while (cur < floors.size() && floors[cur] < start) ++cur;
                // 稀疏列的 used 游标推到 cur：第 a 个 action 之前的 used 数（按类型掩码数）
                while (scan < cur) {
                    const uint32_t bit = 1u << (types[scan] & 31u);
                    if (mStr & bit) ++uStr;
                    if (mV1 & bit) ++uV1;
                    if (mV2 & bit) ++uV2;
                    ++scan;
                }
                entries.push_back({(uint64_t)cur, uStr, uV1, uV2});
            }
            encodeActionIndex(entries, kCheckpointStride, actionIndexBlob);
            actionIndexOk = true;
        } else {
            LOG_W("adocao pack: actions 不是按 floor 非递减的 —— 不写按层索引（窗口化要退回全扫）");
        }
    }

        (void)n;
    }
    // 报告：angleData 在前，然后按 actions 的固定列序（顺序必须稳定，--codec-report 靠它对照）
    {
        const ColumnLayoutStats& st = angleStats[0];
        rep.columns.push_back({"angleData", layoutName(st, anglePacked),
                               (uint64_t)level.angleData.size(),
                               (uint64_t)level.angleData.size() * 8,
                               (uint64_t)angleBlob.size(), 0, 0,
                               st.blocked, st.forced, st.blocks, st.frames,
                               st.singleBytes, st.blockedBytes});
    }
    {
        static const char* kNames[] = {"actions.floor", "actions.type", "actions.strId",
                                       "actions.flag", "actions.val1", "actions.val2"};
        const uint64_t elems[] = {(uint64_t)level.actions.size(), (uint64_t)level.actions.size(),
                                  (uint64_t)actionStats.size() ? (uint64_t)level.actions.size() : 0,
                                  (uint64_t)level.actions.size(), (uint64_t)level.actions.size(),
                                  (uint64_t)level.actions.size()};
        const uint64_t rawPer[] = {(uint64_t)level.actions.size() * 8, (uint64_t)level.actions.size() * 4,
                                   (uint64_t)level.actions.size() * 4, (uint64_t)level.actions.size() * 4,
                                   (uint64_t)level.actions.size() * 4, (uint64_t)level.actions.size() * 4};
        for (size_t i = 0; i < 6 && i < actionStats.size(); ++i) {
            const ColumnLayoutStats& st = actionStats[i];
            rep.columns.push_back({kNames[i], layoutName(st, actionsPacked),
                                   elems[i], rawPer[i], st.blocked ? st.blockedBytes : st.singleBytes,
                                   0, 0,
                                   st.blocked, st.forced, st.blocks, st.frames,
                                   st.singleBytes, st.blockedBytes});
        }
    }

    // ---- 组装段 ----
    std::vector<Section> secs;
    {
        Section s; s.id = (uint8_t)SectionId::Settings; s.blob = std::move(settingsBlob); secs.push_back(std::move(s));
    }
    {
        Section s; s.id = (uint8_t)SectionId::StringPool; s.elements = pool.v.size();
        s.blob = std::move(poolBlob); secs.push_back(std::move(s));
    }
    {
        Section s; s.id = (uint8_t)SectionId::AngleData; s.elements = level.angleData.size();
        s.preStored = anglePacked;
        if (!anglePacked) s.flags |= kFlagLegacyColumns;
        s.blob = std::move(angleBlob); secs.push_back(std::move(s));
    }
    {
        Section s; s.id = (uint8_t)SectionId::Actions; s.elements = level.actions.size();
        s.preStored = actionsPacked;
        if (!actionsPacked) s.flags |= kFlagLegacyColumns;
        s.blob = std::move(actionsBlob); secs.push_back(std::move(s));
    }
    // ---- ⑤ Derived：每 4096 层的续算快照（§8.8 ⑧）----
    // 默认不写（见 PackOptions 的说明）；写的时候用**与生产路径同一套算式**算，所以
    // "从快照重算 = 全谱一次算"能逐位对上（§8.10⑥ 的对拍就是这么验的）。
    if (opts.withCheckpoints) {
        std::vector<FloorCheckpoint> ck;
        std::string cerr_;
        if (!computeCheckpoints(level, kCheckpointStride, ck, cerr_)) {
            err = "checkpoint 计算失败: " + cerr_;
            return false;
        }
        Section s; s.id = (uint8_t)SectionId::Derived; s.elements = ck.size();
        if (!encodeCheckpoints(ck, kCheckpointStride, s.blob)) {
            err = "checkpoint 编码失败";
            return false;
        }
        rep.checkpoints = ck.size();
        secs.push_back(std::move(s));
    }
    if (actionIndexOk) {
        Section s; s.id = (uint8_t)SectionId::ActionIndex; s.elements = 0;
        s.blob = std::move(actionIndexBlob);
        secs.push_back(std::move(s));
    }
    // ---- 相机 / 重复事件：罕见但**必须无损往返**（否则 .adocao 里的运镜会整段消失）----
    // 定长记录、字段顺序固定（打包确定性）；tag 走字符串池下标。order 不存 —— 它 = 记录下标。
    if (!level.cameraEvents.empty()) {
        Section s; s.id = (uint8_t)SectionId::Camera; s.elements = level.cameraEvents.size();
        s.blob.reserve(level.cameraEvents.size() * 38);
        for (size_t i = 0; i < level.cameraEvents.size(); ++i) {
            const LevelData::CameraEvent& e = level.cameraEvents[i];
            putU32(s.blob, (uint32_t)e.floor);
            putU32(s.blob, e.id);
            putF32(s.blob, e.angleOffset);
            putF32(s.blob, e.duration);
            putF32(s.blob, e.x);
            putF32(s.blob, e.y);
            putF32(s.blob, e.rotation);
            putF32(s.blob, e.zoom);
            putU8(s.blob, e.ease);
            putU8(s.blob, e.relativeTo);
            putU8(s.blob, e.flags);
            putU8(s.blob, e.disabled);
            putU16(s.blob, camTagIdx[i]);
        }
        secs.push_back(std::move(s));
    }
    if (!level.repeatEvents.empty()) {
        Section s; s.id = (uint8_t)SectionId::Repeat; s.elements = level.repeatEvents.size();
        s.blob.reserve(level.repeatEvents.size() * 19);
        for (size_t i = 0; i < level.repeatEvents.size(); ++i) {
            const LevelData::RepeatEvent& r = level.repeatEvents[i];
            putU32(s.blob, (uint32_t)r.floor);
            putF32(s.blob, r.repetitions);
            putF32(s.blob, r.interval);
            putF32(s.blob, r.gapLength);
            putU8(s.blob, r.flags);
            putU16(s.blob, repTagIdx[i]);
        }
        secs.push_back(std::move(s));
    }
    if (!level.colorEvents.empty()) {
        Section s; s.id = (uint8_t)SectionId::Colour; s.elements = level.colorEvents.size();
        s.blob.reserve(level.colorEvents.size() * 50);
        for (size_t i = 0; i < level.colorEvents.size(); ++i) {
            const LevelData::ColorEvent& e = level.colorEvents[i];
            putU32(s.blob, (uint32_t)e.floor);
            putU16(s.blob, colIdx[i].type);
            putU16(s.blob, colIdx[i].color);
            putU16(s.blob, colIdx[i].secondary);
            putU16(s.blob, colIdx[i].pulse);
            putU16(s.blob, colIdx[i].style);
            putF32(s.blob, e.cfg.animDuration);
            putF32(s.blob, e.cfg.pulseLength);
            putF32(s.blob, e.cfg.glowIntensity);
            putF32(s.blob, e.duration);
            putU8(s.blob, e.ease);
            putU8(s.blob, e.flags);
            // v4 追加：RecolorTrack 的范围/触发（ColorTrack 事件这些字段为 0）
            putU32(s.blob, (uint32_t)e.startOff);
            putU32(s.blob, (uint32_t)e.endOff);
            putU8(s.blob, (uint8_t)e.startRel);
            putU8(s.blob, (uint8_t)e.endRel);
            putU32(s.blob, (uint32_t)e.gapLength);
            putF32(s.blob, e.angleOffset);
        }
        secs.push_back(std::move(s));
    }
    if (!level.flashEvents.empty()) {
        Section s; s.id = (uint8_t)SectionId::Flash; s.elements = level.flashEvents.size();
        s.blob.reserve(level.flashEvents.size() * 32);
        for (const LevelData::FlashEvent& e : level.flashEvents) {
            putU32(s.blob, (uint32_t)e.floor);
            putF32(s.blob, e.duration);
            putF32(s.blob, e.angleOffset);
            putU8(s.blob, e.plane);
            putU8(s.blob, e.style);
            putU8(s.blob, e.ease);
            putU8(s.blob, 0);
            putU8(s.blob, e.startColor[0]); putU8(s.blob, e.startColor[1]); putU8(s.blob, e.startColor[2]); putU8(s.blob, 255);
            putU8(s.blob, e.endColor[0]);   putU8(s.blob, e.endColor[1]);   putU8(s.blob, e.endColor[2]);   putU8(s.blob, 255);
            putF32(s.blob, e.startOpacity);
            putF32(s.blob, e.endOpacity);
        }
        secs.push_back(std::move(s));
    }
    if (!level.moveTrackEvents.empty()) {
        Section s; s.id = (uint8_t)SectionId::MoveTrack; s.elements = level.moveTrackEvents.size();
        s.blob.reserve(level.moveTrackEvents.size() * 54);
        for (size_t mi = 0; mi < level.moveTrackEvents.size(); ++mi) {
            const LevelData::MoveTrackEvent& e = level.moveTrackEvents[mi];
            putU32(s.blob, (uint32_t)e.floor);
            putF32(s.blob, e.angleOffset);
            putF32(s.blob, e.durBeats);
            putU32(s.blob, (uint32_t)e.gap);
            putU32(s.blob, (uint32_t)e.startOff);
            putU32(s.blob, (uint32_t)e.endOff);
            putU8(s.blob, (uint8_t)e.startRel);
            putU8(s.blob, (uint8_t)e.endRel);
            putU8(s.blob, e.ease);
            putU8(s.blob, e.flags);
            putF32(s.blob, e.offX); putF32(s.blob, e.offY);
            putF32(s.blob, e.rotDeg);
            putF32(s.blob, e.scaleX); putF32(s.blob, e.scaleY);
            putF32(s.blob, e.opacity);
            putU16(s.blob, mvTagIdx[mi]);
        }
        secs.push_back(std::move(s));
    }
    if (!level.pathData.empty()) {
        Section s; s.id = (uint8_t)SectionId::PathData;
        putU64(s.blob, level.pathData.size());
        putBytes(s.blob, level.pathData.data(), level.pathData.size());
        secs.push_back(std::move(s));
    }

    // ---- 段目录（偏移 8 B 对齐，为 mmap 直读）----
    const uint64_t tableBytes = sizeof(Header) + (uint64_t)secs.size() * sizeof(SectionEntry);
    uint64_t off = (tableBytes + 7u) & ~7ull;
    std::vector<SectionEntry> table;
    table.reserve(secs.size());
    // 段级压缩：zstd 19（实测真实文件上比 xz 更小、解码更快、且 archive 本来就链着它）。
    // 太小的段不值得压；压不小就存原样。crc32c 覆盖**实际存储的字节**（读侧先校验再解压）。
    std::vector<std::vector<uint8_t>> stored(secs.size());
    for (size_t i = 0; i < secs.size(); ++i) {
        const std::vector<uint8_t>& blob = secs[i].blob;
        stored[i] = blob;
        if (!secs[i].preStored && blob.size() >= 256) {
            std::vector<uint8_t> z;
            if (g_zcache.take(blob, z)) {
                if (!z.empty()) stored[i] = std::move(z);   // 决策阶段已压过 → 直接复用
            } else {
                z.resize(ZSTD_compressBound(blob.size()));
                const size_t got = ZSTD_compress(z.data(), z.size(), blob.data(), blob.size(), 19);
                if (!ZSTD_isError(got) && got < blob.size()) {
                    z.resize(got);
                    stored[i] = std::move(z);
                }
            }
        }
    }
    for (size_t i = 0; i < secs.size(); ++i) {
        const Section& s = secs[i];
        const std::vector<uint8_t>& st = stored[i];
        SectionEntry e{};
        e.id = s.id;
        e.codec = (!s.preStored && st.size() < s.blob.size()) ? (uint8_t)Codec::SectionZstd
                                                             : (uint8_t)Codec::Raw;
        e.flags = s.flags;
        e.offset = off;
        e.compSize = st.size();
        e.rawSize = s.blob.size();
        e.elemCount = s.elements;
        e.crc32c = crc32c(st.data(), st.size());
        off = (off + st.size() + 7u) & ~7ull;
        table.push_back(e);
    }
    const uint64_t fileSize = off;

    // ---- 头 + 表 ----
    std::vector<uint8_t> hb;
    putBytes(hb, kMagic, 4);
    putU16(hb, kVersion);
    putU16(hb, 0);                                  // flags
    putU16(hb, (uint16_t)secs.size());
    putU16(hb, 0);                                  // reserved
    putU64(hb, fileSize);
    putU32(hb, 0);                                  // headerCrc（待回填）
    {
        uint8_t commit[20];
        writerCommitBytes(commit);
        putBytes(hb, commit, 20);
    }
    {
        uint64_t h = 1469598103934665603ull;        // FNV-1a-64 offset basis
        for (const Section& s : secs) {
            h = fnv1a64(&s.id, 1, h);
            h = fnv1a64(s.blob.data(), s.blob.size(), h);
        }
        putU64(hb, h);                              // 前 8 字节 = 内容指纹
        for (int i = 0; i < 24; ++i) hb.push_back(0);
    }
    for (const SectionEntry& e : table) {
        putU8(hb, e.id);
        putU8(hb, e.codec);
        putU16(hb, e.flags);
        putU64(hb, e.offset);
        putU64(hb, e.compSize);
        putU64(hb, e.rawSize);
        putU64(hb, e.elemCount);
        putU32(hb, e.crc32c);
    }
    // headerCrc 覆盖"头（本字段按 0 参与）+ 整张段表"：段表小，索性一起校验，读侧无从漏检
    const uint32_t hc = crc32c(hb.data(), hb.size());
    std::memcpy(hb.data() + 20, &hc, 4);

    out.assign((size_t)fileSize, 0);
    std::memcpy(out.data(), hb.data(), hb.size());
    for (size_t i = 0; i < secs.size(); ++i)
        if (!stored[i].empty())
            std::memcpy(out.data() + table[i].offset, stored[i].data(), stored[i].size());

    if (hb.size() != (size_t)tableBytes) {
        err = "内部错误：头 + 段表的长度与预期不符";
        return false;
    }
    rep.fileBytes = fileSize;
    for (size_t i = 0; i < secs.size(); ++i)
        if (secs[i].id == (uint8_t)SectionId::Derived) rep.checkpointBytes = stored[i].size();
    if (result) *result = std::move(rep);
    return true;
}

}  // namespace adocao
}  // namespace adofai
