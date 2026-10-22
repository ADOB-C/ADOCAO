#include "LevelData.hpp"
#include "core/timeline/Timeline.hpp"
#include "FloorWindow.hpp"   // ③ 切片 4：按块物化缓存（~LevelData 需要完整类型）
#include "JsonCleaner.hpp"
#include "ByteSource.hpp"   // 容器识别 + 解压后端接口（实现见 archive/）
#include "JsonScan.hpp"     // 结构扫描的 SIMD 字节分类（纯结构，不碰数值）
#include "core/util/Logger.hpp"
#include "core/util/Env.hpp"
#include "core/util/Scheduler.hpp"
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include "core/util/CountLimits.hpp"
#include "core/util/Easing.hpp"


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

namespace { constexpr size_t kRecolorBlock = 4096; constexpr size_t kRecolorMaxEntries = 4u << 20; }

// 快路径回退到旧路径时"在哪个阶段放弃"。以前这里是**静默**的：MYC 这种大谱一直在跑旧路径
// （实测 4.6 s / ~4.4 GB，快路径是 ~0.7 s / ~1.6 GB），却没有任何测试或日志能看出来 ——
// 与当年"窗口路径静默回退让测试全绿"是同一类坑。ADOCAO_FAST_REQUIRE=1 现在会把阶段名带出来。
static const char* g_fastStage = "start";
static const char* g_bufBase = nullptr;      // 文件缓冲区起点（把内部指针换算成文件偏移）
static const char* g_failPos = nullptr;      // 放弃点（内部指针）
static char g_fastStageBuf[96];
// 带字节偏移的阶段名（偏移是相对缓冲区起点，即文件偏移，便于直接去看那一段字节）
static void setFastStage(const char* what, long long off) {
    std::snprintf(g_fastStageBuf, sizeof g_fastStageBuf, "%s@%lld", what, off);
    g_fastStage = g_fastStageBuf;
}

// 事件字符串驻留表（`actionStrTable`）当前归谁所有。**进程级单例**，理由：驻留要在
// `buildAction()`（并行分块里跑的纯函数）中访问表，传指针会污染整条调用链。
// ⚠️ 因此**不支持"同时加载两个 LevelData"**：并行加载两份谱时后者会把前者的表覆盖掉，
// buildAction 于是把字符串驻留进别的关卡的表（strId 语义错，症状是 hitsound 名/动画名串台）。
// 现在所有入口都是"一次加载一份"（GUI 单关卡、CLI 单谱、测试串行），要支持并发加载
// 得先把这张表挪进加载上下文（`LoadContext`）再传下去 —— 别只把这行改成 thread_local ✗
// （并行分块的 worker 之间要共享同一份表，per-thread 会各自建表、strId 对不上）。
// parseNumber 的栈上 token 缓冲：双精度十进制最长 ~24 字符，128 是宽裕上限（超了走堆）
constexpr size_t kNumBufLen = 128;

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

// 字符串扫描走 core/level/JsonScan.hpp（memchr + 转义回退），语义与逐字节版本相同。
using jsonscan::scanStringEnd;
using jsonscan::scanStringEndExact;

// p 指向 '[' 或 '{'；返回配对的闭合符之后的位置。
// 实现走 core/level/JsonScan.hpp：整块掩码抹掉字符串里的括号，只对括号做顺序配对。
// 返回指针与原来的逐字节版本逐字节相同（见该文件开头的说明）。
using jsonscan::skipContainer;

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
        // 先在 [start, e) 里把数字 token 界定出来，再转换 —— **不能**直接把 start 交给 strtod：
        // strtod 要求 NUL 终止，而这里的缓冲区只保证到 e（流式窗口的半窗在 e 之后是上一窗的
        // 残留字节；app 的 mmap 路径更糟：文件末尾不在 e 处终止，读过去可能直接 SIGSEGV）。
        // 越界读会给出错值（实测 Unity.wav_rate 上 4 KB 半窗因此放弃窗口、回退整份解压）。
        const char* q = start;
        if (q < e && (*q == '-' || *q == '+')) ++q;
        while (q < e && *q >= '0' && *q <= '9') ++q;
        if (q < e && *q == '.') { ++q; while (q < e && *q >= '0' && *q <= '9') ++q; }
        if (q < e && (*q == 'e' || *q == 'E')) {
            const char* save = q;
            ++q;
            if (q < e && (*q == '+' || *q == '-')) ++q;
            if (q < e && *q >= '0' && *q <= '9') { while (q < e && *q >= '0' && *q <= '9') ++q; }
            else if (q >= e) {
                // 缓冲区正好在 e / e± 之后用完：**这个数字还没结束**（指数位可以更长），
                // 绝不能退回到 save 把 "4.547473508864641" 当成终值提交 —— 那是**静默错值**，
                // 而且残留的 e 会让流式窗口把整窗原样 carry 走（下一窗第一个字节就是 e，
                // parseNumber 立刻失败 -> carry = 整个半窗）-> stuck -> 静默回退整份解压。
                // 实测：Unity.wav_rate 默认 4 MB 半窗 3/3 拒收、8192 KB 才收（docs/2e9.md §3）。
                // 返回 false = "这里没有一个完整的数字"：区间调用方会放弃快路径（走 DOM），
                // 流式窗口会当作"需要更多数据"把这一项 carry 到下一窗重放。
                p = start;
                return false;
            }
            else q = save;                      // 光秃秃的 e / e+ 之后不是数字：不算指数
        }
        // 注意：**不能**把"token 触到 e"当成截断。本函数的两个调用方（angleData 区间、action 字段）
        // 传进来的 e 都是**该值/该区间的真实末尾**（来自 skipValue / 区域括号），所以一个
        // "正好填满这个跨度"的完整 token 是正常的 —— 早期版本在这里返回 false，于是**所有非整数
        // 的 action 字段**（bpmMultiplier/beatsPerMinute/angleOffset…）都会让整个文件放弃快路径
        // （整数走上面的整数快路径，不经过这里，所以只有小数中招）。流式窗口那条路有它自己的
        // 可续扫描器，不经过本函数。
        if (q <= start) { p = start; return false; }
        // 到这里 [start, q) 是完整 token，复制到本地缓冲再 NUL 终止后转换
        // （仍然走 strtod，保证与旧路径 parseAngleDataFast 逐位同值）。
        // 128 = 双精度十进制的最长表示（~24 字符）+ 大量余量；超长罕见 token 走堆，不截断。
        char buf[kNumBufLen];
        const size_t n = (size_t)(q - start);
        std::string heap;
        char* dst = buf;
        if (n + 1 > sizeof buf) { heap.assign(start, q); dst = &heap[0]; }
        else std::memcpy(buf, start, n);
        dst[n] = '\0';
        char* endp = nullptr;
        double d = std::strtod(dst, &endp);
        if (endp != dst + n) { p = start; return false; }   // 没有整段消费掉：交给上层回退
        p = q;
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
    // 2^31−1 层目标 #0 的契约：**绝不静默截断**。以前这里直接 `out = (int)v`，
    // 于是 parseInt("2147483648") 得到 −2147483648，接着被 floor>=0 && floor<n 的守卫
    // **无声丢掉**（没有日志）。今天 tiles 还是 32 位索引、这个谱确实装不下，
    // 所以先做到"放弃快路径 + 喊出来"；阶段 2 把计数改成 int64 后这条会自然消失。
    if (v > (int64_t)(neg ? 2147483648LL : 2147483647LL)) {
        LOG_E("parseInt: 整数超出 int32 范围（'%.*s'）—— 放弃快路径，不截断",
              (int)(p - start), start);
        p = start;
        return false;
    }
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

// 运镜/重复事件的输出槽。热路径只带一个空 sink，几乎零成本；
// 并行分块各自一个 sink，最后按段序合并（文件顺序 = 稳定次序）。
struct CameraSink {
    std::vector<LevelData::CameraEvent> cameras;
    std::vector<LevelData::RepeatEvent> repeats;
    std::vector<LevelData::ColorEvent> colors;   // ColorTrack / RecolorTrack
    std::vector<LevelData::FlashEvent> flashes;  // Flash
    std::vector<LevelData::MoveTrackEvent> moves; // MoveTrack
};

// 前置声明：actions 在根扫描过程中就地解析（见 scanRootMembers）。
inline bool parseActionRegion(const char* b, const char* end,
                              std::vector<LevelData::FastAction>& out,
                              CameraSink& sink, const char** outEnd);
inline bool parseActionRegionParallel(const char* b, const char* fileEnd,
                                      std::vector<LevelData::FastAction>& out,
                                      CameraSink& sink, const char** outEnd);

// 只遍历根对象的直接成员。逗号允许缺失（cleanJson 会给漏写逗号的文件补上），
// 未知成员和 decorations 整体跳过，全程不复制。
// actions 是边扫边解析的：解析器自己在配对的 ']' 处停下并回报结束位置，于是整段
// actions 只被走一遍（1.5 GB 的谱上，多走一遍就是 840 ms）。
inline bool scanRootMembers(const char* s, const char* e, Regions& r,
                            std::vector<LevelData::FastAction>& actionsOut,
                            CameraSink& cameraSink) {
    const char* p = skipWs(s, e);
    if (p >= e || *p != '{') return false;
    for (++p;;) {
        p = skipWs(p, e);
        setFastStage("scanRootMembers/member", (long long)(p - s));   // 放弃时看这里
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
            setFastStage("scanRootMembers/actions", (long long)(p - s));
            if (!parseActionRegionParallel(p, e, actionsOut, cameraSink, &aEnd)) return false;
            r.actions = p;
            r.actionsEnd = aEnd;
            p = aEnd;
            continue;
        }
        setFastStage("scanRootMembers/value", (long long)(p - s));
        // angleData 是几十 MB 的数字数组，而这里只需要它的结尾位置：先找第一个
        // {']', '"', '[', '{'}，没有引号/嵌套时那个 ']' 就是配对的那个（与 skipValue
        // 返回同一个指针）；出现引号或嵌套容器就整段交回 skipValue。
        // 数值本身仍由 parseAngleDataRegion 逐位解析，这里一个数都不读。
        const char* vEnd = (keyIs(key, "angleData") && *p == '[')
                               ? jsonscan::skipSimpleArrayEnd(p, e)
                               : skipValue(p, e);
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
        // ⚠️ 这里的根成员扫描（成员名/逗号容错/值跳过）与 `WindowParser::step()` 是**两份实现**：
        // 快路径吃整块缓冲、窗口路径要能跨窗挂起，没法直接共用。改 cleanJson 的容错规则时
        // **两处一起改**，否则"明文能读、压缩谱放弃快路径"——`tests/level_parse_test.cpp` 的对拍
        // 会红（两条路各跑一遍），但只在覆盖到的语法上红，逐条扫一遍更稳。
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
    if (keyIs(k, "MoveCamera"))    return T::MoveCamera;     // → cameraEvents（富结构）
    if (keyIs(k, "RepeatEvents"))  return T::RepeatEvents;   // → repeatEvents（富结构）
    if (keyIs(k, "ColorTrack"))    return T::ColorTrack;     // → colorEvents（富结构）
    if (keyIs(k, "RecolorTrack"))  return T::RecolorTrack;   // → colorEvents（范围 + 触发时间）
    if (keyIs(k, "Flash"))         return T::Flash;          // → flashEvents（全屏闪屏）
    if (keyIs(k, "MoveTrack"))     return T::MoveTrack;      // → moveTrackEvents（轨道动画）
    if (keyIs(k, "ScaleRadius"))   return T::ScaleRadius;    // 轨道半径缩放（烘进砖位）
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
    std::string hitsound, trackDisappear, trackAnimation;

    // ── 以下只给 MoveCamera / RepeatEvents 用（rich 模式才填；热路径一个字段都不碰） ──
    bool hasAngleOffset = false; double angleOffset = 0.0;
    bool hasPosition = false;                 // position 数组存在（分量可能全是 null）
    bool hasPosX = false, hasPosY = false; double posX = 0.0, posY = 0.0;
    bool hasRotation = false; double rotation = 0.0;
    bool hasZoom = false; double zoom = 100.0;
    bool hasEase = false; int easeId = 0;
    int  relativeTo = -1;            // -1 未写；0..4 合法；100 写了但名字不认得
    bool hasId = false; double idNum = 0.0;
    const char* tagPtr = nullptr; const char* tagEnd = nullptr;
    bool inactive = false, editorOnly = false;
    uint8_t disabledMask = 0;
    bool isBeat = true, executeOnCurrentFloor = false;
    double repetitions = 0.0, floorCount = 0.0, interval = 1.0, gapLength = 1.0;

    // ── ColorTrack ────────────────────────────────────────────────────
    bool hasTrackColor = false;        std::string trackColor;
    bool hasSecondaryTrackColor = false; std::string secondaryTrackColor;
    bool hasTrackColorType = false;    std::string trackColorType;
    bool hasTrackColorPulse = false;   std::string trackColorPulse;
    bool hasTrackStyle = false;        std::string trackStyle;
    bool hasTrackAnimDuration = false; float trackAnimDuration = 2.0f;
    bool hasTrackPulseLength = false;  float trackPulseLength = 10.0f;
    bool hasTrackGlow = false;         float trackGlow = 100.0f;
    // ── RecolorTrack：范围 ────────────────────────────────────────────
    // ── Flash ────────────────────────────────────────────────────────
    bool hasFlashPlane = false;  std::string flashPlane;
    bool hasStartColor = false;  std::string startColor;
    bool hasEndColor = false;    std::string endColor;
    bool hasStartOpacity = false, hasEndOpacity = false, hasLegacyOpacity = false;
    float startOpacity = 100.0f, endOpacity = 0.0f, legacyOpacity = 100.0f;
    bool hasFlashStyle = false;  std::string flashStyle;
    bool hasLegacyColor = false, hasLegacyColorTo = false;
    std::string legacyColor, legacyColorTo;
    // ── MoveTrack ────────────────────────────────────────────────────
    bool hasOffX = false, hasOffY = false;
    bool hasRotOffset = false;   float rotOffset = 0.0f;
    bool hasScale = false;       float scaleX = 1.0f, scaleY = 1.0f;
    bool hasStartTile = false, hasEndTile = false, hasGapLength = false;
    int32_t startOff = 0, endOff = 0;
    int8_t  startRel = -1, endRel = -1;   // -1 绝对 / 0 ThisTile / 1 Start / 2 End
};


// 取出字符串值。带转义或裸 CR 的一律交回旧路径（旧路径会解转义 / 删 CR）。
inline bool stringValue(const char* p, const char* vEnd, std::string& out) {
    if (*p != '"') return false;
    for (const char* q = p; q < vEnd; ++q)
        if (*q == '\\' || *q == '\r') return false;
    out.assign(p + 1, vEnd - 1);
    return true;
}


// ── rich 模式的小工具（只给 MoveCamera / RepeatEvents 用） ───────────────────
// 原则：rich 解析**从不 return false** —— 它跑在已经成功过一次的对象上，任何看不懂的
// 值（怪类型/转义字符串/非对象）都只是"这个字段没写"，绝不能让整份谱加载失败。
inline bool parseBoolValue(const char* v, const char* e, bool& out) {
    if (e - v == 4 && std::memcmp(v, "true", 4) == 0) { out = true; return true; }
    if (e - v == 5 && std::memcmp(v, "false", 5) == 0) { out = false; return true; }
    if (e > v && *v == '"') {
        std::string s;
        if (!stringValue(v, e, s)) return false;
        out = (s == "Enabled" || s == "true" || s == "True");
        return true;
    }
    return false;
}

// `active: false` / `active: "Disabled"` → 整条事件失效（Re_ADOJAS isEventActive）。
inline void parseActiveValue(const char* v, const char* e, ActionFields& f) {
    if (e - v == 5 && std::memcmp(v, "false", 5) == 0) { f.inactive = true; return; }
    if (e > v && *v == '"') {
        std::string s;
        if (stringValue(v, e, s) && s == "Disabled") f.inactive = true;
    }
}

// `disabled: { "position": true, … }` —— 字段级过滤器（true = 该字段不生效）。
inline void parseDisabledObject(const char* v, const char* e, ActionFields& f) {
    if (e <= v || *v != '{') return;
    const char* p = v + 1;
    for (;;) {
        p = skipWs(p, e);
        if (p >= e || *p == '}') return;
        if (*p == ',') { ++p; continue; }
        if (*p != '"') return;
        const char* kEnd = scanStringEndExact(p, e);
        if (!kEnd) return;
        KeyRef key{p + 1, (size_t)(kEnd - p - 1)};
        p = skipWs(kEnd + 1, e);
        if (p >= e || *p != ':') return;
        p = skipWs(p + 1, e);
        const char* vEnd = skipValue(p, e);
        if (!vEnd) return;
        bool on = false;
        if (parseBoolValue(p, vEnd, on) && on) {
            if (keyIs(key, "position"))        f.disabledMask |= LevelData::kCamDisPosition;
            else if (keyIs(key, "rotation"))   f.disabledMask |= LevelData::kCamDisRotation;
            else if (keyIs(key, "zoom"))       f.disabledMask |= LevelData::kCamDisZoom;
            else if (keyIs(key, "relativeTo")) f.disabledMask |= LevelData::kCamDisRel;
            else if (keyIs(key, "active"))     f.inactive = true;
        }
        p = vEnd;
    }
}

// `ease: "OutCubic"` → Ease id（未知名字 = Linear，与 Re_ADOJAS 的 `|| Linear` 一致）。
inline bool easeIdOf(const char* v, const char* e, int& out) {
    std::string s;
    if (!stringValue(v, e, s)) return false;
    out = (int)easeFromName(s);
    return true;
}

// `position: [x, y]`，两个分量各自可以是 null（Re_ADOJAS：分量 null/缺失 → 该轴不动）。
inline void parsePositionPair(const char* v, const char* e, ActionFields& f) {
    if (e <= v || *v != '[') return;          // 非数组：旧路径与 Re_ADOJAS 都当"没写"
    f.hasPosition = true;
    const char* p = v + 1;
    for (int axis = 0; axis < 2; axis++) {
        p = skipWs(p, e);
        if (p >= e || *p == ']') return;
        if (axis) {                              // 第二轴之前要有逗号
            if (*p != ',') return;
            p = skipWs(p + 1, e);
            if (p >= e || *p == ']') return;
        }
        double num;
        if (parseNumber(p, e, num)) {
            if (axis == 0) { f.posX = num; f.hasPosX = true; }
            else           { f.posY = num; f.hasPosY = true; }
        }
        // else: null / 其它字面量 → 这一轴保持"没写"
        while (p < e && *p != ',' && *p != ']') ++p;
    }
}

// `relativeTo`：字符串（5 个名字之一）或数字（0..4）。
// 语义差别要照抄 Re_ADOJAS 的 parseMovementType：
//   数字越界/非整数 → **Player**（`MOVEMENT_TYPES[raw] || 'Player'`）；
//   字符串认不得   → **Unknown**（原函数返回 undefined → 运行时沿用上一次模式）。
inline void parseRelativeToValue(const char* v, const char* e, ActionFields& f) {
    using C = LevelData;
    if (e > v && *v == '"') {
        std::string s;
        if (!stringValue(v, e, s)) return;
        if      (s == "Player")                f.relativeTo = C::CamPlayer;
        else if (s == "Tile")                  f.relativeTo = C::CamTile;
        else if (s == "Global")                f.relativeTo = C::CamGlobal;
        else if (s == "LastPosition")          f.relativeTo = C::CamLastPosition;
        else if (s == "LastPositionNoRotation")f.relativeTo = C::CamLastPositionNoRotation;
        else                                   f.relativeTo = C::CamUnknown;
        return;
    }
    double d;
    if (parseNumber(v, e, d)) {
        if (d == 0) f.relativeTo = C::CamPlayer;
        else if (d == 1) f.relativeTo = C::CamTile;
        else if (d == 2) f.relativeTo = C::CamGlobal;
        else if (d == 3) f.relativeTo = C::CamLastPosition;
        else if (d == 4) f.relativeTo = C::CamLastPositionNoRotation;
        else f.relativeTo = C::CamPlayer;        // 数字越界 → Player（照抄）
    }
}

// RecolorTrack 的 startTile/endTile：
//   数字 = **绝对层号**；`[offset, relativeTo]`（relativeTo: "ThisTile"|"Start"|"End"|0|1|2）；
//   也容忍字符串 "Start+10" / "ThisTile-5" / "End" / "10"。解析不了就保持"没写"。
inline void parseTileRef(const char* v, const char* e, int32_t& off, int8_t& rel, bool& has) {
    auto relOf = [](const char* p, const char* q) -> int8_t {
        if (q - p == 8 && std::memcmp(p, "ThisTile", 8) == 0) return 0;
        if (q - p == 5 && std::memcmp(p, "Start", 5) == 0) return 1;
        if (q - p == 3 && std::memcmp(p, "End", 3) == 0) return 2;
        double d; if (parseNumber(p, q, d)) return (d == 1.0) ? 1 : (d == 2.0) ? 2 : 0;
        return 0;
    };
    if (v < e && *v == '[') {
        const char* p = skipWs(v + 1, e);
        double num;
        if (p >= e || !parseNumber(p, e, num)) return;
        p = skipWs(p, e);
        if (p < e && *p == ',') p = skipWs(p + 1, e);
        int8_t r = 0;
        if (p < e && *p == '"') {
            const char* q = p + 1;
            while (q < e && *q != '"') ++q;
            r = relOf(p + 1, q);
        } else if (p < e) {
            const char* q = skipValue(p, e);
            r = relOf(p, q ? q : e);
        }
        off = (int32_t)num; rel = -1;  // 数组里的 offset 是"相对"的，base 由 rel 决定
        if (r >= 0) rel = r;
        has = true;
    } else if (v < e && *v == '"') {
        const char* p = v + 1; const char* q = p;
        while (q < e && *q != '"') ++q;
        int8_t r = -1; const char* numStart = p;
        if (q - p >= 5 && std::memcmp(p, "Start", 5) == 0) { r = 1; numStart = p + 5; }
        else if (q - p >= 8 && std::memcmp(p, "ThisTile", 8) == 0) { r = 0; numStart = p + 8; }
        else if (q - p >= 3 && std::memcmp(p, "End", 3) == 0) { r = 2; numStart = p + 3; }
        double n2 = 0.0;
        if (numStart < q) {
            const char* s2 = skipWs(numStart, q);
            bool neg = false;
            if (s2 < q && (*s2 == '+' || *s2 == '-')) { neg = (*s2 == '-'); ++s2; }
            if (s2 < q && parseNumber(s2, q, n2) && neg) n2 = -n2;
        }
        off = (int32_t)n2; rel = r; has = true;
    } else {
        double num;
        if (v < e && parseNumber(v, e, num)) { off = (int32_t)num; rel = -1; has = true; }
    }
}

// p 指向 action 对象的 '{' 之后（调用方已经跳过 '{'）
// rich=false 是热路径：一个相机字段都不解析（9M action 的成本必须保持原样）；
// rich=true 只在认出事 MoveCamera / RepeatEvents 之后对**同一个对象**再跑一遍。
inline bool parseActionObject(const char* p, const char* e, ActionFields& f, bool rich = false) {
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
            if (keyIs(key, "flashStyle")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.flashStyle = std::move(tmp); f.hasFlashStyle = true;
            } else if (keyIs(key, "floor")) {
                if (!parseInt(v, vEnd, f.floor)) return false;
                f.hasFloor = true;
            } else if (rich && keyIs(key, "floorCount")) {
                // RepeatEvents 的 Floor 模式：重复次数取 floorCount（不是 repetitions）
                double fc; if (parseNumber(v, vEnd, fc)) f.floorCount = fc;
            }
            break;
        case 'e':
            if (rich && keyIs(key, "eventTag")) {
                if (*v == '"') { f.tagPtr = v + 1; f.tagEnd = vEnd - 1; }
            } else if (keyIs(key, "endColor")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.endColor = std::move(tmp); f.hasEndColor = true;
            } else if (keyIs(key, "endOpacity")) {
                double d2; if (!parseNumber(v, vEnd, d2)) return false;
                f.endOpacity = (float)d2; f.hasEndOpacity = true;
            } else if (keyIs(key, "endTile")) {
                parseTileRef(v, vEnd, f.endOff, f.endRel, f.hasEndTile);
            } else if (keyIs(key, "eventType")) {
                if (*v != '"') return false;
                for (const char* q = v; q < vEnd; ++q)                 // 转义 / 裸 CR：交回旧路径
                    if (*q == '\\' || *q == '\r') return false;
                f.type = eventTypeOf(KeyRef{v + 1, (size_t)(vEnd - v - 2)});
                f.hasEvent = true;
            } else if (rich && keyIs(key, "ease")) {
                if (easeIdOf(v, vEnd, f.easeId)) f.hasEase = true;
            } else if (rich && keyIs(key, "editorOnly")) {
                bool b; if (parseBoolValue(v, vEnd, b) && b) f.editorOnly = true;
            } else if (rich && keyIs(key, "executeOnCurrentFloor")) {
                bool b; if (parseBoolValue(v, vEnd, b)) f.executeOnCurrentFloor = b;
            }
            break;
        case 's':
            if (keyIs(key, "startColor")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.startColor = std::move(tmp); f.hasStartColor = true;
            } else if (keyIs(key, "startOpacity")) {
                double d2; if (!parseNumber(v, vEnd, d2)) return false;
                f.startOpacity = (float)d2; f.hasStartOpacity = true;
            } else if (keyIs(key, "startTile")) {
                parseTileRef(v, vEnd, f.startOff, f.startRel, f.hasStartTile);
            } else if (keyIs(key, "secondaryTrackColor")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.secondaryTrackColor = std::move(tmp); f.hasSecondaryTrackColor = true;
            } else if (keyIs(key, "scale")) {
                // 数字（等比例）或 [x, y]（分量可 null）
                if (vEnd > v && *v == '[') {
                    const char* q = skipWs(v + 1, vEnd);
                    double sx, sy;
                    if (q < vEnd && *q != ']' && parseNumber(q, vEnd, sx)) {
                        f.scaleX = (float)(sx / 100.0);
                        f.scaleY = f.scaleX;
                        q = skipWs(q, vEnd);
                        if (q < vEnd && *q == ',') q = skipWs(q + 1, vEnd);
                        if (q < vEnd && *q != ']' && parseNumber(q, vEnd, sy)) f.scaleY = (float)(sy / 100.0);
                        f.hasScale = true;
                    }
                } else {
                    double s1;
                    if (parseNumber(v, vEnd, s1)) { f.scaleX = f.scaleY = (float)(s1 / 100.0); f.hasScale = true; }
                }
            } else if (keyIs(key, "speedType")) {
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
            } else if (rich && keyIs(key, "disabled")) {
                parseDisabledObject(v, vEnd, f);
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
                if (!stringValue(v, vEnd, f.trackAnimation)) return false;
                f.hasTrackAnimation = true;
            } else if (rich && keyIs(key, "tag")) {
                std::string tagTmp;
                if (*v == '"' && stringValue(v, vEnd, tagTmp)) { f.tagPtr = v + 1; f.tagEnd = vEnd - 1; }
            } else if (keyIs(key, "trackColor")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.trackColor = std::move(tmp); f.hasTrackColor = true;
            } else if (keyIs(key, "trackColorType")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.trackColorType = std::move(tmp); f.hasTrackColorType = true;
            } else if (keyIs(key, "trackColorPulse")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.trackColorPulse = std::move(tmp); f.hasTrackColorPulse = true;
            } else if (keyIs(key, "trackStyle")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.trackStyle = std::move(tmp); f.hasTrackStyle = true;
            } else if (keyIs(key, "trackColorAnimDuration")) {
                double d2; if (!parseNumber(v, vEnd, d2)) return false;
                f.trackAnimDuration = (float)d2; f.hasTrackAnimDuration = true;
            } else if (keyIs(key, "trackPulseLength")) {
                double d2; if (!parseNumber(v, vEnd, d2)) return false;
                f.trackPulseLength = (float)d2; f.hasTrackPulseLength = true;
            } else if (keyIs(key, "trackGlowIntensity")) {
                double d2; if (!parseNumber(v, vEnd, d2)) return false;
                f.trackGlow = (float)d2; f.hasTrackGlow = true;
            }
            break;
        case 'p':
            if (keyIs(key, "plane")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.flashPlane = std::move(tmp); f.hasFlashPlane = true;
            } else if (keyIs(key, "positionOffset")) {
                if (*v == '[') {                 // 非数组时旧路径直接忽略
                    // 分量可以是 null（MoveTrack 常用 [null, y]）：逐分量独立解析。
                    const char* q = skipWs(v + 1, vEnd);
                    for (int comp = 0; comp < 2; comp++) {
                        if (q >= vEnd || *q == ']') break;
                        double num;
                        if (parseNumber(q, vEnd, num)) {
                            if (comp == 0) { f.offX = (float)num; f.hasOffX = true; }
                            else           { f.offY = (float)num; f.hasOffY = true; }
                        } else {
                            const char* nv = skipValue(q, vEnd);   // null / 非数字：跳过
                            if (!nv) break;
                            q = nv;
                        }
                        q = skipWs(q, vEnd);
                        if (q < vEnd && *q == ',') q = skipWs(q + 1, vEnd);
                    }
                    f.hasOffset = f.hasOffX && f.hasOffY;   // PositionTrack 需要两个分量
                }
            } else if (rich && keyIs(key, "position")) {
                parsePositionPair(v, vEnd, f);
            }
            break;
        case 'r':
            if (keyIs(key, "rotationOffset")) {
                double rd; if (parseNumber(v, vEnd, rd)) { f.rotOffset = (float)rd; f.hasRotOffset = true; }
            } else if (rich) {
                if (keyIs(key, "rotation")) {
                    double rd; if (parseNumber(v, vEnd, rd)) { f.rotation = rd; f.hasRotation = true; }
                } else if (keyIs(key, "relativeTo")) {
                    parseRelativeToValue(v, vEnd, f);
                } else if (keyIs(key, "repeatType")) {
                    std::string s2;
                    if (stringValue(v, vEnd, s2)) f.isBeat = (s2 != "Floor");
                } else if (keyIs(key, "repetitions")) {
                    double rn; if (parseNumber(v, vEnd, rn)) f.repetitions = rn;
                }
            }
            break;
        case 'z':
            if (rich && keyIs(key, "zoom")) {
                double zd; if (parseNumber(v, vEnd, zd)) { f.zoom = zd; f.hasZoom = true; }
            }
            break;
        case 'a':
            if (rich) {
                if (keyIs(key, "angleOffset")) {
                    double ad; if (parseNumber(v, vEnd, ad)) { f.angleOffset = ad; f.hasAngleOffset = true; }
                } else if (keyIs(key, "active")) {
                    parseActiveValue(v, vEnd, f);
                }
            }
            break;
        case 'i':
            if (rich) {
                if (keyIs(key, "id")) {
                    double iv; if (parseNumber(v, vEnd, iv)) { f.idNum = iv; f.hasId = true; }
                } else if (keyIs(key, "interval")) {
                    double in; if (parseNumber(v, vEnd, in)) f.interval = in;
                }
            }
            break;
        case 'g':
            if (keyIs(key, "gapLength")) {
                double gl; if (parseNumber(v, vEnd, gl)) { f.gapLength = (int32_t)gl; f.hasGapLength = true; }
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
        case 'c':
            if (keyIs(key, "color")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.legacyColor = std::move(tmp); f.hasLegacyColor = true;
            } else if (keyIs(key, "colorTo")) {
                std::string tmp;
                if (!stringValue(v, vEnd, tmp)) return false;
                f.legacyColorTo = std::move(tmp); f.hasLegacyColorTo = true;
            }
            break;
        case 'o':
            if (keyIs(key, "opacity")) {
                double d2; if (!parseNumber(v, vEnd, d2)) return false;
                f.legacyOpacity = (float)d2; f.hasLegacyOpacity = true;
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
    case T::ScaleRadius:
        a.val1 = f.hasScale ? f.scaleX : 1.0f;
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
        if (f.hasTrackAnimation) a.strId2 = g_internOwner ? g_internOwner->internActionStr(f.trackAnimation) : 0;
        break;
    default:
        break;
    }
    // 缺 floor 或缺 eventType、以及未知 eventType 的 action 旧路径直接丢弃
    keep = f.hasFloor && f.hasEvent && f.type != T::Other &&
           f.type != T::MoveCamera && f.type != T::RepeatEvents &&
           f.type != T::ColorTrack && f.type != T::RecolorTrack &&
           f.type != T::Flash && f.type != T::MoveTrack;   // 后六者进富结构
    return true;
}

// 把 rich 解析出来的字段落进富结构。**不做任何数值变换** —— 单位/缺省语义全部原样保留，
// 解释权在 core/camera（docs/camera.md §4）。
inline std::string stripColorHash(std::string c) {
    if (!c.empty() && c.front() == '#') c.erase(c.begin());
    return c;
}

inline void decodeSpecialEvent(const ActionFields& f, CameraSink& sink) {
    using T = LevelData::FastAction;
    using C = LevelData;
    if (f.type == T::MoveCamera) {
        C::CameraEvent ev;
        ev.floor = f.floor;
        if (f.hasId && f.idNum >= 0.0 && f.idNum < 4294967295.0) ev.id = (uint32_t)f.idNum;
        ev.angleOffset = (float)f.angleOffset;
        ev.duration = (float)f.duration;
        ev.x = (float)f.posX;
        ev.y = (float)f.posY;
        ev.rotation = (float)f.rotation;
        ev.zoom = (float)f.zoom;
        ev.ease = (uint8_t)f.easeId;
        ev.relativeTo = (f.relativeTo < 0) ? (uint8_t)C::CamAbsent : (uint8_t)f.relativeTo;
        uint8_t flags = 0;
        if (f.hasPosition) flags |= C::kCamHasPosition;
        if (f.hasPosX) flags |= C::kCamHasX;
        if (f.hasPosY) flags |= C::kCamHasY;
        if (f.hasRotation) flags |= C::kCamHasRotation;
        if (f.hasZoom) flags |= C::kCamHasZoom;
        if (f.inactive) flags |= C::kCamInactive;
        if (f.editorOnly) flags |= C::kCamEditorOnly;
        if (f.hasEase) flags |= C::kCamEaseWritten;
        ev.flags = flags;
        ev.disabled = f.disabledMask;
        if (f.tagPtr && f.tagEnd > f.tagPtr) ev.tag.assign(f.tagPtr, (size_t)(f.tagEnd - f.tagPtr));
        sink.cameras.push_back(std::move(ev));
    } else if (f.type == T::RepeatEvents) {
        C::RepeatEvent rp;
        rp.floor = f.floor;
        // Beat 模式取 repetitions，Floor 模式取 floorCount（Re_ADOJAS 的 isBeat 分支）
        rp.repetitions = (float)(f.isBeat ? f.repetitions : f.floorCount);
        rp.interval = (float)f.interval;
        rp.gapLength = (float)f.gapLength;
        uint8_t flags = f.isBeat ? 1 : 0;
        if (f.executeOnCurrentFloor) flags |= 2;
        rp.flags = flags;
        if (f.tagPtr && f.tagEnd > f.tagPtr) rp.tag.assign(f.tagPtr, (size_t)(f.tagEnd - f.tagPtr));
        sink.repeats.push_back(std::move(rp));
    } else if (f.type == T::ColorTrack || f.type == T::RecolorTrack) {
        C::ColorEvent ev;
        ev.floor = f.floor;
        // Re_ADOJAS `createShiftType(event)`：事件没写的字段用硬编码默认值（不是 settings）。
        ev.cfg.colorType = f.hasTrackColorType ? f.trackColorType : std::string("Single");
        ev.cfg.color = stripColorHash(f.hasTrackColor ? f.trackColor : std::string("debb7b"));
        ev.cfg.secondary = stripColorHash(f.hasSecondaryTrackColor ? f.secondaryTrackColor : std::string("ffffff"));
        ev.cfg.pulse = f.hasTrackColorPulse ? f.trackColorPulse : std::string("None");
        ev.cfg.animDuration = f.hasTrackAnimDuration ? f.trackAnimDuration : 2.0f;
        ev.cfg.pulseLength = f.hasTrackPulseLength ? f.trackPulseLength : 10.0f;
        ev.cfg.style = f.hasTrackStyle ? f.trackStyle : std::string("Standard");
        ev.cfg.glowIntensity = f.hasTrackGlow ? f.trackGlow : 100.0f;
        ev.cfg.startFloor = f.floor;
        ev.cfg.startTime = 0.0;
        ev.duration = f.hasDuration ? f.duration : 0.0f;
        ev.ease = (uint8_t)f.easeId;
        uint8_t flags = 0;
        if (f.justThisTile)            flags |= C::kColorJustThisTile;
        if (f.inactive)                flags |= C::kColorInactive;
        if (f.editorOnly)              flags |= C::kColorEditorOnly;
        if (f.hasTrackGlow)            flags |= C::kColorHasGlow;
        if (f.type == T::RecolorTrack) {
            flags |= C::kColorIsRecolor;
            ev.startOff = f.startOff; ev.startRel = f.startRel;
            ev.endOff = f.endOff;     ev.endRel = f.endRel;
            ev.gapLength = f.hasGapLength ? (int32_t)f.gapLength : 0;
            ev.angleOffset = (float)f.angleOffset;
        }
        ev.flags = flags;
        sink.colors.push_back(std::move(ev));
    } else if (f.type == T::Flash) {
        C::FlashEvent ev;
        ev.floor = f.floor;
        ev.duration = f.hasDuration ? f.duration : 1.0f;
        ev.angleOffset = (float)f.angleOffset;
        ev.startOpacity = f.hasStartOpacity ? f.startOpacity / 100.0f
                        : (f.hasLegacyOpacity ? f.legacyOpacity / 100.0f : 1.0f);
        ev.endOpacity = f.hasEndOpacity ? f.endOpacity / 100.0f : 0.0f;
        std::string sc = f.hasStartColor ? f.startColor : (f.hasLegacyColor ? f.legacyColor : std::string("ffffff"));
        std::string ec = f.hasEndColor ? f.endColor : (f.hasLegacyColorTo ? f.legacyColorTo : sc);
        Rgb8 c1 = parseColorHex(sc), c2 = parseColorHex(ec);
        ev.startColor[0]=c1.r; ev.startColor[1]=c1.g; ev.startColor[2]=c1.b;
        ev.endColor[0]=c2.r; ev.endColor[1]=c2.g; ev.endColor[2]=c2.b;
        ev.plane = (f.hasFlashPlane && f.flashPlane == "Background") ? 1 : 0;
        ev.style = f.hasFlashStyle ? (f.flashStyle == "Kill" ? 1 : (f.flashStyle == "StayBlack" ? 2 : 0)) : 0;
        ev.ease = (uint8_t)f.easeId;
        sink.flashes.push_back(ev);
    } else if (f.type == T::MoveTrack) {
        C::MoveTrackEvent ev;
        ev.floor = f.floor;
        ev.angleOffset = (float)f.angleOffset;
        if (f.tagPtr && f.tagEnd > f.tagPtr) ev.tag.assign(f.tagPtr, (size_t)(f.tagEnd - f.tagPtr));
        ev.startOff = f.startOff; ev.startRel = f.startRel;
        ev.endOff = f.endOff;     ev.endRel = f.endRel;
        ev.gap = f.hasGapLength ? (int32_t)f.gapLength : 0;
        ev.ease = (uint8_t)f.easeId;
        ev.durBeats = f.hasDuration ? f.duration : 1.0f;
        ev.offX = f.offX; ev.offY = f.offY;
        ev.rotDeg = f.rotOffset;
        ev.scaleX = f.scaleX; ev.scaleY = f.scaleY;
        ev.opacity = f.hasLegacyOpacity ? f.legacyOpacity / 100.0f : 1.0f;
        uint8_t flags = 0;
        if (f.hasOffX) flags |= C::kMoveOffX;
        if (f.hasOffY) flags |= C::kMoveOffY;
        if (f.hasRotOffset) flags |= C::kMoveRot;
        if (f.hasScale) flags |= C::kMoveScale;
        if (f.hasLegacyOpacity) flags |= C::kMoveOpacity;
        if (f.inactive) flags |= C::kMoveInactive;
        if (f.editorOnly) flags |= C::kMoveEditorOnly;
        ev.flags = flags;
        sink.moves.push_back(ev);
    }
}

// b 指向 '['，end 是整个缓冲区的末尾；遇到配对的 ']' 停下并回报其后的位置。
inline bool parseActionRegion(const char* b, const char* end,
                              std::vector<LevelData::FastAction>& out,
                              CameraSink& sink, const char** outEnd) {
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
        g_failPos = p;
        if (!parseActionObject(p + 1, objEnd, f)) return false;
        LevelData::FastAction a;
        bool keep = false;
        g_failPos = p;
        if (!buildAction(f, a, keep)) return false;
        if (keep) out.push_back(std::move(a));
        else if (f.type == LevelData::FastAction::MoveCamera ||
                 f.type == LevelData::FastAction::RepeatEvents ||
                 f.type == LevelData::FastAction::ColorTrack ||
                 f.type == LevelData::FastAction::RecolorTrack ||
                 f.type == LevelData::FastAction::Flash ||
                 f.type == LevelData::FastAction::MoveTrack) {
            // 罕见事件：对同一个对象再解析一遍富字段（热路径零成本，这里最多几百次）
            ActionFields rich;
            if (parseActionObject(p + 1, objEnd, rich, /*rich=*/true)) decodeSpecialEvent(rich, sink);
        }
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
                                   LevelData::FastAction* dst, size_t slot,
                                   CameraSink& sink) {
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
        } else if (f.type == LevelData::FastAction::MoveCamera ||
                   f.type == LevelData::FastAction::RepeatEvents ||
                   f.type == LevelData::FastAction::ColorTrack ||
                   f.type == LevelData::FastAction::RecolorTrack ||
                   f.type == LevelData::FastAction::Flash ||
                 f.type == LevelData::FastAction::MoveTrack) {
            ActionFields rich;
            if (parseActionObject(p + 1, objEnd, rich, /*rich=*/true)) decodeSpecialEvent(rich, sink);
        }
        p = objEnd;
    }
}

inline bool parseActionRegionParallel(const char* b, const char* fileEnd,
                                      std::vector<LevelData::FastAction>& out,
                                      CameraSink& sink, const char** outEnd) {
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
        if (forced) return parseActionRegion(b, fileEnd, out, sink, outEnd);
        // 小文件不值得多扫一遍、也不值得开线程：直接用融合的单遍解析。
        //
        // 上界同理：1.2 GB 以上的 actions 区间上，多线程读不同区段会把内存带宽/局域性
        // 吃满，实测反而更慢（1.18 GB 谱面：2 段 900 ms、4 段 980 ms、8 段 1474 ms，
        // 而 611 MB 的谱面 8 段只要 82 ms）。那里退回融合的单遍解析——找切分点那趟
        // 扫描本身就要 0.5 s，与其白花不如省掉。真正的解法是让解析在小的常驻窗口上
        // 做（见 TODO：滑窗流水线），那时并行分段是免费的。
        if (remaining < (32u << 20) || remaining >= (768u << 20))
            return parseActionRegion(b, fileEnd, out, sink, outEnd);
        pieces = std::thread::hardware_concurrency();
        if (pieces < 2) pieces = 2;
        if (pieces > 8) pieces = 8;                      // 再多边际收益很小
    }

    std::vector<size_t> targets;
    targets.reserve(pieces - 1);
    for (unsigned i = 1; i < pieces; i++) targets.push_back(remaining * i / pieces);
    // 目标偏移可能落在同一个对象上（小文件），findActionSplits 会把它们合并成更少的段


    const bool dbg = env::flag("ADOCAO_PARSE_DBG");
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
        return parseActionRegion(b, fileEnd, out, sink, outEnd);   // 结构不对/切不出两段
    }

    std::vector<const char*> bounds;
    bounds.reserve(pieces + 1);
    bounds.push_back(b + 1);          // b 是数组的 '['，分段解析器要的是对象起点
    for (const char* sp : splits) bounds.push_back(sp);
    bounds.push_back(regionEnd);
    const size_t n = bounds.size() - 1;
    if (n < 2) return parseActionRegion(b, fileEnd, out, sink, outEnd);

    const double scanMs = msSince(tScan);
    const size_t base = out.size();
    std::vector<size_t> slots(n + 1, 0);
    for (size_t i = 0; i < n; i++) slots[i + 1] = slots[i] + counts[i];
    out.resize(base + slots[n]);                         // 一次分配，之后不再增长

    const size_t camBase = sink.cameras.size();
    const size_t repBase = sink.repeats.size();
    const size_t colBase = sink.colors.size();
    const size_t flBase = sink.flashes.size();
    const size_t mvBase = sink.moves.size();
    std::vector<CameraSink> pieceSinks(n);
    auto tParse = now();
    std::vector<size_t> kept(n, 0);
    std::atomic<bool> failed{false};
    unsigned poolThreads = 0;
    {
        // ② 并行模型：不再**每次调用新建线程池**（那是"五套机制"里最浪费的一套 ✗），
        // 统一走 `Scheduler`（Background 车道：加载期本来就不与帧关键任务抢）。
        poolThreads = Scheduler::instance().workers() + 1;   // +1 = 调用线程也参与
        Scheduler::instance().parallelFor(n, 1, Lane::Background, [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi && !failed.load(std::memory_order_relaxed); i++) {
                size_t k = parseActionRangeInto(bounds[i], bounds[i + 1],
                                                out.data() + base + slots[i], counts[i], pieceSinks[i]);
                if (k == kFail) failed.store(true, std::memory_order_relaxed);
                else kept[i] = k;
            }
        });
    }
    const double parseMs = msSince(tParse);
    if (dbg) std::fprintf(stderr, "[parse] 找切分点 %.1f ms  并行解析 %.1f ms  段数=%zu 线程=%u  failed=%d\n",
                          scanMs, parseMs, n, poolThreads, (int)failed.load());
    if (failed.load()) {
        out.resize(base);
        sink.cameras.resize(camBase); sink.repeats.resize(repBase); sink.colors.resize(colBase); sink.flashes.resize(flBase); sink.moves.resize(mvBase);
        return parseActionRegion(b, fileEnd, out, sink, outEnd);   // 只有切分不可信时才会走到
    }
    // 相机/重复事件按**段序**合并 —— 段是按文件顺序切的，所以结果就是文件顺序。
    for (size_t i = 0; i < n; i++) {
        auto& ps = pieceSinks[i];
        sink.cameras.insert(sink.cameras.end(),
                            std::make_move_iterator(ps.cameras.begin()),
                            std::make_move_iterator(ps.cameras.end()));
        sink.repeats.insert(sink.repeats.end(),
                            std::make_move_iterator(ps.repeats.begin()),
                            std::make_move_iterator(ps.repeats.end()));
        sink.colors.insert(sink.colors.end(),
                           std::make_move_iterator(ps.colors.begin()),
                           std::make_move_iterator(ps.colors.end()));
        sink.flashes.insert(sink.flashes.end(),
                            std::make_move_iterator(ps.flashes.begin()),
                            std::make_move_iterator(ps.flashes.end()));
        sink.moves.insert(sink.moves.end(),
                          std::make_move_iterator(ps.moves.begin()),
                          std::make_move_iterator(ps.moves.end()));
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
    CameraSink cameraSink;               // MoveCamera / RepeatEvents（罕见，富结构）
    LevelData::Settings settings;
    std::string pathText;
    std::string settingsText;
    bool failed = false;
    bool tarContainer = false;           // .ilytx：解压后是 tar，不是 JSON（见 run() 顶部）
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
            else if (f.type == LevelData::FastAction::MoveCamera ||
                     f.type == LevelData::FastAction::RepeatEvents ||
                     f.type == LevelData::FastAction::ColorTrack ||
                     f.type == LevelData::FastAction::RecolorTrack ||
                     f.type == LevelData::FastAction::Flash ||
                 f.type == LevelData::FastAction::MoveTrack) {
                ActionFields rich;
                if (parseActionObject(p + 1, objEnd, rich, /*rich=*/true))
                    decodeSpecialEvent(rich, cameraSink);
            }
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
        const bool dbg = env::flag("ADOCAO_WINDOW_DBG");
        if (!stream.next()) {
            if (dbg) std::fprintf(stderr, "[win] 首块就失败 failed=%d err=%s\n", (int)stream.failed(), stream.error().c_str());
            return stream.failed() ? -1 : 0;
        }
        // `.ilytx` = xz(tar(…))：压缩层认不出来，解开才知道载荷是 tar 而不是 JSON。
        // 它不是"结构解析失败"，而是**这条路根本不适用** —— 明确让路，由 archive 的
        // ilytx 解码接手（LevelData 会退回整份解压那条路，理由走 tarContainer 打印）。
        if (isTarPayload(stream.data(), stream.size())) {
            tarContainer = true;
            return 0;
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
            if (dbg) {
                // 两个数都要打：`本次已提交` 相对本次 step 调用起点，`绝对消费` 相对窗口起点。
                // 只看前者会把"窗口中途转移后 itemStart 落回窗口开头"误读成"一点没消费"。
                const size_t absConsumed = (size_t)(carryFrom - stream.data());
                const size_t carryN = stream.size() - absConsumed;
                const int show = (int)(carryN < 24 ? carryN : 24);
                std::fprintf(stderr, "[win] 换窗 state=%d 本次已提交=%zu 绝对消费=%zu carry=%zu 窗口=%zu 首=%.*s\n",
                             (int)state, consumed, absConsumed, carryN, stream.size(), show, carryFrom);
            }
            // consume() 要的是"窗口内偏移"，不是"相对本次调用起点的字节数"（第一次调用两者
            // 恰好相等，所以这个错被掩盖过：窗口中途状态转移后 carry 会退到已解析对象中间）。
            stream.consume((size_t)(carryFrom - stream.data()));
            if (!stream.next()) {
                if (dbg) std::fprintf(stderr, "[win] 取下一块失败 state=%d stuck=%d failed=%d err=%s 本次已提交=%zu 块内=%zu\n",
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
// **缺省值一律用 `out` 里已有的值**（不是字面量）：这正是"文件里没出现的字段沿用调用方给的
// 默认值"那条约定的实现（CLI/向导可以预置颜色等）。`Settings` 的构造函数默认值与旧的字面量
// 完全相同，所以对"新建对象 + 完整文件"这条常态路径**逐位等价**；
// 区别只出现在"调用方预置了某字段、而文件里恰好没有"的时候 —— 那时旧实现会把预置值冲掉 ✗。
static void readSettings(const rapidjson::Value& s, LevelData::Settings& out) {
    auto getF = [&](const char* k, float d) { return s.HasMember(k) ? s[k].GetFloat() : d; };
    auto getI = [&](const char* k, int d) { return s.HasMember(k) ? s[k].GetInt() : d; };
    auto getS = [&](const char* k, const std::string& d) -> std::string {
        return (s.HasMember(k) && s[k].IsString()) ? s[k].GetString() : d;
    };
    out.songFilename    = getS("song", s.HasMember("songFilename") ? getS("songFilename", out.songFilename)
                                                                  : out.songFilename);
    out.bpm             = getF("bpm", out.bpm);
    out.pitch           = getF("pitch", out.pitch);
    out.offset          = getF("offset", out.offset);
    out.countdownTicks  = getI("countdownTicks", out.countdownTicks);
    if (s.HasMember("separateCountdownTime")) {
        const auto& v = s["separateCountdownTime"];
        out.separateCountdownTime = v.IsBool() ? v.GetBool() : (v.IsInt() ? v.GetInt() != 0 : out.separateCountdownTime);
    }
    out.zoom            = getF("zoom", out.zoom);
    out.rotation        = getF("rotation", out.rotation);
    out.relativeTo      = getS("relativeTo", out.relativeTo);
    out.hitsound        = getS("hitsound", out.hitsound);
    out.hitsoundVolume  = getF("hitsoundVolume", out.hitsoundVolume);
    out.trackColor      = getS("trackColor", out.trackColor);
    out.secondaryTrackColor = getS("secondaryTrackColor", out.secondaryTrackColor);
    out.trackColorType  = getS("trackColorType", out.trackColorType);
    out.trackColorPulse = getS("trackColorPulse", out.trackColorPulse);
    out.trackColorAnimDuration = getF("trackColorAnimDuration", out.trackColorAnimDuration);
    out.trackPulseLength = getF("trackPulseLength", out.trackPulseLength);
    out.trackStyle      = getS("trackStyle", out.trackStyle);
    out.trackGlowIntensity = getF("trackGlowIntensity", out.trackGlowIntensity);
    out.backgroundColor = getS("backgroundColor", out.backgroundColor);
    out.planetEase      = getS("planetEase", out.planetEase);
    out.trackDisappearAnimation = getS("trackDisappearAnimation", out.trackDisappearAnimation);
    out.trackAnimation  = getS("trackAnimation", out.trackAnimation);
    out.beatsBehind     = getF("beatsBehind", out.beatsBehind);
    out.beatsAhead      = getF("beatsAhead", out.beatsAhead);
    if (s.HasMember("stickToFloors")) {
        if (s["stickToFloors"].IsBool()) out.stickToFloors = s["stickToFloors"].GetBool();
        else if (s["stickToFloors"].IsString()) {
            std::string v = s["stickToFloors"].GetString();
            out.stickToFloors = (v == "Enabled" || v == "true" || v == "True");
        }
    }
    if (s.HasMember("position") && s["position"].IsArray() && s["position"].Size() >= 2)
        out.position = {s["position"][0].GetFloat(), s["position"][1].GetFloat()};
    if (s.HasMember("legacyCamRelativeTo") && s["legacyCamRelativeTo"].IsBool())
        out.legacyCamRelativeTo = s["legacyCamRelativeTo"].GetBool();
}

}  // namespace

TileColorConfig LevelData::baseColorConfig() const {
    TileColorConfig cfg;
    cfg.colorType = settings.trackColorType;
    cfg.color = settings.trackColor;
    cfg.secondary = settings.secondaryTrackColor;
    cfg.pulse = settings.trackColorPulse;
    cfg.animDuration = settings.trackColorAnimDuration;
    cfg.pulseLength = settings.trackPulseLength;
    cfg.style = settings.trackStyle;
    cfg.glowIntensity = settings.trackGlowIntensity;
    cfg.startFloor = 0;
    cfg.startTime = 0.0;
    return cfg;
}

const LevelData::ColorEvent* LevelData::colorEventAt(int64_t floor) const {
    // colorEvents 在 finishLoad 里按 (floor, order) 排好。
    // justThisTile：只覆盖它所在的那一层，同层后者赢。
    auto lo = std::lower_bound(colorEvents.begin(), colorEvents.end(), (int32_t)floor,
                               [](const ColorEvent& e, int32_t f) { return e.floor < f; });
    auto ub = std::upper_bound(colorEvents.begin(), colorEvents.end(), (int32_t)floor,
                               [](int32_t f, const ColorEvent& e) { return f < e.floor; });
    const ColorEvent* single = nullptr;
    for (auto it = lo; it != ub; ++it) {
        if (it->flags & (kColorInactive | kColorEditorOnly)) continue;
        if (it->flags & kColorJustThisTile) single = &*it;
    }
    if (single) return single;
    // 持久段：floor<=f 的最后一条（从 upper_bound 往前找第一条非 inactive 的持久事件）。
    for (auto it = ub; it != colorEvents.begin();) {
        --it;
        if (it->flags & (kColorInactive | kColorEditorOnly | kColorIsRecolor)) continue;
        if (it->flags & kColorJustThisTile) continue;
        return &*it;
    }
    return nullptr;
}

void LevelData::buildRecolorIndex(const Timeline& timeline) {
    m_recolorTriggers.clear();
    m_recolorBlocks.clear();
    m_recolorReady = true;
    const int64_t n = tileCount();
    if (n <= 0) return;
    auto resolve = [&](const ColorEvent& ev, int32_t off, int8_t rel) -> int64_t {
        if (rel < 0) return (int64_t)off;                      // 绝对层号
        const int64_t base = (rel == 1) ? 0 : (rel == 2 ? n - 1 : ev.floor);
        return base + (int64_t)off;
    };
    for (const ColorEvent& ev : colorEvents) {
        if (!(ev.flags & kColorIsRecolor)) continue;
        if (ev.flags & (kColorInactive | kColorEditorOnly)) continue;
        int64_t a = resolve(ev, ev.startOff, ev.startRel);
        int64_t b = resolve(ev, ev.endOff, ev.endRel);
        if (a > b) std::swap(a, b);
        a = std::clamp<int64_t>(a, 0, n - 1);
        b = std::clamp<int64_t>(b, 0, n - 1);
        RecolorTrigger rt;
        rt.lo = (int32_t)a;
        rt.hi = (int32_t)b;
        rt.gap = std::max<int32_t>(0, ev.gapLength);
        rt.beat = (double)timeline.durationAt(ev.floor);
        rt.trigger = (double)timeline.startTimeAt(ev.floor) +
                     ((double)ev.angleOffset / 180.0) * rt.beat;
        rt.duration = ev.duration;
        rt.angleOffset = ev.angleOffset;
        rt.ease = ev.ease;
        rt.order = ev.order;
        rt.cfg = ev.cfg;
        if (!(ev.flags & kColorHasGlow)) rt.cfg.glowIntensity = settings.trackGlowIntensity;
        rt.cfg.startFloor = ev.floor;   // pulse/parity 的基准层
        m_recolorTriggers.push_back(std::move(rt));
    }
    if (m_recolorTriggers.empty()) return;
    const size_t blocks = (size_t)((n + kRecolorBlock - 1) / kRecolorBlock);
    m_recolorBlocks.resize(blocks);
    size_t total = 0;
    bool capped = false;
    for (size_t i = 0; i < m_recolorTriggers.size() && !capped; ++i) {
        const RecolorTrigger& e = m_recolorTriggers[i];
        const int64_t step = (int64_t)e.gap + 1;
        for (int64_t f = e.lo; f <= e.hi; f += step) {
            auto& v = m_recolorBlocks[(size_t)f / kRecolorBlock];
            if (v.empty() || v.back() != (uint32_t)i) v.push_back((uint32_t)i);
            if (++total > kRecolorMaxEntries) {
                LOG_W("RecolorTrack: 索引条目超过上限 %zu，后续事件被截断（范围/事件数异常）",
                      (size_t)kRecolorMaxEntries);
                capped = true;
                break;
            }
        }
    }
    for (auto& v : m_recolorBlocks) {
        std::sort(v.begin(), v.end(), [&](uint32_t x, uint32_t y) {
            const RecolorTrigger& a = m_recolorTriggers[x];
            const RecolorTrigger& b = m_recolorTriggers[y];
            if (a.trigger != b.trigger) return a.trigger < b.trigger;
            return a.order < b.order;
        });
    }
    LOG_D("RecolorTrack: %zu 条事件，块表 %zu 块 / %zu 条目", m_recolorTriggers.size(),
          m_recolorBlocks.size(), total);
}

std::string LevelData::trackStyleAt(int64_t floor, double timeSec, const Timeline* timeline) const {
    // 第一步与 baseColorConfig()/baseColorsAt() 同源：settings 默认 + colorEventAt 的静态段。
    std::string style = settings.trackStyle;
    if (const ColorEvent* e = colorEventAt(floor))
        if (!e->cfg.style.empty()) style = e->cfg.style;
    if (!m_recolorReady || m_recolorBlocks.empty() || floor < 0) return style;
    const size_t blk = (size_t)floor / kRecolorBlock;
    if (blk >= m_recolorBlocks.size()) return style;
    // 第二步与 tileColorsAt 的后半段逐字同构（同样的覆盖/gap/trigger 判据），
    // 只是取 cfg.style 而不是颜色。timeSec = +inf 就是"最终状态"（静态形状表用的口径）。
    const RecolorTrigger* last = nullptr;
    for (uint32_t idx : m_recolorBlocks[blk]) {
        const RecolorTrigger& e = m_recolorTriggers[idx];
        if (e.lo > floor || e.hi < floor) continue;
        if (e.gap > 0 && ((floor - e.lo) % ((int64_t)e.gap + 1)) != 0) continue;
        if (e.trigger > timeSec + 1e-9) break;   // 列表按 trigger 升序
        // 事件**自己那一层**还没走到就不算数 —— 否则末尾一条覆盖到前面的 RecolorTrack 会
        // **追溯**改掉已经过去的砖（The Moon 的 64 条 Minimal 就会把前面已定型的 Neon 改小 0.03）。
        // 但 angleOffset 是**度**：负值可以提前触发（-99999 ≈ 负 555 拍 = 从开头就生效），
        // 所以门槛不是"层号 <= 砖号"，而是"这条的提前量够不够跨过这两层"。
        // 精确判据：该砖自己的起始时刻。**别用"每层 1 拍"去估** —— 一层 = 该砖走过的角度
        // /180 拍，90° 砖只有半拍、360° 砖是两拍，层号差和拍数根本不成正比（2026-10-18 用户指出）。
        if (timeline) {
            if (e.trigger > timeline->startTimeAt(floor) + 1e-9) continue;
        } else if (e.cfg.startFloor > floor &&
                   (double)e.angleOffset > -180.0 * (double)(e.cfg.startFloor - floor) - 1e-6) {
            continue;
        }
        last = &e;
    }
    if (last && !last->cfg.style.empty()) style = last->cfg.style;
    return style;
}

int64_t LevelData::trackStyleRunEnd(int64_t floor, double timeSec, const Timeline* timeline) const {
    constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
    (void)timeSec;
    (void)timeline;   // 见下面第 3) 条：块内有触发时本函数不猜，直接不做重用
    int64_t next = kNever;

    // 1) 静态段（settings + colorEventAt）。colorEvents 在 finishLoad 里按 (floor, order) 排好；
    //    colorEventAt 的结果只在"事件层号"处变化，而且：
    //      * 本层有自己的事件 → justThisTile 只覆盖这一层，**下一层**就可能换段；
    //      * 否则 → 下一个事件层号（之前没有任何事件，持久段不变）。
    {
        const auto lb = std::lower_bound(colorEvents.begin(), colorEvents.end(), (int32_t)floor,
                                         [](const ColorEvent& e, int32_t f) { return e.floor < f; });
        const auto ub = std::upper_bound(colorEvents.begin(), colorEvents.end(), (int32_t)floor,
                                         [](int32_t f, const ColorEvent& e) { return f < e.floor; });
        if (lb != ub) next = floor + 1;
        else if (ub != colorEvents.end()) next = (int64_t)ub->floor;
    }
    if (floor < 0 || !m_recolorReady || m_recolorBlocks.empty()) return next;
    const size_t blk = (size_t)floor / kRecolorBlock;
    if (blk >= m_recolorBlocks.size()) return next;

    // 2) 换块：trackStyleAt 只读当前块的触发列表，块号一变换的就是整张表。
    const int64_t blkEnd = (int64_t)(blk + 1) * (int64_t)kRecolorBlock;
    if (blkEnd < next) next = blkEnd;

    // 3) 块里有 RecolorTrack 触发：样式还取决于 `e.trigger` 与 `startTimeAt(floor)` 的比较
    //    （以及 lo/hi/gap）。lo/hi/gap 是精确的整数边界，但**层起始时刻不保证随层号单调** ——
    //    Timeline 对 SetSpeed 的 `bpmMultiplier` 没有钳制（`durationAt` 直接除 BPM），
    //    ≤0 时层起始时刻会原地/倒退；停靠在这上面的"二分找跨越层"就不是可证的。
    //    所以这里**不猜**：返回 `-blkEnd`（"本块不可复用，到 blkEnd 为止"）。调用方在块内
    //    逐砖查询（与改造前完全一样），**不会**再进这个函数 —— 否则每砖两次查询，有触发的
    //    谱反而变慢（实测 2026：56 → 70 ns/砖）。
    //    代价：RecolorTrack 覆盖的块没有收益；收益落在"没有触发的块"（实谱里绝大多数）。
    if (!m_recolorBlocks[blk].empty()) return -blkEnd;
    return next;
}

TileColors LevelData::tileColorsAt(int64_t floor, double timeSec, float amplitude,
                                   const Timeline* timeline) const {
    const TileColors base = baseColorsAt(floor, timeSec, amplitude, timeline);
    if (!m_recolorReady || m_recolorBlocks.empty() || floor < 0) return base;
    const size_t blk = (size_t)floor / kRecolorBlock;
    if (blk >= m_recolorBlocks.size()) return base;
    const auto& list = m_recolorBlocks[blk];
    if (list.empty()) return base;
    const RecolorTrigger* last = nullptr;
    const RecolorTrigger* prev = nullptr;
    for (uint32_t idx : list) {
        const RecolorTrigger& e = m_recolorTriggers[idx];
        if (e.lo > floor || e.hi < floor) continue;
        if (e.gap > 0 && ((floor - e.lo) % ((int64_t)e.gap + 1)) != 0) continue;
        if (e.trigger > timeSec + 1e-9) break;   // 列表按 trigger 升序
        prev = last;
        last = &e;
    }
    if (!last) return base;
    const TileColors target = computeTileColors(last->cfg, timeSec, (int)floor, amplitude);
    if (last->duration <= 0.0f) return target;
    if (last->cfg.colorType != "Single" && last->cfg.colorType != "Stripes") return target;
    const double dur = (double)last->duration * last->beat;
    if (dur <= 0.0 || timeSec >= last->trigger + dur) return target;
    const TileColors from = prev ? computeTileColors(prev->cfg, last->trigger, (int)floor, amplitude)
                                 : base;
    const double q = easeApply((Ease)last->ease, (timeSec - last->trigger) / dur);
    auto mixCh = [&](std::uint8_t a, std::uint8_t b) {
        return (std::uint8_t)std::clamp<long>(
            std::lround((double)a + ((double)b - (double)a) * q), 0, 255);
    };
    TileColors out;
    for (int c = 0; c < 3; c++) {
        out.fill[c] = mixCh(from.fill[c], target.fill[c]);
        out.stroke[c] = mixCh(from.stroke[c], target.stroke[c]);
    }
    out.opacity = target.opacity;
    return out;
}

void LevelData::buildFlashIndex(const Timeline& timeline) {
    // Flash 的 duration 是**拍**（和 MoveTrack/相机一致），不是秒：
    // 2025 floor 995 的 `000000 1.0→0 dur 32` 在 bpm 12150 下 ≈ 0.16 s（几帧），
    // 官方实测"纯黑但只有几帧"；按秒算会黑 32 秒（2026-10-17 用户实测）。
    //
    // 一拍 = **60/bpm**（Re_ADOJAS：TimelineManager 的 secPerBeat、
    // TileColorManager 的 angleOffset 都这么算）。**别用 durationAt(floor)** ——
    // 那是"这一砖走完要多久" = 角度/180 × 拍，只对 180° 砖碰巧相等；更糟的是
    // 末尾合成砖（floor == n-1）的 durationAt() 返回 **0**，于是放在最后一层的
    // Flash 一拍都算不出来：duration 和 angleOffset 同时塌成 0 →
    // "512 拍的黑色淡入" 变成**瞬间全黑**（2026-10-18 用户报"最后一段完全黑屏"）。
    const double pitch = std::max(0.1, (double)settings.pitch / 100.0);
    const auto& bpms = timeline.tileBPMs();
    for (FlashEvent& e : flashEvents) {
        // 末尾合成层也按最后一格真实的 bpm 算；越界一律夹到两端。
        const int64_t last = (int64_t)bpms.size() - 1;
        const int64_t idx = std::clamp<int64_t>(e.floor, 0, last < 0 ? 0 : last);
        const double bpm = bpms.empty() ? 100.0 : std::max(1.0f, bpms[(size_t)idx]);
        const double secPerBeat = 60.0 / bpm;
        e.trigger = (double)timeline.startTimeAt(idx) +
                    ((double)e.angleOffset / 180.0) * secPerBeat;
        e.durSec = (double)e.duration * secPerBeat / pitch;
    }
    std::stable_sort(flashEvents.begin(), flashEvents.end(),
                     [](const FlashEvent& a, const FlashEvent& b) { return a.trigger < b.trigger; });
    LOG_D("Flash: %zu 条事件", flashEvents.size());
}

LevelData::FlashState LevelData::flashStateAt(uint8_t plane, double timeSec) const {
    FlashState st;
    if (flashEvents.empty()) return st;
    auto it = std::upper_bound(flashEvents.begin(), flashEvents.end(), timeSec,
                               [](double v, const FlashEvent& e) { return v < e.trigger; });
    for (auto i = it; i != flashEvents.begin();) {
        --i;
        if (i->plane != plane) continue;
        if (i->style == 1) return st;                       // Kill：立即清空
        st.active = true;
        if (i->style == 2) {                                // StayBlack
            st.color[0] = st.color[1] = st.color[2] = 0;
            st.alpha = 1.0f;
            return st;
        }
        if (i->durSec <= 0.0) {                             // 零时长：停在终值
            st.color[0] = i->endColor[0]; st.color[1] = i->endColor[1]; st.color[2] = i->endColor[2];
            st.alpha = i->endOpacity;
            return st;
        }
        const double p = std::clamp((timeSec - i->trigger) / i->durSec, 0.0, 1.0);
        const double q = easeApply((Ease)i->ease, p);
        for (int c = 0; c < 3; c++) {
            st.color[c] = (uint8_t)std::clamp<long>(std::lround(
                (double)i->startColor[c] + ((double)i->endColor[c] - (double)i->startColor[c]) * q), 0, 255);
        }
        st.alpha = (float)((double)i->startOpacity +
                           ((double)i->endOpacity - (double)i->startOpacity) * q);
        return st;
    }
    return st;
}

void LevelData::buildMoveTrackIndex(const Timeline& timeline) {
    // 同 floor 的事件必须保持**文件顺序**（Re_ADOJAS 按 event.id ?? 文件序）：
    // 例如 2025 floor 2309 的 `scale/opacity 0` 与 `opacity 250` 两条，顺序反了砖就消失。
    for (size_t i = 0; i < moveTrackEvents.size(); i++) moveTrackEvents[i].order = (uint32_t)i;
    m_moveIndex.clear();
    m_moveBlocks.clear();
    m_moveReady = true;
    m_movePad = 0.0f;
    m_moveMaxScale = 1.0f;
    const int64_t n = tileCount();
    if (n <= 0) return;
    const double pitch = std::max(0.1, (double)settings.pitch / 100.0);
    // ── RepeatEvents 展开（与 Re_ADOJAS 的 TimelineManager 同一套规则）──────────
    // tag 是空格分隔的多个 tag；RepeatEvents 与 action 必须**同 floor** 才生效；
    // Beat 模式（interval > 0）：epFloor 不变、角度偏移 += interval*rep*180；
    // Floor 模式：executeOnCurrentFloor → epFloor 移到目标层；否则 epFloor 不变、
    // 偏移用"几何拍差"（entryBeat(target)-entryBeat(base)）*180。
    std::unordered_map<int32_t, std::vector<const RepeatEvent*>> repeatByFloor;
    for (const RepeatEvent& r : repeatEvents) repeatByFloor[r.floor].push_back(&r);
    auto tagMatches = [](const std::string& a, const std::string& b) {
        size_t i = 0;
        while (i < a.size()) {
            while (i < a.size() && a[i] == ' ') ++i;
            size_t j = i;
            while (j < a.size() && a[j] != ' ') ++j;
            if (j > i) {
                const std::string_view w(a.data() + i, j - i);
                size_t p = 0;
                while (p < b.size()) {
                    while (p < b.size() && b[p] == ' ') ++p;
                    size_t q = p;
                    while (q < b.size() && b[q] != ' ') ++q;
                    if (q > p && std::string_view(b.data() + p, q - p) == w) return true;
                    p = q;
                }
            }
            i = j;
        }
        return false;
    };
    auto findRepeat = [&](const MoveTrackEvent& ev) -> const RepeatEvent* {
        auto it = repeatByFloor.find(ev.floor);
        if (it == repeatByFloor.end() || ev.tag.empty()) return nullptr;
        for (const RepeatEvent* r : it->second)
            if (tagMatches(r->tag, ev.tag)) return r;
        return nullptr;
    };
    // entryBeat 只在 Floor 模式且不 executeOnCurrentFloor 时用；O(n) 单趟。
    std::vector<double> entryBeat;
    auto entryBeatOf = [&](int64_t i) -> double {
        if (entryBeat.empty()) {
            entryBeat.assign((size_t)n, 0.0);
            if (n > 1) {
                entryBeat[0] = -1.0;
                double beats = 0.0;
                for (int64_t j = 1; j + 1 < n; j++) {
                    entryBeat[(size_t)j] = beats;
                    const double t0 = timeline.startTimeAt(j);
                    const double t1 = timeline.startTimeAt(j + 1);
                    const double beat = (double)timeline.durationAt(j);
                    beats += (t1 - t0) / (beat > 0.0 ? beat : 1.0);
                }
            }
        }
        return (i >= 0 && (size_t)i < entryBeat.size()) ? entryBeat[(size_t)i] : 0.0;
    };
    struct ExpandedKey { int32_t floor; float ao; };
    std::vector<ExpandedKey> expanded;
    auto emitExpanded = [&](int32_t epFloor, float ao) {
        expanded.push_back({epFloor, ao});
    };
    size_t expansionCap = 500000;
    for (const MoveTrackEvent& src : moveTrackEvents) {
        if (src.flags & (kMoveInactive | kMoveEditorOnly)) continue;
        if ((src.flags & 0x1F) == 0) continue;      // 没有可动属性
        const RepeatEvent* rep = findRepeat(src);
        expanded.clear();
        if (!rep) {
            emitExpanded(src.floor, src.angleOffset);
        } else {
            const bool isBeat = (rep->flags & 1) != 0;   // repeatType（Floor 模式的 interval 无意义）
            const int reps = (int)rep->repetitions;
            const int gap = std::max(1, (int)rep->gapLength);
            const bool execCur = (rep->flags & 2) != 0;
            for (int r = 0; r <= reps; r++) {
                int32_t target = isBeat ? src.floor : src.floor + r * gap;
                if (target >= (int32_t)n) break;
                if (isBeat) {
                    emitExpanded(src.floor, src.angleOffset + rep->interval * (float)r * 180.0f);
                } else if (execCur) {
                    emitExpanded(target, src.angleOffset);
                } else {
                    emitExpanded(src.floor, src.angleOffset +
                        (float)((entryBeatOf(target) - entryBeatOf(src.floor)) * 180.0));
                }
                if (expanded.size() > expansionCap) { LOG_W("MoveTrack: 重复展开超过上限"); break; }
            }
        }
        for (const ExpandedKey& ek : expanded) {
            MoveTrackEvent ev = src;
            ev.floor = ek.floor;
            ev.angleOffset = ek.ao;
            auto floorOf = [&](int32_t off, int8_t rel, int32_t host) -> int64_t {
                if (rel < 0) return (int64_t)off;
                const int64_t b = (rel == 1) ? 0 : (rel == 2 ? n - 1 : host);
                return b + (int64_t)off;
            };
            int64_t a = floorOf(ev.startOff, ev.startRel, ev.floor);
            int64_t b = floorOf(ev.endOff, ev.endRel, ev.floor);
            if (a > b) std::swap(a, b);
            ev.lo = (int32_t)std::clamp<int64_t>(a, 0, n - 1);
            ev.hi = (int32_t)std::clamp<int64_t>(b, 0, n - 1);
            const double beat = (double)timeline.durationAt(ev.floor);
            ev.trigger = (double)timeline.startTimeAt(ev.floor) +
                         ((double)ev.angleOffset / 180.0) * beat;
            ev.durSec = (double)std::max(0.0f, ev.durBeats) * beat / pitch;
            m_moveIndex.push_back(ev);
            const float sc = std::max(std::abs(ev.scaleX), std::abs(ev.scaleY));
            if (sc > m_moveMaxScale) m_moveMaxScale = sc;
            const float pad = std::hypot(src.offX, src.offY) + (sc > 1.0f ? (sc - 1.0f) * 0.75f : 0.0f);
            if (pad > m_movePad) m_movePad = pad;
        }
    }
    if (m_moveIndex.empty()) return;
    const size_t blocks = (size_t)((n + kRecolorBlock - 1) / kRecolorBlock);
    m_moveBlocks.resize(blocks);
    size_t total = 0;
    bool capped = false;
    for (size_t i = 0; i < m_moveIndex.size() && !capped; ++i) {
        const MoveTrackEvent& e = m_moveIndex[i];
        const int64_t step = (int64_t)e.gap + 1;
        for (int64_t f = e.lo; f <= e.hi; f += step) {
            auto& v = m_moveBlocks[(size_t)f / kRecolorBlock];
            if (v.empty() || v.back() != (uint32_t)i) v.push_back((uint32_t)i);
            if (++total > kRecolorMaxEntries) {
                LOG_W("MoveTrack: 索引条目超过上限 %zu，后续事件被截断（范围/事件数异常）",
                      (size_t)kRecolorMaxEntries);
                capped = true;
                break;
            }
        }
    }
    for (auto& v : m_moveBlocks) {
        std::sort(v.begin(), v.end(), [&](uint32_t x, uint32_t y) {
            const auto& a = m_moveIndex[x];
            const auto& b = m_moveIndex[y];
            if (a.trigger != b.trigger) return a.trigger < b.trigger;
            return a.order < b.order;
        });
    }
    LOG_D("MoveTrack: %zu 条事件，块表 %zu 块 / %zu 条目，pad=%.2f maxScale=%.2f",
          m_moveIndex.size(), m_moveBlocks.size(), total, (double)m_movePad,
          (double)m_moveMaxScale);
}

LevelData::MoveTransform LevelData::moveTransformAt(int64_t floor, double timeSec) const {
    MoveTransform out;
    if (!m_moveReady || m_moveBlocks.empty() || floor < 0) return out;
    const size_t blk = (size_t)floor / kRecolorBlock;
    if (blk >= m_moveBlocks.size()) return out;
    const auto& list = m_moveBlocks[blk];
    if (list.empty()) return out;

    struct Prop { bool has=false; float from=0, target=0; double trig=0, dur=0; uint8_t ease=0; };
    Prop px, py, rot, sx, sy, op;
    px.from = py.from = rot.from = 0.0f;
    sx.from = sy.from = op.from = 1.0f;
    sx.target = sy.target = op.target = 1.0f;

    for (uint32_t idx : list) {
        const MoveTrackEvent& e = m_moveIndex[idx];
        if (e.lo > floor || e.hi < floor) continue;
        if (e.gap > 0 && ((floor - e.lo) % ((int64_t)e.gap + 1)) != 0) continue;
        if (e.trigger > timeSec + 1e-9) break;
        auto set = [&](Prop& p, float target) {
            p.from = p.has ? p.target : p.from;
            p.target = target;
            p.has = true;
            p.trig = e.trigger;
            p.dur = e.durSec;
            p.ease = e.ease;
        };
        if (e.flags & kMoveOffX) set(px, e.offX);
        if (e.flags & kMoveOffY) set(py, e.offY);
        // **旋转是绝对角**（Re_ADOJAS `buildTileMoveTrack`：`mesh.rotation.z = rotationOffset`，
        // 覆盖砖自身的朝向），而我们的几何已经把砖的方向烘进去了 → 增量 = 目标角 − 砖自身方向。
        // 写成"在自身方向上再转 rotationOffset"会让每块砖保留各自的方向，整张图的相对排布完全不同
        // （2026-10-21 Plum - R 结尾那一大片散砖就是这么对不上的）。
        if (e.flags & kMoveRot)
            set(rot, (e.rotDeg - tileDirectionAt(floor)) * 3.14159265358979323846f / 180.0f);
        if (e.flags & kMoveScale) { set(sx, e.scaleX); set(sy, e.scaleY); }
        if (e.flags & kMoveOpacity) set(op, e.opacity);
    }
    auto value = [&](const Prop& p) -> float {
        if (!p.has) return p.from;
        if (p.dur <= 0.0 || timeSec >= p.trig + p.dur) return p.target;
        const double t = std::clamp((timeSec - p.trig) / p.dur, 0.0, 1.0);
        const double q = easeApply((Ease)p.ease, t);
        return (float)((double)p.from + ((double)p.target - (double)p.from) * q);
    };
    out.any = px.has || py.has || rot.has || sx.has || sy.has || op.has;
    out.moveX = value(px);
    out.moveY = value(py);
    out.rotDelta = value(rot);
    out.scaleX = value(sx);
    out.scaleY = value(sy);
    out.alpha = value(op);
    return out;
}

const LevelData::ColorEvent* LevelData::persistentColorAtOrBefore(int64_t floor) const {
    for (auto it = colorEvents.rbegin(); it != colorEvents.rend(); ++it) {
        if (it->floor > floor) continue;
        if (it->flags & (kColorInactive | kColorEditorOnly | kColorIsRecolor)) continue;
        if (it->flags & kColorJustThisTile) continue;
        return &*it;
    }
    return nullptr;
}

const LevelData::ColorEvent* LevelData::persistentColorBefore(int64_t floor) const {
    for (auto it = colorEvents.rbegin(); it != colorEvents.rend(); ++it) {
        if (it->floor >= floor) continue;
        if (it->flags & (kColorInactive | kColorEditorOnly | kColorIsRecolor)) continue;
        if (it->flags & kColorJustThisTile) continue;
        return &*it;
    }
    return nullptr;
}

TileColors LevelData::baseColorsAt(int64_t floor, double timeSec, float amplitude,
                                   const Timeline* timeline) const {
    const ColorEvent* e = colorEventAt(floor);
    TileColorConfig cfg = baseColorConfig();
    if (e) {
        cfg = e->cfg;
        if (!(e->flags & kColorHasGlow)) cfg.glowIntensity = settings.trackGlowIntensity;
    }
    const TileColors to = computeTileColors(cfg, timeSec, (int)floor, amplitude);

    // TweenColor：只对 Single/Stripes（Re_ADOJAS `tickColorFade` 的判据）。
    if (!e || !timeline || e->duration <= 0.0f) return to;
    if (cfg.colorType != "Single" && cfg.colorType != "Stripes") return to;

    const double start = timeline->startTimeAt(e->floor);
    const double beat = (double)timeline->durationAt(e->floor);   // 一砖 = 一拍（秒）
    const double dur = (double)e->duration * beat;
    if (dur <= 0.0) return to;
    if (timeSec >= start + dur) return to;

    // "from" = 事件生效前的颜色（Re_ADOJAS 的 Kill(complete:true)：旧 tween 直接跳到目标色）。
    const ColorEvent* fromE = (e->flags & kColorJustThisTile)
                                  ? persistentColorAtOrBefore(e->floor)
                                  : persistentColorBefore(e->floor);
    TileColorConfig fromCfg = fromE ? fromE->cfg : baseColorConfig();
    if (fromE && !(fromE->flags & kColorHasGlow)) fromCfg.glowIntensity = settings.trackGlowIntensity;
    const TileColors from = computeTileColors(fromCfg, start, (int)floor, amplitude);

    if (timeSec <= start) return from;
    const double p = (timeSec - start) / dur;
    const double q = easeApply((Ease)e->ease, p);   // core/util/Easing.hpp；不 clamp（与 Re_ADOJAS 一致）
    auto mixCh = [&](std::uint8_t a, std::uint8_t b) {
        return (std::uint8_t)std::clamp<long>(std::lround((double)a + ((double)b - (double)a) * q), 0, 255);
    };
    TileColors out;
    for (int c = 0; c < 3; c++) {
        out.fill[c] = mixCh(from.fill[c], to.fill[c]);
        out.stroke[c] = mixCh(from.stroke[c], to.stroke[c]);
    }
    out.opacity = to.opacity;   // Re_ADOJAS 的 tickColorFade 也一直用 fromAlpha（= 新事件的 alpha）
    return out;
}

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
    // **幂等**：同一个对象加载两次必须与"新建一个再加载"等价。以前只清了驻留表，于是
    // `actions` 会被追加（两条路都是 `if (actions.empty()) … else insert(...)`）而
    // `angleData` 是替换 —— 结果是"角度新的、事件两份"。现在先回到刚构造的状态。
    // ⚠️ `settings` **例外**：它是"文件里没出现的字段沿用调用方给的值"的载体
    // （CLI 预置颜色、向导的配置），必须留着（见 parseLegacy / 窗口路径里的沿用逻辑）。
    {
        Settings keep = settings;
        *this = LevelData{};
        settings = keep;
    }
    g_internOwner = this;                                  // 事件字符串驻留表归本次加载所有
    actionStrTable.resize(1);                              // id 0 = 空串
    tlsOwner = nullptr;                                // thread-local 缓存失效
    // 压缩容器（.adofai.xz / .adofai.zst，按 magic 判断）：先解开，再当明文解析。
    // 解压后的缓冲区必须活到解析结束，所以放在这个作用域里。
    std::string decompressed;
    const LevelArchiveKind archive = sniffLevelArchive(data, len);
    if (archive == LevelArchiveKind::Adocao) {
        // 自研二进制容器：**明确失败，绝不静默回退**（这和压缩容器的"搞不定就退回整份解压"
        // 是两种态度 —— 那里的回退不改变语义，这里回退意味着把一个坏文件当好文件读了）。
        const ArchiveBackend& bk = archiveBackend();
        std::string why;
        if (!bk.decodeAdocao || !bk.decodeAdocao(data, len, *this, why)) {
            LOG_E("adocao 解码失败: %s", why.empty() ? "archive 后端未注册（缺 install()）" : why.c_str());
            return false;
        }
        return finishLoad(onProgress, exportOnly);      // 与两条 JSON 路径共用同一段收尾
    }
    if (archive != LevelArchiveKind::Plain) {
        // 优先走窗口流水线：不把整份解压结果摊进内存（10 GB 文本那份匿名内存会被系统
        // 压缩/换页，实测吞吐掉到 1/11）。任何搞不定的情况都退回下面的整份解压。
        const bool forceWhole = env::flag("ADOCAO_WHOLE_DECOMPRESS");   // 每次读：测试会在同一进程里切换
        // 压缩输入默认走窗口流水线：逐位一致（fixture 用 4 KB 半窗、1.18 / 1.40 GB 真实谱用
        // 96 KB / 1 MB / 8 MB 半窗都核对过 13 个节的 hash），而且比整份解压省掉"整份文本"
        // 的分配与二次扫描。搞不定的情况一律放弃并落到下面的整份解压，所以默认开启无风险。
        // ADOCAO_WINDOW_KB 调半窗大小；ADOCAO_WHOLE_DECOMPRESS=1 强制退回整份解压。
        // 测试用：要求必须走通窗口路径（不许静默回退），否则直接失败 —— 用来证明用例
        // 真的覆盖了这条路径，而不是每次都在偷偷走整份解压。
        const bool requireWindow = env::flag("ADOCAO_WINDOW_REQUIRE");
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
                        if (cameraEvents.empty()) cameraEvents = std::move(wp.cameraSink.cameras);
                        else cameraEvents.insert(cameraEvents.end(),
                                                 std::make_move_iterator(wp.cameraSink.cameras.begin()),
                                                 std::make_move_iterator(wp.cameraSink.cameras.end()));
                        if (repeatEvents.empty()) repeatEvents = std::move(wp.cameraSink.repeats);
                        else repeatEvents.insert(repeatEvents.end(),
                                                 std::make_move_iterator(wp.cameraSink.repeats.begin()),
                                                 std::make_move_iterator(wp.cameraSink.repeats.end()));
                        if (colorEvents.empty()) colorEvents = std::move(wp.cameraSink.colors);
                        else colorEvents.insert(colorEvents.end(),
                                                std::make_move_iterator(wp.cameraSink.colors.begin()),
                                                std::make_move_iterator(wp.cameraSink.colors.end()));
                        if (flashEvents.empty()) flashEvents = std::move(wp.cameraSink.flashes);
                        else flashEvents.insert(flashEvents.end(),
                                                std::make_move_iterator(wp.cameraSink.flashes.begin()),
                                                std::make_move_iterator(wp.cameraSink.flashes.end()));
                        if (moveTrackEvents.empty()) moveTrackEvents = std::move(wp.cameraSink.moves);
                        else moveTrackEvents.insert(moveTrackEvents.end(),
                                                    std::make_move_iterator(wp.cameraSink.moves.begin()),
                                                    std::make_move_iterator(wp.cameraSink.moves.end()));
                        return finishLoad(onProgress, exportOnly);
                    }
                }
                // rc == 0 或 settings 解析失败 -> 落到整份解压，行为与以前完全一致。
                // 但**不许静默**：回退本身只会在内存峰值与耗时上表现出来，2026-10 那个
                // "大谱上窗口路径其实一次都没被用上"就是这么藏了一轮（docs/2e9.md §3）。
                const char* why = wp.tarContainer ? "ilytx（tar 容器）只能整份解压"
                                : wp.failed       ? (wp.failWhy && *wp.failWhy ? wp.failWhy : "结构解析失败")
                                : stream->stuck() ? "单个值大于半窗"
                                                  : "流式结构不可解析（需要更多数据 / 已到流末尾）";
                if (requireWindow) {
                    LOG_E("windowed parse declined the stream (test hook ADOCAO_WINDOW_REQUIRE): %s", why);
                    return false;
                }
                LOG_W("流式窗口放弃（%s）—— 退回整份解压（半窗 %zu KB，多花一份整份文本的内存）",
                      why, half >> 10);
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
    const bool forceLegacy = env::flag("ADOCAO_FORCE_DOM_PARSE");

    try {
        if (!forceLegacy) {
            if (tryFastParse(data, len, onProgress)) return finishLoad(onProgress, exportOnly);
            // 测试用：要求快路径必须能吃下这个文件（不许静默回退到 DOM），用来界定
            // "窗口路径也必须能吃下"的范围。
            if (env::flag("ADOCAO_FAST_REQUIRE")) {
                LOG_E("fast parse declined the buffer (test hook ADOCAO_FAST_REQUIRE); stage=%s", g_fastStage);
                std::fprintf(stderr, "[ADOCAO] fast parse declined at stage: %s off=%lld\n", g_fastStage,
                             (g_failPos && g_bufBase) ? (long long)(g_failPos - g_bufBase) : -1LL);
                return false;
            }
            LOG_D("fast parse declined at stage: %s (falling back to legacy)", g_fastStage);
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
// 采样估计 angleData 的元素个数：在整段上均匀取若干窗口，数每个窗口里的逗号，
// 按窗口密度外推到整段，**取最大的那个估计**，再加余量。
//
// 为什么不能再用固定常数：实测同一批谱面的每元素字节数相差 4 倍（The Moon 3.53、
// angles360 5.76、MYC 13.39），旧的 /3 于是预留 1.18~4.46 倍，MYC 一个谱就白占 187 MB。
// 为什么估计要偏大：低估会触发 vector 的倍增，那一刻新旧缓冲同时在 —— 峰值翻倍，
// 比多留十几个百分点糟得多。所以取最大窗口估计 + 1/8 余量（实测各谱面都落在 1.1x 上下，
// 既不再有 4 倍浪费，也不会重新分配）。
// 每窗口 32 KB、最多 8 个 → 扫描量与谱面大小无关（总共 256 KB 上限，微秒级）。
static size_t estimateAngleCount(const char* p, size_t span) {
    if (span == 0) return 0;
    const size_t kW = 32u << 10;     // 每个采样窗 32 KB
    const int    kMaxWin = 8;        // 最多 8 个窗（总扫描 ≤ 256 KB，与谱面大小无关）
    size_t best = 0;
    for (int w = 0; w < kMaxWin; ++w) {
        // 均匀铺开：窗口中心按 (w+0.5)/kMaxWin 落在整段上
        const size_t center = (size_t)((double)span * ((double)w + 0.5) / (double)kMaxWin);
        const size_t off = center > kW / 2 ? center - kW / 2 : 0;
        const size_t n = (span - off < kW) ? (span - off) : kW;
        if (n < 16) continue;
        size_t commas = 0;
        for (size_t i = 0; i < n; ++i) if (p[off + i] == ',') ++commas;
        if (commas == 0) continue;
        // 把窗口密度外推到整段（先乘后除，避免整数截断丢精度）
        const size_t est = (size_t)((double)commas * (double)span / (double)n) + 1;
        if (est > best) best = est;
    }
    if (best == 0) return span / 3;        // 退化（一个逗号都没采到）：退回旧启发式
    // +12.5% 余量确保不触发倍增；末尾 +16 是**极小谱面的下限**：窗口采到 1 个逗号时
    // 外推值可能是 1~2，直接 reserve 会让 push_back 立刻扩容一次（白白两倍峰值）。
    return best + best / 8 + 16;
}

bool LevelData::tryFastParse(const char* data, size_t len, ProgressCb onProgress) {
    if (onProgress) onProgress(0.10f, "Parsing angleData...");
    g_bufBase = data;
    g_failPos = nullptr;
    std::vector<FastAction> newActions;   // actions 在根扫描里就地解析
    CameraSink newCamera;
    Regions r;
    g_fastStage = "scanRootMembers";
    if (!scanRootMembers(data, data + len, r, newActions, newCamera)) return false;

    if (onProgress) onProgress(0.12f, "Parsing JSON...");

    std::vector<double> newAngles;
    if (r.angle) {
        // angleData 只有几十 MB，加上整数快路径后 6.77 M 个值只要 ~25 ms。
        // 预留量按采样估出来的每元素字节数算（见 estimateAngleCount），不要用固定常数。
        newAngles.reserve(estimateAngleCount(r.angle, (size_t)(r.angleEnd - r.angle)));
        g_fastStage = "angleData";
        if (!parseAngleDataRegion(r.angle, r.angleEnd, newAngles)) return false;
    }

    Settings newSettings = settings;   // 与旧路径一致：只覆盖文件里出现的字段
    std::string newPath = pathData;
    if (r.settings) {
        // settings 对象很小，交给 cleanJson + RapidJSON 处理，省得再写一个解析器
        std::string sub = cleanJson(std::string(r.settings, (size_t)(r.settingsEnd - r.settings)));
        rapidjson::Document s;
        s.Parse<rapidjson::kParseTrailingCommasFlag>(sub.c_str());
        g_fastStage = "settings";
        if (s.HasParseError() || !s.IsObject()) return false;
        readSettings(s, newSettings);
    }
    if (r.path && r.pathEnd > r.path + 1) {
        // 旧路径用 GetString()（会解转义），带反斜杠就交回旧路径；裸 CR 则照 cleanJson 删掉
        g_fastStage = "pathData(escape)";
        if (std::memchr(r.path, '\\', (size_t)(r.pathEnd - r.path))) return false;
        newPath.assign(r.path + 1, (size_t)(r.pathEnd - r.path - 1));
        newPath.erase(std::remove(newPath.begin(), newPath.end(), '\r'), newPath.end());
    }

    if (onProgress) onProgress(0.15f, "Extracting level data...");

    angleData = std::move(newAngles);
    if (actions.empty()) actions = std::move(newActions);
    else actions.insert(actions.end(), std::make_move_iterator(newActions.begin()),
                        std::make_move_iterator(newActions.end()));
    if (cameraEvents.empty()) cameraEvents = std::move(newCamera.cameras);
    else cameraEvents.insert(cameraEvents.end(), std::make_move_iterator(newCamera.cameras.begin()),
                             std::make_move_iterator(newCamera.cameras.end()));
    if (colorEvents.empty()) colorEvents = std::move(newCamera.colors);
    else colorEvents.insert(colorEvents.end(), std::make_move_iterator(newCamera.colors.begin()),
                            std::make_move_iterator(newCamera.colors.end()));
    if (flashEvents.empty()) flashEvents = std::move(newCamera.flashes);
    else flashEvents.insert(flashEvents.end(), std::make_move_iterator(newCamera.flashes.begin()),
                            std::make_move_iterator(newCamera.flashes.end()));
    if (moveTrackEvents.empty()) moveTrackEvents = std::move(newCamera.moves);
    else moveTrackEvents.insert(moveTrackEvents.end(), std::make_move_iterator(newCamera.moves.begin()),
                                std::make_move_iterator(newCamera.moves.end()));
    if (repeatEvents.empty()) repeatEvents = std::move(newCamera.repeats);
    else repeatEvents.insert(repeatEvents.end(), std::make_move_iterator(newCamera.repeats.begin()),
                             std::make_move_iterator(newCamera.repeats.end()));
    settings = newSettings;
    pathData = std::move(newPath);
    return true;
}

// 旧路径：cleanJson + RapidJSON DOM。保持原样，是回退路径也是对拍基准。

// ── DOM（旧路径）版本的相机/重复事件读取 ─────────────────────────────────────
// 旧路径是**对拍基准**：字段语义必须与快路径的 rich 解析逐字段一致（测试里直接比结构体）。
namespace {

int jsonCamRelativeTo(const rapidjson::Value& v) {
    using C = LevelData;
    if (v.IsString()) {
        std::string s = v.GetString();
        if (s == "Player") return C::CamPlayer;
        if (s == "Tile") return C::CamTile;
        if (s == "Global") return C::CamGlobal;
        if (s == "LastPosition") return C::CamLastPosition;
        if (s == "LastPositionNoRotation") return C::CamLastPositionNoRotation;
        return C::CamUnknown;
    }
    if (v.IsNumber()) {
        double d = v.GetDouble();
        if (d == 0) return C::CamPlayer;
        if (d == 1) return C::CamTile;
        if (d == 2) return C::CamGlobal;
        if (d == 3) return C::CamLastPosition;
        if (d == 4) return C::CamLastPositionNoRotation;
        return C::CamPlayer;                 // 数字越界 → Player（照抄 parseMovementType）
    }
    return C::CamAbsent;
}

void jsonReadCameraEvent(const rapidjson::Value& a, std::vector<LevelData::CameraEvent>& out) {
    using C = LevelData;
    C::CameraEvent ev;
    ev.floor = a["floor"].GetInt();
    if (a.HasMember("id") && a["id"].IsNumber()) {
        double d = a["id"].GetDouble();
        if (d >= 0.0 && d < 4294967295.0) ev.id = (uint32_t)d;
    }
    if (a.HasMember("angleOffset") && a["angleOffset"].IsNumber()) ev.angleOffset = a["angleOffset"].GetFloat();
    if (a.HasMember("duration") && a["duration"].IsNumber()) ev.duration = a["duration"].GetFloat();
    if (a.HasMember("position") && a["position"].IsArray()) {
        ev.flags |= C::kCamHasPosition;
        auto& p = a["position"];
        if (p.Size() >= 1 && p[0].IsNumber()) { ev.x = p[0].GetFloat(); ev.flags |= C::kCamHasX; }
        if (p.Size() >= 2 && p[1].IsNumber()) { ev.y = p[1].GetFloat(); ev.flags |= C::kCamHasY; }
    }
    if (a.HasMember("rotation") && a["rotation"].IsNumber()) { ev.rotation = a["rotation"].GetFloat(); ev.flags |= C::kCamHasRotation; }
    if (a.HasMember("zoom") && a["zoom"].IsNumber()) { ev.zoom = a["zoom"].GetFloat(); ev.flags |= C::kCamHasZoom; }
    if (a.HasMember("ease") && a["ease"].IsString()) {
        ev.ease = (uint8_t)easeFromName(a["ease"].GetString());
        ev.flags |= C::kCamEaseWritten;
    }
    if (a.HasMember("relativeTo")) ev.relativeTo = (uint8_t)jsonCamRelativeTo(a["relativeTo"]);
    if (a.HasMember("disabled") && a["disabled"].IsObject()) {
        auto& d = a["disabled"];
        auto on = [&](const char* k) { return d.HasMember(k) && d[k].IsBool() && d[k].GetBool(); };
        if (on("position"))   ev.disabled |= C::kCamDisPosition;
        if (on("rotation"))   ev.disabled |= C::kCamDisRotation;
        if (on("zoom"))       ev.disabled |= C::kCamDisZoom;
        if (on("relativeTo")) ev.disabled |= C::kCamDisRel;
        if (on("active"))     ev.flags |= C::kCamInactive;
    }
    if (a.HasMember("active")) {
        auto& v = a["active"];
        if (v.IsBool() && !v.GetBool()) ev.flags |= C::kCamInactive;
        else if (v.IsString() && std::string(v.GetString()) == "Disabled") ev.flags |= C::kCamInactive;
    }
    if (a.HasMember("editorOnly") && a["editorOnly"].IsBool() && a["editorOnly"].GetBool())
        ev.flags |= C::kCamEditorOnly;
    if (a.HasMember("tag") && a["tag"].IsString()) ev.tag = a["tag"].GetString();
    out.push_back(std::move(ev));
}

void jsonReadRepeatEvent(const rapidjson::Value& a, std::vector<LevelData::RepeatEvent>& out) {
    using C = LevelData;
    C::RepeatEvent rp;
    rp.floor = a["floor"].GetInt();
    bool isBeat = true;
    if (a.HasMember("repeatType") && a["repeatType"].IsString())
        isBeat = std::string(a["repeatType"].GetString()) != "Floor";
    if (isBeat) rp.repetitions = (a.HasMember("repetitions") && a["repetitions"].IsNumber())
                                     ? a["repetitions"].GetFloat() : 0.0f;
    else        rp.repetitions = (a.HasMember("floorCount") && a["floorCount"].IsNumber())
                                     ? a["floorCount"].GetFloat() : 0.0f;
    rp.interval = (a.HasMember("interval") && a["interval"].IsNumber()) ? a["interval"].GetFloat() : 1.0f;
    rp.gapLength = (a.HasMember("gapLength") && a["gapLength"].IsNumber()) ? a["gapLength"].GetFloat() : 1.0f;
    rp.flags = isBeat ? 1 : 0;
    if (a.HasMember("executeOnCurrentFloor") && a["executeOnCurrentFloor"].IsBool()
        && a["executeOnCurrentFloor"].GetBool()) rp.flags |= 2;
    if (a.HasMember("tag") && a["tag"].IsString()) rp.tag = a["tag"].GetString();
    out.push_back(std::move(rp));
}

void jsonTileRef(const rapidjson::Value& v, int32_t& off, int8_t& rel) {
    auto relOf = [](const rapidjson::Value& r) -> int8_t {
        if (r.IsString()) {
            std::string s = r.GetString();
            if (s == "Start") return 1;
            if (s == "End") return 2;
            if (s == "ThisTile") return 0;
        } else if (r.IsNumber()) {
            const double d = r.GetDouble();
            return (d == 1.0) ? 1 : (d == 2.0) ? 2 : 0;
        }
        return 0;
    };
    if (v.IsArray() && v.Size() >= 1 && v[0].IsNumber()) {
        off = (int32_t)v[0].GetDouble();
        rel = (v.Size() >= 2) ? relOf(v[1]) : 0;
    } else if (v.IsNumber()) {
        off = (int32_t)v.GetDouble(); rel = -1;
    } else if (v.IsString()) {
        std::string s = v.GetString();
        int8_t r = -1; size_t p = 0;
        if (s.rfind("Start", 0) == 0) { r = 1; p = 5; }
        else if (s.rfind("ThisTile", 0) == 0) { r = 0; p = 8; }
        else if (s.rfind("End", 0) == 0) { r = 2; p = 3; }
        double n2 = 0.0;
        if (p < s.size()) {
            bool neg = false; size_t q = p;
            if (s[q] == '+' || s[q] == '-') { neg = (s[q] == '-'); ++q; }
            try { n2 = std::stod(s.substr(q)); } catch (...) { n2 = 0.0; }
            if (neg) n2 = -n2;
        }
        off = (int32_t)n2; rel = r;
    }
}

void jsonReadMoveTrackEvent(const rapidjson::Value& a, std::vector<LevelData::MoveTrackEvent>& out) {
    using C = LevelData;
    C::MoveTrackEvent ev;
    ev.floor = a["floor"].GetInt();
    if (a.HasMember("startTile")) jsonTileRef(a["startTile"], ev.startOff, ev.startRel);
    if (a.HasMember("endTile")) jsonTileRef(a["endTile"], ev.endOff, ev.endRel);
    ev.gap = (a.HasMember("gapLength") && a["gapLength"].IsNumber()) ? (int32_t)a["gapLength"].GetDouble() : 0;
    ev.durBeats = (a.HasMember("duration") && a["duration"].IsNumber()) ? a["duration"].GetFloat() : 1.0f;
    ev.angleOffset = (a.HasMember("angleOffset") && a["angleOffset"].IsNumber()) ? a["angleOffset"].GetFloat() : 0.0f;
    if (a.HasMember("eventTag") && a["eventTag"].IsString()) ev.tag = a["eventTag"].GetString();
    if (a.HasMember("ease") && a["ease"].IsString()) ev.ease = (uint8_t)easeFromName(a["ease"].GetString());
    uint8_t flags = 0;
    if (a.HasMember("positionOffset") && a["positionOffset"].IsArray()) {
        const auto& p = a["positionOffset"];
        if (p.Size() >= 1 && p[0].IsNumber()) { ev.offX = p[0].GetFloat(); flags |= C::kMoveOffX; }
        if (p.Size() >= 2 && p[1].IsNumber()) { ev.offY = p[1].GetFloat(); flags |= C::kMoveOffY; }
    }
    if (a.HasMember("rotationOffset") && a["rotationOffset"].IsNumber()) {
        ev.rotDeg = a["rotationOffset"].GetFloat(); flags |= C::kMoveRot;
    }
    if (a.HasMember("scale")) {
        const auto& sc = a["scale"];
        if (sc.IsNumber()) { ev.scaleX = ev.scaleY = (float)(sc.GetDouble() / 100.0); flags |= C::kMoveScale; }
        else if (sc.IsArray()) {
            bool any = false;
            if (sc.Size() >= 1 && sc[0].IsNumber()) { ev.scaleX = (float)(sc[0].GetDouble() / 100.0); any = true; }
            if (sc.Size() >= 2 && sc[1].IsNumber()) { ev.scaleY = (float)(sc[1].GetDouble() / 100.0); any = true; }
            if (any) flags |= C::kMoveScale;
        }
    }
    if (a.HasMember("opacity") && a["opacity"].IsNumber()) {
        ev.opacity = (float)(a["opacity"].GetDouble() / 100.0); flags |= C::kMoveOpacity;
    }
    if (a.HasMember("active")) {
        const auto& v = a["active"];
        if ((v.IsBool() && !v.GetBool()) || (v.IsString() && std::string(v.GetString()) == "Disabled"))
            flags |= C::kMoveInactive;
    }
    if (a.HasMember("editorOnly") && a["editorOnly"].IsBool() && a["editorOnly"].GetBool())
        flags |= C::kMoveEditorOnly;
    ev.flags = flags;
    out.push_back(ev);
}

void jsonReadFlashEvent(const rapidjson::Value& a, std::vector<LevelData::FlashEvent>& out) {
    using C = LevelData;
    C::FlashEvent ev;
    ev.floor = a["floor"].GetInt();
    ev.duration = (a.HasMember("duration") && a["duration"].IsNumber()) ? a["duration"].GetFloat() : 1.0f;
    ev.angleOffset = (a.HasMember("angleOffset") && a["angleOffset"].IsNumber()) ? a["angleOffset"].GetFloat() : 0.0f;
    ev.startOpacity = (a.HasMember("startOpacity") && a["startOpacity"].IsNumber())
                          ? a["startOpacity"].GetFloat() / 100.0f
                          : ((a.HasMember("opacity") && a["opacity"].IsNumber()) ? a["opacity"].GetFloat() / 100.0f : 1.0f);
    ev.endOpacity = (a.HasMember("endOpacity") && a["endOpacity"].IsNumber())
                        ? a["endOpacity"].GetFloat() / 100.0f : 0.0f;
    auto str = [&](const char* k) -> std::string {
        return (a.HasMember(k) && a[k].IsString()) ? std::string(a[k].GetString()) : std::string();
    };
    std::string sc = str("startColor"); if (sc.empty()) sc = str("color"); if (sc.empty()) sc = "ffffff";
    std::string ec = str("endColor"); if (ec.empty()) ec = str("colorTo"); if (ec.empty()) ec = sc;
    const Rgb8 c1 = parseColorHex(sc), c2 = parseColorHex(ec);
    ev.startColor[0]=c1.r; ev.startColor[1]=c1.g; ev.startColor[2]=c1.b;
    ev.endColor[0]=c2.r; ev.endColor[1]=c2.g; ev.endColor[2]=c2.b;
    ev.plane = (str("plane") == "Background") ? 1 : 0;
    const std::string fs = str("flashStyle");
    ev.style = (fs == "Kill") ? 1 : ((fs == "StayBlack") ? 2 : 0);
    if (a.HasMember("ease") && a["ease"].IsString()) ev.ease = (uint8_t)easeFromName(a["ease"].GetString());
    out.push_back(ev);
}

void jsonReadColorEvent(const rapidjson::Value& a, std::vector<LevelData::ColorEvent>& out,
                        bool recolor = false) {
    using C = LevelData;
    C::ColorEvent ev;
    ev.floor = a["floor"].GetInt();
    auto str = [&](const char* k, const char* dflt) -> std::string {
        if (a.HasMember(k) && a[k].IsString()) {
            std::string v = a[k].GetString();
            if (!v.empty() && v.front() == '#') v.erase(v.begin());
            return v;
        }
        return dflt;
    };
    ev.cfg.colorType = str("trackColorType", "Single");
    ev.cfg.color = str("trackColor", "debb7b");
    ev.cfg.secondary = str("secondaryTrackColor", "ffffff");
    ev.cfg.pulse = str("trackColorPulse", "None");
    ev.cfg.style = str("trackStyle", "Standard");
    ev.cfg.animDuration = (a.HasMember("trackColorAnimDuration") && a["trackColorAnimDuration"].IsNumber())
                              ? a["trackColorAnimDuration"].GetFloat() : 2.0f;
    ev.cfg.pulseLength = (a.HasMember("trackPulseLength") && a["trackPulseLength"].IsNumber())
                             ? a["trackPulseLength"].GetFloat() : 10.0f;
    if (a.HasMember("trackGlowIntensity") && a["trackGlowIntensity"].IsNumber()) {
        ev.cfg.glowIntensity = a["trackGlowIntensity"].GetFloat();
        ev.flags |= C::kColorHasGlow;
    }
    ev.cfg.startFloor = ev.floor;
    ev.cfg.startTime = 0.0;
    if (a.HasMember("duration") && a["duration"].IsNumber()) ev.duration = a["duration"].GetFloat();
    if (a.HasMember("ease") && a["ease"].IsString()) ev.ease = (uint8_t)easeFromName(a["ease"].GetString());
    if (a.HasMember("justThisTile")) {
        auto& v = a["justThisTile"];
        if (v.IsBool()) ev.flags |= v.GetBool() ? C::kColorJustThisTile : 0;
        else if (v.IsString()) {
            std::string sv = v.GetString();
            if (sv == "Enabled" || sv == "true" || sv == "True") ev.flags |= C::kColorJustThisTile;
        }
    }
    if (a.HasMember("active")) {
        auto& v = a["active"];
        if (v.IsBool() && !v.GetBool()) ev.flags |= C::kColorInactive;
        else if (v.IsString() && std::string(v.GetString()) == "Disabled") ev.flags |= C::kColorInactive;
    }
    if (a.HasMember("editorOnly") && a["editorOnly"].IsBool() && a["editorOnly"].GetBool())
        ev.flags |= C::kColorEditorOnly;
    if (recolor) {
        ev.flags |= C::kColorIsRecolor;
        if (a.HasMember("startTile")) jsonTileRef(a["startTile"], ev.startOff, ev.startRel);
        if (a.HasMember("endTile")) jsonTileRef(a["endTile"], ev.endOff, ev.endRel);
        ev.gapLength = (a.HasMember("gapLength") && a["gapLength"].IsNumber())
                           ? (int32_t)a["gapLength"].GetDouble() : 0;
        ev.angleOffset = (a.HasMember("angleOffset") && a["angleOffset"].IsNumber())
                             ? a["angleOffset"].GetFloat() : 0.0f;
    }
    out.push_back(std::move(ev));
}

}  // namespace

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
            else if (et == "MoveCamera") { jsonReadCameraEvent(a, cameraEvents); continue; }
            else if (et == "RepeatEvents") { jsonReadRepeatEvent(a, repeatEvents); continue; }
            else if (et == "ColorTrack") { jsonReadColorEvent(a, colorEvents); continue; }
            else if (et == "RecolorTrack") { jsonReadColorEvent(a, colorEvents, true); continue; }
            else if (et == "Flash") { jsonReadFlashEvent(a, flashEvents); continue; }
            else if (et == "MoveTrack") { jsonReadMoveTrackEvent(a, moveTrackEvents); continue; }
            else if (et == "ScaleRadius") act.type = FastAction::ScaleRadius;
            else continue;
            if (act.type == FastAction::SetSpeed) {
                if (a.HasMember("speedType") && std::string(a["speedType"].GetString()) == "Multiplier") {
                    act.flag = true; act.val1 = a.HasMember("bpmMultiplier") ? a["bpmMultiplier"].GetFloat() : 1.0f;
                } else { act.val1 = a.HasMember("beatsPerMinute") ? a["beatsPerMinute"].GetFloat() : 0.0f; }
            } else if (act.type == FastAction::Pause) {
                act.val1 = a.HasMember("duration") ? a["duration"].GetFloat() : 0.0f;
            } else if (act.type == FastAction::ScaleRadius) {
                act.val1 = (a.HasMember("scale") && a["scale"].IsNumber())
                               ? a["scale"].GetFloat() / 100.0f : 1.0f;
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
                if (a.HasMember("trackAnimation")) {
                    act.flag = true;   // flag2: has trackAnimation
                    act.strId2 = g_internOwner ? g_internOwner->internActionStr(a["trackAnimation"].GetString()) : 0;
                }
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

    // 相机事件的 `order` = 文件顺序（同 time 同 id 时的最终 tie-break）。
    // 所有解析路径（快/窗口/DOM/容器）都在这里收口，所以只在这一处赋值。
    for (size_t i = 0; i < cameraEvents.size(); i++)
        cameraEvents[i].order = (uint32_t)i;
    for (size_t i = 0; i < colorEvents.size(); i++)
        colorEvents[i].order = (uint32_t)i;
    // 按 (floor, 文件顺序) 排好：`colorEventAt` 用二分找"最后一条 floor<=f 的持久段"，
    // 渲染侧每帧每砖查一次，不能线性扫。order 保留文件顺序（同 floor 后者赢）。
    std::stable_sort(colorEvents.begin(), colorEvents.end(),
                     [](const ColorEvent& a, const ColorEvent& b) {
                         return a.floor != b.floor ? a.floor < b.floor : a.order < b.order;
                     });

    // Convert pathData → angleData if needed
    if (!pathData.empty() && angleData.empty()) {
        convertPathToAngles();
    }

    if (!exportOnly) {
        if (onProgress) onProgress(0.30f, "Calculating tile positions...");
        calculateTilePositions();
    }
    if (onProgress) onProgress(0.40f, "Processing actions...");
    refreshDerivedCaches();
    if (!exportOnly) {
        applyPositionTrackOffsets();
        applyRadiusScaleOffsets();
    }
    return true;
}

void LevelData::calculateTilePositions() {
    tiles.clear();
    if (angleData.empty()) return;

    const int64_t nFull = (int64_t)angleData.size();
    if (!fitsInt32(nFull)) {
        LOG_E("calculateTilePositions 需要 64 位索引（2^31−1 目标 #0 阶段 3）：%lld 层超出 int32",
              (long long)nFull);
        return;
    }
    const int n = (int)nFull;

    // Build "floats" array: 999 = midspin (previous + 180)
    std::vector<float> floats(n);
    for (int i = 0; i < n; i++) {
        if (angleData[i] == 999.0) {
            floats[i] = (i > 0 ? floats[i - 1] : 0.0f) + 180.0f;
        } else {
            floats[i] = angleData[i];
        }
    }

    // 先 reserve(n+1) 再 resize(n)：末尾还要 push_back 第 n+1 块"extra"砖（见函数末），
    // 若直接 resize(n) 再 push_back，libc++ 会按 2× 增长 → 容量永久停在 2n，
    // 于是每层常驻是 48 B 而不是 24 B（实测三张谱全部 cap = 2n：1e8 层白扔 2.4 GB，
    // 2^31-1 层 51 GB。见 docs/scale-1e8-to-2e9.md §7）。
    tiles.reserve((size_t)n + 1);
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

void LevelData::refreshDerivedCaches() {
    // **幂等**：先清空全部派生结构再按 `actions` 重建（ADOCAO-E 改完事件可以反复调）。
    // 层数走 `angleCount()` —— `releaseAngleData()` 之后 `angleData` 是空的，而位打包存储里
    // 还有层数（原来这里读 `angleData.size()`，等于要求"必须在压缩之前调"的隐式约束）。
    // +1 是第 0 层那一格；用 int64 算，避免 2^31−1 时 `+1` 有符号溢出（UBSan 报过）。
    const int64_t nFull = countFromAngleCount(angleCount());
    if (!fitsInt32(nFull)) {
        LOG_E("refreshDerivedCaches 需要 64 位索引（2^31−1 目标 #0 阶段 3）：%lld 层超出 int32",
              (long long)nFull);
        return;
    }
    const int n = (int)nFull;
    tileBPMs.assign(n, settings.bpm);
    tileHasTwirl.assign(n, false);
    tileHasSetSpeed.assign(n, false);
    // 稀疏/有条件分配的几支：不清就会留下上一次的条目（改完事件后"幽灵 hitsound/偏移"）
    tileHitsounds.clear();
    tileHitsoundVolumes.clear();
    tilePositionOffsets.clear();
    atStates.clear();
    hasAtStates = false;
    bookmarkFloors.clear();

    // ③ 2026-10-16：SetSpeed 改成**事件表**（旧的是"每层一个槽"的 12 B/层稠密数组）：
    //   ① 同层可以有**多条** SetSpeed（先绝对 `Bpm` 再 `Multiplier` 是常见写法），旧结构里
    //      第二条会把第一条的字段覆盖掉 → 绝对值丢失。MYC 第 27473 层正是（Bpm 8000 + ×2）：
    //      旧代码得 72000、正确是 16000（`Timeline` 逐条按文件序应用，一直是对的）。
    //   ② 那张稠密数组只要谱里有一条 SetSpeed 就 `resize(n)`（MYC 315 条事件白占 81 MB）。
    struct SSEv { int floor = 0; float val = 0.0f; bool isMultiplier = false; };
    std::vector<SSEv> ssEvents;
    // 按需分配：没有任何 SetSpeed 事件时这 n x 12 B 是白占的（实测 25 万层的无害谱上，
    // 峰值快照里就有一块 250001 x 12 = 3,000,012 B 的分配）。1e8 层就是 1.2 GB。
    // 空 vector 与原来逐位等价：默认构造的 SS 全 false/0，下面那个逐层循环体什么都不做。


    // 每个 SetHitsound 事件一条。原来存 std::string（40 B/条），改成存驻留 id：12 B/条，
    // 而 2^31 层 × 0.9 事件/层时这是 72 GB → 18 GB 的差别（见 docs/scale-1e8-to-2e9.md §13）。
    // strId == 0 表示空串（= 用 settings.hitsound），与 FastAction::strId 的约定一致。
    struct HSChange { int floor; float volume; uint16_t strId; };
    static_assert(sizeof(HSChange) == 12, "HSChange 必须保持 12 B（大谱的事件数是十亿级）");
    std::vector<HSChange> hsChanges;

    // settings.hitsound 的驻留 id：一次求得。原来逐层比较的是两个 std::string（1e9 层就是
    // 10 亿次字符串比较），现在只是一次整数比较。
    const uint16_t settingsHitsoundId = internActionStr(settings.hitsound);

    for (auto& a : actions) {
        int floor = a.floor;
        if (floor < 0 || floor >= n) continue;
        switch (a.type) {
        case FastAction::Twirl:
            tileHasTwirl[floor] = true; break;
        case FastAction::SetSpeed:
            tileHasSetSpeed[floor] = true;
            ssEvents.push_back({floor, a.val1, a.flag});
            break;
        case FastAction::PositionTrack:
            tilePositionOffsets[floor] = {a.val1, a.val2, a.flag}; break;
        case FastAction::SetHitsound:
            // strId == 0 就是空串（= 用 settings.hitsound），与上面 push 的 type 语义逐字一致。
            hsChanges.push_back({floor, a.flag ? a.val1 : settings.hitsoundVolume,
                                 a.strId ? a.strId : settingsHitsoundId}); break;
        case FastAction::Bookmark:
            bookmarkFloors.push_back(floor); break;
        case FastAction::AnimateTrack:
            hasAtStates = true;
            {
                ATState st;
                st.hasDA = !actionStr(a).empty();
                st.da    = actionStr(a);
                st.hasAA = a.flag && !actionStr2(a).empty();
                st.aa    = actionStr2(a);
                st.bb    = a.val1;   // <0 = 事件没写
                st.ba    = a.val2;
                atStates[floor] = std::move(st);
            }
            break;
        default: break;
        }
    }

    float runningBPM = settings.bpm;
    // 空 = 本谱一条 SetSpeed 都没有：tileBPMs 上面已经 assign 成全 settings.bpm，
    // 而原循环在这种情况下本来就只写回 settings.bpm —— 所以整段跳过是逐位等价的。
    if (!ssEvents.empty()) {
        // 同层多条必须按**文件序**应用（与 `Timeline::precalculateTiming` 的 flatActions 一致）：
        // 稳定排序只按 floor，同层保持原顺序。
        std::stable_sort(ssEvents.begin(), ssEvents.end(),
                         [](const SSEv& a, const SSEv& b) { return a.floor < b.floor; });
        size_t si = 0;
        for (int i = 0; i < n; i++) {
            while (si < ssEvents.size() && ssEvents[si].floor <= i) {
                const SSEv& e = ssEvents[si++];
                if (e.isMultiplier) runningBPM *= e.val; else runningBPM = e.val;
            }
            tileBPMs[i] = runningBPM;
        }
    }

    if (!hsChanges.empty()) {
        uint16_t curId = settingsHitsoundId;      // 驻留 id 取代原来的 std::string curHS
        float curVol = settings.hitsoundVolume;
        size_t ci = 0;
        for (int i = 0; i < n; i++) {
            while (ci < hsChanges.size() && hsChanges[ci].floor <= i) {
                curId = hsChanges[ci].strId; curVol = hsChanges[ci].volume; ci++;
            }
            if (curId != settingsHitsoundId) tileHitsounds[i] = actionStrTable[curId];
            if (curVol != settings.hitsoundVolume) {
                if (tileHitsoundVolumes.size() != (size_t)n)
                    tileHitsoundVolumes.assign((size_t)n, std::numeric_limits<float>::quiet_NaN());
                tileHitsoundVolumes[(size_t)i] = curVol;
            }
        }
    }

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
    const int64_t nFull = (int64_t)tiles.size();
    if (!fitsInt32(nFull)) {
        LOG_E("applyPositionTrackOffsets 需要 64 位索引（#0 阶段 3）：%lld 层超出 int32",
              (long long)nFull);
        return;
    }
    const int n = (int)nFull;

    std::vector<std::pair<int, TilePositionOffset>> sorted(tilePositionOffsets.begin(), tilePositionOffsets.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
    size_t oi = 0;

    // justThisTile 的语义 = **只作用于本层**（点加），三份来源一致：
    //   * 编辑器指南（adofaieditor.gitbook.io → Position Track → "Only change this tile"）：
    //     "If enabled, all settings from this event will only affect the event tile"；
    //   * Re_ADOJAS 的移植（src/lib/Player/PositionTrackManager.ts）：justThisTile → pointX[floor] += …
    //     （非 justThisTile 才是 bitAdd 后缀加）；
    //   * 它在 The Moon 那段上的渲染量出来也对得上：花瓣环离星心 1.5~2.2 倍星半径，
    //     正好落在点加模型的预测内（点加中位 1.46 / 最大 2.28），而"一律累积"会把花瓣甩到 3~5 倍外。
    // 非 justThisTile（缺省/"Disabled"）= 后缀加：本层及之后所有层都吃到（累进 cumX/cumY）。
    // 注：官方的 `vector`（累积偏移）只用于 relativeTo，我们还没实现 relativeTo。
    for (int i = 0; i < n; i++) {
        double px = 0.0, py = 0.0;       // 本层的"一次性"偏移（同层多个事件累加）
        while (oi < sorted.size() && sorted[oi].first == i) {
            const TilePositionOffset& o = sorted[oi].second;
            if (o.justThisTile) {
                px += o.offsetX; py += o.offsetY;
            } else {
                cumX += o.offsetX; cumY += o.offsetY;
            }
            oi++;
        }
        tiles[i].position[0] += cumX + px;
        tiles[i].position[1] += cumY + py;
    }
}

void LevelData::applyRadiusScaleOffsets() {
    m_hasScaleRadius = false;
    std::vector<std::pair<int, float>> evs;
    for (const FastAction& a : actions) {
        if (a.type == FastAction::ScaleRadius) evs.push_back({a.floor, a.val1});
    }
    if (evs.empty()) return;
    std::sort(evs.begin(), evs.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    m_hasScaleRadius = true;
    const int64_t n = (int64_t)tiles.size();
    if (n < 2) return;
    float cur = 1.0f;
    size_t ei = 0;
    double ox = 0.0, oy = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        while (ei < evs.size() && (int64_t)evs[ei].first == i) { cur = evs[ei].second; ++ei; }
        if (i > 0 && cur != 1.0f) {
            const double theta = (double)tiles[(size_t)i - 1].direction * 3.14159265358979323846 / 180.0 + 3.14159265358979323846;
            const double off = 1.0 - (double)cur;
            ox += std::cos(theta) * off;
            oy += std::sin(theta) * off;
        }
        if (i > 0) {
            tiles[(size_t)i].position[0] += ox;
            tiles[(size_t)i].position[1] += oy;
        }
    }
    LOG_D("ScaleRadius: %zu 条事件（偏移已烘进砖位）", evs.size());
}

void WinCacheDeleter::operator()(FloorWindowCache* p) const { delete p; }

LevelData::~LevelData() = default;

bool LevelData::expandAngles() {
    if (!m_angleStore.valid()) return !angleData.empty();   // 本来就是密集数组
    if (angleData.empty()) {
        const int64_t n = m_angleStore.size();
        angleData.resize((size_t)n);
        for (int64_t i = 0; i < n; ++i) angleData[(size_t)i] = m_angleStore.at(i);
    }
    m_angleStore = AngleStore{};                            // 失效：之后 angleAt() 走密集数组
    return !angleData.empty();
}

bool LevelData::setAngles(std::vector<double> angles) {
    m_angleStore = AngleStore{};                            // 整段换掉，位打包下标作废
    angleData = std::move(angles);
    return !angleData.empty();
}

bool LevelData::setAngle(int64_t i, double v) {
    if (!expandAngles()) return false;
    if (i < 0 || (size_t)i >= angleData.size()) return false;
    angleData[(size_t)i] = v;
    return true;
}

bool LevelData::rebuildPositions(std::string* err) {
    if (tilesWindowed()) {
        if (err) *err = "已丢弃密集 tiles（窗口化），不能就地重建几何 —— 只能由容器窗口重算";
        return false;
    }
    if (angleData.empty()) {
        if (err) *err = "angleData 为空：先 expandAngles()（或 setAngles()）";
        return false;
    }
    // 顺序有讲究：位置是角度的纯函数（`calculateTilePositions` 会整段重写 tiles），
    // 派生缓存重建（含 tilePositionOffsets）之后再叠加一次偏移 —— 这样反复调用不会
    // 把 PositionTrack 的偏移叠加两遍。
    calculateTilePositions();
    refreshDerivedCaches();
    applyPositionTrackOffsets();
        applyRadiusScaleOffsets();
    return true;
}

bool LevelData::releaseAngleData(std::string* err) {
    const size_t rawBytes = angleData.size() * sizeof(double);
    std::string e;
    if (!m_angleStore.build(angleData, e)) { if (err) *err = e; return false; }
    // 护栏：小谱 / 取值集很大时，**字典本体**可能比原数组还大（实测 diag_glide：1024 取值
    // → 10 bit/层 + 8 KB 字典 = 9.25 B/层 > 8 B/层）。不划算就原样保留。
    if (m_angleStore.bytes() >= rawBytes) {
        m_angleStore = AngleStore{};
        if (err) *err = "紧凑存储不比原数组小（字典占主导）";
        return false;
    }
    std::vector<double>().swap(angleData);     // 字典 + 位打包已经能逐位还原
    return true;
}

void LevelData::releaseMemory() {
    // Free arrays no longer needed after loading completes
    actions.clear(); actions.shrink_to_fit();
    // ⚠️ 窗口化时**不能**清 `tilePositionOffsets`：provider 物化块要用它合成 PositionTrack
    // （MYC 实测 26,480 条 ≈ 1.6 MB；非窗口化时照旧清掉）
    if (!tilesWindowed()) tilePositionOffsets.clear();
    tileHitsounds.clear();
    tileHitsoundVolumes.clear(); tileHitsoundVolumes.shrink_to_fit();
    // 粘性：这两支清掉之后，`Timeline::getHitsoundTimestampGroups()` 再也算不出正确的分组
    // （类型/音量都会退化成 settings 里的默认值）→ 让那个函数明确报错，而不是静默给错表。
    hitsoundCachesReleased = true;
    // atStates 只有 Timeline::build() 用（它在本函数之前就跑完了），运行时只靠 hasAtStates 判定，
    // 所以实体可以释放：每条 101 B（两个 std::string + 2 float + bool），AnimateTrack 密集的谱上很可观。
    std::unordered_map<int, ATState>().swap(atStates);
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
