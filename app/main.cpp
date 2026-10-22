#include "app/HitsoundTypes.hpp"
#include "core/util/Progress.hpp"
#include "core/level/LevelData.hpp"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "archive/AdocaoEventColumns.hpp"
#include <set>

#include "archive/AdocaoJsonWriter.hpp"
#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoSchema.hpp"
#include "archive/AdocaoWriter.hpp"     // `adocao pack`：把已加载的谱打成 .adocao
#include "archive/AdocaoDiagnostics.hpp"  // `--codec-report`：§8.4 的判据统计（纯诊断）
#include <fstream>
#include "core/level/LevelPath.hpp"
#include "Application.hpp"
#include "app/MapExport.hpp"
#include "LauncherWindow.hpp"
#include "core/util/Logger.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
// 这两个必须在条件块之外：插进 `#ifndef _WIN32` 里的话，macOS/Linux 编得过、
// Windows 直接 "not declared"（CI 真踩过一次）。
#include "archive/Install.hpp"
#include "app/AssetSetup.hpp"
#ifdef _WIN32
#include <io.h>        // _isatty / _fileno
#else
#include <unistd.h>
#endif



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

// 子命令：不带 = 播放/向导（这一条语法**不变**，外部 launcher 就靠它）。
// 无头那一族（今晚才加、没有外部用户）折进子命令，于是每个模式一屏帮助、
// 且模式内可以把 --map- 前缀去掉（--map-size → map --size）。
enum class Mode { Play, Image, Tiles, Stitch, Export, Pack, Json };

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Image:  return "image";
        case Mode::Tiles:  return "tiles";
        case Mode::Stitch: return "stitch";
        case Mode::Export: return "export";
        case Mode::Pack:   return "pack";
        case Mode::Json:   return "json";
        default:           return "";
    }
}

// ── 帮助 ─────────────────────────────────────────────────────────────────────
// 每组一屏；开发开关与环境变量钩子只在 --help --all 里出现。
void printHelp(Mode mode, bool all) {
    switch (mode) {
    case Mode::Pack:
        std::fprintf(stderr,
            "adocao pack <谱> <输出.adocao> [选项]      把谱面打成 .adocao（开发/格式验证工具）\n"
            "\n"
            "  列式存储：每列按取值基数选编码（字典/常量/差分/位打包/原始），**逐位无损**。\n"
            "  只存输入（角度 + 事件）；砖位置/朝向/BPM/时间线都由输入重算，不占文件。\n"
            "\n"
            "  --codec-report            打印每列的编码、元素数、朴素/编码后字节、字典项数、位宽\n"
            "  --checkpoints             额外写 Derived 段（每 4096 层的续算快照，默认关）\n"
            "  --v7                      写 v7 事件库段（自描述 schema + 列；v6 会丢的类型也能无损装下）\n"
            "  --v7-all                  同上但**不去重**（全部事件进 v7，供 `adocao json` 无损回写）\n"
            "  --v7-only                 **v7 唯一存储**：不写 v6 的 Actions/Camera/… 段（读侧从 v7 还原）\n"
            "  --compress=auto|zstd|lzma|none  段级压缩器（auto = 两者都试、取更小；默认 auto）\n"
            "  --level=N                 压缩级别（0 = 编码器默认：zstd 19 / lzma 9|EXTREME；取 0..22）\n"
            "  --schema-report           打印 v7 自描述 schema（类型/字段/kind 分布）与事件列体积\n"
            "  --events=all|playable|minimal  裁剪导出：playable = v5 能力面，minimal = 只留影响几何/计时的\n"
            "\n"
            "  例：adocao pack \"<谱>\" out.adocao --codec-report\n"
            "      adocao pack \"<谱>\" out.adocao --schema-report --events=playable\n");
        return;
    case Mode::Json:
        std::fprintf(stderr,
            "adocao json <in.adocao> <out.adofai>     把 v7 容器回写成明文 .adofai\n"
            "\n"
            "  需要文件里有 v7 事件库段（`pack --v7-all` 打的包）；只搬运 v7 段与 settings 原文，\n"
            "  不做任何语义改写。旧格式（没有 v7 段）会**明确报错**，不输出残缺谱面。\n");
        return;
    case Mode::Image:
        std::fprintf(stderr,
            "adocao image <out.png> --level <谱> [选项]      导出一张图，一条命令\n"
            "\n"
            "  默认走**矢量全景**（真无头、不碰 GL、5 秒出 16K）；加 --1px 走 1px=1tile 瓦片路线。\n"
            "\n"
            "  尺寸 / 路线:\n"
            "    --size WxH           矢量画布（默认 4096x4096；16K 用 16384x6616）\n"
            "    --1px                1 层 = 1 像素（内部自己出瓦片→拼接；巨谱要跑几小时）\n"
            "    --scale N            仅 --1px：块内取**最亮**降采样（细线不消失，但会块状量化）\n"
            "    --1bit               仅**单色谱**：写成 1-bit 索引色（体积 1/32；颜色多于两种会塌）\n"
            "    --keep-tiles <dir>   保留中间瓦片（默认临时目录，成功后自动删除）\n"
            "  外观:\n"
            "    --time-color         按谱面时间六档彩虹（否则沿进度渐变）\n"
            "    --bg <hex>|transparent   --padding F（默认 0 = 贴边；给正数才留白）   --thickness N（<=1 = 1px 硬线）\n"
            "    （--size/--bg/--padding/--thickness/--native 只对矢量路线生效；--1px 不读取它们）\n"
            "    --range A-B          只画第 A..B 层（看密集“结”）   --native  1 像素 = 1 世界单位\n"
            "  资源:\n"
            "    --threads N          线程数（默认 4 = 2P+2E；stitch/mono 用）\n"
            "\n"
            "  例：adocao image ~/16k.png --level \"<谱>\" --size 16384x6616 --time-color --thickness 1\n");
        return;
    case Mode::Tiles:
        std::fprintf(stderr,
            "adocao tiles <dir> --level <谱> [选项]          1px=1tile 瓦片（只写有墨的块）\n"
            "\n"
            "  --block N           分块边长（默认 4096）\n"
            "  --threads N         线程数（默认硬件并发；stitch 默认 4 = 2P+2E）\n"
            "  --time-color        瓦片按谱面时间彩虹着色\n"
            "\n"
            "  瓦片名自带绝对坐标 x%%07lld_y%%07lld_w%%lld_h%%lld.png，拼接时不需要清单。\n"
            "  衔接：adocao stitch <dir> <out.png>（1-bit 单色请用 adocao image --1px --1bit）\n");
        return;
    case Mode::Stitch:
        std::fprintf(stderr,
            "adocao stitch <dir> <out.png> [选项]            把 tiles 的瓦片拼成一张 PNG\n"
            "\n"
            "  --scale N           N>1 整数盒式降采样，取块内**最亮**（细线不会消失，但会块状\n"
            "                      量化 → 高倍看是阶梯）；N=1 走并行拼接\n"
            "  --threads N         线程数（默认 4 = 2P+2E，用 macOS QoS 分核）\n"
            "\n"
            "  测试钩子：ADOCAO_STITCH_Y0 / _ROWS 只输出源行区间（用于与串行路径逐行对拍）、\n"
            "            ADOCAO_STITCH_SEQ=1 强制旧的串行流式路径。\n");
        return;
    case Mode::Export:
        std::fprintf(stderr,
            "adocao export --level <谱>                      导出该谱的 hitsound 混音 WAV\n"
            "\n"
            "  以前是 `--export` 开关，现在只有这一个入口。\n"
            "  强制 hitsound 类型：--force-hitsound [TYPE]；raw-pcm = 把每层音量当 PCM 直通。\n");
        return;
    default: break;
    }

    std::fprintf(stderr,
        "ADOCAO — A Dance of Fire and Ice 谱面查看器\n"
        "\n"
        "用法:\n"
        "  adocao                                   打开向导\n"
        "  adocao <谱.adofai> [选项]                 直接播放\n"
        "  adocao export --level <谱>                导出 hitsound WAV\n"
        "  adocao json <in.adocao> <out.adofai>       把 v7 容器回写成明文（无损）\n"
        "  adocao image  <out.png>    --level <谱>   导出一张图（默认矢量；--1px 瓦片路线）\n"
        "  adocao tiles  <dir>        --level <谱>   只出瓦片（进阶）\n"
        "  adocao stitch <dir> <out.png>             只拼瓦片（进阶）\n"
        "\n"
        "播放选项:\n"
        "  --level <file>  --music <file>  --width N  --height N  --fullscreen\n"
        "  --fill <hex>  --stroke <hex>  --bg <hex>  --no-auto-stroke\n"
        "  --msaa N                                 抗锯齿采样数（0/2/4/8）\n"
        "  --windowed | --borderless                窗口 / 无边框全屏（默认 Borderless；= osu! 的 Borderless 语义）\n"
        "  --no-hitsound  --force-hitsound [TYPE]  --auto-play  --no-trail\n"
        "  --show-all-tiles                          忽略 appear/disappear，显示全部砖（= 编辑器全图视角，诊断/对拍用）\n"
        "  --capture-fit-all                         抓帧时相机包住全部砖（复现编辑器全图构图）\n"
        "  --start-at SEC            播放从该时刻开始（等价于选砖后按空格；含音乐 seek）\n"
        "  --cull-block N            剔除的粗块粒度（层/块，默认 4096；0 = 关闭索引走全扫）\n"
        "  --trail-duration SEC | --trail-tiles N   拖尾长度（二选一，默认关）\n"
        "  --trail-sample-rate N | --trail-target-fps N            固定 / 自适应采样率\n"
        "  --trail-samples-per-tile N  --trail-rate-min N  --trail-rate-max N\n"
        "  --trail-sagitta PX        按每一采样点的折线偏离不超过 PX 像素反推采样率（随 zoom 自适应；0=关）\n"
        "\n"
        "全局:\n"
        "  --progress | --no-progress               进度（默认：stderr 是终端才显示）\n"
        "  -h, --help [--all]                       这份帮助 / 含开发选项与环境变量钩子\n");
    if (all) {
        std::fprintf(stderr,
            "\n开发:\n"
            "  --debug                                  调试控制台 + 默认关 hitsounds\n"
            "  --capture <out.png> [--capture-time SEC | --capture-tile N] [--capture-zoom Z]\n"
            "            [--capture-camera]   # 抓帧也走关卡相机（settings + MoveCamera；此时 zoom 由相机决定）\n"
            "                                           确定性抓一帧写 PNG 后退出（像素 diff 用）\n"
            "\n环境变量（测试/调参钩子，正常不用）:\n"
            "  ADOCAO_MIX_THREADS  ADOCAO_FORCE_DOM_PARSE  ADOCAO_PARSE_PIECES  ADOCAO_WINDOW_KB\n"
            "  ADOCAO_WINDOW_REQUIRE  ADOCAO_FAST_REQUIRE  ADOCAO_WHOLE_DECOMPRESS  ADOCAO_STITCH_*\n");
    } else {
        std::fprintf(stderr, "\n（--help --all 另列开发开关）\n");
    }
}

// 关了子命令之后旧开关会被判"未知" —— 直接告诉他新写法，别让他去猜。
bool migrationHint(const char* flag) {
    struct { const char* old; const char* now; } kMap[] = {
        { "--export",          "adocao export --level <谱>" },
        { "--map",             "adocao image <out.png> --level <谱>" },
        { "--map-size",        "adocao image ... --size WxH" },
        { "--map-bg",          "adocao image ... --bg <hex>|transparent" },
        { "--map-time-color",  "adocao image ... --time-color（tiles 也有）" },
        { "--map-padding",     "adocao image ... --padding F" },
        { "--map-thickness",   "adocao image ... --thickness N" },
        { "--map-tiles",       "adocao image ... --range A-B" },
        { "--map-native",      "adocao image ... --native" },
        { "--map-native-all",  "adocao image ... --1px（或 adocao tiles <dir>）" },
        { "--map-block",       "adocao image ... --block N" },
        { "--map-threads",     "--threads N（image/tiles/stitch 都接）" },
        { "--map-stitch",      "adocao stitch <dir> <out.png>" },
        { "--map-stitch-scale","adocao stitch ... --scale N" },
        { "--map-mono",        "adocao image ... --1px --1bit" },
    };
    for (const auto& e : kMap) {
        if (std::strcmp(flag, e.old) == 0) {
            std::fprintf(stderr, "  新写法: %s\n", e.now);
            return true;
        }
    }
    return false;
}

int usageErr(Mode m, const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    if (m == Mode::Play) std::fprintf(stderr, "用 `adocao --help` 看用法。\n");
    else                 std::fprintf(stderr, "用 `adocao %s --help` 看用法。\n", modeName(m));
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    // 压缩容器（.adofai.xz/.zst）的解压实现：core 只认接口，这里把 archive 模块注册进去。
    // 必须在任何 loadFromFile 之前 —— GUI 与无头子命令（image/tiles/stitch/export）都走这里。
    adofai::archive::install();
    // 资产布局（zip 名 / data 目录 / 搜索根 / hitsounds 目录）：GUI、CLI、无头三条路都要，
    // 所以放在 main 顶部一次配好（以前只在 runApplication 里配，走 --level 的 CLI 路径漏了）。
    configureAssetPaths();
    bool debug = false;
    Mode mode = Mode::Play;
    int argStart = 1;

    if (argc > 1) {                                  // 子命令识别（只认第一参数）
        const char* a = argv[1];
        if      (std::strcmp(a, "image") == 0)  { mode = Mode::Image;  argStart = 2; }
        else if (std::strcmp(a, "tiles") == 0)  { mode = Mode::Tiles;  argStart = 2; }
        else if (std::strcmp(a, "stitch") == 0) { mode = Mode::Stitch; argStart = 2; }
        else if (std::strcmp(a, "export") == 0) { mode = Mode::Export; argStart = 2; }
        else if (std::strcmp(a, "pack") == 0)   { mode = Mode::Pack;   argStart = 2; }
        else if (std::strcmp(a, "json") == 0)   { mode = Mode::Json;   argStart = 2; }
        else if (std::strcmp(a, "map") == 0 || std::strcmp(a, "mono") == 0) {
            std::fprintf(stderr, "`%s` 已并入 image：\n  adocao image <out.png> --level <谱>%s\n",
                         a, std::strcmp(a, "mono") == 0 ? " --1px --1bit" : "");
            return 2;
        }
    }

    LauncherConfig cli;
    std::vector<std::string> pos;                    // 子命令的位置参数（out.png / dir …）
    // 无头族选项
    std::string size, bg, range, keepTiles;
    bool native = false, timeColor = false, onePx = false, oneBit = false;
    float padding = -1.0f, thickness = -1.0f;        // <0 = 用默认
    int block = 4096, threads = 0, scale = 1, progressForce = 0;
    bool codecReport = false;
    bool packCheckpoints = false;
    bool packSchemaReport = false;   // pack --schema-report：打印 v7 自描述 schema 的统计
    bool packV7 = false;             // pack --v7：写 v7 事件库段（EventSchema/EventColumns/Coverage）
    bool packV7All = false;          // pack --v7-all：不去重（`adocao json` 回写需要全部事件）
    bool packV7Only = false;         // pack --v7-only：v7 唯一存储（不写 v6 事件段）
    std::string packEventsPreset = "all";   // pack --events=all|playable|minimal（裁剪导出）
    adocao::Compress packCompress = adocao::Compress::Auto;   // pack --compress=auto|zstd|lzma|none
    int packLevel = 0;                                        // pack --level=N（0 = 编码器默认）   // pack --checkpoints：写 Derived 段（每 4096 层的续算快照）                    // pack：打印每列的体积明细

    // CLI-FLAGS-BEGIN —— scripts/check-cli-help.sh 只在这个区间里找开关名
    for (int i = argStart; i < argc; i++) {
        const char* a = argv[i];
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            bool all = false;                        // --help --all 才列开发开关
            for (int j = i + 1; j < argc; ++j) if (std::strcmp(argv[j], "--all") == 0) all = true;
            printHelp(mode, all);
            return 0;                                // 在任何 GL 初始化之前返回
        }
        else if (std::strcmp(a, "--progress") == 0)    progressForce = 1;
        else if (std::strcmp(a, "--no-progress") == 0) progressForce = -1;
        else if (a[0] != '-')                          { pos.push_back(a); continue; }

        // ── 播放 / 导出（语法与老版本完全一致；--export 也仍然可用）
        else if (mode == Mode::Play || mode == Mode::Export) {
                 if (std::strcmp(a, "--debug") == 0)          debug = true;
            else if (std::strcmp(a, "--level") == 0     && i+1<argc) cli.levelPath = resolveLevelPath(argv[++i]);
            else if (std::strcmp(a, "--music") == 0     && i+1<argc) cli.musicPath = argv[++i];
            else if (std::strcmp(a, "--width") == 0     && i+1<argc) cli.resolutionW = atoi(argv[++i]);
            else if (std::strcmp(a, "--height") == 0    && i+1<argc) cli.resolutionH = atoi(argv[++i]);
            else if (std::strcmp(a, "--fullscreen") == 0)           { cli.fullscreen = true; cli.windowMode = 1; }
            else if (std::strcmp(a, "--windowed") == 0)             { cli.fullscreen = false; cli.windowMode = 0; }
            else if (std::strcmp(a, "--borderless") == 0)           { cli.fullscreen = true;  cli.windowMode = 1; }
            else if (std::strcmp(a, "--fill") == 0      && i+1<argc) { cli.trackFillColor = argv[++i]; cli.trackColorOverride = true; }
            else if (std::strcmp(a, "--stroke") == 0    && i+1<argc) { cli.trackStrokeColor = argv[++i]; cli.trackColorOverride = true; }
            else if (std::strcmp(a, "--bg") == 0        && i+1<argc) { cli.backgroundColor = argv[++i]; cli.bgExplicit = true; }
            else if (std::strcmp(a, "--no-auto-stroke") == 0)        cli.autoStroke = false;
            else if (std::strcmp(a, "--no-hitsound") == 0)           cli.enableHitsounds = false;
            else if (std::strcmp(a, "--force-hitsound") == 0) {
                if (i+1 < argc && argv[i+1][0] != '-') {
                    cli.forceHitsoundType = argv[++i];
                    // 类型表与向导共用（app/HitsoundTypes.hpp），避免两份清单漂移
                    bool ok = false;
                    for (const char* t : kHitsoundTypes) if (cli.forceHitsoundType == t) { ok = true; break; }
                    if (!ok) { LOG_W("Unknown hitsound type '%s', defaulting to Kick", cli.forceHitsoundType.c_str()); cli.forceHitsoundType = "Kick"; }
                } else {
                    cli.forceHitsoundType = "Kick";
                }
            }
            else if (std::strcmp(a, "--auto-play") == 0)             cli.autoPlay = true;
            else if (std::strcmp(a, "--start-at") == 0 && i+1<argc)    cli.startAt = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--msaa") == 0 && i+1<argc)      cli.msaaSamples = atoi(argv[++i]);
            else if (std::strcmp(a, "--no-trail") == 0)              cli.showTrail = false;
            else if (std::strcmp(a, "--show-all-tiles") == 0)        cli.showAllTiles = true;
            else if (std::strcmp(a, "--capture-fit-all") == 0)       cli.captureFitAll = true;
            else if (std::strcmp(a, "--cull-block") == 0 && i+1<argc) cli.cullBlockFloors = atoi(argv[++i]);
            else if (std::strcmp(a, "--capture") == 0 && i+1<argc)      cli.capturePath = argv[++i];
            else if (std::strcmp(a, "--capture-time") == 0 && i+1<argc) cli.captureTime = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--capture-tile") == 0 && i+1<argc) {
                // 2^31−1 层目标 #0：`atoi` 溢出是 UB（层号 > 2^31−1 会被静默算错），
                // 所以显式解析 + 范围检查 + 明确报错。
                char* endp = nullptr;
                const long long v = std::strtoll(argv[++i], &endp, 10);
                if (!endp || endp == argv[i] || *endp != '\0' || v < 0 || v > 2147483647LL) {
                    std::fprintf(stderr,
                                 "--capture-tile 需要一个 0..2147483647 的整数（给的是 '%s'）\n",
                                 argv[i]);
                    std::exit(2);
                }
                cli.captureTile = (int)v;
            }
            else if (std::strcmp(a, "--capture-zoom") == 0 && i+1<argc) cli.captureZoom = (float)atof(argv[++i]);
            // 抓帧也走关卡相机（settings + MoveCamera 时间线）；默认关，保住既有像素基线的口径
            else if (std::strcmp(a, "--capture-camera") == 0) cli.captureCamera = true;
            else if (std::strcmp(a, "--trail-duration") == 0 && i+1<argc) cli.trailDuration = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--trail-sample-rate") == 0 && i+1<argc) { cli.trailSampleRate = (float)atof(argv[++i]); cli.trailAdaptive = false; }
            else if (std::strcmp(a, "--trail-target-fps") == 0 && i+1<argc) { cli.trailTargetFps = (float)atof(argv[++i]); cli.trailAdaptive = true; }
            else if (std::strcmp(a, "--trail-rate-min") == 0 && i+1<argc) cli.trailRateMin = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--trail-rate-max") == 0 && i+1<argc) cli.trailRateMax = (float)atof(argv[++i]);
            // 采样率随"每秒走过的轨道"放大（步进 + 该层相对角扫过的弧），取代固定 Hz
            else if (std::strcmp(a, "--trail-samples-per-tile") == 0 && i+1<argc) { cli.trailSamplesPerTile = (float)atof(argv[++i]); cli.trailPerTile = true; }
            // 拖尾按"层数"而不是秒数计量（0.4s 在 BPM 1.5M 的谱上是几万层）
            else if (std::strcmp(a, "--trail-tiles") == 0 && i+1<argc) { cli.trailTiles = (float)atof(argv[++i]); cli.trailLengthInTiles = true; }
            else if (std::strcmp(a, "--trail-sagitta") == 0 && i+1<argc) { cli.trailSagitta = (float)atof(argv[++i]); }
            else goto unknown;
        }
        // ── adocao image <out.png> --level <谱> [--1px [--scale N] [--1bit]]
        else if (mode == Mode::Pack) {
            if (std::strcmp(a, "--codec-report") == 0) codecReport = true;
            else if (std::strcmp(a, "--checkpoints") == 0) packCheckpoints = true;
            else if (std::strcmp(a, "--schema-report") == 0) packSchemaReport = true;
            else if (std::strcmp(a, "--v7") == 0) packV7 = true;
            else if (std::strcmp(a, "--v7-all") == 0) { packV7 = true; packV7All = true; }
            else if (std::strcmp(a, "--v7-only") == 0) { packV7 = true; packV7Only = true; }
            else if (std::strncmp(a, "--events=", 9) == 0) packEventsPreset = a + 9;
            else if (std::strncmp(a, "--compress=", 11) == 0) {
                const char* v = a + 11;
                if (!std::strcmp(v, "auto")) packCompress = adocao::Compress::Auto;
                else if (!std::strcmp(v, "zstd")) packCompress = adocao::Compress::Zstd;
                else if (!std::strcmp(v, "lzma") || !std::strcmp(v, "xz")) packCompress = adocao::Compress::Lzma;
                else if (!std::strcmp(v, "none") || !std::strcmp(v, "raw")) packCompress = adocao::Compress::None;
                else { std::fprintf(stderr, "--compress 只认 auto|zstd|lzma|none\n"); return 2; }
            }
            else if (std::strcmp(a, "--level") == 0 && i+1 < argc) {
                packLevel = std::atoi(argv[++i]);
                if (packLevel < 0 || packLevel > 22) { std::fprintf(stderr, "--level 取 0..22\n"); return 2; }
            }
            else { std::fprintf(stderr, "未知选项: %s（pack 模式）\n", a); return 2; }
        }
        else if (mode == Mode::Image) {
                 if (std::strcmp(a, "--level") == 0      && i+1<argc) cli.levelPath = resolveLevelPath(argv[++i]);
            else if (std::strcmp(a, "--size") == 0       && i+1<argc) size = argv[++i];
            else if (std::strcmp(a, "--bg") == 0         && i+1<argc) bg = argv[++i];
            else if (std::strcmp(a, "--time-color") == 0)             timeColor = true;
            else if (std::strcmp(a, "--padding") == 0    && i+1<argc) padding = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--thickness") == 0  && i+1<argc) thickness = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--range") == 0      && i+1<argc) range = argv[++i];
            else if (std::strcmp(a, "--native") == 0)                 native = true;
            else if (std::strcmp(a, "--1px") == 0)                    onePx = true;
            else if (std::strcmp(a, "--1bit") == 0)                   oneBit = true;   // 隐含 --1px
            else if (std::strcmp(a, "--scale") == 0      && i+1<argc) scale = atoi(argv[++i]);
            else if (std::strcmp(a, "--block") == 0      && i+1<argc) block = atoi(argv[++i]);
            else if (std::strcmp(a, "--threads") == 0    && i+1<argc) threads = atoi(argv[++i]);
            else if (std::strcmp(a, "--keep-tiles") == 0 && i+1<argc) keepTiles = argv[++i];
            else goto unknown;
        }
        // ── adocao tiles <dir> --level <谱>
        else if (mode == Mode::Tiles) {
                 if (std::strcmp(a, "--level") == 0 && i+1<argc) cli.levelPath = resolveLevelPath(argv[++i]);
            else if (std::strcmp(a, "--block") == 0 && i+1<argc) block = atoi(argv[++i]);
            else if (std::strcmp(a, "--threads") == 0 && i+1<argc) threads = atoi(argv[++i]);
            else if (std::strcmp(a, "--time-color") == 0) timeColor = true;
            else goto unknown;
        }
        // ── adocao stitch <dir> <out.png> / adocao mono <dir> <out.png>
        else if (mode == Mode::Stitch) {
                 if (std::strcmp(a, "--scale") == 0 && i+1<argc) scale = atoi(argv[++i]);
            else if (std::strcmp(a, "--threads") == 0 && i+1<argc) threads = atoi(argv[++i]);
            else goto unknown;
        }
        else goto unknown;

        continue;
    unknown:
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') continue;   // 单独的 "--" 按约定忽略
        if (a[0] == '-' && a[1] == '-' && a[2] != '\0') {
            std::fprintf(stderr, "未知选项: %s（%s 模式）\n", a, mode == Mode::Play ? "播放" : modeName(mode));
            if (!migrationHint(a)) {
                if (mode == Mode::Play) std::fprintf(stderr, "用 `adocao --help` 看全部开关。\n");
                else std::fprintf(stderr, "用 `adocao %s --help` 看该子命令的开关。\n", modeName(mode));
            }
            return 2;
        }
        std::fprintf(stderr, "无法解析的参数: %s\n", a);
        return 2;
    }
    if (cli.resolutionW <= 0 || cli.resolutionH <= 0) {
        std::fprintf(stderr, "--width/--height 必须是正整数（收到 %dx%d）\n", cli.resolutionW, cli.resolutionH);
        return 2;
    }
    // CLI-FLAGS-END

    {   // 默认：stderr 是终端就显示；--progress 强制显示（管道/后台也能看）
        bool tty = false;
#ifdef _WIN32
        tty = _isatty(_fileno(stderr)) != 0;
#else
        tty = isatty(fileno(stderr)) != 0;
#endif
        progress::enable(progressForce > 0 || (progressForce == 0 && tty));
    }

    // adocao json <in.adocao> <out.adofai>：把 v7 容器回写成明文（ADOCAO-E 无损回写的基础）
    if (mode == Mode::Json) {
        if (pos.size() < 2) return usageErr(mode, "adocao json 需要 <in.adocao> <out.adofai>");
        LevelData lv;
        if (!lv.loadFromFile(resolveLevelPath(pos[0]))) {
            std::fprintf(stderr, "加载失败: %s\n", pos[0].c_str());
            return 1;
        }
        std::string out;
        std::string err;
        if (!adocao::writeLevelJson(lv, out, err)) {
            std::fprintf(stderr, "回写失败: %s\n", err.c_str());
            return 1;
        }
        std::ofstream f(pos[1], std::ios::binary);
        if (!f) { std::fprintf(stderr, "无法写入: %s\n", pos[1].c_str()); return 1; }
        f.write(out.data(), (std::streamsize)out.size());
        std::printf("json: %s → %s（%zu B）\n", pos[0].c_str(), pos[1].c_str(), out.size());
        return 0;
    }

    // ── 子命令分发（都在任何 GL 初始化之前）
    if (mode == Mode::Pack) {
        if (pos.size() < 2) return usageErr(mode, "adocao pack 需要 <谱> <输出.adocao>");
        LevelData lv;
        lv.preserveRawJson = true;   // v7：留下 actions/decorations 的原始切片（建 schema 用）
        if (!lv.loadFromFile(resolveLevelPath(pos[0]))) {
            std::fprintf(stderr, "加载失败: %s\n", pos[0].c_str());
            return 1;
        }
        if (packSchemaReport) {
            adocao::SchemaBuilder sb;
            std::string serr;
            if (true) {
                std::printf("原始成员: %zu 条 action + decorations %zu B\n",
                            lv.rawActionsJson.size(), lv.rawDecorationsJson.size());
                // 事件列的真实体积（v7 的预期大小）：建列后按"列字节 + 位图 + 字典 + schema"合计
                // 裁剪：只保留指定类型（playable = v5 能力面；minimal = 影响几何/计时的）
                static const std::set<std::string> kPlayable = {
                    "Twirl","SetSpeed","PositionTrack","SetHitsound","Bookmark","Pause","AnimateTrack",
                    "MoveCamera","ColorTrack","RecolorTrack","Flash","MoveTrack","RepeatEvents","ScaleRadius"};
                static const std::set<std::string> kMinimal = {
                    "Twirl","SetSpeed","PositionTrack","ScaleRadius","AnimateTrack"};
                const std::set<std::string>* allow = nullptr;
                if (packEventsPreset == "playable") allow = &kPlayable;
                else if (packEventsPreset == "minimal") allow = &kMinimal;
                std::vector<std::string> evs;
                std::vector<uint8_t> srcs;
                size_t dropped = 0;
                (void)serr;
                for (const auto& js : lv.rawActionsJson) {
                    rapidjson::Document d;
                    d.Parse(js.c_str(), js.size());
                    const char* t = (d.HasParseError() || !d.HasMember("eventType")) ? "" : d["eventType"].GetString();
                    if (allow && !allow->count(t)) { ++dropped; continue; }
                    evs.push_back(js);
                    srcs.push_back(0);
                }
                if (!lv.rawDecorationsJson.empty()) {
                    rapidjson::Document dd;
                    dd.Parse(lv.rawDecorationsJson.c_str(), lv.rawDecorationsJson.size());
                    if (!dd.HasParseError() && dd.IsArray()) {
                        for (const auto& v : dd.GetArray()) {
                            rapidjson::StringBuffer b;
                            rapidjson::Writer<rapidjson::StringBuffer> w(b);
                            v.Accept(w);
                            if (allow && !allow->count(v["eventType"].GetString())) { ++dropped; continue; }
                            evs.emplace_back(b.GetString(), b.GetSize());
                            srcs.push_back(1);
                        }
                    }
                }
                // schema 必须在**裁剪之后**建：只描述保留下来的类型/字段
                for (size_t i = 0; i < evs.size(); ++i) {
                    rapidjson::Document d;
                    d.Parse(evs[i].c_str(), evs[i].size());
                    if (d.HasParseError() || !d.HasMember("eventType")) continue;
                    sb.addEvent(d["eventType"].GetString(),
                                srcs[i] ? adocao::EventSource::Decorations : adocao::EventSource::Actions, d);
                }
                sb.finalize();
                std::fputs(adocao::schemaSummary(sb.schema()).c_str(), stdout);
                adocao::EventColumns cols;
                std::string cerr;
                if (adocao::buildEventColumns(sb.schema(), evs, srcs, cols, cerr)) {
                    size_t payload = 0, bitmaps = 0, dicts = 0;
                    for (const auto& c : cols.fields) {
                        payload += c.payload.size();
                        bitmaps += c.present.size() + c.nulls.size() + c.nulls0.size() + c.nulls1.size();
                        for (const auto& s : c.dictStrings) dicts += s.size() + 2;
                    }
                    const size_t schemaBytes = sb.serialize().size();
                    const std::vector<uint8_t> blob = adocao::serializeEventColumns(sb.schema(), cols);
                    const size_t z = adocao::zstdEstimate(blob);
                    {
                        static const char* kn[] = {"Bool","Int","Float","GridFloat","Enum","Color",
                                                   "String","Tag","Pair","Nested","RawJSON"};
                        size_t byKind[11] = {0};
                        size_t dictByKind[11] = {0};
                        for (size_t f = 0; f < cols.fields.size(); ++f) {
                            const size_t k = (size_t)adocao::Kind(sb.schema().fields[f].kind);
                            byKind[k] += cols.fields[f].payload.size();
                            for (const auto& ds : cols.fields[f].dictStrings) dictByKind[k] += ds.size() + 2;
                        }
                        for (int k = 0; k < 11; ++k)
                            if (byKind[k] || dictByKind[k])
                                std::printf("   kind %-9s payload %7zu B  字典 %7zu B\n", kn[k], byKind[k], dictByKind[k]);
                    }
                    std::printf("事件列: %zu 条 → payload %zu B + 位图 %zu B + 字典 %zu B + schema %zu B = %.1f KB；"
                                "整段 zstd-19 → %.1f KB（含 schema %.1f KB）\n",
                                evs.size(), payload, bitmaps, dicts, schemaBytes,
                                (double)(payload + bitmaps + dicts + schemaBytes) / 1024.0,
                                (double)(z + adocao::zstdEstimate(sb.serialize())) / 1024.0,
                                (double)(z + adocao::zstdEstimate(sb.serialize())) / 1024.0);
                    if (dropped) std::printf("  裁剪: 事件 %zu → %zu（丢掉 %zu 条，preset=%s）\n",
                                             lv.rawActionsJson.size() + (lv.rawDecorationsJson.empty() ? 0 : 1),
                                             evs.size(), dropped, packEventsPreset.c_str());
                } else {
                    std::fprintf(stderr, "事件列构建失败: %s\n", cerr.c_str());
                }
            } else {
                std::fprintf(stderr, "schema 构建失败: %s\n", serr.c_str());
            }
        }
        // 裁剪导出：先把 raw 切片按预设过滤掉（v7 段只装保留下来的）
        uint32_t droppedForV7 = 0;
        if (packV7 && packEventsPreset != "all") {
            static const std::set<std::string> kPlayable = {
                "Twirl","SetSpeed","PositionTrack","SetHitsound","Bookmark","Pause","AnimateTrack",
                "MoveCamera","ColorTrack","RecolorTrack","Flash","MoveTrack","RepeatEvents","ScaleRadius"};
            static const std::set<std::string> kMinimal = {
                "Twirl","SetSpeed","PositionTrack","ScaleRadius","AnimateTrack"};
            const std::set<std::string>& allow =
                (packEventsPreset == "minimal") ? kMinimal : kPlayable;
            std::vector<std::string> kept;
            kept.reserve(lv.rawActionsJson.size());
            for (const auto& js : lv.rawActionsJson) {
                rapidjson::Document d;
                d.Parse(js.c_str(), js.size());
                const char* t = (d.HasParseError() || !d.HasMember("eventType")) ? "" : d["eventType"].GetString();
                if (allow.count(t)) kept.push_back(js);
                else ++droppedForV7;
            }
            lv.rawActionsJson = std::move(kept);
            if (!lv.rawDecorationsJson.empty()) {
                rapidjson::Document dd;
                dd.Parse(lv.rawDecorationsJson.c_str(), lv.rawDecorationsJson.size());
                size_t n = (!dd.HasParseError() && dd.IsArray()) ? dd.Size() : 0;
                droppedForV7 += (uint32_t)n;
                lv.rawDecorationsJson.clear();   // 预设都不含装饰
            }
        }
        std::vector<uint8_t> bytes;
        std::string err;
        adocao::PackResult rep;
        adocao::PackOptions popts;
        popts.withCheckpoints = packCheckpoints;
        popts.compress = packCompress;
        popts.level = packLevel;
        popts.withV7Events = packV7;
        popts.v7IncludeLegacy = packV7All;
        popts.v7Only = packV7Only;
        popts.droppedEvents = droppedForV7;
        if (!adocao::packLevel(lv, bytes, err, &rep, popts)) {
            std::fprintf(stderr, "打包失败: %s\n", err.c_str());
            return 1;
        }
        if (packV7) {
            LevelData chk;
            adocao::DerivedInfo di;
            std::string cerr;
            if (!adocao::unpackLevel(bytes.data(), bytes.size(), chk, cerr, &di)) {
                std::fprintf(stderr, "v7 自检失败（解容器）: %s\n", cerr.c_str());
                return 1;
            }
            if (chk.v7SchemaBytes.empty() || chk.v7ColumnsBytes.empty()) {
                std::fprintf(stderr, "v7 自检失败：段缺失\n");
                return 1;
            }
            std::printf("v7 自检: schema %zu B + 事件列 %zu B%s\n", chk.v7SchemaBytes.size(),
                        chk.v7ColumnsBytes.size(),
                        chk.v7CoverageBytes.empty() ? "（未裁剪）" : "（含 Coverage）");
        }
        std::ofstream f(pos[1], std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "无法写入: %s\n", pos[1].c_str());
            return 1;
        }
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        if (!f) {
            std::fprintf(stderr, "写入失败: %s\n", pos[1].c_str());
            return 1;
        }
        const double tiles = (double)lv.angleData.size();
        std::fprintf(stderr, "pack: %zu 层 / %zu 个 action → %.2f MB",
                     lv.angleData.size(), lv.actions.size(), (double)bytes.size() / 1048576.0);
        if (tiles > 0) std::fprintf(stderr, "（%.3f B/层）", (double)bytes.size() / tiles);
        if (rep.checkpoints)
            std::fprintf(stderr, "  + checkpoint %llu 条 / %llu B（每 4096 层一条，§8.8）",
                         (unsigned long long)rep.checkpoints,
                         (unsigned long long)rep.checkpointBytes);
        std::fprintf(stderr, "\n");
        if (codecReport && tiles > 0) {
            std::fprintf(stderr, "  列明细：\n");
            for (const adocao::ColumnReport& c : rep.columns) {
                std::fprintf(stderr,
                             "    %-14s %-12s n=%-9llu 朴素 %10llu B  编码 %9llu B  %7.4f B/值"
                             "  字典 %6llu  位宽 %2d\n",
                             c.name.c_str(), c.codec.c_str(), (unsigned long long)c.elements,
                             (unsigned long long)c.rawBytes, (unsigned long long)c.encodedBytes,
                             c.elements ? (double)c.encodedBytes / (double)c.elements : 0.0,
                             (unsigned long long)c.dictEntries, c.bits);
                // v3 分块（§8.3）：单块 vs 分块的**实际存储字节** —— "分块赚了多少 / 亏了多少"
                // 必须一眼看得出来，否则"块大小按列定"就没有判据（§8.3 的警告）。
                if (c.singleBytes || c.blockedBytes) {
                    const double ratio = c.blockedBytes
                                             ? (double)c.singleBytes / (double)c.blockedBytes : 0.0;
                    std::fprintf(stderr,
                                 "    %-14s   └ 分块：单块 %llu B → %llu B（%.3f×）%s  %llu 块 / %llu 帧\n",
                                 "", (unsigned long long)c.singleBytes,
                                 (unsigned long long)c.blockedBytes, ratio,
                                 c.forced ? "✓ 强制分块（窗口化要求）"
                                          : (c.blocked ? "✓ 分块更小" : "✗ 分块更大"),
                                 (unsigned long long)c.blocks, (unsigned long long)c.frames);
                }
            }

            // ---- §8.4 判据统计：每列长什么样（纯诊断，不参与上面的编码决策）----
            std::vector<adocao::ColumnDiagnostics> diag;
            std::string derr;
            if (adocao::diagnoseLevel(lv, adocao::DiagOptions{}, diag, derr)) {
                std::fprintf(stderr,
                             "\n  判据（§8.4；只管说明为什么，与上面选中的编码无关；"
                             "建议是**未计段级 zstd** 的裸估算，只有实编才是权威）：\n"
                             "    列              n          值集    游程   1阶基数 原始/环绕"
                             "   2阶基数 原始/环绕   环绕率 原始→掩码  网格     周期(相关)      "
                             "窗口唯一@p        建议\n"
                             "    整数域（残差只在整数域里算，§8.2①）：scale / Δ² varint B 原始→环绕 / "
                             "|Δ²|max 原始→环绕\n");
                bool sampledAny = false;
                for (const adocao::ColumnDiagnostics& d : diag) {
                    const bool sampled = d.n != d.nTotal;
                    sampledAny = sampledAny || sampled;
                    char period[32] = "-";
                    if (d.period)
                        std::snprintf(period, sizeof(period), "%d(%.2f)", d.period, d.periodPeak);
                    char win[48] = "-";
                    if (d.motifPeriod)
                        std::snprintf(win, sizeof(win), "%llu/%llu@p%d",
                                      (unsigned long long)d.windowUnique,
                                      (unsigned long long)d.windowCount, d.motifPeriod);
                    char grid[16] = "-";
                    if (d.gridExp2 >= 0) std::snprintf(grid, sizeof(grid), "2^-%d", d.gridExp2);
                    else if (d.gridExp10 >= 0) std::snprintf(grid, sizeof(grid), "10^-%d", d.gridExp10);
                    char name[24];
                    std::snprintf(name, sizeof(name), "%s%s", d.name.c_str(), sampled ? "*" : "");
                    std::fprintf(stderr,
                                 "    %-14s %-10llu %7llu %7llu  %9llu/%-9llu %9llu/%-9llu  "
                                 "%6.3f→%-6.3f %-8s %-14s %-17s %s\n",
                                 name, (unsigned long long)d.n,
                                 (unsigned long long)d.distinct, (unsigned long long)d.runs,
                                 (unsigned long long)d.d1Raw, (unsigned long long)d.d1Wrapped,
                                 (unsigned long long)d.d2Raw, (unsigned long long)d.d2Wrapped,
                                 d.wrapRateRaw, d.wrapRateMasked, grid, period, win, d.verdict);
                    if (d.gridScale > 0.0 || d.d2WrapBytes > 0.0)
                        std::fprintf(stderr,
                                     "                   └ 整数域 scale=%g（10^%d）：Δ² varint "
                                     "%.2f→%.2f B/值，|Δ²|max %lld→%lld\n",
                                     d.gridScale, d.gridExp10, d.d2RawBytes, d.d2WrapBytes,
                                     (long long)d.d2RawMax, (long long)d.d2WrapMax);
                    if (!d.note.empty())
                        std::fprintf(stderr, "                   └ %s\n", d.note.c_str());
                    if (d.name == "tiles.position")
                        std::fprintf(stderr, "                   └ 空间自重叠率 %.4f（1 砖格子）\n",
                                     d.overlap);
                }
                if (sampledAny)
                    std::fprintf(stderr,
                                 "    注：n 带 * = 该列更长，只统计了前缀样本"
                                 "（环绕率/差分基数建立在相邻对上，跨步抽样会破坏它）。\n");
            } else {
                std::fprintf(stderr, "  判据统计失败: %s\n", derr.c_str());
            }
        }
        return 0;
    }
    if (mode == Mode::Image) {
        if (pos.size() < 1)        return usageErr(mode, "adocao image 需要 <输出.png>");
        if (cli.levelPath.empty()) return usageErr(mode, "adocao image 需要 --level <谱>");
        const std::string& outPng = pos[0];
        if (!onePx && !oneBit)     // 默认：矢量全景，一条命令直接出图
            return exportLevelMap(cli.levelPath, outPng, size, bg, range, native, timeColor,
                                  padding, thickness);
        if (oneBit && scale != 1)  std::fprintf(stderr, "提示：--scale 对 --1bit 无效（索引色不做降采样）\n");
        // 1px=1tile：内部出瓦片 → 拼接（或 1-bit 索引色）→ 清中间产物
        namespace fs = std::filesystem;
        const std::string tmp = keepTiles.empty()
            ? (fs::temp_directory_path() /
               ("adocao-tiles-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string()
            : keepTiles;
        std::error_code ec;
        fs::create_directories(tmp, ec);
        std::fprintf(stderr, "1px=1tile：出瓦片到 %s（完成后%s）\n", tmp.c_str(),
                     keepTiles.empty() ? "删除" : "保留");
        const int rc = exportLevelMapNativeAll(cli.levelPath, tmp, range, block, threads, timeColor);
        if (rc != 0) return rc;
        const int rc2 = oneBit ? exportLevelMapMono1(tmp, outPng, threads)
                               : stitchLevelMapTiles(tmp, outPng, scale, threads);
        if (keepTiles.empty()) fs::remove_all(tmp, ec);
        return rc2;
    }
    if (mode == Mode::Stitch) {
        if (pos.size() < 2) return usageErr(mode, "adocao stitch 需要 <瓦片目录> <输出.png>");
        return stitchLevelMapTiles(pos[0], pos[1], scale, threads);
    }
    if (mode == Mode::Tiles) {
        if (pos.size() < 1)        return usageErr(mode, "adocao tiles 需要 <输出目录>");
        if (cli.levelPath.empty()) return usageErr(mode, "adocao tiles 需要 --level <谱>");
        return exportLevelMapNativeAll(cli.levelPath, pos[0], range, block, threads, timeColor);
    }
    if (mode == Mode::Export) cli.exportHitsounds = true;

    // 播放模式的位置参数 = 谱面（等价于 --level，方便 `adocao x.adofai`）
    if (mode == Mode::Play && !pos.empty() && cli.levelPath.empty())
        cli.levelPath = resolveLevelPath(pos[0]);

    // 抓帧必须是**确定性**的（AGENTS：不走 wall clock / 音频 / **帧时间反馈**），但拖尾的自适应
    // 采样率正是由测得的帧时间驱动的 → 同一帧在不同性能的构建上会画出**不同长度**的拖尾。
    // 2026-10 真踩过：一个数值逐位相同、只省内存的改动，因为快了几毫秒，让 51 个抓帧状态里 31 个变色。
    if (!cli.capturePath.empty()) cli.trailAdaptive = false;
    if (!cli.levelPath.empty()) return runApplicationFromCLI(cli, debug);
    return runApplication(debug);
}
