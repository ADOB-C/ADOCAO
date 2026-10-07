#include "app/HitsoundTypes.hpp"
#include "core/util/Progress.hpp"
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
#ifndef _WIN32
#include <unistd.h>
#endif



namespace adofai {}          // 前置声明：本文件可能不直接 include 库头
using namespace adofai;      // 库侧公共 API 在 adofai:: 里（P1：为 ADOFAI.Lib 做准备）

namespace {

// 子命令：不带 = 播放/向导（这一条语法**不变**，外部 launcher 就靠它）。
// 无头那一族（今晚才加、没有外部用户）折进子命令，于是每个模式一屏帮助、
// 且模式内可以把 --map- 前缀去掉（--map-size → map --size）。
enum class Mode { Play, Image, Tiles, Stitch, Export };

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Image:  return "image";
        case Mode::Tiles:  return "tiles";
        case Mode::Stitch: return "stitch";
        case Mode::Export: return "export";
        default:           return "";
    }
}

// ── 帮助 ─────────────────────────────────────────────────────────────────────
// 每组一屏；开发开关与环境变量钩子只在 --help --all 里出现。
void printHelp(Mode mode, bool all) {
    switch (mode) {
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
            "    --time-color          按谱面时间六档彩虹   --gradient  沿进度渐变（默认整条纯色 DEBB7B）\n"
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
            "  衔接：adocao stitch <dir> <out.png> / adocao mono <dir> <out.png>\n");
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
        "  adocao image  <out.png>    --level <谱>   导出一张图（默认矢量；--1px 瓦片路线）\n"
        "  adocao tiles  <dir>        --level <谱>   只出瓦片（进阶）\n"
        "  adocao stitch <dir> <out.png>             只拼瓦片（进阶）\n"
        "\n"
        "播放选项:\n"
        "  --level <file>  --music <file>  --width N  --height N  --fullscreen\n"
        "  --fill <hex>  --stroke <hex>  --bg <hex>  --no-auto-stroke\n"
        "  --msaa N  --no-exclusive                 抗锯齿 / 无边框全屏（默认独占）\n"
        "  --no-hitsound  --force-hitsound [TYPE]  --auto-play  --no-trail  --export\n"
        "  --trail-duration SEC | --trail-tiles N   拖尾长度（二选一，默认关）\n"
        "  --trail-sample-rate N | --trail-target-fps N            固定 / 自适应采样率\n"
        "  --trail-samples-per-tile N  --trail-rate-min N  --trail-rate-max N\n"
        "\n"
        "全局:\n"
        "  --progress | --no-progress               进度（默认：stderr 是终端才显示）\n"
        "  -h, --help [--all]                       这份帮助 / 含开发选项与环境变量钩子\n");
    if (all) {
        std::fprintf(stderr,
            "\n开发:\n"
            "  --debug                                  调试控制台 + 默认关 hitsounds\n"
            "  --capture <out.png> [--capture-time SEC | --capture-tile N] [--capture-zoom Z]\n"
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
    bool  gradient = false;                          // --gradient：沿进度渐变（默认纯色）
    int block = 4096, threads = 0, scale = 1, progressForce = 0;

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
            else if (std::strcmp(a, "--fullscreen") == 0)           cli.fullscreen = true;
            else if (std::strcmp(a, "--fill") == 0      && i+1<argc) cli.trackFillColor = argv[++i];
            else if (std::strcmp(a, "--stroke") == 0    && i+1<argc) cli.trackStrokeColor = argv[++i];
            else if (std::strcmp(a, "--bg") == 0        && i+1<argc) cli.backgroundColor = argv[++i];
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
            else if (std::strcmp(a, "--msaa") == 0 && i+1<argc)      cli.msaaSamples = atoi(argv[++i]);
            else if (std::strcmp(a, "--no-exclusive") == 0)          cli.exclusiveFullscreen = false;   // 默认独占全屏
            else if (std::strcmp(a, "--no-trail") == 0)              cli.showTrail = false;
            else if (std::strcmp(a, "--capture") == 0 && i+1<argc)      cli.capturePath = argv[++i];
            else if (std::strcmp(a, "--capture-time") == 0 && i+1<argc) cli.captureTime = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--capture-tile") == 0 && i+1<argc) cli.captureTile = atoi(argv[++i]);
            else if (std::strcmp(a, "--capture-zoom") == 0 && i+1<argc) cli.captureZoom = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--trail-duration") == 0 && i+1<argc) cli.trailDuration = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--trail-sample-rate") == 0 && i+1<argc) { cli.trailSampleRate = (float)atof(argv[++i]); cli.trailAdaptive = false; }
            else if (std::strcmp(a, "--trail-target-fps") == 0 && i+1<argc) { cli.trailTargetFps = (float)atof(argv[++i]); cli.trailAdaptive = true; }
            else if (std::strcmp(a, "--trail-rate-min") == 0 && i+1<argc) cli.trailRateMin = (float)atof(argv[++i]);
            else if (std::strcmp(a, "--trail-rate-max") == 0 && i+1<argc) cli.trailRateMax = (float)atof(argv[++i]);
            // 采样率随"每秒走过的轨道"放大（步进 + 该层相对角扫过的弧），取代固定 Hz
            else if (std::strcmp(a, "--trail-samples-per-tile") == 0 && i+1<argc) { cli.trailSamplesPerTile = (float)atof(argv[++i]); cli.trailPerTile = true; }
            // 拖尾按"层数"而不是秒数计量（0.4s 在 BPM 1.5M 的谱上是几万层）
            else if (std::strcmp(a, "--trail-tiles") == 0 && i+1<argc) { cli.trailTiles = (float)atof(argv[++i]); cli.trailLengthInTiles = true; }
            else goto unknown;
        }
        // ── adocao image <out.png> --level <谱> [--1px [--scale N] [--1bit]]
        else if (mode == Mode::Image) {
                 if (std::strcmp(a, "--level") == 0      && i+1<argc) cli.levelPath = resolveLevelPath(argv[++i]);
            else if (std::strcmp(a, "--size") == 0       && i+1<argc) size = argv[++i];
            else if (std::strcmp(a, "--bg") == 0         && i+1<argc) bg = argv[++i];
            else if (std::strcmp(a, "--time-color") == 0)             timeColor = true;
            else if (std::strcmp(a, "--gradient") == 0)               gradient = true;
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
        if (a[0] == '-' && a[1] == '-' && a[2] != '\0') {   // 单独的 "--" 按约定忽略
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

    // ── 子命令分发（都在任何 GL 初始化之前）
    if (mode == Mode::Image) {
        if (pos.size() < 1)        return usageErr(mode, "adocao image 需要 <输出.png>");
        if (cli.levelPath.empty()) return usageErr(mode, "adocao image 需要 --level <谱>");
        const std::string& outPng = pos[0];
        if (!onePx && !oneBit)     // 默认：矢量全景，一条命令直接出图
            return exportLevelMap(cli.levelPath, outPng, size, bg, range, native, timeColor,
                                  padding, thickness, gradient);
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

    if (!cli.levelPath.empty()) return runApplicationFromCLI(cli, debug);
    return runApplication(debug);
}
