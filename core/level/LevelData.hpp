#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <rapidjson/document.h>

#include "core/level/AngleStore.hpp"   // ③ 切片 4：angleData 的紧凑存储
#include "core/level/TileColor.hpp"   // baseColorConfig() / TileColorConfig

// Parsed .adofai level file
// 驻留实现：互斥量与 thread-local 缓存都在 .cpp 文件作用域，按驻留表地址做缓存键；
// LevelData 里因此没有 mutex / thread_local 数据成员（保持可拷贝，MSVC 也接受）。

namespace adofai {

class Timeline;

uint16_t internActionStrIn(std::vector<std::string>& table, const std::string& v);

struct LevelData;
class FloorWindowCache;

// 缓存句柄的删除器：`operator()` 定义在 LevelData.cpp（那里 FloorWindowCache 是完整类型）。
// 这样头里可以安全地持有 `unique_ptr<FloorWindowCache, …>` —— 见下面 m_winCache 的注释。
struct WinCacheDeleter { void operator()(FloorWindowCache* p) const; };

struct LevelData {
    // v7 打包期的"原样成员"捕获（**只在 pack 用**，播放路径一个字节也不多留）：
    //   * actions 里**每一条**事件的原始 JSON 切片（含我们解码不了的 SetFilter/MoveDecorations/…）；
    //   * decorations 整个数组的原始切片。
    // 目的：容器 v7 的 schema 从实际 JSON 自动发现类型/字段，做到"新增事件类型不改格式"。
    // 默认关（false）→ 播放路径的内存与行为完全不变。
    bool preserveRawJson = false;
    std::vector<std::string> rawActionsJson;
    std::string rawDecorationsJson;
    std::string rawSettingsJson;   // settings 对象原样（回写时用它，避免逐键建模）
    // 解析原始成员；失败只警告（pack 会退回旧段），不影响已经解析好的数据。
    bool captureRawMembers(const char* data, size_t length);
    // **v7 容器专用入口**：把一条事件的 JSON 对象喂进来，走与 JSON 路径**同一套**字段映射
    // 与路由（simple → actions；camera/colour/flash/moveTrack/repeat → 各自的富结构）。
    // 这样"容器 v7 成为唯一存储"不需要重写一遍解码逻辑（重写必然漂移）。
    bool ingestEventJson(const char* begin, const char* end, std::string& err);

    // **v7 直接喂字段**（不经 JSON）：容器把列值按"角色"送进来，core 负责映射进
    // `ActionFields` → `buildAction` / `decodeSpecialEvent`，与 JSON 路径共用同一套路由。
    // role 由 `fieldRoleForName()` 在加载时按 (类型,字段) 算一次，热路径只查表。
    enum class FieldRole : uint8_t {
        Unknown = 0,
        // 简单事件（FastAction）
        Floor, EventType, SpeedType, BpmMultiplier, BeatsPerMinute, BeatsBehind, BeatsAhead,
        Duration, Hitsound, HitsoundVolume, TrackAnimation, TrackDisappear,
        AngleOffset, PositionOffset, Scale, Opacity, JustThisTile, EditorOnly,
        // 富事件（相机/颜色/闪屏/轨道动画/重复）
        Position, Rotation, Zoom, RelativeTo, Ease, EventTag, Tag, Id, RepeatType,
        Repetitions, Interval, GapLength, ExecuteOnCurrentFloor, FloorCount,
        StartTile, EndTile, TrackColor, SecondaryTrackColor, TrackColorType, TrackColorPulse,
        TrackStyle, TrackColorAnimDuration, TrackPulseLength, TrackGlowIntensity,
        Plane, StartColor, EndColor, StartOpacity, EndOpacity, FlashStyle, RotationOffset,
        Count_
    };
    struct EventFieldValue {
        FieldRole role = FieldRole::Unknown;
        enum T : uint8_t { Num, Bool, Str, Pair } type = Num;
        double num = 0.0;
        bool b = false;
        const char* str = nullptr;   // Str；Pair 时表示第二分量（如 startTile 的 "ThisTile"）
        uint32_t len = 0;
        double a = 0.0, c = 0.0;     // Pair 的两个数值分量
        uint8_t nullA = 0, nullC = 0;   // 分量是否 null
    };
    static FieldRole fieldRoleForName(const char* name, size_t len);
    bool ingestEventFields(const EventFieldValue* v, size_t n, std::string& err);
    // v7 段（读 `.adocao` v7 时填；写出来给 `adocao json` 回写与"裁剪/丢失"诊断用）
    std::vector<uint8_t> v7SchemaBytes;
    std::vector<uint8_t> v7ColumnsBytes;
    std::vector<uint8_t> v7CoverageBytes;
    // v7 的 EventIndex：每 4096 层一个起点（事件下标），`eventBlockSorted=0` 表示谱面事件不是
    // 按 floor 升序写的（此时索引不可用于二分，只能线性扫）。
    std::vector<uint32_t> eventBlockStart;
    uint32_t eventBlockShift = 12;
    bool eventBlockSorted = false;
    bool v7ColumnsBlocked = false;   // EventColumns 段是分块容器（每 4096 事件一块）
    // **事件窗口**（可选）：在加载前设好，`.adocao` v7 就只解出 `floor ∈ [lo, hi)` 的事件。
    // 用途：2e9 的窗口化加载 / 诊断（WYSIWYG 验证）。未设 = 全量（默认）。
    bool hasEventWindow = false;
    int64_t eventWindowLo = 0, eventWindowHi = 0;
    void setEventWindow(int64_t lo, int64_t hi) { hasEventWindow = true; eventWindowLo = lo; eventWindowHi = hi; }

    struct Settings {
        int    version = 15;
        float  bpm = 100.0f;
        float  offset = 0.0f;        // ms
        int    countdownTicks = 4;
        bool   separateCountdownTime = false;   // 决定时间基准（Re_ADOJAS getTimeOrigin）
        float  zoom = 100.0f;
        float  rotation = 0.0f;
        std::string relativeTo = "Player";
        std::array<float, 2> position = {0.0f, 0.0f};
        std::string songFilename;
        std::string hitsound = "Kick";
        float  hitsoundVolume = 100.0f;
        std::string trackColor = "debb7b";
        std::string secondaryTrackColor = "ffffff";
        // 砖色/样式（Re_ADOJAS TileColorManager 的 settings 默认段）：
        std::string trackColorType = "Single";     // Single/Stripes/Glow/Blink/Switch/Rainbow/Volume
        std::string trackColorPulse = "None";      // None/Forward/Backward
        float  trackColorAnimDuration = 2.0f;      // 秒
        float  trackPulseLength = 10.0f;           // 层
        std::string trackStyle = "Standard";       // Standard/Neon/NeonLight/Basic/Gems/Minimal
        float  trackGlowIntensity = 100.0f;        // 只透传（辉光），不参与 fill/stroke
        std::string backgroundColor = "000000";
        bool   stickToFloors = true;
        std::string planetEase = "Linear";
        std::string trackDisappearAnimation = "None";
        std::string trackAnimation = "None";
        float  beatsBehind = 4.0f;
        float  beatsAhead  = 3.0f;
        // 音高（%）：运镜的事件时长与跟随速度都按 1/(pitch/100) 缩放（docs/camera.md §5.5）。
        // 缺省 100 = 不变调；Re_ADOJAS 用 `Math.max(0.1, pitch/100)` 兜底。
        float  pitch = 100.0f;
        // 老语义：事件没写 relativeTo 时，position 变成"相对当前相机"的**增量**（docs/camera.md §4.7）。
        bool   legacyCamRelativeTo = false;
    };

    // settings 里的默认砖色 → 颜色模型配置（加载后不随事件变的那一段）。
    TileColorConfig baseColorConfig() const;

    // 32 B → 24 B：`index` 从来只被写入（`= i`），没有任何读者 —— 层号就是它在 `tiles` 里的位置。
    struct Tile {
        // 原 `float angle` 无人读（渲染/时间线只用 direction + position），删掉后
        // 结构仍是 24 B（20 B + 对齐填充），但少了一个会误导人的"有角度值"字段。
        float direction = 0.0f;
        std::array<double, 2> position = {0.0, 0.0};
    };
    // 内存预算护栏：1e9 层时每 8 B 就是 8 GB。加字段前先问"这个值能不能推导/能不能窗口化"。
    static_assert(sizeof(Tile) == 24, "Tile 膨胀了：先确认这 8 B/层（1e9 层 = 8 GB）是必要的");

    // ── 相机：MoveCamera / RepeatEvents ────────────────────────────────────
    // 运镜事件**与砖数无关**（一个事件一条，最多几百条），所以存富结构、单独一个 vector，
    // 不塞进 16 B 的 FastAction（那会把 9M action 的 16 B 变成 24 B = 白扔 72 MB）。
    // 字段语义、缺省值、单位见 docs/camera.md §4.1：
    //   position = 砖（TILE_SIZE=1）｜rotation = 度｜zoom = 百分比｜duration = 拍｜angleOffset = 度。
    // "字段没写" 与 "写了 0" 必须能区分 —— 所以有 hasX/hasY/hasRotation/hasZoom 位。
    enum CamMove : uint8_t {
        CamPlayer = 0,
        CamTile,
        CamGlobal,
        CamLastPosition,
        CamLastPositionNoRotation,
        CamUnknown = 0xFE,   // 写了 relativeTo 但不是合法值（→ 沿用上一次模式）
        CamAbsent  = 0xFF,   // 没写 relativeTo
    };
    // 位序是**跨实现契约**：tests/camera_gold.inc 里写的就是这些数值（static_assert 钉住）。
    enum CamFlags : uint8_t {
        // `position` **数组存在**（哪怕两个分量都是 null）。它和 hasX/hasY 是两件事：
        // Re_ADOJAS 用 `Array.isArray(event.position)` 判 positionUsed，`[null,null]` 会走
        // dedup/NaN 归零路径，而"没写 position"不会 —— docs/camera.md §4.3/§4.4。
        kCamHasPosition = 1 << 0,
        kCamHasX        = 1 << 1,
        kCamHasY        = 1 << 2,
        kCamHasRotation = 1 << 3,
        kCamHasZoom     = 1 << 4,
        kCamInactive    = 1 << 5,   // active:false / "Disabled" / disabled.active
        kCamEditorOnly  = 1 << 6,
        kCamEaseWritten = 1 << 7,
    };
    enum CamDisabled : uint8_t {
        kCamDisPosition = 1 << 0,
        kCamDisRotation = 1 << 1,
        kCamDisZoom     = 1 << 2,
        kCamDisRel      = 1 << 3,
    };
    struct CameraEvent {
        int32_t  floor = 0;
        uint32_t id = 0xFFFFFFFFu;   // 缺失 = 排最后（Re_ADOJAS 的 `event.id ?? Infinity`）
        uint32_t order = 0;          // 文件顺序（同 time 同 id 时的最终 tie-break；finishLoad 里赋值）
        float    angleOffset = 0.0f;
        float    duration = 0.0f;
        float    x = 0.0f, y = 0.0f;
        float    rotation = 0.0f;
        float    zoom = 100.0f;
        uint8_t  ease = 0;           // Ease id（core/util/Easing.hpp）；未写/未知 = Linear
        uint8_t  relativeTo = CamAbsent;
        uint8_t  flags = 0;          // CamFlags
        uint8_t  disabled = 0;       // CamDisabled（disabled 字段级过滤）
        std::string tag;             // eventTag（RepeatEvents 按空格拆 tag 匹配）
    };
    struct RepeatEvent {
        int32_t  floor = 0;
        float    repetitions = 0.0f; // Beat: 重复次数；Floor: floorCount
        float    interval = 1.0f;    // 拍（Beat 模式；Re_ADOJAS 用 `interval > 0` 判 Beat）
        float    gapLength = 1.0f;   // 砖（Floor 模式）
        uint8_t  flags = 0;          // bit0 isBeat(repeatType != Floor) / bit1 executeOnCurrentFloor
        std::string tag;             // 原样（空格分隔的多个 tag，展开时拆）
    };

    // ── 砖色：ColorTrack ────────────────────────────────────────────────
    // 与相机事件同理：一条事件一个富结构（最多几百条），不塞进 FastAction。
    // 缺省值按 Re_ADOJAS `createShiftType(event)`：事件没写的字段用**硬编码**默认值
    // （不是 settings —— settings 只提供 level 的默认段）。唯一例外是 trackGlowIntensity：
    // 没写时回落到 settings.trackGlowIntensity，由 kColorHasGlow 标记。
    enum ColorFlags : uint8_t {
        kColorJustThisTile = 1 << 0,
        kColorInactive     = 1 << 1,   // active:false / "Disabled"
        kColorEditorOnly   = 1 << 2,
        kColorHasGlow      = 1 << 3,   // 写了 trackGlowIntensity（否则用 settings）
        kColorIsRecolor    = 1 << 4,   // RecolorTrack（范围 + 触发时间；ColorTrack 是"从此往后"）
    };
    struct ColorEvent {
        TileColorConfig cfg;           // 已把事件缺省值填好（startFloor = floor, startTime = 0）
        int32_t  floor = 0;
        uint32_t order = 0;            // 文件顺序（同 floor 时后者覆盖）
        float    duration = 0.0f;      // TweenColor 时长（秒；0 = 瞬切）
        uint8_t  ease = 0;
        uint8_t  flags = 0;            // ColorFlags
        // ── RecolorTrack（kColorIsRecolor）────────────────────────────
        // startTile/endTile 原样保存：`rel = -1` = 绝对层号；0 ThisTile / 1 Start / 2 End。
        // 解析成绝对范围在 `buildRecolorIndex()`（那时才知道总层数）。
        int32_t  startOff = 0, endOff = 0;
        int8_t   startRel = -1, endRel = -1;
        int32_t  gapLength = 0;
        float    angleOffset = 0.0f;   // 触发时间 = startTime(floor) + angleOffset/180 * 一拍
    };

    // ── MoveTrack（轨道动画：位置/旋转/缩放/不透明度）──────────────────
    // 语义对齐 Re_ADOJAS `buildTileMoveTrack`：每条事件对**指定属性**做一次补间，
    // positionOffset = 相对基础位置（target = base + offset），rotationOffset = **绝对角度**
    // （覆盖砖自身旋转），scale/opacity = 绝对值（%/100）；duration 单位是**拍**；
    // 同属性的旧补间 Kill(complete)（跳到旧目标）再补；末值保持。
    struct MoveTrackEvent {
        int32_t floor = 0;
        float   angleOffset = 0.0f;
        std::string tag;            // eventTag（空格分隔；RepeatEvents 展开用）
        int32_t startOff = 0, endOff = 0;
        int8_t  startRel = -1, endRel = -1;
        int32_t gap = 0;
        int32_t lo = 0, hi = 0;        // resolved
        double  trigger = 0.0;
        float   durBeats = 0.0f;   // 原始（拍）
        double  durSec = 0.0;
        uint8_t ease = 0;
        uint32_t order = 0;
        uint8_t flags = 0;             // bit0 offX / bit1 offY / bit2 rot / bit3 scale / bit4 opacity
        float   offX = 0.0f, offY = 0.0f;
        float   rotDeg = 0.0f;
        float   scaleX = 1.0f, scaleY = 1.0f;
        float   opacity = 1.0f;
    };
    enum MoveFlags : uint8_t {
        kMoveOffX = 1 << 0, kMoveOffY = 1 << 1, kMoveRot = 1 << 2,
        kMoveScale = 1 << 3, kMoveOpacity = 1 << 4,
        kMoveInactive = 1 << 5, kMoveEditorOnly = 1 << 6,
    };
    struct MoveTransform {
        bool any = false;
        float rotDelta = 0.0f;     // 弧度（值 - 砖的基础朝向）
        float scaleX = 1.0f, scaleY = 1.0f;
        float alpha = 1.0f;
        float moveX = 0.0f, moveY = 0.0f;   // 相对基础位置
    };
    void buildMoveTrackIndex(const Timeline& timeline);
    // ScaleRadius（轨道半径缩放）：逐砖继承的 radiusScale，按入口方向累进成位置偏移，
    // 直接烘进 tiles[i].position（必须在 finishLoad 里、位置算完之后）。
    void applyRadiusScaleOffsets();
    bool hasScaleRadiusEvents() const { return m_hasScaleRadius; }
    MoveTransform moveTransformAt(int64_t floor, double timeSec) const;
    float movePad() const { return m_movePad; }
    // 已展开（RepeatEvents 处理完、范围/触发时刻已解析）的 MoveTrack 事件表。
    // 逐砖关键帧时间轴（core/timeline/TileAnimation）复用它建帧 —— 别再自己展开一遍。
    const std::vector<MoveTrackEvent>& moveEvents() const { return m_moveIndex; }

    // ── Flash（全屏闪屏）──────────────────────────────────────────────
    // 语义见 Re_ADOJAS `FlashEffect`：每条事件把某个 plane 从 start 补到 end；
    // duration<=0 或补完后**保持**终值，直到下一次 Flash/Kill。
    struct FlashEvent {
        int32_t floor = 0;
        float duration = 0.0f;      // 原始（拍；buildFlashIndex 换算成 durSec）
        double durSec = 0.0;        // 秒 = duration × 该层一拍 / pitch
        float angleOffset = 0.0f;   // 触发偏移（度）→ trigger = startTime(floor) + angleOffset/180*拍
        float startOpacity = 1.0f;  // 0..1（事件里的 0..100 已归一）
        float endOpacity = 1.0f;
        uint8_t startColor[3] = {255, 255, 255};
        uint8_t endColor[3] = {255, 255, 255};
        uint8_t plane = 0;          // 0 Foreground / 1 Background
        uint8_t style = 0;          // 0 Flash / 1 Kill / 2 StayBlack
        uint8_t ease = 0;
        double trigger = 0.0;       // buildFlashIndex 填
    };
    struct FlashState {
        bool active = false;
        uint8_t color[3] = {0, 0, 0};
        float alpha = 0.0f;
    };
    void buildFlashIndex(const Timeline& timeline);
    // plane: 0 FG / 1 BG；t = 关卡时间（秒）。无事件 → inactive。
    FlashState flashStateAt(uint8_t plane, double timeSec) const;

    // ── RecolorTrack 的运行时索引（buildRecolorIndex() 之后有效）──────────
    // 每 4096 层一个块，块内是该块"可能命中"的事件下标（按 trigger 排序）。
    // 查询 = 块表里取最后一条 trigger<=t 且范围覆盖该层的事件 —— O(块内事件数)。
    struct RecolorTrigger {
        int32_t lo = 0, hi = 0, gap = 0;
        double  trigger = 0.0;    // 秒
        double  beat = 0.0;       // 该事件 floor 的一拍（秒）
        float   angleOffset = 0.0f;  // 度：负值 = 提前触发（-99999 这类极端值 = 从开头就生效）
        float   duration = 0.0f;  // 拍
        uint8_t ease = 0;
        uint32_t order = 0;
        TileColorConfig cfg;
    };
    void buildRecolorIndex(const Timeline& timeline);
    // 逐砖视觉是否随关卡时间变化（颜色事件/MoveTrack；Flash 不算，它不碰砖实例）。
    bool animatesTiles() const { return !colorEvents.empty() || !moveTrackEvents.empty(); }

    Settings settings;
    std::vector<double> angleData;
    std::string        pathData;       // raw pathData string (alternative to angleData)
    std::vector<Tile>  tiles;
    std::vector<CameraEvent> cameraEvents;   // MoveCamera（未展开 RepeatEvents）
    std::vector<RepeatEvent> repeatEvents;   // RepeatEvents（只服务运镜展开；其他事件暂不消费）
    std::vector<ColorEvent> colorEvents;     // ColorTrack / RecolorTrack（未展开；按 floor 查）
    std::vector<FlashEvent> flashEvents;     // Flash（buildFlashIndex 后按 trigger 排序）
    std::vector<MoveTrackEvent> moveTrackEvents;   // MoveTrack（buildMoveTrackIndex 后有效）

    // floor 处生效的颜色事件：
    //   * f 正好是某条 `justThisTile` 事件的层 → 该层**最后一条** justThisTile（覆盖持久段）；
    //   * 否则 floor <= f 的最后一条持久事件；
    //   * 都没有 → nullptr（用 settings 默认段）。
    // inactive / editorOnly 的一律跳过（Re_ADOJAS `isEventActive`）。
    // 注意：这是**线性**查找，给测试/一次性查询用；渲染侧应先建段表（下一步）。
    const ColorEvent* colorEventAt(int64_t floor) const;

    // floor/t 时刻的最终 fill/stroke（含 Volume 幅度）。
    // `timeline` 非空时按 TweenColor 语义处理 `duration`（拍 × 该层 BPM → 秒）：
    // 事件在 `startTimeAt(floor)` 被"跨过"时开始渐变，`start+dur` 后到位；`t < start` 显示上一段目标色。
    // nullptr = 不插值（纯静态色，测试/一次性查询用）。
    TileColors tileColorsAt(int64_t floor, double timeSec, float amplitude = 0.0f,
                            const Timeline* timeline = nullptr) const;

    // 基础色（settings + ColorTrack；不含 RecolorTrack）—— tileColorsAt 的第一步。
    TileColors baseColorsAt(int64_t floor, double timeSec, float amplitude = 0.0f,
                            const Timeline* timeline = nullptr) const;

    // 该砖在 `timeSec` 时刻的 trackStyle（几何档位）。
    // 规则的**另一半**与 tileColorsAt 相同：先取 colorEventAt 的静态段（settings 或 ColorTrack），
    // 再叠加最后一条 `trigger <= timeSec` 且范围覆盖该层的 RecolorTrack 的 cfg.style。
    // 为什么必须走这条：trackStyle **只**出现在 RecolorTrack / ColorTrack 事件里，而
    // colorEventAt 刻意跳过 kColorIsRecolor —— 只用它会让所有砖退化成 Standard，
    // 于是 Neon/Basic 少了"放大半宽 +0.025 / 半长 +0.02"的搭接，接缝直接露背景
    // （2025 全曲 1793 条 Neon 全中招，画面里是一圈白框 + 被邻砖圆角啃掉一角）。
    // `timeline` 非空时用**精确**判据：事件的 trigger 必须早于该砖自己的起始时刻；
    // 为空时退回"每层 1 拍"的估计（只用于测试/无 Timeline 的路径，别当正式语义）。
    std::string trackStyleAt(int64_t floor, double timeSec,
                             const Timeline* timeline = nullptr) const;

    // **样式段**查询（只用于跳过重复计算，不改变语义）。
    struct StyleRun {
        // 样式不再变化的层号（不含）= 安全复用区间 `[floor, end)` 的上界；
        // `INT64_MAX` = 再也不会变。
        int64_t end = std::numeric_limits<int64_t>::max();
        // false = **本块内不可复用**（块内有 RecolorTrack 触发，见 .cpp 的论证），
        // 此时 `end` 只是本块的结束层号，调用方应在 `[floor, end)` 内逐层查询，
        // 且**不要**每层再调本函数（否则有触发的谱反而变慢）。
        bool reusable = true;
    };
    StyleRun trackStyleRunEnd(int64_t floor, double timeSec,
                              const Timeline* timeline = nullptr) const;

    // 某层处生效的**持久段**（忽略 justThisTile / inactive / editorOnly）；没有 → nullptr。
    const ColorEvent* persistentColorAtOrBefore(int64_t floor) const;
    // 严格早于 floor 的最后一条持久段（TweenColor 的 "from" 色）。
    const ColorEvent* persistentColorBefore(int64_t floor) const;

    // Lightweight action (avoids nlohmann DOM allocation for millions of actions)
    // 事件里的字符串（hitsound 名、trackDisappear 动画名）绝大多数是重复的少数几个值，
    // 而又只有这两类事件用到。以前每个 action 内联一个 std::string（32 B）：1.18 GB / 912 万
    // action 的谱面上"结构本身"就是 417 MB，其中 292 MB 是这个缓冲。现在只存 16 位 id，
    // 字符串驻留到 actionStrTable，结构 48 B -> 16 B（417 MB -> 146 MB）。
    struct FastAction {
        int floor = 0;
        uint16_t strId = 0;                 // 0 = 空串；索引进 actionStrTable
        // AnimateTrack 专用：`trackAnimation` 的字符串（strId 存的是 `trackDisappearAnimation`）。
        // 2026-10-21 之前只留了一个 `flag`（"有没有写 trackAnimation"），字符串本身被丢掉 →
        // 逐事件的 appear 类型永远等于全局 settings，跟 Re_ADOJAS 不一致。
        uint16_t strId2 = 0;
        // MoveCamera / RepeatEvents **不留在 actions 里**（keep=false）：它们走 cameraEvents /
        // repeatEvents 两个富结构。这两个枚举值只用于"解析时认出它"。
        enum Type : uint8_t { Twirl, SetSpeed, PositionTrack, SetHitsound, Bookmark, Pause, AnimateTrack, MoveCamera, RepeatEvents, ColorTrack, RecolorTrack, Flash, MoveTrack, Other, ScaleRadius } type = Other;
        bool flag = false;
        float val1 = 0, val2 = 0;
    };
    std::vector<FastAction> actions;
    std::vector<std::string> actionStrTable{std::string()};   // 驻留表（id 0 = 空串）
    // 事件字符串入表：按内容去重。热路径（同一类型重复几百万次）走 thread-local 缓存、不加锁；
    // 未命中才加锁查/插，所以分块并行的 worker 也是安全的。
    uint16_t internActionStr(const std::string& v) { return internActionStrIn(actionStrTable, v); }
    const std::string& actionStr(const FastAction& a) const {
        return a.strId < actionStrTable.size() ? actionStrTable[a.strId] : actionStrTable[0];
    }
    // AnimateTrack 的 `trackAnimation`（strId2）；空串 = 事件没写。
    const std::string& actionStr2(const FastAction& a) const {
        return a.strId2 < actionStrTable.size() ? actionStrTable[a.strId2] : actionStrTable[0];
    }

    // ③ 切片 4（第 1 步）：**运行期读者不要再直接碰 `tiles[i]`** —— `tiles` 现在会按块物化，
    // 这两个访问器就是那层接口。未窗口化时直读密集数组（行为逐位不变）。
    //
    // ⚠️ **按值**返回（不是引用）：物化下一块会替换窗口缓存，引用会失效；按值返回也顺带
    // 让"同时持有两块里的位置"（`PositionSolver` 就同时读 tileIdx 与第 0 层）天然安全。
    // ⚠️ **不能**只写 `tiles.size()`：丢过密集数组之后它是 0，而全运行期（mesh 装配、capture、
    // jump、最近砖搜索）都拿它当"谱面砖数"。窗口化时由 provider 记着那个数。
    int64_t tileCount() const;

    // RecolorTrack 的运行时索引（buildRecolorIndex 填）
    std::vector<RecolorTrigger> m_recolorTriggers;
    std::vector<std::vector<uint32_t>> m_recolorBlocks;
    bool m_recolorReady = false;
    // MoveTrack 运行时索引
    std::vector<MoveTrackEvent> m_moveIndex;
    std::vector<std::vector<uint32_t>> m_moveBlocks;
    bool m_moveReady = false;
    bool m_hasScaleRadius = false;
    float m_movePad = 0.0f;
    float m_moveMaxScale = 1.0f;
    // 可选 `wc`：并行剔除时传**本线程自己的克隆**（见 `FloorWindowCache::clone()`）；
    // 不传 = 用共享缓存（单线程路径，语义与以前逐位相同）。
    std::array<double, 2> tilePositionAt(int64_t i, FloorWindowCache* wc = nullptr) const;
    float tileDirectionAt(int64_t i, FloorWindowCache* wc = nullptr) const;
    // 本线程的私有窗口克隆（首次调用克隆一次，随线程存活；level 换了会自动重建）
    FloorWindowCache* threadWindowCache() const;
    // ③ 切片 4：运行期读角度一律走 `angleAt()` —— 加载收尾会把 `angleData` 换成**位精确紧凑存储**
    // （字典 + 位打包下标；MYC 实测 45 个取值 → 0.75 B/层，54 MB → 4.8 MB）。
    // 加载期（解析 / Timeline::build / computeCheckpoints / TileMesh::build）仍读原数组。
    double angleAt(int64_t i) const {
        if (m_angleStore.valid()) return m_angleStore.at(i);
        return (i >= 0 && (size_t)i < angleData.size()) ? angleData[(size_t)i] : 180.0;
    }
    int64_t angleCount() const {
        return m_angleStore.valid() ? m_angleStore.size() : (int64_t)angleData.size();
    }
    // 把 `angleData` 换紧凑存储（取值集过大则失败、原数组保留）
    bool releaseAngleData(std::string* err = nullptr);
    bool anglesCompacted() const { return m_angleStore.valid(); }
    size_t angleStoreBytes() const { return m_angleStore.bytes(); }
    int64_t angleStoreUnique() const { return m_angleStore.uniqueCount(); }
    int angleStoreBitsPerIndex() const { return m_angleStore.bitsPerIndex(); }
    void setAngleForTest(int64_t i, double v) { m_angleStore.setForTest(i, v); }

    // 建"按块物化"缓存（加载收尾调用；失败就保持密集数组）
    bool buildFloorWindowCache(const std::vector<double>& startTimes,
                               const std::vector<float>& bpms,
                               const std::vector<bool>& isCW, std::string* err = nullptr);
    bool dropTilesArray();          // 丢掉密集 tiles（成功 = 之后走窗口）
    bool tilesWindowed() const;
    size_t windowBlocksMaterialized() const;
    void invalidateFloorWindowForTest();   // 仅测试用（见 FloorWindowCache::invalidateWindowForTest）

    struct TilePositionOffset {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        // 官方语义（对照 Re_ADOJAS 的移植确认）：false = 偏移**继承**到之后所有层；
        // true = **只作用于本层**。消费方见 applyPositionTrackOffsets()。
        bool  justThisTile = false;
    };

    // Per-tile event data (computed from actions)
    //
    // ⚠️ **存活契约**（2026-10-17 补，之前只散在 app/GameWindow.cpp 的注释里）：
    //   `TileMesh` 在**绘制期**（而不是只在 build 期）读这三张表 —— 每砖图标位是剔除时按窗口
    //   现派生的（`TileMesh::deriveRange`），而 provider 合成 SetSpeed 事件也要 `tileBPMs`
    //   （`FloorWindow::computeFloorWindow`）。所以：
    //     * `releaseMemory()` **必须留着它们**（4 B/层 + 2 bit/层），谁清谁负责；
    //     * 窗口化（`dropTilesArray()`）之后 `tileHasTwirl/tileHasSetSpeed` **绝对不能清**：
    //       清了就会派生出 icon=0（实测块 0 有 2148 处不一致、出图 858 个边缘散点）；
    //     * 用 `refreshDerivedCaches()` 重建，别手搓。
    std::vector<float> tileBPMs;      // BPM for each tile (after SetSpeed events)
    std::vector<bool>  tileHasTwirl;  // true if tile has a Twirl event
    std::vector<bool>  tileHasSetSpeed; // true if tile has a SetSpeed event
    std::unordered_map<int, std::string> tileHitsounds;      // per-tile hitsound override (sparse)
    // per-tile hitsound volume override. 稠密：有覆盖时长度 = 层数，"无覆盖"用 NaN 表示；
    // 整张谱没有覆盖时为空 vector。以前是 unordered_map<int,float>：在"每层都有 SetHitsound"
    // 的 audio-as-chart 谱上（实测 TNR 1.18 GB / 915 万层）有 457 万条 ≈ 209 MB，
    // 稠密后只要 37 MB。
    std::vector<float> tileHitsoundVolumes;
    bool hasHitsoundVolume(int floor) const {
        return floor >= 0 && (size_t)floor < tileHitsoundVolumes.size()
               && !std::isnan(tileHitsoundVolumes[(size_t)floor]);
    }
    std::unordered_map<int, TilePositionOffset> tilePositionOffsets; // sparse
    std::vector<int> bookmarkFloors;  // Bookmark event floors

    // AnimateTrack state overrides (sparse, floor → state)
    // 逐事件的 AnimateTrack 状态。**只记"事件写了什么"**，缺省/继承的语义全在
    // `Timeline::build()` 里按 Re_ADOJAS 的规则做（见 TimelineManager.ts:1046-1116）：
    //   * trackAnimation 缺失 → 继承上一条；beatsAhead 缺失 → 沿用上一条
    //   * trackDisappearAnimation 缺失 → **None**（不继承）；beatsBehind 缺失 → 沿用上一条
    // float 用负数表示"事件没写这个字段"。
    struct ATState {
        std::string da, aa;             // 事件原文（da 空 = 没写；aa 空 = 没写）
        float bb = -1.0f, ba = -1.0f;   // <0 = 没写
        bool hasAA = false;             // 写了 trackAnimation（flag2 的判据）
        bool hasDA = false;             // 写了 trackDisappearAnimation
    };
    std::unordered_map<int, ATState> atStates;
    // 粘性标志：releaseMemory() 会清掉 atStates 的实体（每条 101 B，巨谱上是几十 MB~几百 MB），
    // 但 Timeline 与 LevelScene 都要按"有没有 AnimateTrack"来判定 TrackVis —— 那个判定发生在
    // 释放之后，所以不能再看 .empty()。三处判据（Timeline.cpp、app/LevelScene.cpp、
    // tests/level_parse_test.cpp 的自检）必须一起用它。
    bool hasAtStates = false;
    // 粘性标志（同 hasAtStates 的理由）：`releaseMemory()` 会清掉每砖 hitsound 覆盖与音量
    // （它们只服务加载期的打拍音合成）。清完之后 `Timeline::getHitsoundTimestampGroups()`
    // 再算就是**静默错值**（类型退化成 settings.hitsound、音量退化成 settings.hitsoundVolume），
    // 所以用这个标志让它**明确报错**而不是给一份看起来正常的错表。
    bool hitsoundCachesReleased = false;

    // ---- 所有权与拷贝/移动（2026-10-17 补）----
    // 这里原来只有析构、没有禁拷贝：`LevelData a; LevelData b = a;` 会让两个对象各持同一个
    // `m_winCache`，析构时 **double free**（缓存自己删了拷贝，但裸指针绕过了那层保护）。
    // 现在缓存是 move-only 的 unique_ptr（自定义删除器，见下），于是拷贝自动禁用、移动安全。
    LevelData() = default;
    ~LevelData();   // 定义在 LevelData.cpp（FloorWindowCache 在那里才是完整类型）
    LevelData(const LevelData&) = delete;
    LevelData& operator=(const LevelData&) = delete;
    LevelData(LevelData&&) = default;              // unique_ptr 的 move 会把源置空 ✓
    LevelData& operator=(LevelData&&) = default;
    void releaseMemory();  // free data no longer needed after loading
    // ③ 切片 4：按块物化缓存（未建/未窗口化时为 null，访问器走密集数组）。
    // 用 unique_ptr + **自定义删除器**（`WinCacheDeleter::operator()` 在 .cpp 里定义）：
    // libc++ 里 `unique_ptr<不完整类型>` 的默认删除器只要被本 TU 构造/析构就会实例化
    // "delete an incomplete type"（原来因此写成裸指针），而 `LevelData.hpp` 不能包含
    // `FloorWindow.hpp`（后者要 LevelData 的嵌套类型）。自定义删除器的 `operator()`
    // 只声明不定义，就没有那个实例化问题 —— 同时白拿 move-only 语义。
    std::unique_ptr<FloorWindowCache, WinCacheDeleter> m_winCache;
    AngleStore m_angleStore;   // ③ 切片 4：`angleData` 的紧凑存储（releaseAngleData 之后生效）

    using ProgressCb = std::function<void(float pct, const char* stage)>;

    // ---- 派生病重与写回（ADOCAO-E 编辑器要用的公开入口；2026-10-17）----
    // 幂等地从 `actions` 重建**全部每砖派生缓存**：tileBPMs / tileHasTwirl / tileHasSetSpeed /
    // tileHitsounds / tileHitsoundVolumes / tilePositionOffsets / bookmarkFloors / atStates。
    // 每次调用都先清空再重建，所以可以反复调（加载收尾就是调它一次）；**不碰 `tiles`**。
    // 层数取自 `angleCount()` → `releaseAngleData()` 之后也能用。
    void refreshDerivedCaches();

    // 角度写回。`releaseAngleData()` 之后角度只剩位打包下标（AngleStore 只允许改成字典里
    // **已有**的取值），所以编辑任意角度前要先 `expandAngles()` 回到密集数组。
    // 改完谁改了谁负责重建：`refreshDerivedCaches()`（事件派生）+ `rebuildPositions()`（几何）。
    bool expandAngles();                          // 紧凑 → 密集；已是密集时无操作
    bool setAngles(std::vector<double> angles);   // 整体替换（顺带展开）
    bool setAngle(int64_t i, double v);           // 单点替换（需要时自动展开）
    // 从当前 `angleData` 重建 `tiles` 的位置/方向 + PositionTrack 偏移。
    // 窗口化（`dropTilesArray()` 之后）不可用 —— 那时几何只能由容器窗口重算。
    bool rebuildPositions(std::string* err = nullptr);

    bool loadFromFile(const std::string& filepath, ProgressCb onProgress = nullptr, bool exportOnly = false);
    bool loadFromString(const std::string& jsonStr, ProgressCb onProgress = nullptr, bool exportOnly = false);
    // Same thing on a raw buffer (used by loadFromFile's mmap and by tests)
    bool loadFromBuffer(const char* data, size_t length, ProgressCb onProgress = nullptr, bool exportOnly = false);

private:
    // Fast path: scan angleData/actions straight out of the buffer, reproducing what
    // cleanJson() does to the file. Returns false (and leaves the object untouched) as
    // soon as it meets something it cannot reproduce, so the caller can fall back to
    // parseLegacy(). Set ADOCAO_FORCE_DOM_PARSE=1 to force the legacy path (A/B tests).
    bool tryFastParse(const char* data, size_t length, ProgressCb onProgress);
    // Legacy path: cleanJson + RapidJSON DOM. Also the fallback and the A/B reference.
    // 真正的加载实现（幂等重置 + 三条解析路径）；public 入口负责 v7 的原始成员捕获。
    bool loadFromBufferImpl(const char* data, size_t length, ProgressCb onProgress, bool exportOnly);
    bool parseLegacy(const std::string& cleanedJson, ProgressCb onProgress, bool exportOnly);
    // Shared tail: pathData → angleData, tile positions, actions, position offsets
    bool finishLoad(ProgressCb onProgress, bool exportOnly);

    void calculateTilePositions();
    void convertPathToAngles();
    void applyPositionTrackOffsets();
    static float pathCharToAngle(char c);
};

}  // namespace adofai
