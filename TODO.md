# TODO

> **目标见 `docs/2e9.md`**（21 亿层：内存与 n 解耦 / 任意 seek / 不掉帧）。
> **本文件只放"还没做的、能动手的"** —— 每条 ≤3 行、给出处。设计/实测/历史不在这里：
> 分析进 `docs/`，做过的进 `git log`。（2026-11-02 从 918 行压到 100 行以内：
> 原来混着 349+139 行的设计分析、75 行已完成历史，还有几条早就不成立的旧账 ——
> 长分析已搬到 `docs/timeline-checkpoints.md`。）

## 我这条线（渲染 / 动画 / 时间线）

- [x] ~~**PositionTrack 的 `rotation` / `scale` / `opacity`**~~ —— **2026-11-02 已实现**。
      权威 `PositionTrackManager.ts`（b4590e4）：`tileX = v; if (!justThisTile) curX = v;`，
      消费侧 `Player.ts:6855` 把它当 mesh 的基础变换。实现：解析 → `TilePositionOffset` 富字段 →
      `applyPositionTrackOffsets` 里按**持久状态 + 本层事件值**累积 → **稀疏 run/override 表**
      （`m_posRuns` / `m_posOv`，不占 12 B/砖）→ `posTrackXformAt()` → `tileXformAt()` 叠加。
      验收：`tests/position_xform_test.cpp`（继承 / justThisTile 不推进持久状态 / 通道独立 / 消极对照）。
      **像素门槛：57 个状态里只有 `myc_t30_z25` 变**（diff=464 px，maxdelta=187）——
      原因是 MYC 第 1123 层 `scale: 90`，那块砖现在按 90% 画（三联图 `build/myc_pos_ab.png` 验过），
      属**真缺口修复**，基线已重存。
- [x] ~~**PositionTrack 的 `relativeTo`**~~ —— **2026-11-02 已实现（含位置算法）**。
      解析：`[offset,"ThisTile"|"Start"|"End"]`（`IDFromTile` 三种语义 + clamp）。
      算法：权威是两遍，现在 `applyPositionTrackOffsets` 也是两遍 ——
      Pass 1 只算 change（读**未烘**的原始砖位 + `bitSum`/`point` 查询，`vector` 按权威只在
      非 justThisTile 后更新），Pass 2 才烘进砖位。事件排序改成 `stable_sort`（同层顺序 = 文件序）。
      踩到的坑：`stringValue(p, vEnd, out)` 的 `vEnd` 必须是**这个字符串**的末尾（它做
      `assign(p+1, vEnd-1)`），传整个数组末尾会拿到 `End"]` 这种残渣 → 类型永远落回 ThisTile。
      验收：`tests/position_xform_test.cpp` 的 relativeTo 段（ThisTile +2 / Start −4 / End 继承）；
      像素门槛 **57/57 逐字节不变**（全语料 61,865 条都是 `[0,"ThisTile"]` = target 指向自己 → 恒 0），
      正好证明这条新路径不会动到现有谱面。

- [ ] **Hold / Release**（DLC 长按）：每个 hold 一个 mesh（uniform 驱动填充）+ `holdLength`。
- [ ] **Bloom / Flash**：官方 Hidden/VideoBloom，`MediumAmount` 定标 = **0.28**（参照 Re_ADOJAS）。
- [x] ~~**建帧 ~700 ms 拆三段**~~ —— **2026-11-02 结案：那 700 ms 是测量时的机器争用，不是回退**。
      同一探针、同一张谱，今天（load 1.6）稳定 **372~386 ms**，与最早的 361~383 ms 吻合；
      拆开看（`ADOCAO_PROFILE=1` 的 `anim-prof:` 行，2.64e6）：
      **MoveTrack 0.0 ms + AnimateTrack 181.2 ms + buildFlipEvents 193.3 ms（3,990,674 条事件）**
      —— 事件表在建帧里占一半，那是我后来加"rangeStart/rangeEnd 边界翻转"把事件数翻倍的结果。
      教训：**测量要和构建/其它作业错开**，我上一轮在 `load 2.2` 就下结论"不是负载"，其实
      5/15 分钟均值还有 6.0（自己的全量重编还没散），于是白查了两个对照。

- [ ] **MultiPlanet ≥3 星：每砖边界两颗星跳 1 砖** —— **等裁决**：参考实现同样跳，
      修 = 主动偏离权威。根因见 `git log --grep=MultiPlanet`。

## 加载 / 内存（2e9 主线）

- [ ] **加载期常驻内存**：MYC 走真无头 `adocao image` 的实测口径见 `docs/scale-1e8-to-2e9.md`。
- [ ] **Timeline 检查点化**（2^31−1 层三笔账之一）：设计与步骤在 `docs/timeline-checkpoints.md`。
- [ ] **`tiles` 按块物化 provider（切片 4）**：同上，`docs/timeline-checkpoints.md` 末节。

## `.adocao` v3 编码候选（细节与实测见 `docs/adocao-format.md` §9）

- [ ] ② `WrapDelta(mod 360)`：13.60 MB → **0.62 MB（22×）**
- [ ] ②' `Varint` / `RleVarint`（**不做差分**）
- [ ] ③ `Motif`（窗口字典，傅里叶/epicycle）：合成实测 **75~220×**
- [ ] ④ `Predict(k)` + 残差后端（**86×**；残差只在整数域算、闭链、每块复位）
- [ ] ⑤ `FixedPoint`（二进制网格，100×+）/ `XorFloat`（全精度漂移 ~1.9×）
- [ ] ⑨ 段级 `min(zstd, xz)`；零拷贝（`Raw` + 对齐直读）

## `.adocao` v7

- [ ] **段内按块压缩**：唯一能真省窗口加载时间的办法（块间不共享上下文 → 体积会涨，先量）。
- [ ] v7 加载仍比明文慢 ~4 ms（lzma ≈2.3 + 列解码 ≈2）：只剩"热列快解"，收益小、改动大。
- [ ] 把窗口解码接进 `unpackLevel`（现在仍全量解码后再 ingest）。
- [ ] `Coverage` 在应用里提示（打开被裁剪的容器时说清缺了哪些类型）。
- [ ] 体积：2025 `all` 72.9 KB → 目标 60 KB（已否掉的优化见 `docs/adocao-format.md` §9.3）。

## 别人的线

- [ ] **并行模型 ①–⑥ + LOD 预算**（`perf` session，见 `docs/parallelism.md`）。

## 小配置项（低优先）

- [ ] 帧率上限从编译期常量改成运行时配置；缩放范围可配置；剔除边距可配置。

## 已排除（别再查）

- **Tile 索引与行星位置"差 100 倍"**：拿**空间距离**当成了"走过的砖数"，折叠轨道上两者本就不同。
- **`adocao_window` 在 Linux 偶发红**：FMA 收缩（GCC `fast` vs Clang `on`），已全局 `-ffp-contract=off`。
- **PositionTrack 同层多事件只生效最后一个**：结构早就是 `vector` + `push_back`，逐事件累加。
- **状态字段的 `justThisTile` 语义**：权威确认"只改本砖、不推进持续状态"，我们同构。
- **`rangeGrace`**：0~0.5 观测不到出图差异（保留原值）；**`maxKeyframes`** 曾是死旋钮，已改成真限制器。
- **拖尾"sagitta 半径恒 1"**：`curvatureRadius()` 已改用 `orbitRadiusAt()`（ScaleRadius）。

## 明确不做（有理由，别再提）

- **装饰物渲染**（2026-10-22 拍板）：深度依赖 Unity shader/材质/timeline，数据已在 v7 里不会丢。
- **无头视频**（构想）：缺口在离屏 GL 上下文 + 无音频设备时的时间源。
- **WinUI3 / SwiftUI 启动器**：独立仓库 / 纯构想。
- **MoveTrack 的 tile 位置/旋转/缩放动画**：仅记录，不排期。
