// `.ilytx`（Re_ADOJAS 自研容器 = xz(tar(ilybin))）的读取用例。
//
// 样本是**用 Re_ADOJAS 自己的编码器**生成的（`tests/gen_ilytx_fixtures.mjs` 直接 import
// 他们仓库里的 `ilybin.ts` / `tar.ts`）—— 绝不在这边手抄格式再自产自销，否则"自己写、
// 自己读"的对拍永远是绿的（AGENTS.md 的教训）。
//
// 三件事：
//   ① 等价性：`ilytx_object.ilytx` 必须与参照谱 `ilytx_ref.adofai` 加载出**逐位相同**的
//      level（angleData 按位 / actions 全字段 / settings / tiles / 派生数组）；
//   ② 紧凑模式：`ilytx_compact.ilytx`（direction 走 f32）角度只有 f32 量化误差，其余相同；
//   ③ 坏数据：缺 level.ilybin / tar 被截断 / ilybin magic 被改坏 → **必须干净失败**，
//      而不是静默产出一份残缺谱面（这正是 ilytx 与 .adocao 共同的纪律）。
//
// 用法：adocao_ilytx_test <level_fixtures 目录>

#include "archive/Install.hpp"
#include "core/level/LevelData.hpp"

using namespace adofai;

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool loadBytes(const std::vector<char>& bytes, LevelData& lv) {
    if (bytes.empty()) return false;
    return lv.loadFromBuffer(bytes.data(), bytes.size(), nullptr, false);
}

// angleData 必须**逐位**相同（不是"近似"）：ilytx 的 direction 段就是 angleData 的值，
// 走 f64 / 缩放差分都应是位精确往返。
bool sameAngles(const LevelData& a, const LevelData& b) {
    if (a.tileCount() != b.tileCount()) return false;
    for (int64_t i = 0; i < a.tileCount(); ++i) {
        const double x = a.angleAt(i), y = b.angleAt(i);
        if (std::memcmp(&x, &y, sizeof(double)) != 0) return false;
    }
    return true;
}

double maxAngleDiff(const LevelData& a, const LevelData& b) {
    double worst = 0;
    for (int64_t i = 0; i < a.tileCount() && i < b.tileCount(); ++i)
        worst = std::max(worst, std::fabs(a.angleAt(i) - b.angleAt(i)));
    return worst;
}

bool sameActions(const LevelData& a, const LevelData& b) {
    if (a.actions.size() != b.actions.size()) return false;
    for (size_t i = 0; i < a.actions.size(); ++i) {
        const auto &x = a.actions[i], &y = b.actions[i];
        const float f1x = x.val1, f1y = y.val1, f2x = x.val2, f2y = y.val2;
        if (x.floor != y.floor || x.type != y.type || x.flag != y.flag) return false;
        if (std::memcmp(&f1x, &f1y, 4) != 0 || std::memcmp(&f2x, &f2y, 4) != 0) return false;
        if (a.actionStr(x) != b.actionStr(y)) return false;      // 同一事件串（含 hitsound 名）
    }
    return true;
}

bool sameSettings(const LevelData& a, const LevelData& b) {
    const auto &s = a.settings, &t = b.settings;
    return s.version == t.version && s.bpm == t.bpm && s.offset == t.offset
        && s.countdownTicks == t.countdownTicks && s.zoom == t.zoom && s.rotation == t.rotation
        && s.relativeTo == t.relativeTo && s.position == t.position && s.hitsound == t.hitsound
        && s.hitsoundVolume == t.hitsoundVolume && s.trackColor == t.trackColor
        && s.secondaryTrackColor == t.secondaryTrackColor && s.backgroundColor == t.backgroundColor
        && s.beatsBehind == t.beatsBehind && s.beatsAhead == t.beatsAhead;
}

// 每砖派生量（位置/角度/图标位）逐位相同 —— 这是"渲染结果一致"的充分条件
bool sameTiles(const LevelData& a, const LevelData& b) {
    if (a.tileCount() != b.tileCount()) return false;
    for (int64_t i = 0; i < a.tileCount(); ++i) {
        const auto pa = a.tilePositionAt(i), pb = b.tilePositionAt(i);
        if (std::memcmp(&pa[0], &pb[0], 8) != 0 || std::memcmp(&pa[1], &pb[1], 8) != 0) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: adocao_ilytx_test <level_fixtures 目录>\n");
        return 2;
    }
    adofai::archive::install();          // 解压/容器后端（core 只认接口，要显式注册）
    const std::string dir = argv[1];

    // ---- 参照谱 ----
    LevelData ref;
    const auto refBytes = readFile(dir + "/ilytx_ref.adofai");
    check(!refBytes.empty(), "参照谱 ilytx_ref.adofai 读得到");
    check(loadBytes(refBytes, ref), "参照谱（明文 .adofai）加载成功");
    std::printf("参照谱: %lld 层 / %zu 个事件 / bpm=%.3f\n", (long long)ref.tileCount(),
                ref.actions.size(), (double)ref.settings.bpm);

    // ---- ① 对象模式：与参照谱逐位相同 ----
    {
        LevelData lv;
        const auto bytes = readFile(dir + "/ilytx_object.ilytx");
        check(!bytes.empty(), "ilytx_object.ilytx 读得到");
        check(loadBytes(bytes, lv), "ilytx（对象模式）加载成功");
        check(lv.tileCount() == ref.tileCount(), "层数与参照谱相同");
        check(sameAngles(lv, ref), "angleData **逐位**相同（direction 段位精确往返）");
        check(sameActions(lv, ref), "actions 全字段相同（floor/type/flag/val1/val2/事件串）");
        check(sameSettings(lv, ref), "settings 相同（bpm/offset/zoom/颜色/位置…）");
        check(sameTiles(lv, ref), "每砖位置**逐位**相同（派生几何一致）");
    }

    // ---- ② 紧凑模式：f32 量化误差 ----
    {
        LevelData lv;
        const auto bytes = readFile(dir + "/ilytx_compact.ilytx");
        check(!bytes.empty(), "ilytx_compact.ilytx 读得到");
        check(loadBytes(bytes, lv), "ilytx（紧凑模式）加载成功");
        check(lv.tileCount() == ref.tileCount(), "紧凑模式层数相同");
        const double worst = maxAngleDiff(lv, ref);
        std::printf("紧凑模式 angleData 最大偏差: %.3g 度（f32 量化）\n", worst);
        check(worst < 1e-3, "紧凑模式角度只有 f32 量化误差（< 1e-3 度）");
        check(sameActions(lv, ref), "紧凑模式 actions 仍然全字段相同");
        check(sameSettings(lv, ref), "紧凑模式 settings 仍然相同");
    }

    // ---- ②b 小数角度（第一组全是整数，f32 恰好精确 → 量化那条路根本没被走到）----
    {
        LevelData fracRef;
        const auto rb = readFile(dir + "/ilytx_frac_ref.adofai");
        check(!rb.empty(), "小数角参照谱 ilytx_frac_ref.adofai 读得到");
        check(loadBytes(rb, fracRef), "小数角参照谱加载成功");

        LevelData obj;
        const auto ob = readFile(dir + "/ilytx_frac_object.ilytx");
        check(loadBytes(ob, obj), "小数角 ilytx（对象模式）加载成功");
        check(sameAngles(obj, fracRef), "小数角 angleData **逐位**相同（to_chars 最短往返足够）");
        check(sameTiles(obj, fracRef), "小数角每砖位置**逐位**相同");

        LevelData cmp;
        const auto cb = readFile(dir + "/ilytx_frac_compact.ilytx");
        check(loadBytes(cb, cmp), "小数角 ilytx（紧凑模式）加载成功");
        const double worst = maxAngleDiff(cmp, fracRef);
        std::printf("小数角紧凑模式 angleData 最大偏差: %.3g 度\n", worst);
        check(worst > 0.0, "紧凑模式**确实**是 f32 量化过的（否则这条用例没测到 f32 路径）");
        check(worst < 1e-3, "f32 量化误差 < 1e-3 度（物理不可见）");
        check(sameActions(cmp, fracRef) && sameSettings(cmp, fracRef),
              "紧凑模式 actions/settings 不受 f32 影响");
    }

    // ---- ③ 坏数据必须干净失败 ----
    {
        const char* bad[] = {"ilytx_missing_level.ilytx", "ilytx_truncated.ilytx",
                             "ilytx_badmagic.ilytx"};
        for (const char* name : bad) {
            LevelData lv;
            const auto bytes = readFile(dir + "/" + name);
            check(!bytes.empty(), (std::string(name) + " 读得到").c_str());
            const bool ok = loadBytes(bytes, lv);
            check(!ok, (std::string("坏样本 ") + name + " **必须**加载失败（不许静默产半份谱）").c_str());
        }
    }

    if (g_fail) {
        std::printf("\n%d 项失败\n", g_fail);
        return 1;
    }
    std::printf("\n全部通过\n");
    return 0;
}
