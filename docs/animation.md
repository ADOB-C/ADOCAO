# 动画系统重写（对齐 Re_ADOJAS）

> 2026-10-21 用户拍板：现有 `AnimationTrack` 是"可见性时间窗 + 每帧解析式重算"，语义上就不对，
> **必须重写**成 Re_ADOJAS 的**逐砖关键帧时间轴**（`TimelineManager`）。

## 1. 现在的实现错在哪（不是补丁能修的）

| 语义 | 现有实现 | Re_ADOJAS（权威） |
|---|---|---|
| 数据模型 | `appearTime/disappearTime` 两个时间点 + `animAt()` 每帧算增量 | 每砖 6 条**关键帧时间轴**：`positionX/Y`、`rotation`、`scaleX/Y`、`opacity` |
| 旋转 | 在砖自身朝向上**再加** `rotationOffset` | `mesh.rotation.z = rotationOffset`，**绝对角**（覆盖自身朝向） |
| 旋转枢轴 | 砖心 | **砖起点**（几何原点） |
| 消失 | 到 `disappear` 时刻就把砖从可见集里删掉 | 动画播完（`Scatter/Shrink/Fade` 的 tween 结束）才透明/缩到 0 |
| 事件叠加 | MoveTrack 链式 from→target | `Kill(complete)`：旧 tween 先跳到自己终点，新 tween 从那里开始 |
| 优先级 | MoveTrack 与 AnimateTrack 各自为政 | **先建 MoveTrack、后建 AnimateTrack**，后者覆盖前者 |
| 场景外可见性 | 时间窗判定 | 砖一直在场景里，只是 `opacity/scale` 被动画改 |

后果就是 2026-10-21 看到的两个现象：`R`（Plum - R）结尾 `Scatter_Far` **看不见散开**；
`R` 的 MoveTrack 位置/朝向对不上。

## 2. 目标数据模型（逐字照抄 Re_ADOJAS）

```cpp
template <int N> struct Channel {           // 一条属性的关键帧序列（按 time 升序）
    struct Key { double time; float value; uint8_t ease; };  // ease = 255 → 瞬时（无 tween）
    std::vector<Key> keys;
    float sample(double t) const;           // 二分 + easeApply（不 clamp 越界值）
    void addKeyframe(double t, float v);    // 覆盖同刻
    void addTween(double t0, double t1, float from, float to, Ease e,
                  float fallback);          // Kill(complete) + push
};
struct TileAnim {
    Channel<0> posX, posY, rot, scaleX, scaleY, opacity;
    double rangeStart, rangeEnd;            // 该砖动画活跃区间（含 0.1 s 宽限）
};
```

- 只给**真的有动画的砖**建时间轴（`std::vector<std::optional<TileAnim>>` 或稀疏 map），
  否则超大谱会直接 OOM —— Re_ADOJAS 用 `GLOBAL_ANIM_TILE_LIMIT = 100000` 兜底，我们照抄。
- 采样是 O(log k)，每帧只对**活跃砖**做（`rangeStart <= t < rangeEnd`，按 start 排序 + 前缀 max end 二分）。
- 内存账：一条关键帧 16 B；Plum（730 砖 / 128 个事件）≈ 几千帧 → 几十 KB。
  2³¹ 层目标下不能全量建，**只对稀疏事件覆盖的砖建**，其余砖走"无动画"快路径（现有分支保留）。

## 3. 进度（2026-10-21）

- [x] `core/timeline/Keyframes.{hpp,cpp}` + ctest `keyframes`（11 条断言）
- [x] `core/timeline/TileAnimation.{hpp,cpp}`：MoveTrack + AnimateTrack **建帧**
      （稀疏索引、零分配采样、上限保护）；ctest `tile_animation` 与旧路径逐值对拍
- [x] 渲染接线：`trackXformAt` 走 `TileAnimation::sample`；无动画的砖仍走零成本快路径
- [x] 删掉"时间窗隐藏"：砖一直在场景里，靠 opacity/scale 归零隐藏；
      逐帧只把"完全透明且缩到 0"的动画砖标不可见（`hiddenAt()` 区间外 O(1) 缓存）
- [ ] 删除 `m_tileAppearTimes/m_tileDisappearTimes` 与 `recomputeWindow`（现在只是不再用于可见性，
      仍在 build 里算，属于可删的冗余）
- [ ] 与 Re_ADOJAS 逐通道数值对拍（harness 已就绪）；像素门槛已按新行为重存（54/54）

**实测（2025，1080p MSAA4，播放 4 s，`ADOCAO_PROFILE=1`）**：`frame=0.871 ms` 均值
（`vis=0.180`、`animXform=0.218`、`cull=0.426`）—— 新的关键帧采样没有带来可测的帧时回退。

## 4. 迁移步骤（每一步都要能验收）

1. **核心**：`core/timeline/Keyframes.{hpp,cpp}`（通道/采样/Kill-complete）+ ctest
   `keyframes_test`（手工算例：覆盖、tween 中途、`Kill(complete)`、越界缓动）。
2. **MoveTrack 建帧**：把 `LevelData::moveTransformAt` 的链式设置换成"逐砖建关键帧"，
   旋转改绝对角、枢轴改砖起点（已于 2026-10-21 单独修过，这里改为由关键帧直接给出）。
3. **AnimateTrack 建帧**：把 `Timeline::animAt` 的解析式改成建帧（appear/disappear 各类型），
   继承规则沿用 2026-10-21 已对齐的版本（`hasAA/hasDA`、`flag2`、`num5/6`、`×pitch`）。
4. **渲染**：`trackXformAt` 改为 `sampleAll(tile, t)`；**删掉** `m_tileAppearTimes/m_tileDisappearTimes`
   与窗口重算（`recomputeWindow`），可见性只保留"活跃区间"这一层（纯性能，不影响出图）。
5. **验收**：
   * `keyframes_test` 逐值对拍；
   * headless harness（`tools/ref-ab/headless/`）对 `R` / 2025 / 2021 / 2026 采样 6 条通道逐值比较；
   * 像素门槛按新默认重存（会变，属于预期）；
   * `--show-all-tiles` 与 Re_ADOJAS `#/editor` 并排图。

## 5. 明确不做

- 不引入 Re_ADOJAS 的 `Mesh`/three.js 概念；只借它的**时间轴语义**。
- 不为 2³¹ 层做全量关键帧；超大谱按 `GLOBAL_ANIM_TILE_LIMIT` 跳过全局动画（与参考一致），
  MoveTrack 的大范围事件仍用现有块表 + 稀疏建帧，后续可进 `.adocao` 的 Derived 段。

## 时间单位（2026-10-24 连着踩了三处）

**谱面里所有 "duration / beatsAhead / beatsBehind / angleOffset 的拍数" 都按"一拍 = 60/bpm"换算，
不是"这一砖走完要多久"。** 后者是 `Timeline::durationAt(i)` = `(转角/180) x 一拍`，
在 90° 砖上只有一半、U-turn 上是两倍。

| 事件 | 正确公式（= Re_ADOJAS） | 曾经写成 |
|---|---|---|
| `MoveTrack` trigger | `startTimeAt(floor) + (angleOffset/180) * 60/bpm` | `durationAt(floor)` |
| `MoveTrack` duration | `durBeats * (60/bpm) / pitch` | `durBeats * durationAt(floor) / pitch` |
| `RecolorTrack` trigger | `tileTime + (angleOffset/180) * (60/bpm)` | 同上错法 |
| `TweenColor` duration | `durBeats * (60/bpm)` | `dur * durationAt(floor)` |
| `Flash` | `dur * (60/bpm) / pitch` | ✓ 本来就对（2026-10-18 修过） |
| `AnimateTrack` 出现/消失 | `60/bpm / pitch` + `beatsAhead/Behind` | ✓ 本来就对 |
| `MoveCamera` | `(60/(bpm*pitch))` | ✓ 本来就对 |

**症状**（用户口径）："有些砖慢慢飘走，有些快速飞走" —— 因为 `durationAt/一拍` 在 0.5~2 之间，
同一张谱里不同角度的砖会按不同倍率缩放时长。

**护栏**：`tests/anim_timing_test.cpp`（ctest `anim_timing`）用 120 BPM + 混排角度的合成谱钉死
三类事件：2 拍必须正好 1.0 s、`angleOffset 90°` 必须正好晚 0.25 s；并带**反面对照**
（90° 砖错算成 0.5 s、U-turn 错算成 2.0 s）。
