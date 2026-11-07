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

- [x] ~~**Hold / Release**（DLC 长按）~~ —— **2026-11-03 已实现**（`Release` 权威里就没有这个事件）。
      * 解析：逐砖 `holdLength = duration`、**无继承**、末砖跳过；稀疏表（语料只有 3 个文件用到）。
        **两条解析路径都要认** —— 只改快路径时 Fledgling（非法尾逗号走 DOM 路径）的 Hold 被丢，
        带子永远不建，这是本次踩的坑。
      * 渲染：`render/HoldBands` —— 每块 Hold 砖一条 ribbon，圆心从砖位**线性平移**到
        `下一砖 − rTail·dir(exitAngle)`，半径 rStart→rTail，角度扫完 totalAngle（含整圈），
        宽度 `max(0.15, 0.275·(rStart+rTail)/2)`，UV.y 沿路径，进度只改 uniform。
      * 与参考的差异：官方那圈**彩虹贴图**我们没有，用"球色 + 中央白线"近似。
      * 验收：`tests/hold_test.cpp`（解析）+ 像素门槛新增 **hold_band_mid / hold_band_done**
        两个状态（Fledgling floor 2768，真实谱面）—— 门槛现在 **59/59**，ctest **56/56**。

- [x] ~~**拖尾画成横贯屏幕的直条**~~ —— **2026-11-07 修好**。根因不是渲染：`positionAtTile`
      **漏了长按滑动圆心**（`positionAt` 有）。于是拖尾采样停在砖上（-54.99），而拖尾**末点**用的
      `frame.*Position`（走 `positionAt`）已经滑到 -24.99 —— 差恰好一个 `holdDistance`（30 单位），
      这段距离全摊在最后一条边上 → 一条横贯全屏的直条（Fledgling t=254/255/256 分别差 20/30/40）。
      修法：抽出 `applyHoldSlide()`，两个求值器**共用同一份**。验收：末点与末采样**逐位相等**；
      `ADOCAO_TRAIL_CHECK` 0/321 差异（`FAST` 也是 0）；ctest 57/57；像素门槛 59/59 逐字节不变。
  * 教训：`positionAt` 与 `positionAtTile` 是**两份代码**，任何"只在一处实现"的几何量都会让
    拖尾末点与采样分家。`ADOCAO_TRAIL_CHECK` 现在会**断言这两者一致**（负向对照验过：去掉那一步
    立刻 `LOG_E`，差值 29.999919）。
  * 另一个把这条藏了很久的原因：t=254~256 的谱面本身是全黑（`--no-trail` 纯黑），**只有拖尾可见**，
    所以看到的就是那根直条；宽视角（`--capture-zoom 20`）下才看得出正常时它只是一小段弧。
- [ ] **`trail_parity_test` 复用同一个 `TrailTileConsts`**：现在每次都新建，所以
      "跨砖复用时旧字段没清零"（`slideX/slideY` 那次的 50 单位 bug）这类错它**抓不到** ——
      改成模拟真实循环（一个 struct 反复填）才覆盖得到。

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

- [x] ~~长按（Hold）~~ —— **2026-11-03 全部落地**：解析（逐砖/无继承/末砖跳过/两条解析路径）、
      扫角 `+holdLen·2π`（时长变长）、`holdDistance` 推砖、滑动圆心（行星/相机/拖尾）、
      长按带渲染（彩虹贴图按官方 `holdTex.png` 实测特征程序生成）、透明度跟随起点砖。
      门槛新增 `hold_band_mid`（t=252.2）与 `hold_band_done`（t=250.5）两个状态，**59/59**。
      教训三条：① 着色器改动可能被资产包覆盖，要用 `glGetShaderSource` 看**真正附着**的那份；
      ② 长按段行星/拖尾必须走同一个求值器（`trailPointAt` 那份副本踩了两次）；
      ③ 抓图"丢带子"其实是抓在了谱面自己的黑闪里（t=254）。

## 别人的线

- [ ] **并行模型 ①–⑥ + LOD 预算**（`perf` session，见 `docs/parallelism.md`）。

## 小配置项（低优先）

- [ ] 帧率上限从编译期常量改成运行时配置；缩放范围可配置；剔除边距可配置。

## 已排除（别再查）

- **空格停止后"看不见轨道"**（2026-11-07）：空格是**停止并回到 t=0**。两个坑：
  ① 那一步以前只 `snapTo` 了位置（后来干脆改成**相机原地保留** —— 用户要"还看着刚才那一块"），
  但 `resetToInitialView()` 仍给"加载完"那一处用，别让两处再各写一遍；
  ② 逐砖可见性只靠 TrackVis 分支那句"恢复 `0..m_lastHiddenEnd`"，**keyframe 模型那条路根本没还原**
  → 停下后该显示的砖不显示。现在 `LevelScene::resetVisualState()` 全量还原（无位图时**不分配**，
  2^31 层是 268 MB）。实测 Fledgling：播放中 `drawn` 0~2（谱面自己把砖藏了），停止后 `drawn` 62~215。

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
