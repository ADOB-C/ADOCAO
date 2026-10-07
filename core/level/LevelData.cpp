#include "LevelData.hpp"
#include "JsonCleaner.hpp"
#include "ByteSource.hpp"   // 容器识别 + 解压后端接口（实现见 archive/）
#include "core/util/Logger.hpp"
#include "core/util/ThreadPool.hpp"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>


// ===========================================================================
// .adofai JSON 读取
//
// 文件格式取决于当年写它的编辑器/版本，而 cleanJson() 就是这些年攒下来的容错层：
// 去掉 \r、去掉前置/重复/尾随逗号、给漏写逗号的旧文件补上逗号。这里面没有任何
// 版本相关的分支，所以这里不去猜版本号，而是**逐条复现 cleanJson 的规则**；
// 只要遇到复现不了的东西，就放弃并交回 cleanJson + RapidJSON 的老路。
// 两条路共用同一段收尾处理（finishLoad）和同一个 settings 读取函数，因此同一个
// 关卡无论走哪条路都会得到完全一样的 LevelData。
//
// 为什么：301 MB 的谱在 ifstream+stringstream 上花了 2.0 s、cleanJson 花 1.2 s，
// 还没开始解析就已经过去了 3.2 s，随后 6.2 M 个 action 的 DOM 又要 0.44 s。
// 快路径 mmap 文件，直接对着映射区扫描 angleData/actions，全程不复制。
// ===========================================================================

// 事件字符串驻留表的"当前属主"：buildAction 等自由函数拿不到 LevelData，用这个指针访问。
// loadFromBuffer 开始时指向 this，于是表仍然是 per-LevelData 的。

namespace adofai {

static LevelData* g_internOwner = nullptr;

namespace {
std::mutex& actionStrMtx() { static std::mutex m; return m; }
thread_local const void* tlsOwner = nullptr;     // 缓存键的一部分：驻留表地址
thread_local std::string tlsValue;
thread_local uint16_t tlsId = 0;
}  // namespace

// 事件字符串驻留：按内容去重。热路径（同一类型重复几百万次）走 thread-local 缓存、不加锁；
// 未命中才加锁查/插，所以分块并行的 worker 也是安全的。缓存键带表地址，换表（新一次加载）即失效。
uint16_t internActionStrIn(std::vector<std::string>& table, const std::string& v) {
    if (v.empty()) return 0;
    const void* owner = (const void*)&table;
    if (tlsOwner == owner && tlsValue == v) return tlsId;
    std::lock_guard<std::mutex> lk(actionStrMtx());
    for (size_t i = 1; i < table.size(); i++) {
        if (table[i] == v) { tlsOwner = owner; tlsValue = v; tlsId = (uint16_t)i; return (uint16_t)i; }
    }
    if (table.size() >= 65535) return 0;
    table.push_back(v);
    const uint16_t id = (uint16_t)(table.size() - 1);
    tlsOwner = owner; tlsValue = v; tlsId = id;
    return id;
}


namespace {

inline bool jsonWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline const char* skipWs(const char* p, const char* e) {
    while (p < e && jsonWs(*p)) ++p;
    return p;
}

// p 指向开引号；返回闭引号位置，失败返回 nullptr。
inline const char* scanStringEnd(const char* p, const char* e) {
    for (++p; p < e; ++p) {
        if (*p == '\\') { if (p + 1 >= e) return nullptr; ++p; continue; }
        if (*p == '"') return p;
    }
    return nullptr;
}

// 与 scanStringEnd 相同，但字符串里出现转义或裸 CR 就直接放弃：
//   * RapidJSON 非 in-situ 模式同样会解转义（"x\\y" -> x\y），快路径不去猜它的
//     转义表（\uXXXX / 代理对 / 非法转义都有各自的细节），遇到就交回旧路径；
//   * cleanJson 连字符串里的裸 CR 也删，那是快路径不该自己糊弄的语义差异。
// 我们真正取用的字符串（键名 / eventType / hitsound / trackDisappearAnimation …）
// 在真实谱面里从不含转义，所以这条放弃分支实际上不会触发。
inline const char* scanStringEndExact(const char* p, const char* e) {
    for (++p; p < e; ++p) {
        char c = *p;
        if (c == '\\' || c == '\r') return nullptr;
        if (c == '"') return p;
    }
    return nullptr;
}

// p 指向 '[' 或 '{'；返回配对的闭合符之后的位置。
inline const char* skipContainer(const char* p, const char* e) {
    int depth = 0;
    while (p < e) {
        char c = *p;
        if (c == '"') { const char* q = scanStringEnd(p, e); if (!q) return nullptr; p = q + 1; continue; }
        if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') { if (--depth == 0) return p + 1; }
        ++p;
    }
    return nullptr;
}

// 跳过任意一个 JSON 值。不读的值也要挡住明显非法的 token（True / None / +1 / 裸词…）：
// 那些文件 DOM 会拒绝，快路径不能反而把它放过去。
inline const char* skipValue(const char* p, const char* e) {
    if (p >= e) return nullptr;
    if (*p == '"') { const char* q = scanStringEnd(p, e); return q ? q + 1 : nullptr; }
    if (*p == '[' || *p == '{') return skipContainer(p, e);
    char c = *p;
    if (!(c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n')) return nullptr;
    while (p < e && !jsonWs(*p) && *p != ',' && *p != ']' && *p != '}') ++p;
    return p;
}

struct KeyRef { const char* p; size_t n; };

inline bool keyIs(KeyRef k, const char* lit) {
    size_t n = std::strlen(lit);
    return k.n == n && std::memcmp(k.p, lit, n) == 0;
}

// 数字。整数走快路径（<=15 位十进制整数在 double 里精确）；带小数点/指数或超长
// 的一律交给 strtod，和旧路径 parseAngleDataFast 完全同值。
inline bool parseNumber(const char*& p, const char* e, double& out) {
    const char* start = p;
    if (p < e && (*p == '-' || *p == '+')) ++p;
    if (p >= e || *p < '0' || *p > '9') { p = start; return false; }
    uint64_t v = 0;
    int digits = 0;
    while (p < e && *p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); ++digits; ++p; }
    if (digits > 15 || (p < e && (*p == '.' || *p == 'e' || *p == 'E'))) {
        char* endp = nullptr;
        double d = std::strtod(start, &endp);
        if (endp == start) { p = start; return false; }
        p = endp;
        out = d;
        return true;
    }
    out = (start[0] == '-') ? -(double)v : (double)v;
    return true;
}

// 整数 token（旧路径用 GetInt()，非整数会走 UB，所以这里直接放弃）。
inline bool parseInt(const char*& p, const char* e, int& out) {
    const char* start = p;
    bool neg = false;
    if (p < e && (*p == '-' || *p == '+')) { neg = (*p == '-'); ++p; }
    if (p >= e || *p < '0' || *p > '9') { p = start; return false; }
    int64_t v = 0;
    while (p < e && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
    if (p < e && (*p == '.' || *p == 'e' || *p == 'E')) { p = start; return false; }
    out = (int)(neg ? -v : v);
    return true;
}

struct Regions {
    const char* angle = nullptr;       // '['
    const char* angleEnd = nullptr;    // ']' 之后
    const char* actions = nullptr;     // '['
    const char* actionsEnd = nullptr;  // ']' 之后
    const char* settings = nullptr;    // '{'
    const char* settingsEnd = nullptr; // '}' 之后
    const char* path = nullptr;        // 开引号
    const char* pathEnd = nullptr;     // 闭引号
};

// 前置声明：actions 在根扫描过程中就地解析（见 scanRootMembers）。
inline bool parseActionRegion(const char* b, const char* end,
                              std::vector<LevelData::FastAction>& out, const char** outEnd);
inline bool parseActionRegionParallel(const char* b, const char* fileEnd,
                                      std::vector<LevelData::FastAction>& out, const char** outEnd);

// 只遍历根对象的直接成员。逗号允许缺失（cleanJson 会给漏写逗号的文件补上），
// 未知成员和 decorations 整体跳过，全程不复制。
// actions 是边扫边解析的：解析器自己在配对的 ']' 处停下并回报结束位置，于是整段
// actions 只被走一遍（1.5 GB 的谱上，多走一遍就是 840 ms）。
inline bool scanRootMembers(const char* s, const char* e, Regions& r,
                            std::vector<LevelData::FastAction>& actionsOut) {
    const char* p = skipWs(s, e);
    if (p >= e || *p != '{') return false;
    for (++p;;) {
        p = skipWs(p, e);
        if (p >= e) return false;
        if (*p == '}') return true;
        if (*p == ',') { ++p; continue; }        // 前置/重复/尾随逗号
        if (*p != '"') return false;             // 成员名必须是字符串
        const char* kEnd = scanStringEndExact(p, e);
        if (!kEnd) return false;
        KeyRef key{p + 1, (size_t)(kEnd - p - 1)};
        p = skipWs(kEnd + 1, e);
        if (p >= e || *p != ':') return false;
        p = skipWs(p + 1, e);
        if (*p == '[' && keyIs(key, "actions") && !r.actions) {
            const char* aEnd = nullptr;
            if (!parseActionRegionParallel(p, e, actionsOut, &aEnd)) return false;
            r.actions = p;
            r.actionsEnd = aEnd;
            p = aEnd;
            continue;
        }
        const char* vEnd = skipValue(p, e);
        if (!vEnd) return false;
        // 重复键取第一个（RapidJSON 的 FindMember 也是第一个）
        if (keyIs(key, "angleData")) {
            if (!r.angle && *p == '[') { r.angle = p; r.angleEnd = vEnd; }
        } else if (keyIs(key, "settings")) {
            if (!r.settings && *p == '{') { r.settings = p; r.settingsEnd = vEnd; }
        } else if (keyIs(key, "pathData")) {
            if (!r.path && *p == '"') { r.path = p; r.pathEnd = vEnd - 1; }
        }
        p = vEnd;
    }
}

// angleData 区间 -> double 数组。遇到字符串/嵌套数组/乱字符一律放弃：旧路径的
// strtod 循环会“跳过”这些字符继续读，行为不同，不能自作主张。
inline bool parseAngleDataRegion(const char* b, const char* e, std::vector<double>& out) {
    const char* p = b + 1;
    for (;;) {
        p = skipWs(p, e);
        if (p >= e) return false;
        if (*p == ']') return true;
        if (*p == ',') { ++p; continue; }
        double v;
        if (!parseNumber(p, e, v)) return false;
        out.push_back(v);
    }
}

inline LevelData::FastAction::Type eventTypeOf(KeyRef k) {
    using T = LevelData::FastAction;
    if (keyIs(k, "Twirl"))         return T::Twirl;
    if (keyIs(k, "SetSpeed"))      return T::SetSpeed;
    if (keyIs(k, "PositionTrack")) return T::PositionTrack;
    if (keyIs(k, "SetHitsound"))   return T::SetHitsound;
    if (keyIs(k, "Bookmark"))      return T::Bookmark;
    if (keyIs(k, "Pause"))         return T::Pause;
    if (keyIs(k, "AnimateTrack"))  return T::AnimateTrack;
    return T::Other;
}

// 一个 action 对象里收集到的原始字段。按字段收集、最后按类型组装，语义与旧路径
// 逐个 eventType 分支读取的方式一致。
struct ActionFields {
    bool hasFloor = false, hasEvent = false;
    int floor = 0;
    LevelData::FastAction::Type type = LevelData::FastAction::Other;
    bool isMultiplier = false;
    bool hasBpmMultiplier = false, hasBeatsPerMinute = false, hasDuration = false;
    bool hasHitsoundVolume = false, hasBeatsBehind = false, hasBeatsAhead = false;
    bool hasOffset = false, hasJustThisTile = false;
    bool hasTrackAnimation = false, hasTrackDisappear = false;
    float bpmMultiplier = 1.0f, beatsPerMinute = 0.0f, duration = 0.0f;
    float hitsoundVolume = 0.0f, beatsBehind = 0.0f, beatsAhead = 0.0f;
    float offX = 0.0f, offY = 0.0f;
    bool justThisTile = false;
    std::string hitsound, trackDisappear;
};

// 取出字符串值。带转义或裸 CR 的一律交回旧路径（旧路径会解转义 / 删 CR）。
inline bool stringValue(const char* p, const char* vEnd, std::string& out) {
    if (*p != '"') return false;
    for (const char* q = p; q < vEnd; ++q)
        if (*q == '\\' || *q == '\r') return false;
    out.assign(p + 1, vEnd - 1);
    return true;
}

// p 指向 action 对象的 '{' 之后（调用方已经跳过 '{'）
inline bool parseActionObject(const char* p, const char* e, ActionFields& f) {
    for (;;) {
        p = skipWs(p, e);
        if (p >= e) return false;
        if (*p == '}') return true;
        if (*p == ',') { ++p; continue; }        // 前置/重复/尾随逗号
        if (*p != '"') return false;
        const char* kEnd = scanStringEndExact(p, e);
        if (!kEnd) return false;
        KeyRef key{p + 1, (size_t)(kEnd - p - 1)};
        p = skipWs(kEnd + 1, e);
        if (p >= e || *p != ':') return false;
        p = skipWs(p + 1, e);
        const char* v = p;
        const char* vEnd = skipValue(v, e);
        if (!vEnd) return false;

        // 谱里 action 对象常常带一大堆我们不关心的键（scale/opacity/relativeTo…），
        // 按首字符分派后每个未知键只花一次比较。
        double d;
        switch (*key.p) {
        case 'f':
            if (keyIs(key, "floor")) {
                if (!parseInt(v, vEnd, f.floor)) return false;
                f.hasFloor = true;
            }
            break;
        case 'e':
            if (keyIs(key, "eventType")) {
                if (*v != '"') return false;
                for (const char* q = v; q < vEnd; ++q)                 // 转义 / 裸 CR：交回旧路径
                    if (*q == '\\' || *q == '\r') return false;
                f.type = eventTypeOf(KeyRef{v + 1, (size_t)(vEnd - v - 2)});
                f.hasEvent = true;
            }
            break;
        case 's':
            if (keyIs(key, "speedType")) {
                std::string str;
                if (!stringValue(v, vEnd, str)) return false;
                f.isMultiplier = (str == "Multiplier");
            }
            break;
        case 'b':
            if (keyIs(key, "bpmMultiplier")) {
                if (!parseNumber(v, vEnd, d)) return false;
                f.bpmMultiplier = (float)d; f.hasBpmMultiplier = true;
            } else if (keyIs(key, "beatsPerMinute")) {
                if (!parseNumber(v, vEnd, d)) return false;
                f.beatsPerMinute = (float)d; f.hasBeatsPerMinute = true;
            } else if (keyIs(key, "beatsBehind")) {
                if (!parseNumber(v, vEnd, d)) return false;
                f.beatsBehind = (float)d; f.hasBeatsBehind = true;
            } else if (keyIs(key, "beatsAhead")) {
                if (!parseNumber(v, vEnd, d)) return false;
                f.beatsAhead = (float)d; f.hasBeatsAhead = true;
            }
            break;
        case 'd':
            if (keyIs(key, "duration")) {
                if (!parseNumber(v, vEnd, d)) return false;
                f.duration = (float)d; f.hasDuration = true;
            }
            break;
        case 'h':
            if (keyIs(key, "hitsound")) {
                if (!stringValue(v, vEnd, f.hitsound)) return false;
            } else if (keyIs(key, "hitsoundVolume")) {
                if (!parseNumber(v, vEnd, d)) return false;
                f.hitsoundVolume = (float)d; f.hasHitsoundVolume = true;
            }
            break;
        case 't':
            if (keyIs(key, "trackDisappearAnimation")) {
                if (!stringValue(v, vEnd, f.trackDisappear)) return false;
                f.hasTrackDisappear = true;
            } else if (keyIs(key, "trackAnimation")) {
                if (*v != '"') return false;
                f.hasTrackAnimation = true;
            }
            break;
        case 'p':
            if (keyIs(key, "positionOffset")) {
                if (*v == '[') {                 // 非数组时旧路径直接忽略
                    const char* q = skipWs(v + 1, vEnd);
                    double x, y;
                    if (q < vEnd && *q != ']' && parseNumber(q, vEnd, x)) {
                        q = skipWs(q, vEnd);
                        if (q < vEnd && *q == ',') q = skipWs(q + 1, vEnd);
                        if (q < vEnd && *q != ']' && parseNumber(q, vEnd, y)) {
                            f.offX = (float)x; f.offY = (float)y; f.hasOffset = true;
                        }
                    }
                }
            }
            break;
        case 'j':
            if (keyIs(key, "justThisTile")) {
                if (vEnd - v == 4 && std::memcmp(v, "true", 4) == 0) {
                    f.justThisTile = true; f.hasJustThisTile = true;
                } else if (vEnd - v == 5 && std::memcmp(v, "false", 5) == 0) {
                    f.justThisTile = false; f.hasJustThisTile = true;
                } else if (*v == '"') {
                    std::string str;
                    if (!stringValue(v, vEnd, str)) return false;
                    f.justThisTile = (str == "Enabled" || str == "true" || str == "True");
                    f.hasJustThisTile = true;
                } else {
                    int n;
                    if (parseInt(v, vEnd, n)) { f.justThisTile = (n != 0); f.hasJustThisTile = true; }
                    // 其它（如 1.0）旧路径三个 Is* 全不成立 -> 保持 false
                }
            }
            break;
        default:
            break;
        }
        p = vEnd;
    }
}

inline bool buildAction(const ActionFields& f, LevelData::FastAction& a, bool& keep) {
    using T = LevelData::FastAction;
    if (f.hasFloor) a.floor = f.floor;
    a.type = f.type;
    switch (f.type) {
    case T::SetSpeed:
        if (f.isMultiplier) { a.flag = true; a.val1 = f.hasBpmMultiplier ? f.bpmMultiplier : 1.0f; }
        else                { a.val1 = f.hasBeatsPerMinute ? f.beatsPerMinute : 0.0f; }
        break;
    case T::Pause:
        a.val1 = f.hasDuration ? f.duration : 0.0f;
        break;
    case T::PositionTrack:
        if (f.hasOffset) { a.val1 = f.offX; a.val2 = f.offY; }
        a.flag = f.justThisTile;
        break;
    case T::SetHitsound:
        a.strId = g_internOwner ? g_internOwner->internActionStr(f.hitsound) : 0;
        a.val1 = f.hasHitsoundVolume ? f.hitsoundVolume : 0.0f;
        a.flag = f.hasHitsoundVolume;      // 是否写了音量：负值/0 都是合法值，不能拿 val1>0 猜
        break;
    case T::AnimateTrack:
        a.val1 = f.hasBeatsBehind ? f.beatsBehind : -1.0f;
        a.val2 = f.hasBeatsAhead ? f.beatsAhead : -1.0f;
        if (f.hasTrackDisappear) a.strId = g_internOwner ? g_internOwner->internActionStr(f.trackDisappear) : 0;
        a.flag = f.hasTrackAnimation;
        break;
    default:
        break;
    }
    // 缺 floor 或缺 eventType、以及未知 eventType 的 action 旧路径直接丢弃
    keep = f.hasFloor && f.hasEvent && f.type != T::Other;
    return true;
}

// b 指向 '['，end 是整个缓冲区的末尾；遇到配对的 ']' 停下并回报其后的位置。
inline bool parseActionRegion(const char* b, const char* end,
                              std::vector<LevelData::FastAction>& out, const char** outEnd) {
    const char* p = b + 1;
    for (;;) {
        p = skipWs(p, end);
        if (p >= end) return false;
        if (*p == ']') { *outEnd = p + 1; return true; }
        if (*p == ',') { ++p; continue; }
        if (*p != '{') return false;
        const char* objEnd = skipContainer(p, end);
        if (!objEnd) return false;
        ActionFields f;
        if (!parseActionObject(p + 1, objEnd, f)) return false;
        LevelData::FastAction a;
        bool keep = false;
        if (!buildAction(f, a, keep)) return false;
        if (keep) out.push_back(std::move(a));
        p = objEnd;
    }
}

// ---- 分块并行解析 actions ----------------------------------------------------
//
// 大谱面的时间几乎全花在这一段上（1.5 GB 明文实测 ~1.6 s，单线程），而数组里每个
// action 对象彼此独立，所以可以切段并行、最后按序拼接。
//
// 切分点必须是"某个 action 对象的 '{'"，靠一趟只做括号/字符串配对的扫描找出来
// （不做字段提取，比整段解析便宜 3-4 倍：1.18 GB / 9.1 M 对象实测 447 ms）。这一趟
// 同时也能得到数组的结束位置，于是根扫描那边省掉了原先"融合"时多走的一遍。

// 在 [begin, fileEnd) 里，从 begin（数组的 '['）开始，为每个 target 偏移找第一个
// 落点之后的对象起点。找不到数组结尾、或没凑齐所有 target 就返回 false。
inline bool findActionSplits(const char* begin, const char* fileEnd,
                             const std::vector<size_t>& targets,
                             std::vector<const char*>& splits, std::vector<size_t>& counts,
                             const char** outRegionEnd) {
    const char* p = begin + 1;
    size_t ti = 0;
    counts.assign(1, 0);
    while (p < fileEnd) {
        p = skipWs(p, fileEnd);
        if (p >= fileEnd) return false;
        if (*p == ']') {
            *outRegionEnd = p + 1;
            return true;          // 数组比目标还短时，能切几段就切几段
        }
        if (*p == ',') { ++p; continue; }
        if (*p != '{') return false;
        if (ti < targets.size() && (size_t)(p - begin) >= targets[ti]) {
            splits.push_back(p);
            counts.push_back(0);
            ++ti;
        }
        counts.back()++;          // 这一段有几个 action 对象（即槽位大小上界）
        const char* objEnd = skipContainer(p, fileEnd);
        if (!objEnd) return false;
        p = objEnd;
    }
    return false;
}

// 解析 [b, end) 里的 action 对象，写进 dst 的 slot 个槽位。end 要么是下一段的起点
// （对象边界），要么是数组结尾之后。
// 返回写入个数；返回 kFail 表示这段没有正好落在 end 上（切分点不可信）。
// 写进预先分配好的槽位而不是各自 push_back：8 段各一个 vector 再合并，等于把整份
// actions 写两遍，实测峰值 3.20 -> 5.18 GB，在内存紧张的机器上直接翻车。
constexpr size_t kFail = (size_t)-1;
inline size_t parseActionRangeInto(const char* b, const char* end,
                                   LevelData::FastAction* dst, size_t slot) {
    size_t n = 0;
    const char* p = b;
    for (;;) {
        p = skipWs(p, end);
        if (p >= end) return n;                          // 正好到切分点
        if (*p == ']') return (p + 1 == end) ? n : kFail;// 最后一段：数组结尾必须正好是 end
        if (*p == ',') { ++p; continue; }
        if (*p != '{') return kFail;
        const char* objEnd = skipContainer(p, end);
        if (!objEnd) return kFail;
        ActionFields f;
        if (!parseActionObject(p + 1, objEnd, f)) return kFail;
        LevelData::FastAction a;
        bool keep = false;
        if (!buildAction(f, a, keep)) return kFail;
        if (keep) {
            if (n >= slot) return kFail;                 // 槽位估算错了，别越界
            dst[n++] = std::move(a);
        }
        p = objEnd;
    }
}

inline bool parseActionRegionParallel(const char* b, const char* fileEnd,
                                      std::vector<LevelData::FastAction>& out, const char** outEnd) {
    const size_t remaining = (size_t)(fileEnd - b);

    // 测试钩子（同 ADOCAO_MIX_THREADS 的路子）：显式指定段数就照办，哪怕文件很小——
    // 否则 fixture 全都低于下面的阈值，CI 上这条分块路径永远跑不到。
    //   ADOCAO_PARSE_PIECES=1  退回单线程（对拍用）
    //   ADOCAO_PARSE_PIECES=N  强制切 N 段
    unsigned pieces = 0;
    bool forced = false;
    if (const char* env = std::getenv("ADOCAO_PARSE_PIECES")) {
        int v = std::atoi(env);
        if (v > 0) { pieces = (unsigned)v; forced = true; }
    }
    if (pieces < 2) {
        if (forced) return parseActionRegion(b, fileEnd, out, outEnd);
        // 小文件不值得多扫一遍、也不值得开线程：直接用融合的单遍解析。
        //
        // 上界同理：1.2 GB 以上的 actions 区间上，多线程读不同区段会把内存带宽/局域性
        // 吃满，实测反而更慢（1.18 GB 谱面：2 段 900 ms、4 段 980 ms、8 段 1474 ms，
        // 而 611 MB 的谱面 8 段只要 82 ms）。那里退回融合的单遍解析——找切分点那趟
        // 扫描本身就要 0.5 s，与其白花不如省掉。真正的解法是让解析在小的常驻窗口上
        // 做（见 TODO：滑窗流水线），那时并行分段是免费的。
        if (remaining < (32u << 20) || remaining >= (768u << 20))
            return parseActionRegion(b, fileEnd, out, outEnd);
        pieces = std::thread::hardware_concurrency();
        if (pieces < 2) pieces = 2;
        if (pieces > 8) pieces = 8;                      // 再多边际收益很小
    }

    std::vector<size_t> targets;
    targets.reserve(pieces - 1);
    for (unsigned i = 1; i < pieces; i++) targets.push_back(remaining * i / pieces);
    // 目标偏移可能落在同一个对象上（小文件），findActionSplits 会把它们合并成更少的段


    const bool dbg = std::getenv("ADOCAO_PARSE_DBG") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto msSince = [](auto a) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    };
    auto tScan = now();
    std::vector<const char*> splits;
    splits.reserve(pieces - 1);
    std::vector<size_t> counts;
    const char* regionEnd = nullptr;
    if (!findActionSplits(b, fileEnd, targets, splits, counts, &regionEnd) || splits.empty()) {
        return parseActionRegion(b, fileEnd, out, outEnd);   // 结构不对/切不出两段
    }

    std::vector<const char*> bounds;
    bounds.reserve(pieces + 1);
    bounds.push_back(b + 1);          // b 是数组的 '['，分段解析器要的是对象起点
    for (const char* sp : splits) bounds.push_back(sp);
    bounds.push_back(regionEnd);
    const size_t n = bounds.size() - 1;
    if (n < 2) return parseActionRegion(b, fileEnd, out, outEnd);

    const double scanMs = msSince(tScan);
    const size_t base = out.size();
    std::vector<size_t> slots(n + 1, 0);
    for (size_t i = 0; i < n; i++) slots[i + 1] = slots[i] + counts[i];
    out.resize(base + slots[n]);                         // 一次分配，之后不再增长

    auto tParse = now();
    std::vector<size_t> kept(n, 0);
    std::atomic<bool> failed{false};
    unsigned poolThreads = 0;
    {
        ThreadPool pool((unsigned)n - 1);                // parallelFor 会让调用线程也干活
        poolThreads = pool.threadCount() + 1;            // +1 = 调用线程也参与
        pool.parallelFor(0, n, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi && !failed.load(std::memory_order_relaxed); i++) {
                size_t k = parseActionRangeInto(bounds[i], bounds[i + 1],
                                                out.data() + base + slots[i], counts[i]);
                if (k == kFail) failed.store(true, std::memory_order_relaxed);
                else kept[i] = k;
            }
        }, 1);
    }
    const double parseMs = msSince(tParse);
    if (dbg) std::fprintf(stderr, "[parse] 找切分点 %.1f ms  并行解析 %.1f ms  段数=%zu 线程=%u  failed=%d\n",
                          scanMs, parseMs, n, poolThreads, (int)failed.load());
    if (failed.load()) {
        out.resize(base);
        return parseActionRegion(b, fileEnd, out, outEnd);   // 只有切分不可信时才会走到
    }

    // 压实：每段末尾的空槽（被丢弃的 action）移走。没有丢弃时位移为 0，直接零拷贝。
    auto tMerge = now();
    size_t write = base;
    for (size_t i = 0; i < n; i++) {
        const size_t from = base + slots[i];
        if (kept[i] > 0 && from != write)
            std::move(out.begin() + from, out.begin() + from + kept[i], out.begin() + write);
        write += kept[i];
    }
    if (dbg) std::fprintf(stderr, "[parse] 压实 %.1f ms  合计 %.1f ms  pieces=%zu kept=%zu\n",
                          msSince(tMerge), msSince(tScan), n, write - base);
    out.resize(write);
    *outEnd = regionEnd;
    return true;
}

// ---------------------------------------------------------------------------
// 窗口化解析：数据由 ArchiveStream 分块提供（一次只有半个窗口的字节），
// 每个子扫描器遵守同一约定：
//   返回 end   = 窗口用完，这一项还没处理完。调用方把"这一项的起点"之后的字节
//                carry 到下一块，并让扫描器从头重放该项（半成品输出要丢掉）。
//   返回 < end = 这一项处理完了，返回位置是它后面的第一个字节。
// 输出的结构先放在局部，全部成功后再移进 LevelData（失败时对象保持不动）。
// ---------------------------------------------------------------------------
struct WindowParser {
    std::vector<double> angles;
    std::vector<LevelData::FastAction> actions;
    LevelData::Settings settings;
    std::string pathText;
    std::string settingsText;
    bool failed = false;
    const char* failAt = nullptr;       // 诊断用：失败发生在窗口里的哪个位置
    const char* failWhy = "";
    long failA = -1, failB = -1;         // 诊断用：附加数字（如对象长度 / 窗口剩余）
    bool fail(const char* at, const char* why) { failed = true; failAt = at; failWhy = why; return false; }

    enum class State { Members, InAngles, InActions, InSettings, InPath, InSkip, Done };
    State state = State::Members;
    bool seenAngles = false, seenSettings = false, seenActions = false, seenPath = false;
    bool rootOpened = false, bomChecked = false;

    // 子扫描器的跨窗状态
    const char* itemStart = nullptr;      // 当前项（值）在窗口里的起点
    bool resetOnResume = false;           // 重放前要清掉半成品
    bool inString = false, escaped = false, started = false, inLiteral = false;
    int depth = 0;

    // ---- angleData：边流边转 double，不留文本 ----
    const char* parseAngles(const char* p, const char* end) {
        for (;;) {
            while (p < end && (jsonWs(*p) || *p == ',')) ++p;
            if (p >= end) { itemStart = p; return end; }        // 窗口用完
            if (*p == ']') return p + 1;                        // 数组结束
            itemStart = p;
            const char* q = p;
            double v;
            // 数字被窗口切断、或根本解析不了：都先当作"需要更多数据"（挂起时返回 end，
            // 由调用方把这一项 carry 到下一块）。真要是垃圾，下一块还是同样字节、推进不了
            // -> stream.stuck() -> 整段退回整份解压。
            if (!parseNumber(q, end, v)) return end;
            if (q >= end) return end;
            angles.push_back(v);
            p = q;
        }
    }

    // ---- actions：每个对象必须完整落在窗口内 ----
    const char* parseActions(const char* p, const char* end) {
        for (;;) {
            while (p < end && (jsonWs(*p) || *p == ',')) ++p;
            if (p >= end) { itemStart = p; return end; }         // 窗口用完
            if (*p == ']') return p + 1;                         // 数组结束
            if (*p != '{') { fail(p, "actions 元素不是对象"); return p; }
            itemStart = p;
            const char* objEnd = skipContainer(p, end);
            if (!objEnd) return end;                             // 对象跨窗：carry 补齐后重放
            ActionFields f;
            if (!parseActionObject(p + 1, objEnd, f)) {
                failA = (long)(objEnd - p); failB = (long)(end - p);
                fail(p, "action 对象解析失败"); return p;
            }
            LevelData::FastAction a;
            bool keep = false;
            if (!buildAction(f, a, keep)) { fail(p, "action 组装失败"); return p; }
            if (keep) actions.push_back(std::move(a));
            p = objEnd;
        }
    }

    // ---- settings：原样抄下来（几 KB），稍后交给 cleanJson + RapidJSON ----
    const char* copySettings(const char* p, const char* end) {
        if (resetOnResume) { settingsText.clear(); depth = 0; inString = escaped = false; resetOnResume = false; }
        for (; p < end; ++p) {
            char c = *p;
            settingsText.push_back(c);
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) return p + 1; }
        }
        return end;
    }

    // ---- pathData：抄字符串内容（裸 CR 丢掉；带转义就退回整份路径，与现在一致）----
    const char* copyPath(const char* p, const char* end) {
        if (resetOnResume) { pathText.clear(); resetOnResume = false; started = false; }
        if (!started) {                                       // 首次进入：吃掉开引号
            if (*p != '"') { fail(p, "pathData 不是字符串"); return p; }
            ++p;
            started = true;
        }
        for (; p < end; ++p) {
            char c = *p;
            if (c == '\\') { fail(p, "pathData 里有转义"); return p; }
            if (c == '"') return p + 1;
            if (c != '\r') pathText.push_back(c);
        }
        return end;
    }

    // ---- 其它成员（decorations 等）：跳过整个值 ----
    const char* skipOne(const char* p, const char* end) {
        if (resetOnResume) { depth = 0; inString = escaped = started = inLiteral = false; resetOnResume = false; }
        while (p < end) {
            char c = *p;
            if (inLiteral) {
                while (p < end && !jsonWs(*p) && *p != ',' && *p != ']' && *p != '}') ++p;
                if (p >= end) return end;
                inLiteral = false;
                return p;
            }
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') { inString = false; if (depth == 0) return p + 1; }
                ++p;
                continue;
            }
            if (depth == 0 && !started) {
                if (c == '"') { inString = true; started = true; ++p; continue; }
                if (c != '[' && c != '{') {
                    if (!(c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n')) {
                        fail(p, "值首字符非法");
                        return p;
                    }
                    started = true;
                    inLiteral = true;
                    continue;
                }
                started = true;
                depth = 1;
                ++p;
                continue;
            }
            if (c == '"') { inString = true; ++p; continue; }
            if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') { if (--depth == 0) return p + 1; }
            ++p;
        }
        return end;
    }

    const char* step(const char* p, const char* end) {
        // 本次调用里"还没提交"的起点就是 p：token 级扫描器（settings/path/skip）整项重放，
        // 角度/action 子扫描器会把它推进到当前元素的起点（已提交的元素不能重放）。
        itemStart = p;
        switch (state) {
        case State::InAngles: { const char* q = parseAngles(p, end); if (q < end) state = State::Members; return q; }
        case State::InActions: { const char* q = parseActions(p, end); if (q < end) state = State::Members; return q; }
        case State::InSettings: { const char* q = copySettings(p, end); if (q < end) state = State::Members; return q; }
        case State::InPath: { const char* q = copyPath(p, end); if (q < end) state = State::Members; return q; }
        case State::InSkip: { const char* q = skipOne(p, end); if (q < end) state = State::Members; return q; }
        case State::Done: return p;
        case State::Members: break;
        }

        if (!rootOpened) {                                    // 根对象的 '{'
            if (!bomChecked) {
                bomChecked = true;
                if ((size_t)(end - p) >= 3 && (unsigned char)p[0] == 0xEF
                    && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
            }
            while (p < end && jsonWs(*p)) ++p;
            if (p >= end) { itemStart = p; return p; }
            if (*p != '{') { fail(p, "actions 元素不是对象"); return p; }
            rootOpened = true;
            ++p;
        }
        for (;;) {
            while (p < end && (jsonWs(*p) || *p == ',')) ++p;
            if (p >= end) { itemStart = p; return p; }
            if (*p == '}') { state = State::Done; return p + 1; }
            if (*p != '"') { fail(p, "成员名不是字符串"); return p; }
            itemStart = p;
            const char* kEnd = scanStringEndExact(p, end);
            if (!kEnd) return p;                              // 键跨窗
            KeyRef key{p + 1, (size_t)(kEnd - p - 1)};
            const char* q = skipWs(kEnd + 1, end);
            if (q >= end || *q != ':') { if (q < end) fail(q, "成员名后不是 :"); return p; }
            q = skipWs(q + 1, end);
            if (q >= end) return p;                           // 值还没开始
            if (keyIs(key, "angleData") && !seenAngles && *q == '[') {
                seenAngles = true; state = State::InAngles; itemStart = q + 1; return q + 1;
            }
            if (keyIs(key, "settings") && !seenSettings && *q == '{') {
                seenSettings = true; state = State::InSettings; itemStart = q; resetOnResume = true; return q;
            }
            if (keyIs(key, "pathData") && !seenPath && *q == '"') {
                seenPath = true; state = State::InPath; itemStart = q; resetOnResume = true; started = false; return q;
            }
            if (keyIs(key, "actions") && !seenActions && *q == '[') {
                seenActions = true; state = State::InActions; itemStart = q + 1; return q + 1;
            }
            state = State::InSkip; itemStart = q; resetOnResume = true; return q;
        }
    }

    // 跑完整个流。1 = 完成；0 = 结构上搞不定（退回整份解压）；-1 = 解压失败
    int run(WindowSource& stream) {
        const bool dbg = std::getenv("ADOCAO_WINDOW_DBG") != nullptr;
        if (!stream.next()) {
            if (dbg) std::fprintf(stderr, "[win] 首块就失败 failed=%d err=%s\n", (int)stream.failed(), stream.error().c_str());
            return stream.failed() ? -1 : 0;
        }
        const char* p = stream.data();
        const char* end = p + stream.size();
        static const char* const kStateName[] = {"Members", "InAngles", "InActions", "InSettings", "InPath", "InSkip", "Done"};
        for (;;) {
            const State before = state;
            const char* q = step(p, end);
            if (dbg && state != before)
                std::fprintf(stderr, "[win] 转移 窗内%td/%zu %s -> %s (q=%td item=%td carry=%zu)\n",
                             p - stream.data(), stream.size(), kStateName[(int)before], kStateName[(int)state],
                             q - stream.data(), itemStart - stream.data(), (size_t)(itemStart - p));
            if (failed) {
                if (dbg) std::fprintf(stderr, "[win] 放弃 state=%d 偏移=%td/%zu 位置=%td 原因=%s 字节=%.24s\n",
                                      (int)state, p - stream.data(), stream.size(),
                                      failAt ? failAt - stream.data() : -1, failWhy,
                                      failAt ? failAt : "");
                if (dbg && failA >= 0) std::fprintf(stderr, "[win]   对象长=%ld 窗口剩余=%ld\n", failA, failB);
                return 0;
            }
            if (state == State::Done) {
                stream.consume((size_t)(q - p));
                return 1;
            }
            if (q < end) { p = q; continue; }
            // 需要更多数据：把当前项起点之后的字节 carry 过去，下次从头重放该项
            // 挂起时从哪里开始 carry：
            //   * skip/settings/path 这三个子扫描器自己带着状态（括号深度、是否在字符串里、
            //     已抄下的字节），可以从窗口末尾接着扫，所以整窗都算已提交 —— 于是单个值
            //     无论多大都能跨窗（超大 decorations 数组、超长 levelDesc 都不会再放弃）。
            //   * angleData/actions 只能停在元素边界，必须把它当前那个不完整的元素 carry 过去。
            const bool resumable = state == State::InSettings || state == State::InPath || state == State::InSkip;
            const char* carryFrom = resumable ? end : itemStart;
            const size_t consumed = (size_t)(carryFrom - p);   // 诊断用：相对本次调用起点
            if (dbg) std::fprintf(stderr, "[win] 换窗 state=%d 已提交=%zu carry=%zu 窗口=%zu\n",
                                  (int)state, consumed, stream.size() - consumed, stream.size());
            // consume() 要的是"窗口内偏移"，不是"相对本次调用起点的字节数"（第一次调用两者
            // 恰好相等，所以这个错被掩盖过：窗口中途状态转移后 carry 会退到已解析对象中间）。
            stream.consume((size_t)(carryFrom - stream.data()));
            if (!stream.next()) {
                if (dbg) std::fprintf(stderr, "[win] 取下一块失败 state=%d stuck=%d failed=%d err=%s 已消费=%zu 块内=%zu\n",
                                      (int)state, (int)stream.stuck(), (int)stream.failed(),
                                      stream.error().c_str(), consumed, stream.size());
                return stream.failed() ? -1 : 0;
            }
            if (stream.stuck()) {
                if (dbg) std::fprintf(stderr, "[win] stuck（单个值大于半窗）state=%d\n", (int)state);
                return 0;
            }
            p = stream.data();
            end = p + stream.size();
            // 不再无条件 resetOnResume：进入这些状态时已经初始化过，续扫必须保留状态
        }
    }
};

// settings 的读取：新旧两条路共用，保证字段语义一致。
static void readSettings(const rapidjson::Value& s, LevelData::Settings& out) {
    auto getF = [&](const char* k, float d) { return s.HasMember(k) ? s[k].GetFloat() : d; };
    auto getI = [&](const char* k, int d) { return s.HasMember(k) ? s[k].GetInt() : d; };
    auto getS = [&](const char* k, const char* d) -> std::string {
        return (s.HasMember(k) && s[k].IsString()) ? s[k].GetString() : d;
    };
    out.bpm             = getF("bpm", 100.0f);
    out.offset          = getF("offset", 0.0f);
    out.countdownTicks  = getI("countdownTicks", 4);
    out.zoom            = getF("zoom", 100.0f);
    out.rotation        = getF("rotation", 0.0f);
    out.relativeTo      = getS("relativeTo", "Player");
    out.hitsound        = getS("hitsound", "Kick");
    out.hitsoundVolume  = getF("hitsoundVolume", 100.0f);
    out.trackColor      = getS("trackColor", "debb7b");
    out.secondaryTrackColor = getS("secondaryTrackColor", "ffffff");
    out.backgroundColor = getS("backgroundColor", "000000");
    out.planetEase      = getS("planetEase", "Linear");
    out.trackDisappearAnimation = getS("trackDisappearAnimation", "None");
    out.trackAnimation  = getS("trackAnimation", "None");
    out.beatsBehind     = getF("beatsBehind", 4.0f);
    out.beatsAhead      = getF("beatsAhead", 3.0f);
    if (s.HasMember("stickToFloors")) {
        if (s["stickToFloors"].IsBool()) out.stickToFloors = s["stickToFloors"].GetBool();
        else if (s["stickToFloors"].IsString()) {
            std::string v = s["stickToFloors"].GetString();
            out.stickToFloors = (v == "Enabled" || v == "true" || v == "True");
        }
    }
    if (s.HasMember("position") && s["position"].IsArray() && s["position"].Size() >= 2)
        out.position = {s["position"][0].GetFloat(), s["position"][1].GetFloat()};
}

}  // namespace

// Fast parser: extract angleData float array from JSON without DOM allocation.
// 保留：它是旧路径（也是回退路径）用的参考实现。
static std::vector<double> parseAngleDataFast(const char* json, size_t len, size_t& outArrayEnd) {
    const char* key = "\"angleData\"";
    const char* pos = (const char*)std::memchr(json, '"', len);
    while (pos) {
        size_t remain = len - (pos - json);
        if (remain >= 11 && std::memcmp(pos, key, 11) == 0) {
            pos += 11;
            while (pos < json + len && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n'))
                pos++;
            if (*pos != '[') return {};
            pos++; // skip '['
            break;
        }
        pos++;
        pos = (const char*)std::memchr(pos, '"', json + len - pos);
    }
    if (!pos) return {};

    std::vector<double> result;
    result.reserve((len - (pos - json)) / 30);
    while (pos < json + len) {
        while (pos < json + len && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
            pos++;
        if (pos >= json + len) break;
        if (*pos == ']') { outArrayEnd = pos - json + 1; break; }
        if (*pos == ',') { pos++; continue; }
        char* end;
        double val = strtod(pos, &end);
        if (end == pos) { pos++; continue; }
        result.push_back(val);
        pos = end;
    }
    return result;
}

// 可移植读法（测试 / 嵌入方用）。app 走 FileMap + loadFromBuffer：1.5 GB 的谱
// 用 ifstream 复制一份要多花 ~250 ms 和 1.5 GB 常驻内存，mmap 没有这份副本。
bool LevelData::loadFromFile(const std::string& filepath, ProgressCb onProgress, bool exportOnly) {
    if (onProgress) onProgress(0.05f, "Reading file...");
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        LOG_E("Cannot open level file: %s", filepath.c_str());
        return false;
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size <= 0) {
        LOG_E("Cannot open level file: %s", filepath.c_str());
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::string content((size_t)size, '\0');
    file.read(&content[0], size);
    return loadFromBuffer(content.data(), content.size(), onProgress, exportOnly);
}

bool LevelData::loadFromString(const std::string& jsonStr, ProgressCb onProgress, bool exportOnly) {
    return loadFromBuffer(jsonStr.data(), jsonStr.size(), onProgress, exportOnly);
}

bool LevelData::loadFromBuffer(const char* data, size_t len, ProgressCb onProgress, bool exportOnly) {
    g_internOwner = this;                                  // 事件字符串驻留表归本次加载所有
    actionStrTable.resize(1);                              // id 0 = 空串
    tlsOwner = nullptr;                                // thread-local 缓存失效
    // 压缩容器（.adofai.xz / .adofai.zst，按 magic 判断）：先解开，再当明文解析。
    // 解压后的缓冲区必须活到解析结束，所以放在这个作用域里。
    std::string decompressed;
    const LevelArchiveKind archive = sniffLevelArchive(data, len);
    if (archive != LevelArchiveKind::Plain) {
        // 优先走窗口流水线：不把整份解压结果摊进内存（10 GB 文本那份匿名内存会被系统
        // 压缩/换页，实测吞吐掉到 1/11）。任何搞不定的情况都退回下面的整份解压。
        const bool forceWhole = std::getenv("ADOCAO_WHOLE_DECOMPRESS") != nullptr;   // 每次读：测试会在同一进程里切换
        // 压缩输入默认走窗口流水线：逐位一致（fixture 用 4 KB 半窗、1.18 / 1.40 GB 真实谱用
        // 96 KB / 1 MB / 8 MB 半窗都核对过 13 个节的 hash），而且比整份解压省掉"整份文本"
        // 的分配与二次扫描。搞不定的情况一律放弃并落到下面的整份解压，所以默认开启无风险。
        // ADOCAO_WINDOW_KB 调半窗大小；ADOCAO_WHOLE_DECOMPRESS=1 强制退回整份解压。
        // 测试用：要求必须走通窗口路径（不许静默回退），否则直接失败 —— 用来证明用例
        // 真的覆盖了这条路径，而不是每次都在偷偷走整份解压。
        const bool requireWindow = std::getenv("ADOCAO_WINDOW_REQUIRE") != nullptr;
        if (!forceWhole) {
            // 半窗默认 4 MB：实测比 96 MB 更快（TNR 1928 vs 2062 ms——小窗口更贴缓存），
            // 峰值内存也只剩 2x8 MB。单个 JSON 值大于半窗时窗口路径会放弃并回退整份解压。
            size_t half = 4u << 20;
            if (const char* env = std::getenv("ADOCAO_WINDOW_KB")) {   // 测试用：极小窗口
                long kb = std::atol(env);
                if (kb >= 4) half = (size_t)kb * 1024;
            }
            const ArchiveBackend& bk = archiveBackend();
            std::unique_ptr<WindowSource> stream(bk.makeWindow ? bk.makeWindow() : nullptr);
            if (stream && stream->open(data, len, archive, half)) {
                if (onProgress) onProgress(0.05f, "Decompressing level...");
                WindowParser wp;
                wp.settings = settings;         // 与整份路径一致：文件里没有的字段沿用旧值
                const int rc = wp.run(*stream);
                if (rc < 0) {
                    LOG_E("Cannot decompress level (%s): %s",
                          archive == LevelArchiveKind::Xz ? "xz" : "zstd", stream->error().c_str());
                    return false;
                }
                if (rc == 1) {
                    if (wp.seenSettings) {
                        std::string sub = cleanJson(wp.settingsText);
                        rapidjson::Document sd;
                        sd.Parse<rapidjson::kParseTrailingCommasFlag>(sub.c_str());
                        if (sd.HasParseError() || !sd.IsObject()) { wp.failed = true; }
                        else readSettings(sd, wp.settings);
                    }
                    if (!wp.failed) {
                        if (onProgress) onProgress(0.15f, "Extracting level data...");
                        angleData = std::move(wp.angles);
                        settings = wp.settings;
                        if (wp.seenPath) pathData = std::move(wp.pathText);
                        if (actions.empty()) actions = std::move(wp.actions);
                        else actions.insert(actions.end(), std::make_move_iterator(wp.actions.begin()),
                                            std::make_move_iterator(wp.actions.end()));
                        return finishLoad(onProgress, exportOnly);
                    }
                }
                // rc == 0 或 settings 解析失败 -> 落到整份解压，行为与以前完全一致
                if (requireWindow) {
                    LOG_E("windowed parse declined the stream (test hook ADOCAO_WINDOW_REQUIRE)");
                    return false;
                }
            }
        }
        if (onProgress) onProgress(0.05f, "Decompressing level...");
        std::string reason;
        // 解压在这类谱面上约占 1/4 加载时间，进度条给它相应的一段（0.05 -> 0.20）
        auto progress = [&](float p) {
            if (onProgress) onProgress(0.05f + p * 0.15f, "Decompressing level...");
        };
        const ArchiveBackend& bk = archiveBackend();
        if (!bk.decodeWhole) {
            LOG_E("Cannot decompress level (%s): 这个构建没有 archive 模块（没调用 adofai::archive::install()）",
                  archive == LevelArchiveKind::Xz ? "xz" : "zstd");
            return false;
        }
        if (!bk.decodeWhole(data, len, archive, decompressed, reason, progress)) {
            LOG_E("Cannot decompress level (%s): %s",
                  archive == LevelArchiveKind::Xz ? "xz" : "zstd", reason.c_str());
            return false;
        }
        data = decompressed.data();
        len = decompressed.size();
    }

    // Skip UTF-8 BOM if present (RapidJSON rejects it at offset 0)
    if (len >= 3 && (unsigned char)data[0] == 0xEF
        && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        data += 3;
        len -= 3;
    }
    if (len == 0) {
        LOG_E("Cannot open level file: empty");
        return false;
    }

    // ADOCAO_FORCE_DOM_PARSE=1 强制走旧路径（对拍用，见 tests/level_parse_test.cpp）。
    // 每次加载都读一遍环境变量，测试才能在同一个进程里对拍两条路径。
    const bool forceLegacy = (std::getenv("ADOCAO_FORCE_DOM_PARSE") != nullptr);

    try {
        if (!forceLegacy) {
            if (tryFastParse(data, len, onProgress)) return finishLoad(onProgress, exportOnly);
            // 测试用：要求快路径必须能吃下这个文件（不许静默回退到 DOM），用来界定
            // "窗口路径也必须能吃下"的范围。
            if (std::getenv("ADOCAO_FAST_REQUIRE") != nullptr) {
                LOG_E("fast parse declined the buffer (test hook ADOCAO_FAST_REQUIRE)");
                return false;
            }
        }
        if (onProgress) onProgress(0.10f, "Parsing angleData...");
        std::string content(data, len);
        return parseLegacy(cleanJson(content), onProgress, exportOnly);
    } catch (const std::exception& e) {
        LOG_E("JSON parse error: %s", e.what());
        return false;
    }
}

// 快路径：mmap 出来的原文直接扫。任何一步复现不了 cleanJson 的语义就返回 false，
// 此时 this 还没有被改动过，调用方会走旧路径。
bool LevelData::tryFastParse(const char* data, size_t len, ProgressCb onProgress) {
    if (onProgress) onProgress(0.10f, "Parsing angleData...");
    std::vector<FastAction> newActions;   // actions 在根扫描里就地解析
    Regions r;
    if (!scanRootMembers(data, data + len, r, newActions)) return false;

    if (onProgress) onProgress(0.12f, "Parsing JSON...");

    std::vector<double> newAngles;
    if (r.angle) {
        // angleData 只有几十 MB，加上整数快路径后 6.77 M 个值只要 ~25 ms
        newAngles.reserve((size_t)(r.angleEnd - r.angle) / 3);
        if (!parseAngleDataRegion(r.angle, r.angleEnd, newAngles)) return false;
    }

    Settings newSettings = settings;   // 与旧路径一致：只覆盖文件里出现的字段
    std::string newPath = pathData;
    if (r.settings) {
        // settings 对象很小，交给 cleanJson + RapidJSON 处理，省得再写一个解析器
        std::string sub = cleanJson(std::string(r.settings, (size_t)(r.settingsEnd - r.settings)));
        rapidjson::Document s;
        s.Parse<rapidjson::kParseTrailingCommasFlag>(sub.c_str());
        if (s.HasParseError() || !s.IsObject()) return false;
        readSettings(s, newSettings);
    }
    if (r.path && r.pathEnd > r.path + 1) {
        // 旧路径用 GetString()（会解转义），带反斜杠就交回旧路径；裸 CR 则照 cleanJson 删掉
        if (std::memchr(r.path, '\\', (size_t)(r.pathEnd - r.path))) return false;
        newPath.assign(r.path + 1, (size_t)(r.pathEnd - r.path - 1));
        newPath.erase(std::remove(newPath.begin(), newPath.end(), '\r'), newPath.end());
    }

    if (onProgress) onProgress(0.15f, "Extracting level data...");

    angleData = std::move(newAngles);
    if (actions.empty()) actions = std::move(newActions);
    else actions.insert(actions.end(), std::make_move_iterator(newActions.begin()),
                        std::make_move_iterator(newActions.end()));
    settings = newSettings;
    pathData = std::move(newPath);
    return true;
}

// 旧路径：cleanJson + RapidJSON DOM。保持原样，是回退路径也是对拍基准。
bool LevelData::parseLegacy(const std::string& jsonStr, ProgressCb onProgress, bool exportOnly) {
    // Fast path: parse angleData directly without JSON DOM allocation
    size_t angleDataEnd = 0;
    angleData = parseAngleDataFast(jsonStr.c_str(), jsonStr.size(), angleDataEnd);

    // Helper: strip a JSON array value from a string, replacing it with [].
    // Returns the stripped string. Prevents nlohmann from parsing huge unused arrays.
    auto stripArray = [](const std::string& src, const char* key) -> std::string {
        const char* p = std::strstr(src.c_str(), key);
        if (!p) return src;
        size_t keyStart = p - src.c_str();
        size_t arrStart = keyStart + strlen(key);
        while (arrStart < src.size() && (src[arrStart] == ' ' || src[arrStart] == ':'
               || src[arrStart] == '\t' || src[arrStart] == '\n'))
            arrStart++;
        if (arrStart >= src.size() || src[arrStart] != '[') return src;
        // Count brackets to find matching ]
        int depth = 1;
        size_t arrEnd = arrStart + 1;
        bool inString = false;
        for (; arrEnd < src.size() && depth > 0; arrEnd++) {
            char c = src[arrEnd];
            if (c == '"' && (arrEnd == 0 || src[arrEnd-1] != '\\')) inString = !inString;
            if (inString) continue;
            if (c == '[') depth++;
            else if (c == ']') depth--;
        }
        std::string out;
        out.reserve(keyStart + 2 + (src.size() - arrEnd));
        out.append(src, 0, arrStart);
        out += "[]";
        out.append(src, arrEnd, std::string::npos);
        return out;
    };

    // RapidJSON: parse full JSON (angleData + decorations stripped)
    // This handles both actions and settings in one DOM
    std::string stripped = jsonStr;
    if (angleDataEnd > 0) {
        stripped = stripArray(jsonStr, "\"angleData\"");
    }
    stripped = stripArray(stripped, "\"decorations\"");

    if (onProgress) onProgress(0.12f, "Parsing JSON...");
    rapidjson::Document root;
    root.Parse<rapidjson::kParseTrailingCommasFlag>(stripped.c_str());
    if (root.HasParseError()) {
        LOG_E("RapidJSON parse error at offset %zu, code %d",
              root.GetErrorOffset(), (int)root.GetParseError());
        return false;
    }

    if (onProgress) onProgress(0.15f, "Extracting level data...");

    // Actions
    if (root.HasMember("actions") && root["actions"].IsArray()) {
        auto& arr = root["actions"];
        for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
            auto& a = arr[i];
            if (!a.IsObject() || !a.HasMember("floor") || !a.HasMember("eventType")) continue;
            FastAction act;
            act.floor = a["floor"].GetInt();
            std::string et = a["eventType"].GetString();
            if (et == "Twirl") act.type = FastAction::Twirl;
            else if (et == "SetSpeed") act.type = FastAction::SetSpeed;
            else if (et == "PositionTrack") act.type = FastAction::PositionTrack;
            else if (et == "SetHitsound") act.type = FastAction::SetHitsound;
            else if (et == "Bookmark") act.type = FastAction::Bookmark;
            else if (et == "Pause") act.type = FastAction::Pause;
            else if (et == "AnimateTrack") act.type = FastAction::AnimateTrack;
            else continue;
            if (act.type == FastAction::SetSpeed) {
                if (a.HasMember("speedType") && std::string(a["speedType"].GetString()) == "Multiplier") {
                    act.flag = true; act.val1 = a.HasMember("bpmMultiplier") ? a["bpmMultiplier"].GetFloat() : 1.0f;
                } else { act.val1 = a.HasMember("beatsPerMinute") ? a["beatsPerMinute"].GetFloat() : 0.0f; }
            } else if (act.type == FastAction::Pause) {
                act.val1 = a.HasMember("duration") ? a["duration"].GetFloat() : 0.0f;
            } else if (act.type == FastAction::PositionTrack) {
                if (a.HasMember("positionOffset") && a["positionOffset"].IsArray() && a["positionOffset"].Size() >= 2) {
                    act.val1 = a["positionOffset"][0].GetFloat(); act.val2 = a["positionOffset"][1].GetFloat();
                }
                if (a.HasMember("justThisTile")) {
                    if (a["justThisTile"].IsBool()) act.flag = a["justThisTile"].GetBool();
                    else if (a["justThisTile"].IsInt()) act.flag = a["justThisTile"].GetInt() != 0;
                    else if (a["justThisTile"].IsString()) {
                        std::string v = a["justThisTile"].GetString();
                        act.flag = (v == "Enabled" || v == "true" || v == "True");
                    }
                }
            } else if (act.type == FastAction::SetHitsound) {
                act.strId = g_internOwner ? g_internOwner->internActionStr(a.HasMember("hitsound") ? std::string(a["hitsound"].GetString()) : std::string()) : 0;
                act.val1 = a.HasMember("hitsoundVolume") ? a["hitsoundVolume"].GetFloat() : 0.0f;
                act.flag = a.HasMember("hitsoundVolume");
            } else if (act.type == FastAction::AnimateTrack) {
                act.val1 = -1.0f; act.val2 = -1.0f; // sentinel: not set
                if (a.HasMember("trackDisappearAnimation")) act.strId = g_internOwner ? g_internOwner->internActionStr(a["trackDisappearAnimation"].GetString()) : 0;
                if (a.HasMember("trackAnimation"))  act.flag = true; // flag2: has trackAnimation
                if (a.HasMember("beatsBehind")) act.val1 = a["beatsBehind"].GetFloat();
                if (a.HasMember("beatsAhead"))  act.val2 = a["beatsAhead"].GetFloat();
            }
            actions.push_back(act);
        }
    }

    // Settings
    if (root.HasMember("settings") && root["settings"].IsObject()) {
        readSettings(root["settings"], settings);
    }

    // pathData
    if (root.HasMember("pathData") && root["pathData"].IsString())
        pathData = root["pathData"].GetString();

    // actions already parsed above
    // decorations: not used, skip parsing entirely

    return finishLoad(onProgress, exportOnly);
}

bool LevelData::finishLoad(ProgressCb onProgress, bool exportOnly) {
    if (onProgress) onProgress(0.20f, "Processing level data...");

    // Convert pathData → angleData if needed
    if (!pathData.empty() && angleData.empty()) {
        convertPathToAngles();
    }

    if (!exportOnly) {
        if (onProgress) onProgress(0.30f, "Calculating tile positions...");
        calculateTilePositions();
    }
    if (onProgress) onProgress(0.40f, "Processing actions...");
    processActions();
    if (!exportOnly) {
        applyPositionTrackOffsets();
    }
    return true;
}

void LevelData::calculateTilePositions() {
    tiles.clear();
    if (angleData.empty()) return;

    int n = static_cast<int>(angleData.size());

    // Build "floats" array: 999 = midspin (previous + 180)
    std::vector<float> floats(n);
    for (int i = 0; i < n; i++) {
        if (angleData[i] == 999.0) {
            floats[i] = (i > 0 ? floats[i - 1] : 0.0f) + 180.0f;
        } else {
            floats[i] = angleData[i];
        }
    }

    tiles.resize(n);
    double curX = 0.0, curY = 0.0;  // double for precision

    for (int i = 0; i < n; i++) {
        tiles[i].position = {curX, curY};
        tiles[i].direction = floats[i];

        double rad = (double)floats[i] * 3.14159265358979323846 / 180.0;
        curX += std::cos(rad);
        curY += std::sin(rad);
    }

    // Append extra tile (infinite rotation reference)
    if (n > 0) {
        Tile extra;
        double dir = 0.0, length = 1.0;
        if (n > 1) {
            double dx = (double)tiles[n-1].position[0] - (double)tiles[n-2].position[0];
            double dy = (double)tiles[n-1].position[1] - (double)tiles[n-2].position[1];
            length = std::sqrt(dx*dx + dy*dy);
            if (length > 0.01) dir = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
            if (length < 0.01) length = 1.0;
        }
        double rad = dir * 3.14159265358979323846 / 180.0;
        extra.position = {
            (float)(tiles[n-1].position[0] + std::cos(rad) * length),
            (float)(tiles[n-1].position[1] + std::sin(rad) * length)
        };
        extra.angle = 180.0f;
        extra.direction = (float)dir;
        tiles.push_back(extra);
    }
}

// ADOFAI pathData → angleData conversion
// Based on ADOFAI-JS official mapping table (src/pathdata/index.ts)

float LevelData::pathCharToAngle(char c) {
    switch (c) {
        case 'R': return 0;
        case 'p': return 15;
        case 'J': return 30;
        case 'E': return 45;
        case 'T': return 60;
        case 'o': return 75;
        case 'U': return 90;
        case 'q': return 105;
        case 'G': return 120;
        case 'Q': return 135;
        case 'H': return 150;
        case 'W': return 165;
        case 'L': return 180;
        case 'x': return 195;
        case 'N': return 210;
        case 'Z': return 225;
        case 'F': return 240;
        case 'V': return 255;
        case 'D': return 270;
        case 'Y': return 285;
        case 'B': return 300;
        case 'C': return 315;
        case 'M': return 330;
        case 'A': return 345;
        case '5': return 555;   // multi-hit stack 5
        case '6': return 666;   // multi-hit stack 6
        case '7': return 777;   // multi-hit stack 7
        case '8': return 888;   // multi-hit stack 8
        case '!': return 999;   // midspin
        default:  return 0;
    }
}

void LevelData::processActions() {
    int n = (int)angleData.size() + 1;  // +1 for tile 0 (tiles may be empty in export mode)
    tileBPMs.assign(n, settings.bpm);
    tileHasTwirl.assign(n, false);
    tileHasSetSpeed.assign(n, false);
    bookmarkFloors.clear();

    struct SS { float multiplier = 0.0f; float bpm = 0.0f; bool isMultiplier = false; };
    std::vector<SS> setSpeedByFloor(n);

    struct HSChange { int floor; std::string type; float volume; };
    std::vector<HSChange> hsChanges;

    for (auto& a : actions) {
        int floor = a.floor;
        if (floor < 0 || floor >= n) continue;
        switch (a.type) {
        case FastAction::Twirl:
            tileHasTwirl[floor] = true; break;
        case FastAction::SetSpeed:
            tileHasSetSpeed[floor] = true;
            { SS& ev = setSpeedByFloor[floor]; ev.isMultiplier = a.flag;
              if (a.flag) ev.multiplier = a.val1; else ev.bpm = a.val1; }
            break;
        case FastAction::PositionTrack:
            tilePositionOffsets[floor] = {a.val1, a.val2, a.flag}; break;
        case FastAction::SetHitsound:
            hsChanges.push_back({floor, actionStr(a).empty() ? settings.hitsound : actionStr(a),
                                 a.flag ? a.val1 : settings.hitsoundVolume}); break;
        case FastAction::Bookmark:
            bookmarkFloors.push_back(floor); break;
        case FastAction::AnimateTrack:
            atStates[floor] = {actionStr(a).empty() ? settings.trackDisappearAnimation : actionStr(a),
                               settings.trackAnimation, // aa not parsed yet; use global
                               a.val1 >= 0 ? a.val1 : settings.beatsBehind,
                               a.val2 >= 0 ? a.val2 : settings.beatsAhead,
                               a.flag};
            break;
        default: break;
        }
    }

    float runningBPM = settings.bpm;
    for (int i = 0; i < n; i++) {
        if (setSpeedByFloor[i].isMultiplier) runningBPM *= setSpeedByFloor[i].multiplier;
        else if (setSpeedByFloor[i].bpm > 0.0f) runningBPM = setSpeedByFloor[i].bpm;
        tileBPMs[i] = runningBPM;
    }

    if (!hsChanges.empty()) {
        std::string curHS = settings.hitsound;
        float curVol = settings.hitsoundVolume;
        size_t ci = 0;
        for (int i = 0; i < n; i++) {
            while (ci < hsChanges.size() && hsChanges[ci].floor <= i) {
                curHS = hsChanges[ci].type; curVol = hsChanges[ci].volume; ci++;
            }
            if (curHS != settings.hitsound) tileHitsounds[i] = curHS;
            if (curVol != settings.hitsoundVolume) {
                if (tileHitsoundVolumes.size() != (size_t)n)
                    tileHitsoundVolumes.assign((size_t)n, std::numeric_limits<float>::quiet_NaN());
                tileHitsoundVolumes[(size_t)i] = curVol;
            }
        }
    }

    std::sort(bookmarkFloors.begin(), bookmarkFloors.end());

    // Viewer convenience: tile 0 and the final tile are always jump targets,
    // even when the chart defines no Bookmark events (Ctrl+←/→ while stopped).
    bookmarkFloors.push_back(0);
    if (n > 1) bookmarkFloors.push_back(n - 1);
    std::sort(bookmarkFloors.begin(), bookmarkFloors.end());
    bookmarkFloors.erase(std::unique(bookmarkFloors.begin(), bookmarkFloors.end()),
                         bookmarkFloors.end());
}

void LevelData::applyPositionTrackOffsets() {
    if (tilePositionOffsets.empty()) return;

    double cumX = 0.0, cumY = 0.0;
    int n = (int)tiles.size();

    std::vector<std::pair<int, TilePositionOffset>> sorted(tilePositionOffsets.begin(), tilePositionOffsets.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
    size_t oi = 0;

    for (int i = 0; i < n; i++) {
        while (oi < sorted.size() && sorted[oi].first == i) {
            cumX += sorted[oi].second.offsetX;
            cumY += sorted[oi].second.offsetY;
            oi++;
        }
        tiles[i].position[0] += cumX;
        tiles[i].position[1] += cumY;
    }
}

void LevelData::releaseMemory() {
    // Free arrays no longer needed after loading completes
    actions.clear(); actions.shrink_to_fit();
    tilePositionOffsets.clear();
    tileHitsounds.clear();
    tileHitsoundVolumes.clear(); tileHitsoundVolumes.shrink_to_fit();
    std::string().swap(pathData);
    // angleData kept: needed by TileMesh::build() for midspin detection
    // tileBPMs kept: needed by buildIcons() for SetSpeed icon coloring
    // tileHasTwirl/tileHasSetSpeed kept: needed by buildIcons()
    // bookmarkFloors kept: needed during gameplay for Ctrl+Left/Right navigation
}

void LevelData::convertPathToAngles() {
    if (pathData.empty()) return;

    angleData.clear();
    angleData.reserve(pathData.size());

    for (char c : pathData) {
        angleData.push_back(pathCharToAngle(c));
    }
}

}  // namespace adofai
