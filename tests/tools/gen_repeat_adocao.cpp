// 测试侧工具：把一张谱面**重复 N 遍**写成 `.adocao`（不经过 JSON、不产生大文件）。
//
// 为什么在 tests/ 而不是产品里（AGENTS.md:243）：
//   ADOCAO 是**查看器**，不该具备"产出谱面"的能力。需要造谱面数据就放 tests/，
//   这里的二进制不随产品发布。
//
// 用途：Won't You 20×（10.305959 s/遍）→ 20 遍 = 206.12 s，配 `…20x20.ogg`。
//
// 关键语义：**每遍开头必须重置 BPM**。图谱的 SetSpeed 是连续状态，直接把 actions 平移后
// 接起来的话，第 2 遍会接着第 1 遍结尾的 BPM 跑（实测 tile 7,000,000 处 t=724 s 而不是 10.4 s）。
// 所以这里在每一遍的第一砖注入一条 `SetSpeed(speedType=Bpm, beatsPerMinute=settings.bpm)`。
//
// 内存：只复制 angleData + actions（写容器用不到 tiles/位置，它们由解码方重算），
//       实测 20 遍（1.354 亿砖）峰值 ~6 GB，不起 swap。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/level/LevelData.hpp"
#include "archive/AdocaoWriter.hpp"
#include "archive/Install.hpp"
#include "core/timeline/Timeline.hpp"

using namespace adofai;

int main(int argc, char** argv) {
    adofai::archive::install();   // 解码 .adocao（--inspect）需要注册后端
    std::string levelPath, outPath;
    int repeat = 20;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--level") && i + 1 < argc) levelPath = argv[++i];
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else if (!std::strcmp(argv[i], "--song") && i + 1 < argc) { /* 见下 */ }
    }
    // 诊断：`--inspect <file.adocao> --floor N` 打印该层附近的所有 SetSpeed
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--inspect") && i + 1 < argc) {
            std::string f = argv[++i];
            int64_t around = 0;
            for (int j = 1; j + 1 < argc; ++j)
                if (!std::strcmp(argv[j], "--floor")) around = std::atoll(argv[j + 1]);
            LevelData lv;
            if (!lv.loadFromFile(f, nullptr)) { std::fprintf(stderr, "打不开 %s\n", f.c_str()); return 1; }
            int shown = 0, setspeed = 0;
            for (const auto& a : lv.actions) {
                if (a.type != LevelData::FastAction::SetSpeed) continue;
                ++setspeed;
                if (std::llabs((long long)a.floor - (long long)around) <= 3 && shown < 12) {
                    std::printf("SetSpeed floor=%d val1=%.6g flag=%d\n", a.floor, (double)a.val1, (int)a.flag);
                    ++shown;
                }
            }
            std::printf("共 %zu 条 action / %d 条 SetSpeed（附近打印 %d 条）\n", lv.actions.size(), setspeed, shown);
            Timeline tl;
            tl.build(lv);
            const int64_t cnt = lv.tileCount();
            std::printf("砖数 %lld\n", (long long)cnt);
            const int64_t perUnit = (cnt - 1) / 2;
            {
                long double s1 = 0, s2 = 0;
                int64_t firstDiff = -1;
                for (int64_t L = 0; L < 1123; ++L) {
                    const double d1 = tl.durationAt(L), d2 = tl.durationAt(L + perUnit);
                    if (firstDiff < 0 && d1 != d2) firstDiff = L;
                    s1 += d1; s2 += d2;
                }
                std::printf("  copy1[0..1122] 合计 %.12Lf  copy2 %.12Lf  差 %.6Lg  首差 tile %lld\n",
                            s1, s2, s2 - s1, (long long)firstDiff);
            }
            for (int64_t L = perUnit - 3; L <= perUnit + 2; ++L) {
                if (L < 0 || L >= cnt) continue;
                std::printf("  边界 tile %lld: ang=%.1f dur=%.9g t=%.9f\n", (long long)L,
                            (double)lv.angleAt(L), (double)tl.durationAt(L), tl.startTimeAt(L));
            }
            for (int64_t L = 0; L < 4; ++L) {
                std::printf("  tile %lld: ang=%.3f dur=%.9g | tile %lld: ang=%.3f dur=%.9g\n",
                            (long long)L, (double)lv.angleAt(L), (double)tl.durationAt(L),
                            (long long)(L + perUnit), (double)lv.angleAt(L + perUnit),
                            (double)tl.durationAt(L + perUnit));
            }
            const int64_t locals[] = {1123, 27473, 53729, 4000000, 6770910};
            for (int64_t L : locals) {
                if (L + perUnit >= cnt) continue;
                std::printf("local %lld: bpm1=%.6g bpm2=%.6g  t1=%.6f t2=%.6f (Δt=%.6f)\n",
                            (long long)L, (double)tl.tileBPMs()[(size_t)L],
                            (double)tl.tileBPMs()[(size_t)(L + perUnit)], tl.startTimeAt(L),
                            tl.startTimeAt(L + perUnit), tl.startTimeAt(L + perUnit) - tl.startTimeAt(L));
            }
            int64_t probes[] = {0, (int64_t)around, cnt - 1};
            for (int64_t p2 : probes) {
                if (p2 < 0 || p2 >= cnt) continue;
                std::printf("floor %lld: bpm=%.6g  t=%.6f\n", (long long)p2,
                            (double)tl.tileBPMs()[(size_t)p2], tl.startTimeAt(p2));
            }
            return 0;
        }
    }
    if (levelPath.empty() || outPath.empty() || repeat < 1) {
        std::fprintf(stderr,
                     "用法: gen_repeat_adocao --level <谱.adofai> --repeat N --out <out.adocao>\n");
        return 2;
    }

    LevelData base;
    if (!base.loadFromFile(levelPath, nullptr)) {
        std::fprintf(stderr, "加载失败: %s\n", levelPath.c_str());
        return 1;
    }
    const int64_t n = base.tileCount();
    if (n < 2) { std::fprintf(stderr, "砖数太少\n"); return 1; }
    std::printf("基础谱: %lld 砖, %zu 条 action, bpm=%.6g\n", (long long)n, base.actions.size(),
                (double)base.settings.bpm);

    // 每遍实际占的砖数 = angleData 长度 + 1 = n（共享交界砖），所以一遍的位移是 n-1 个角度。
    // 事件 floor 的偏移量必须用 **per**（= n-1），用 n 会让后面每遍整体错开 k 砖。
    const int64_t per = n - 1;
    std::vector<double> angles;
    angles.reserve((size_t)per * (size_t)repeat);
    for (int k = 0; k < repeat; ++k)
        for (int64_t i = 0; i < per; ++i) angles.push_back(base.angleAt(i));

    // 方向状态（Twirl）也要在每遍开头复位：Twirl 是**翻转**语义，如果一遍里的 Twirl 是奇数条，
    // 下一遍的转向就整体镜像（实测 copy2 的 90° 砖变成 270°、时长 ×3）。偶数则天然对齐。
    size_t twirls = 0;
    for (const auto& a : base.actions)
        if (a.type == LevelData::FastAction::Twirl) ++twirls;
    const bool injectTwirl = (twirls % 2) != 0;
    std::printf("Twirl %zu 条（%s）→ %s\n", twirls, injectTwirl ? "奇数" : "偶数",
                injectTwirl ? "每遍开头补一条 Twirl 复位方向" : "方向天然对齐，不补");

    std::vector<LevelData::FastAction> actions;
    actions.reserve(base.actions.size() * (size_t)repeat + (size_t)repeat);
    for (int k = 0; k < repeat; ++k) {
        const int64_t off = (int64_t)k * per;
        if (k > 0) {
            // 每遍开头把 BPM 重置回 settings.bpm（绝对值事件）
            LevelData::FastAction reset;
            reset.type = LevelData::FastAction::SetSpeed;
            reset.floor = (int)off;
            reset.val1 = base.settings.bpm;
            reset.flag = false;   // false = beatsPerMinute（绝对值）
            actions.push_back(reset);
            if (injectTwirl) {
                // 放在**交界砖的前一砖**：Twirl 会改变它所在砖的 totalAngle（实测放在交界砖上
                // 会让每遍多出 3 ms = 一块 180° 砖的时长），放前一砖则只翻转"进入下一遍"的方向，
                // 不改变任何砖的转角。
                LevelData::FastAction tw;
                tw.type = LevelData::FastAction::Twirl;
                tw.floor = (int)off;
                actions.push_back(tw);
            }
        }
        for (const auto& a : base.actions) {
            LevelData::FastAction c = a;
            c.floor = (int)((int64_t)a.floor + off);
            actions.push_back(c);
        }
    }

    LevelData rep;
    rep.settings = base.settings;
    rep.settings.songFilename = "Won't You Make a Song with Me 20x20.ogg";
    rep.actionStrTable = base.actionStrTable;
    rep.angleData = std::move(angles);
    rep.actions = std::move(actions);

    // 基础谱的映射/数组先放掉，给打包腾内存
    base = LevelData{};

    std::vector<uint8_t> out;
    std::string err;
    adocao::PackOptions opts;   // 默认：不写 checkpoints、auto 压缩
    adocao::PackResult res;
    if (!adocao::packLevel(rep, out, err, &res, opts)) {
        std::fprintf(stderr, "packLevel 失败: %s\n", err.c_str());
        return 1;
    }
    std::FILE* f = std::fopen(outPath.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "写不了 %s\n", outPath.c_str()); return 1; }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("已写出 %s（%.2f MB，%d 遍 × %lld 砖 = %lld 砖）\n", outPath.c_str(),
                (double)out.size() / (1024.0 * 1024.0), repeat, (long long)n, (long long)n * repeat);
    return 0;
}
