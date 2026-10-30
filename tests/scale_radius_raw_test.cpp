// `ScaleRadius` 的**原始事件**必须活着穿过 `.adocao`（2026-10-29 ADOFAI.Lib 报）。
//
// 背景：v6 的 Actions 段只存 `val1 = scale`（解析时被"烘"进砖位，播放够用），**原始 JSON 没了**。
// 于是 ADOFAI.Lib 从 `.adocao` 反向导出 `.adofai` 时写不出这条事件，它的 `canExport` 只能拒绝
// （`LevelJsonWriter.cpp` 里那段注释："模型里没有，所以任何实现都不可能还原"）。
//
// 现在 `ScaleRadius` 从 `AdocaoWriter` 的 `kLegacyStored` 里移出来 → 进 v7 事件段（原始 JSON 无损）。
// 本用例钉住三件事：
//  ① 默认打包（`withV7Events`、**不带** `v7IncludeLegacy`）产出的容器里，v7 事件段必须能解出
//     那条 `ScaleRadius`，且字段（floor / scale）与源谱逐字一致；
//  ② 解出来的 JSON 与源谱里那条事件**文本相同**（不是"重新拼一个"）；
//  ③ 反面对照：把 `ScaleRadius` 加回 `kLegacyStored` 那种情况等价于"v7 段里没有它" ——
//     这里用 `v7IncludeLegacy=false` 且**从 v6 段推断**来验：没有 v7 段时解不出任何事件
//     （说明这条用例抓的正是"原始事件在不在 v7 里"）。

#include "AdocaoJsonWriter.hpp"
#include "archive/AdocaoEventColumns.hpp"
#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoWriter.hpp"
#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace adofai;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

// 一条 ScaleRadius + 若干普通砖。`scale` 用 80（= 0.8）便于确认数值没被改写。
const char* kChart = R"JSON({
  "angleData": [0,180,180,180,180,180,180,180],
  "settings": {"bpm": 120, "offset": 0, "countdownTicks": 0, "pitch": 100,
               "trackColor": "debb7b", "backgroundColor": "000000", "trackStyle": "Standard"},
  "actions": [
    {"floor": 2, "eventType": "ScaleRadius", "scale": 80},
    {"floor": 4, "eventType": "Twirl"}
  ]
})JSON";

bool loadLevel(LevelData& lv) {
    // `preserveRawJson` 必须在**解析前**设：它决定解析时要不要把每条 action 的原始文本
    // 记进 `rawActionSpans`（加载后再设就晚了，v7 段会一条事件都没有）。
    lv.preserveRawJson = true;
    return lv.loadFromBuffer(kChart, std::strlen(kChart));
}

// 把容器里的 v7 事件段**回写成明文 JSON**（测试侧回写器 = ADOFAI.Lib 导出走的那条路）
bool exportJson(const LevelData& lv, std::string& json) {
    std::string err;
    if (!adocao::writeLevelJson(lv, json, err)) {
        std::printf("   （回写失败：%s）\n", err.c_str());
        return false;
    }
    return true;
}

bool containsScaleRadius(const std::string& json, std::string* got) {
    const size_t at = json.find("ScaleRadius");
    if (at == std::string::npos) return false;
    if (got) {
        const size_t b = json.rfind('{', at);
        const size_t e = json.find('}', at);
        *got = (b != std::string::npos && e != std::string::npos) ? json.substr(b, e - b + 1) : "";
    }
    return true;
}

}  // namespace

int main() {
    archive::install();

    // ① 默认打包（不带 v7IncludeLegacy）→ v7 事件段里必须有 ScaleRadius
    {
        LevelData a;
        if (!loadLevel(a)) {
            std::printf("FAIL 源谱没加载成功\n");
            return 1;
        }
        adocao::PackOptions opts;
        opts.withV7Events = true;      // 生产默认（`pack_adocao --v7`）
        std::vector<uint8_t> bytes;
        std::string err;
        if (!adocao::packLevel(a, bytes, err, nullptr, opts)) {
            std::printf("FAIL 打包失败：%s\n", err.c_str());
            return 1;
        }
        LevelData b;
        adocao::DerivedInfo derived;
        if (!adocao::unpackLevel(bytes.data(), bytes.size(), b, err, &derived)) {
            std::printf("FAIL 解包失败：%s\n", err.c_str());
            return 1;
        }
        if (b.v7SchemaBytes.empty() || b.v7ColumnsBytes.empty()) {
            check(false, "容器里没有 v7 事件段（ScaleRadius 只能被丢掉了）");
            return 1;
        }
        std::string json;
        if (!exportJson(b, json)) {
            check(false, "v7 事件段解不出来（回写失败）");
            return 1;
        }
        std::string got;
        if (!containsScaleRadius(json, &got)) {
            std::printf("FAIL 回写出的 JSON 里没有 ScaleRadius（%zu B）\n", json.size());
            return 1;
        }
        // ② 字段逐字一致（floor 2 / scale 80）
        if (got.find("\"floor\":2") == std::string::npos &&
            got.find("\"floor\": 2") == std::string::npos) {
            std::printf("FAIL 解出的 ScaleRadius 丢了 floor：%s\n", got.c_str());
            return 1;
        }
        if (got.find("80") == std::string::npos) {
            std::printf("FAIL 解出的 ScaleRadius 丢了 scale：%s\n", got.c_str());
            return 1;
        }
        std::printf("ok   ScaleRadius 原始事件活着穿过容器：%s\n", got.c_str());
    }

    // ③ 反面对照：关掉 v7 事件段 → 解不出任何事件（证明上面那条断言的来源就是 v7 段）
    {
        LevelData a;
        if (!loadLevel(a)) return 1;
        adocao::PackOptions opts;
        opts.withV7Events = false;
        std::vector<uint8_t> bytes;
        std::string err;
        if (!adocao::packLevel(a, bytes, err, nullptr, opts)) {
            std::printf("FAIL 反面对照打包失败：%s\n", err.c_str());
            return 1;
        }
        LevelData b;
        adocao::DerivedInfo derived;
        if (!adocao::unpackLevel(bytes.data(), bytes.size(), b, err, &derived)) {
            std::printf("FAIL 反面对照解包失败：%s\n", err.c_str());
            return 1;
        }
        std::string json;
        const bool exported = exportJson(b, json);
        if (exported && containsScaleRadius(json, nullptr)) {
            check(false, "反面对照失效：没有 v7 段却回写出了 ScaleRadius");
            return 1;
        }
        std::printf("ok   反面对照：没有 v7 事件段时回写直接失败/写不出 ScaleRadius（exported=%d）\n",
                    (int)exported);
    }

    if (g_fail) {
        std::printf("scale_radius_raw: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("scale_radius_raw: 全部通过\n");
    return 0;
}
