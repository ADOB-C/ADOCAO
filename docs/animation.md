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
- [ ] 与 Re_ADOJAS 逐通道数值对拍（harness 已就绪）；像素门槛已按新行为重存（56/56）

**实测（2025，1080p MSAA4，播放 4 s，`ADOCAO_PROFILE=1`）**：`frame=0.871 ms` 均值
（`vis=0.180`、`animXform=0.218`、`cull=0.426`）—— 新的关键帧采样没有带来可测的帧时回退。

## 3.1 appear 动画的 t=0 初始状态（2026-10-31 修）

症状（用户原话）：**"不应该看见的看见了"** —— 谱面开头整条轨道都画出来了，而它本该随着行星
前进逐块浮现。

`TileAnimation::build` 末尾有一段**通用**的 t=0 基准帧：任何通道只要第一帧在 t>0，就在 t=0
补一个 base 值（位移/旋转 0、缩放/透明度 1）。它修的是 MM2 那个"未来有一条 `scale=0` 的关键帧
→ `Channel::sample` 在 t=0 就返回 0 → 整条轨道从第一帧起消失"的坑。

但它和 appear 动画**语义冲突**：参考实现每个 appear case 的第一句都是

```ts
this.addKeyframe(entity, 'opacity', 0, 0, null);   // case 'Fade'
```

它写的是**"还没出现"的状态（0）**，不是 base（1）。我们的基准帧把 t=0 钉成 1，于是淡入窗口
之前那块砖是**可见**的，直到 `appearStart` 才突然掉到 0 再淡回来。

修法：建帧时记下每块砖的 appear 类型（`appearPins`），**基准帧补完之后**再按类型覆盖 t=0：
Fade → `opacity 0`；Extend/Grow/Grow_Spin → `scale 0`；Assemble(_Far) → 随机位移与旋转；
Drop/Rise → `positionY ±8` 且 `scale 0`。disappear 不碰 t=0（MM2 那类只靠 disappear 的砖
仍然吃基准帧，不会被误判成不可见）。

实测（2.64e6 那张，spec：`trackAnimation: Fade` / `beatsAhead: 8` / `beatsBehind: 0`）：
tile 20 的 opacity 关键帧从 `0.000:1.000 …` 变成 `0.000:0.000 6.082:0.000 6.454:1.000 …`，
`hiddenBefore` 由 0 变 1；t=2.0 zoom12 那一帧 1280x720 少画约 **11,300 px（1.23%）**——
修前看到的是一条长轨道，修后只剩行星前后约 9 块砖。

**为什么门槛当时没红**：55 个状态全贴得很近（`singularity_mid` zoom 25、`sr_radius_ramp`
zoom 400），视野里就那几块砖，全在 8 拍以内 → 这条路径零覆盖。补了 `trackvis_fade_ahead`
（同一张谱 t=2.0、zoom 12）。

### 3.1.1 建帧上限不是"防呆"，是预算（2026-10-31）

用户当天的原话：**"我之前就说你的各种东西都太保守了吧"**。这句话对应三个真 bug，
全是"用一个小常数把极端情况静默截断"：

| 常数 | 原值 | 症状 | 现在 |
|---|---|---|---|
| `progress` 的时长阈值 | `> 0.0001` s | 砖 < 100 µs 时行星被钉在砖终点 → 轨迹变折线 | `> 0.0`（见 `docs/level-loading.md`） |
| `TrailSamplingConfig::maxPoints` | 8192 | 2^18 的采样率被砍成 20480/s | 2^18 |
| `kMaxAnimatedTiles` / `kMaxKeyframes` | 500k / 4M | 2.64e6 那张谱（997,664 砖）**后半张没有关键帧**，全局 Fade 只作用在前一半 | 4M / 20M |

最后一条的实测：改完后 **997,664 砖全部建帧、4,988,361 条关键帧 = 77.9 MB**（`截断=0`），
峰值 RSS 799 MB（这谱本来就在 520 MB 量级）。像素门槛 57/57 不变、ctest 53/53。

规则（写给以后的自己）：**上限只能是"有实测代价的预算"，不能是拍脑袋的阈值**；
一旦触发必须 `LOG_W` 说出来、并且在文档里记下"多大谱会触发、触发后画面错在哪"。
参考实现只限制"**没有任何 AT 事件** + 超过 100,000 砖的纯全局动画"（`GLOBAL_ANIM_TILE_LIMIT`），
我们那条 `skipGlobalAnim` 与它一致，别再加更严的。

### 3.2 第一条关键帧之前要回落 base（2026-10-31，同上一条的后半）

上一条修完立刻炸出下一个症状：**"暂停的时候什么都没有了"**。

两个原因叠在一起：

* `Channel::sample()` 在**第一条关键帧之前**返回首帧的值。appear 的首帧是"还没出现"（0），
  于是整条轨道在 t<0 就是黑的；
* `LevelScene::updateTileVisibility()` 把这个负时间**夹成了 0** —— 夹完正好落进
  "首帧=0"的区间，于是开场/暂停必黑。

参考实现的语义相反：`TimelineManager.sample()` 在首帧之前返回 `undefined`，
**调用方回落到实体的 base 值**（透明度 1 = 可见）。所以：

* `Channel::sampleOr(t, base)` 新增（`sample()` 保持原样，别的地方还在用）；
  `TileAnimation::sample()` 与 `hiddenAt()` 全部改走它；
* `hiddenAt()` 对 `t < 0` 直接返回"不可见 = 否"（我们的关键帧都在 t ≥ 0，t<0 只可能是倒计时）；
* `updateTileVisibility()` 不再把负时间夹到 0（旧注释那句"倒计时按 t=0 处理"正是在这个
  夹取上栽的 —— 它想避免的黑屏，恰恰是这个夹取造成的）。

复现口径：2.64e6 那张谱 `--capture-time -1`（= 停止态 `timeInLevel = -preRoll`）——
修前整屏只剩行星，修后整条轨道都在；`--capture-time 0.3` 仍是"行星前后约 9 块砖"。
`track_visibility` 的 ④ 钉这一条（负向对照：把 `sampleOr` 换回 `sample` → opacity 0 ≠ 1，
立刻 FAIL）。

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
