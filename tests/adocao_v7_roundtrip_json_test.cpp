// **无损性验收**：`.adocao` → 明文 `.adofai` → 重新打包，两个容器必须**逐字节相同**。
//
// 这是"容器没丢东西"最强的证据（比逐字段比对更强：任何顺序/格式差异都会让字节不同）。
// 回写器 `AdocaoJsonWriter` 只活在 `tests/` 里 —— ADOCAO 不做导出功能，它只是这条验收的工具。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "AdocaoJsonWriter.hpp"
#include "archive/AdocaoReader.hpp"
#include "archive/AdocaoWriter.hpp"
#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"

using namespace adofai;

static int g_fail = 0;

int main(int argc, char** argv) {
    archive::install();
    int ok = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        LevelData a;
        a.preserveRawJson = true;
        if (!a.loadFromFile(path)) { std::printf("FAIL %s: 读不了\n", path.c_str()); ++g_fail; continue; }
        adocao::PackOptions opts;
        opts.withV7Events = true;
        opts.v7IncludeLegacy = true;   // 回写需要全部事件
        std::vector<uint8_t> first;
        std::string err;
        if (!adocao::packLevel(a, first, err, nullptr, opts)) {
            std::printf("FAIL %s: 打包 %s\n", path.c_str(), err.c_str());
            ++g_fail;
            continue;
        }
        adocao::DerivedInfo derived;
        LevelData b;
        if (!adocao::unpackLevel(first.data(), first.size(), b, err, &derived)) {
            std::printf("FAIL %s: 解包 %s\n", path.c_str(), err.c_str());
            ++g_fail;
            continue;
        }
        std::string json;
        if (!adocao::writeLevelJson(b, json, err)) {
            std::printf("FAIL %s: 回写 %s\n", path.c_str(), err.c_str());
            ++g_fail;
            continue;
        }
        LevelData c;
        c.preserveRawJson = true;
        if (!c.loadFromString(json)) {
            std::printf("FAIL %s: 回写后的明文读不回来\n", path.c_str());
            ++g_fail;
            continue;
        }
        std::vector<uint8_t> second;
        if (!adocao::packLevel(c, second, err, nullptr, opts)) {
            std::printf("FAIL %s: 重新打包 %s\n", path.c_str(), err.c_str());
            ++g_fail;
            continue;
        }
        if (first != second) {
            std::printf("FAIL %s: 容器不逐字节相同（%zu vs %zu B）\n", path.c_str(), first.size(), second.size());
            ++g_fail;
            continue;
        }
        std::printf("ok   %-28s %6zu B 往返逐字节相同\n",
                    std::filesystem::path(path).filename().string().c_str(), first.size());
        ++ok;
    }
    std::printf("adocao_v7_roundtrip_json: %d 张通过，%d 项失败\n", ok, g_fail);
    return g_fail ? 1 : 0;
}
