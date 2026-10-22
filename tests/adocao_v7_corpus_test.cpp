// **v7 语料回归**：把仓库里的 `tests/charts/*.adofai` 逐张 pack 成 `.adocao`（v7-only）再解回来，
// 与直接读 JSON 得到的 LevelData 逐字段比对。
//
// 为什么不在仓库里提交 `.adocao` 二进制：这些容器只是谱面的另一种编码，重新生成是确定性的，
// 提交生成物等于把"同一个东西"存两份（而且二进制 дифф 不可读）。用打包往返测同一件事更干净。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoWriter.hpp"
#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"

using namespace adofai;

static int g_fail = 0, g_charts = 0;

static void fail(const std::string& chart, const std::string& what) {
    std::printf("FAIL %-28s %s\n", chart.c_str(), what.c_str());
    ++g_fail;
}

static std::string base(const std::string& p) { return std::filesystem::path(p).filename().string(); }

static bool compare(const LevelData& a, const LevelData& b, const std::string& chart) {
    bool ok = true;
    auto chk = [&](bool c, const char* what) {
        if (!c) { fail(chart, what); ok = false; }
    };
    if (a.actions.size() != b.actions.size() || a.cameraEvents.size() != b.cameraEvents.size() ||
        a.colorEvents.size() != b.colorEvents.size()) {
        std::printf("  计数: actions %zu/%zu camera %zu/%zu colour %zu/%zu flash %zu/%zu move %zu/%zu\n",
                    a.actions.size(), b.actions.size(), a.cameraEvents.size(), b.cameraEvents.size(),
                    a.colorEvents.size(), b.colorEvents.size(), a.flashEvents.size(), b.flashEvents.size(),
                    a.moveTrackEvents.size(), b.moveTrackEvents.size());
    }
    chk(a.angleData.size() == b.angleData.size(), "angleData 数量");
    chk(a.actions.size() == b.actions.size(), "actions 数量");
    chk(a.cameraEvents.size() == b.cameraEvents.size(), "camera 数量");
    chk(a.colorEvents.size() == b.colorEvents.size(), "colour 数量");
    chk(a.flashEvents.size() == b.flashEvents.size(), "flash 数量");
    chk(a.moveTrackEvents.size() == b.moveTrackEvents.size(), "moveTrack 数量");
    chk(a.settings.bpm == b.settings.bpm, "bpm");
    for (size_t i = 0; i < a.actions.size() && i < b.actions.size(); ++i) {
        const auto& x = a.actions[i];
        const auto& y = b.actions[i];
        if (x.floor != y.floor || x.type != y.type || x.flag != y.flag ||
            x.val1 != y.val1 || x.val2 != y.val2 || x.strId != y.strId) {
            char msg[256];
            std::snprintf(msg, sizeof msg, "action #%zu: floor %d/%d type %d/%d flag %d/%d v1 %.3f/%.3f v2 %.3f/%.3f str %u/%u",
                          i, x.floor, y.floor, (int)x.type, (int)y.type, (int)x.flag, (int)y.flag,
                          x.val1, y.val1, x.val2, y.val2, x.strId, y.strId);
            fail(chart, msg);
            ok = false;
            break;
        }
    }
    for (size_t i = 0; i < a.cameraEvents.size() && i < b.cameraEvents.size(); ++i) {
        const auto& x = a.cameraEvents[i];
        const auto& y = b.cameraEvents[i];
        if (x.floor != y.floor || x.zoom != y.zoom || x.x != y.x || x.y != y.y ||
            x.rotation != y.rotation || x.angleOffset != y.angleOffset || x.ease != y.ease ||
            x.relativeTo != y.relativeTo || x.flags != y.flags || x.tag != y.tag) {
            char msg[256];
            std::snprintf(msg, sizeof msg, "camera #%zu: floor %d/%d zoom %.2f/%.2f xy %.2f,%.2f/%.2f,%.2f rot %.2f/%.2f rel %d/%d tag '%s'/'%s'",
                          i, x.floor, y.floor, x.zoom, y.zoom, x.x, x.y, y.x, y.y, x.rotation, y.rotation,
                          x.relativeTo, y.relativeTo, x.tag.c_str(), y.tag.c_str());
            fail(chart, msg);
            ok = false;
            break;
        }
    }
    for (size_t i = 0; i < a.colorEvents.size() && i < b.colorEvents.size(); ++i) {
        const auto& x = a.colorEvents[i];
        const auto& y = b.colorEvents[i];
        if (x.floor != y.floor || x.cfg.color != y.cfg.color || x.cfg.secondary != y.cfg.secondary ||
            x.cfg.colorType != y.cfg.colorType || x.cfg.pulse != y.cfg.pulse ||
            x.cfg.style != y.cfg.style || x.duration != y.duration || x.ease != y.ease ||
            x.startOff != y.startOff || x.endOff != y.endOff) {
            char msg[320];
            std::snprintf(msg, sizeof msg, "colour #%zu: '%s'/'%s' sec '%s'/'%s' type '%s'/'%s' pulse '%s'/'%s' style '%s'/'%s' dur %.3f/%.3f",
                          i, x.cfg.color.c_str(), y.cfg.color.c_str(), x.cfg.secondary.c_str(), y.cfg.secondary.c_str(),
                          x.cfg.colorType.c_str(), y.cfg.colorType.c_str(), x.cfg.pulse.c_str(), y.cfg.pulse.c_str(),
                          x.cfg.style.c_str(), y.cfg.style.c_str(), x.duration, y.duration);
            fail(chart, msg);
            ok = false;
            break;
        }
    }
    for (size_t i = 0; i < a.flashEvents.size() && i < b.flashEvents.size(); ++i) {
        const auto& x = a.flashEvents[i];
        const auto& y = b.flashEvents[i];
        if (x.floor != y.floor || x.duration != y.duration || x.startOpacity != y.startOpacity ||
            x.endOpacity != y.endOpacity || std::memcmp(x.startColor, y.startColor, 3) != 0 ||
            std::memcmp(x.endColor, y.endColor, 3) != 0 || x.plane != y.plane || x.style != y.style) {
            fail(chart, "flash #" + std::to_string(i) + " 字段不一致");
            ok = false;
            break;
        }
    }
    for (size_t i = 0; i < a.moveTrackEvents.size() && i < b.moveTrackEvents.size(); ++i) {
        const auto& x = a.moveTrackEvents[i];
        const auto& y = b.moveTrackEvents[i];
        if (x.floor != y.floor || x.startOff != y.startOff || x.endOff != y.endOff ||
            x.startRel != y.startRel || x.gap != y.gap || x.durBeats != y.durBeats ||
            x.offX != y.offX || x.rotDeg != y.rotDeg || x.scaleX != y.scaleX || x.opacity != y.opacity) {
            fail(chart, "moveTrack #" + std::to_string(i) + " 字段不一致");
            ok = false;
            break;
        }
    }
    return ok;
}

int main(int argc, char** argv) {
    archive::install();
    std::vector<std::string> charts;
    for (int i = 1; i < argc; ++i) {
        std::error_code ec;
        if (std::filesystem::is_directory(argv[i], ec)) {
            for (const auto& e : std::filesystem::directory_iterator(argv[i], ec)) {
                const std::string p = e.path().string();
                if (p.size() > 7 && p.compare(p.size() - 7, 7, ".adofai") == 0) charts.push_back(p);
            }
        } else {
            charts.push_back(argv[i]);
        }
    }
    std::sort(charts.begin(), charts.end());
    for (const auto& path : charts) {
        LevelData a;
        // v7 的事件列来自"原始成员捕获"，必须先打开（生产路径里由 `pack` 打开）
        a.preserveRawJson = true;
        if (!a.loadFromFile(path)) { fail(base(path), "读不了源谱"); continue; }
        adocao::PackOptions opts;
        opts.v7Only = true;
        std::vector<uint8_t> bytes;
        std::string err;
        if (!adocao::packLevel(a, bytes, err, nullptr, opts)) { fail(base(path), "打包: " + err); continue; }
        adocao::DerivedInfo derived;
        LevelData b;
        if (!adocao::unpackLevel(bytes.data(), bytes.size(), b, err, &derived)) {
            fail(base(path), "解包: " + err);
            continue;
        }
        if (compare(a, b, base(path))) ++g_charts;
    }
    std::printf("adocao_v7_corpus: %d 张谱通过，%d 项失败\n", g_charts, g_fail);
    return g_fail ? 1 : 0;
}
