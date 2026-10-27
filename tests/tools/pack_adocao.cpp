// 测试侧工具：把 .adofai 打成 .adocao（原来的 `adocao pack` 子命令）。
//
// 为什么搬到 tests/（AGENTS.md「本项目不该具备产出谱面的能力」）：
//   ADOCAO 是查看器，产品面不该有"把谱面写成文件"的入口；需要造数据就放 tests/tools，
//   这个二进制不随产品发布。搬出来顺带让产品不再链接 lzma **编码器**（实测 −56 KB）。
//
// 用法与原来的 `adocao pack` 一致：
//   pack_adocao <谱.adofai> <out.adocao> [--v7] [--v7-only] [--events=all|playable|minimal]
//                [--codec-report] [--checkpoints] [--schema-report]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "core/level/LevelData.hpp"
#include "core/level/LevelPath.hpp"
#include "archive/AdocaoDiagnostics.hpp"
#include "archive/AdocaoEventColumns.hpp"
#include "archive/AdocaoSchema.hpp"
#include "archive/AdocaoReader.hpp"   // v7 自检要 unpackLevel
#include "archive/AdocaoWriter.hpp"

using namespace adofai;

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    bool packV7 = false, packV7Only = false, codecReport = false, packCheckpoints = false;
    bool packSchemaReport = false;
    std::string packEventsPreset = "all";
    adocao::Compress packCompress = adocao::Compress::Auto;
    int packLevel = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] != '-') { pos.push_back(a); continue; }
        if (!std::strcmp(a, "--codec-report")) codecReport = true;
        else if (!std::strcmp(a, "--checkpoints")) packCheckpoints = true;
        else if (!std::strcmp(a, "--schema-report")) packSchemaReport = true;
        else if (!std::strcmp(a, "--v7")) packV7 = true;
        else if (!std::strcmp(a, "--v7-only")) { packV7 = true; packV7Only = true; }
        else if (!std::strncmp(a, "--events=", 9)) packEventsPreset = a + 9;
        else if (!std::strncmp(a, "--level=", 8)) packLevel = std::atoi(a + 8);
        else if (!std::strncmp(a, "--compress=", 11)) {
            const char* v = a + 11;
            if (!std::strcmp(v, "auto")) packCompress = adocao::Compress::Auto;
            else if (!std::strcmp(v, "zstd")) packCompress = adocao::Compress::Zstd;
            else if (!std::strcmp(v, "lzma") || !std::strcmp(v, "xz")) packCompress = adocao::Compress::Lzma;
            else if (!std::strcmp(v, "none") || !std::strcmp(v, "raw")) packCompress = adocao::Compress::None;
            else { std::fprintf(stderr, "--compress 只认 auto|zstd|lzma|none\n"); return 2; }
        }
        else { std::fprintf(stderr, "未知选项: %s\n", a); return 2; }
    }

    if (pos.size() < 2) {
        std::fprintf(stderr, "用法: pack_adocao <谱> <输出.adocao> [--v7] [--v7-only] "
                             "[--events=all|playable|minimal] [--codec-report] [--checkpoints]\n");
        return 2;
    }
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
                        lv.rawActionCount(), lv.rawDecorationsJson.size());
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
            for (size_t ai = 0; ai < lv.rawActionCount(); ++ai) {
                const std::string_view js = lv.rawAction(ai);
                rapidjson::Document d;
                d.Parse(js.data(), js.size());
                const char* t = (d.HasParseError() || !d.HasMember("eventType")) ? "" : d["eventType"].GetString();
                if (allow && !allow->count(t)) { ++dropped; continue; }
                evs.emplace_back(js);
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
                                         lv.rawActionCount() + (lv.rawDecorationsJson.empty() ? 0 : 1),
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
        kept.reserve(lv.rawActionCount());
        for (size_t ai = 0; ai < lv.rawActionCount(); ++ai) {
            const std::string_view js = lv.rawAction(ai);
            rapidjson::Document d;
            d.Parse(js.data(), js.size());
            const char* t = (d.HasParseError() || !d.HasMember("eventType")) ? "" : d["eventType"].GetString();
            if (allow.count(t)) kept.emplace_back(js);
            else ++droppedForV7;
        }
        // 过滤后的切片：内容变了，必须自持一份 blob（不能再指向 mmap 区）
        lv.rawBlob.clear();
        lv.rawActionSpans.clear();
        lv.rawActionSpans.reserve(kept.size());
        for (const auto& js : kept) {
            const uint32_t off = (uint32_t)lv.rawBlob.size();
            lv.rawBlob += js;
            lv.rawActionSpans.push_back({off, (uint32_t)js.size()});
        }
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
        // **允许没有 v7 段**：去重后可能一条事件都不剩（整张谱只有 Pause/SetSpeed 之类，
        // 它们全由 v6 段承载）。那不是错误，只是"这张谱 v7 无话可说"。
        if (chk.v7SchemaBytes.empty() && chk.v7ColumnsBytes.empty()) {
            std::printf("v7 自检: 无事件需要 v7 段（全部由 v6 段承载）\n");
        } else if (chk.v7SchemaBytes.empty() || chk.v7ColumnsBytes.empty()) {
            std::fprintf(stderr, "v7 自检失败：段缺失\n");
            return 1;
        } else {
            std::printf("v7 自检: schema %zu B + 事件列 %zu B%s\n", chk.v7SchemaBytes.size(),
                        chk.v7ColumnsBytes.size(),
                        chk.v7CoverageBytes.empty() ? "（未裁剪）" : "（含 Coverage）");
        }
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

    return 0;
}
