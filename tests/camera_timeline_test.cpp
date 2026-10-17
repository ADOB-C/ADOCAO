// 相机时间线（core/camera/CameraTimeline）展开与排序的单元测试。
//
// 金标是**手算**的（公式来自 docs/camera.md §2.2/§2.3 + Re_ADOJAS 的 TimelineManager）：
//   时间 = tileStartTimes[floor] + (angleOffset/180) * (60/bpm[floor])
//   Beat 重复   ：floor 不变，angleOffset += interval * rep * 180
//   Floor 重复  ：executeOnCurrentFloor → 搬到目标砖；否则留在基砖，偏移 = entryBeat 差 × 180
//   entryBeat   ：eb[0] = -1（哨兵）、eb[j] = Σ_{f<j} 每砖拍数、最后一砖 0
// 这个用例把"哨兵 -1"也钉住了 —— 它是原版行为，看着像 bug，改了就和 ADOFAI 不一致。

#include "core/camera/CameraTimeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adofai;

namespace {

int g_fail = 0;

void expectEq(const char* what, double got, double want, int line) {
    if (std::fabs(got - want) > 1e-9) {
        std::fprintf(stderr, "FAIL line %d %s: got=%.12g want=%.12g\n", line, what, got, want);
        ++g_fail;
    }
}
void expectEqI(const char* what, long long got, long long want, int line) {
    if (got != want) {
        std::fprintf(stderr, "FAIL line %d %s: got=%lld want=%lld\n", line, what, got, want);
        ++g_fail;
    }
}
#define EXPECT_EQ(a, b)      expectEq(#a, (double)(a), (double)(b), __LINE__)
#define EXPECT_EQ_I(a, b)    expectEqI(#a, (long long)(a), (long long)(b), __LINE__)

// 11 块砖沿 +x，间距 1；每砖 1.0 s（BPM 120 的 180° 砖 = 2 拍），最后一块是 extra 砖。
LevelData makeLevel() {
    LevelData lv;
    lv.tiles.resize(11);
    for (int i = 0; i < 11; i++) {
        lv.tiles[i].position = {(double)i, 0.0};
        lv.tiles[i].direction = 180.0f;
    }
    return lv;
}

std::vector<double> makeTimes() {
    std::vector<double> t(11);
    for (int i = 0; i < 11; i++) t[i] = i * 1.0;
    return t;
}
std::vector<float> makeBpms() {
    return std::vector<float>(11, 120.0f);
}

LevelData::CameraEvent cam(int32_t floor, float angleOffset, const char* tag, uint32_t id, uint32_t order) {
    LevelData::CameraEvent e;
    e.floor = floor;
    e.angleOffset = angleOffset;
    e.id = id;
    e.order = order;
    e.tag = tag ? tag : "";
    return e;
}

void testNoRepeat() {
    LevelData lv = makeLevel();
    lv.cameraEvents.push_back(cam(2, 90.0f, nullptr, 1, 0));
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 1);
    EXPECT_EQ(tl.keys()[0].time, 2.0 + (90.0 / 180.0) * 0.5);   // 2.25
    EXPECT_EQ(tl.keys()[0].anchorX, 2.0);
    EXPECT_EQ(tl.keys()[0].bpm, 120.0f);
    EXPECT_EQ_I(tl.keys()[0].ev.floor, 2);
}

void testBeatRepeat() {
    LevelData lv = makeLevel();
    LevelData::RepeatEvent rp;
    rp.floor = 1;
    rp.tag = "cam";
    rp.repetitions = 3.0f;      // rep = 0,1,2,3
    rp.interval = 2.0f;         // 每次 +2*180° = 4 拍？不：2*180° = 360° = 2 拍 = 1.0 s
    rp.flags = 1;               // Beat 模式
    lv.repeatEvents.push_back(rp);
    lv.cameraEvents.push_back(cam(1, 0.0f, "cam", 1, 0));
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 4);
    const double want[4] = {1.0, 2.0, 3.0, 4.0};   // 每次 +360° = 2 拍 @120BPM = 1.0 s
    for (int i = 0; i < 4 && i < (int)tl.keys().size(); i++) {
        EXPECT_EQ(tl.keys()[i].time, want[i]);
        EXPECT_EQ_I(tl.keys()[i].ev.floor, 1);        // Beat 模式留在基砖
        EXPECT_EQ(tl.keys()[i].anchorX, 1.0);
    }
}

void testFloorRepeatImplicit() {
    LevelData lv = makeLevel();
    LevelData::RepeatEvent rp;
    rp.floor = 1;
    rp.tag = "cam";
    rp.repetitions = 2.0f;      // Floor 模式的 floorCount = 2 → 目标 1,2,3
    rp.gapLength = 1.0f;
    rp.flags = 0;               // Floor 模式
    lv.repeatEvents.push_back(rp);
    lv.cameraEvents.push_back(cam(1, 0.0f, "cam", 1, 0));
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 3);
    // entryBeat: eb[0]=-1, eb[1]=0, eb[2]=2, eb[3]=4（每砖 2 拍）
    // rep0: 0 拍 → 1.0；rep1: (eb2-eb1)*180=360° = 2 拍 → 2.0；rep2: (eb3-eb1)*180=720° → 3.0
    const double want[3] = {1.0, 2.0, 3.0};
    for (int i = 0; i < 3 && i < (int)tl.keys().size(); i++) {
        EXPECT_EQ(tl.keys()[i].time, want[i]);
        EXPECT_EQ_I(tl.keys()[i].ev.floor, 1);        // 留在基砖
    }
}

void testFloorRepeatExecuteOnCurrentFloor() {
    LevelData lv = makeLevel();
    LevelData::RepeatEvent rp;
    rp.floor = 1;
    rp.tag = "cam";
    rp.repetitions = 1.0f;      // floorCount = 1 → 目标 1,3
    rp.gapLength = 2.0f;
    rp.flags = 2;               // Floor + executeOnCurrentFloor
    lv.repeatEvents.push_back(rp);
    lv.cameraEvents.push_back(cam(1, 0.0f, "cam", 1, 0));
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 2);
    EXPECT_EQ(tl.keys()[0].time, 1.0);
    EXPECT_EQ_I(tl.keys()[0].ev.floor, 1);
    EXPECT_EQ(tl.keys()[1].time, 3.0);
    EXPECT_EQ_I(tl.keys()[1].ev.floor, 3);
    EXPECT_EQ(tl.keys()[1].anchorX, 3.0);            // 锚点跟着目标砖走
}

void testMultipleTagsFirstWins() {
    LevelData lv = makeLevel();
    LevelData::RepeatEvent a;
    a.floor = 1; a.tag = "noise"; a.repetitions = 9.0f; a.interval = 1.0f; a.flags = 1;
    LevelData::RepeatEvent b;
    b.floor = 1; b.tag = "cam";   b.repetitions = 1.0f; b.interval = 1.0f; b.flags = 1;
    lv.repeatEvents.push_back(a);
    lv.repeatEvents.push_back(b);
    lv.cameraEvents.push_back(cam(1, 0.0f, "x cam y", 1, 0));   // 空格分隔，命中 cam
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 2);                          // 用 b 的 1 次重复（0 和 1）
    EXPECT_EQ(tl.keys()[0].time, 1.0);
    EXPECT_EQ(tl.keys()[1].time, 1.5);                         // interval 1 拍 @120BPM = 0.5 s
}

void testInactiveAndEditorOnly() {
    LevelData lv = makeLevel();
    auto a = cam(2, 0.0f, nullptr, 1, 0); a.flags |= LevelData::kCamInactive;
    auto b = cam(3, 0.0f, nullptr, 2, 1); b.flags |= LevelData::kCamEditorOnly;
    auto c = cam(4, 0.0f, nullptr, 3, 2);
    lv.cameraEvents = {a, b, c};
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 1);
    EXPECT_EQ_I(tl.keys()[0].ev.floor, 4);
    EXPECT_EQ_I(tl.droppedInactive(), 2);
}

void testTieOrderByIdThenFileOrder() {
    LevelData lv = makeLevel();
    // 同一时刻（floor 2, angleOffset 0）：id 9 / id 3 / 无 id ×2
    auto a = cam(2, 0.0f, nullptr, 9, 0);
    auto b = cam(2, 0.0f, nullptr, 3, 1);
    auto c = cam(2, 0.0f, nullptr, 0xFFFFFFFFu, 2);
    auto d = cam(2, 0.0f, nullptr, 0xFFFFFFFFu, 3);
    lv.cameraEvents = {a, b, c, d};
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 4);
    EXPECT_EQ_I(tl.keys()[0].ev.id, 3);
    EXPECT_EQ_I(tl.keys()[1].ev.id, 9);
    EXPECT_EQ_I(tl.keys()[2].ev.order, 2);   // 无 id 的两个按文件顺序，排在 id 之后
    EXPECT_EQ_I(tl.keys()[3].ev.order, 3);
}

void testEmpty() {
    LevelData lv = makeLevel();
    CameraTimeline tl;
    tl.build(lv, makeTimes(), makeBpms());
    EXPECT_EQ_I(tl.keys().size(), 0);
    EXPECT_EQ_I(tl.empty(), 1);
}

}  // namespace

int main() {
    testEmpty();
    testNoRepeat();
    testBeatRepeat();
    testFloorRepeatImplicit();
    testFloorRepeatExecuteOnCurrentFloor();
    testMultipleTagsFirstWins();
    testInactiveAndEditorOnly();
    testTieOrderByIdThenFileOrder();

    if (g_fail) { std::fprintf(stderr, "camera_timeline: %d 处失败\n", g_fail); return 1; }
    std::printf("camera_timeline: 8 组用例通过（含 entryBeat 哨兵 -1、多 tag、id/文件序 tie-break）\n");
    return 0;
}
