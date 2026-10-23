# 摄像机（Re_ADOJAS 权威语义 + 移植基准）

> **权威顺序**：`Re_ADOJAS`（作者 Xbodwf）与官方游戏/编辑器指南/任何其他实现冲突时，一律以它为准
> （AGENTS.md「权威顺序」）。本文是把它**读透**的结果，不是另一种实现方案。
> 本机克隆：`~/Repos/Re_ADOJAS`，本文对应 commit **6ccf519**（2026-10-16）。
> 选这个 commit 的理由：camera 那条线在 `315ed55`（2026-09-13，official camera/decoration parity）
> 才大修完，`3b13075`（2026-10-06，hold 跟随）是最后一条核心改动，之后只剩兼容性修复 ——
> 也就是说**语义已经固化**，读它不会边读边变（这正是我们等 Xbodwf 的原因）。
> 主要源码：`src/lib/Player/CameraController.ts`（643 行）、`Player.ts` 的
> `updateCameraFollow`/`syncCameraFromController`、`TimelineManager.ts` 的事件展开。
>
> **方法**：逐行读代码 + **用 node 直接跑 `CameraController.ts` 做探针实测**（Node 26 的
> type-stripping 能直接加载 `.ts`，只需把三个 import 的扩展名补成 `.ts`）。§8 的所有数字都是
> 探针跑出来的，不是推的。踩过的坑见 §9，移植清单见 §10。

---

## 0. 一页速查

**一句话**：ADOFAI 的摄像机 = **一台绝对定位的 rig（`camParent`）+ 一个只会在 rig 里做"定点追赶"的
局部层（`pos`）**；`MoveCamera` 事件只驱动 rig 上的 4 条独立 tween（x / y / 旋转 / 缩放），
而"跟着球走"是局部层每块砖重新起步的一次 2 拍线性追赶。两者相加才是相机世界坐标。

```
相机世界坐标 = camParent + pos          （Rotation / Zoom 各自独立，不进这个和）

camParent  ← MoveCamera 的 4 条 tween（互相独立、可被打断、打断时"跳到终点"）
pos        ← followMode 时：每块砖从上一帧 pos 出发，用 camspeed（= 2 拍）线性追 topos
             followMode=false 时：pos = 0（此时 camParent 就是绝对世界坐标）
```

**6 个公式（全部要用 double）**

| 量 | 公式 | 说明 |
|---|---|---|
| 事件时间 | `tileStartTimes[floor] + (angleOffset/180) * (60/bpmFloor)` | 180° = 1 拍；bpmFloor 已含 SetSpeed |
| 事件时长 | `duration * 60/(bpmFloor * pitch)` | `duration` 的单位是**拍**；pitch = settings.pitch/100 |
| 跟随速度 | `camspeed = 60/(bpmCurrent * pitch) * 2` | **当前砖**的 BPM，不是事件的 bpmFloor |
| 跟随插值 | `pos = frompos + (topos + offset + holdOffset - frompos) * clamp01(timer/camspeed)` | 线性，不是指数阻尼 |
| 缩放 | `zoomSize = event.zoom/100`；对外 `adoZoom = zoomSize*100` | 100 = 默认 |
| 上屏 | `camera.zoom = (100/adoZoom) * userZoomMultiplier` | 视野高 5.47 砖 @adoZoom 100 |

**每帧固定 7 步**（顺序不能换，§3）：
回退守卫 → 处理所有到期事件（逐事件先 `applyTweens(事件时刻)`）→ 切砖判定（`updateFollowCam`）
→ `timer += dt` → `applyTweens(本帧时刻)` → 跟随层 lerp → 交给 Player 上屏。

**5 种 `relativeTo`**：`Player`（rig 存行星相对偏移）/ `Tile`（rig 存世界坐标，锚在事件所在砖）/
`Global`（世界绝对）/ `LastPosition`（rig 累加，且继承当前旋转）/ `LastPositionNoRotation`（累加，不继承）。

---

## 1. 两层模型

### 1.1 两层坐标（+ 两个标量）

源码顶部注释直接点明这是原版 `scrCamera` + `ffxCameraPlus` 的移植：

```
camParent (rig, 绝对世界位置)   ← MoveCamera tween
camera.localPosition (跟随层)   ← Lerp(frompos, topos, timer / camspeed)
```

- **`camParent`**：rig。`followMode=false` 时它就是**相机世界坐标**；`followMode=true`（Player 模式）时
  它是**相对枢轴行星的偏移**（所以 `settings.position = [1,0]` 表示"永远站在枢轴右边 1 格"）。
- **`pos`**：跟随层。Player 模式下它自己会收敛到行星世界坐标，于是 `camParent + pos` 才是世界坐标。
- **`camAngle` / `zoomSize`**：不参与上面那个和。旋转与缩放是相机自己 transform 的一部分，
  所以只改旋转/缩放的运镜**完全不碰 rig**（相机位置仍由跟随层决定；探针 E 里 zoom 200→50 时位置
  照常跟随，I 里 rot/zoom 瞬变而位置只由事件给的 pos 决定）。

### 1.2 `followMode` 到底表示什么

| `followMode` | `pos` | `camParent` 含义 | 谁会设置 |
|---|---|---|---|
| `true` | 追赶行星世界坐标 | 行星相对偏移 | 初始 / 任何 `relativeTo: Player` 事件 |
| `false` | 恒 0 | 绝对世界坐标 | 任何 `relativeTo: Tile` / `Global` 事件 |

两种模式互切时**都要做一次坐标换算**，保证画面不跳（§4.6）：

- `false → true`（进 Player）：`camParent -= planetWorldPos`，`pos = planetWorldPos`，`timer = 0`；
- `true → false`（进 Tile/Global）：`camParent = camParent + pos`（即当前世界坐标），`pos = 0`。

> 注意换算用的 `planetWorldPos` 是**本帧**的行星位置，不是事件时刻的。正常播放时二者差一帧或为零，
> 但 `seek()` 里传的是**目标时刻**的行星位置（§6.4），所以"seek 到中途"和"从头播到中途"在
> 含 free↔follow 切换的谱面上可能不逐位相同。

### 1.3 状态量清单（照着建结构体即可）

| 字段 | 类型 | 作用 |
|---|---|---|
| `camParent` | {x,y} | rig；绝对或行星相对，看 `followMode` |
| `camAngle` | deg | 旋转，直接给相机 |
| `zoomSize` | ×（1 = 100） | 缩放，对外 ×100 |
| `frompos` / `topos` / `pos` | {x,y} | 跟随层三件套 |
| `timer` | s | 跟随层已追赶时长 |
| `followMode` | bool | 见 §1.2 |
| `offset` | {x,y} | **死字段**：只在 lerp 里被加，全库没有任何写入点，恒 0（别给它编语义） |
| `holdOffset` | {x,y} | 长按滑动偏移，Player 每帧写（§5.3） |
| `lastEventRelativePosition` | {x,y} | 上一个 `Tile`/`Global` 事件的锚点（§4.6 的怪路径用它） |
| `lastUsedMovementType` | 枚举 | 事件没写 `relativeTo` 时沿用 |
| `lastTileCamFloor` | int | 上一次 `Tile` 事件的砖号（去重判定用） |
| `legacyRelativeTo` | bool | 来自 `settings.legacyCamRelativeTo`（§4.7） |
| `followMovingPlatforms` | bool | **恒 false**，见 §5.4 |
| 4×`TweenState` | {active,startTime,duration,from,to,ease} | moveX / moveY / rotation / zoom |
| `cameraTimeline` | `{time, event}[]` | 预计算时间线 |
| `lastCameraTimelineIndex` | int | 游标；-1 = 一个都还没处理 |
| `lastFollowTile` | int | 上次触发 `updateFollowCam` 的砖号 |

### 1.4 为什么必须是两层（而不是"一个目标点 + 平滑"）

因为两者**时间常数不同、语义也不同**：

- 运镜（rig）由 `duration`（拍）控制，可被下一个运镜**截断**，且截断语义是"旧 tween 先跳到它的终点"；
- 跟随（local）永远存在、永远 2 拍、永远从上一帧位置重新起步，连"相机被运镜搬到别处"这件事
  它都会在下一次切砖时接管。

把两者合成一个"目标 + 阻尼"会让"运镜中途切砖"和"运镜结束后跟随从哪继续"都算错。原版把
`camParent` 和 `camera.localPosition` 分开，正是为了这两个自由度互不干扰。

---

## 2. 时间线

### 2.1 事件从哪来：`TimelineManager`，不是 `CameraController`

`Player.loadLevel()` 里：

```ts
const expandedCamera = this.timelineManager.cameraEvents.map(e => ({
    time: e.time,
    event: { ...e.event, floor: e.floor, angleOffset: e.angleOffset },
}));
this.cameraController.loadCameraTimeline(expandedCamera);
```

也就是说 `MoveCamera` 的**时间**是在 `TimelineManager` 里算的（`src/lib/Player/TimelineManager.ts`
的 `_cameraEvents`），`CameraController` 只收结果。

### 2.2 时间公式与"哪个 BPM"

```ts
const bpm = this.tileBPM[floor] || 100;
const secPerBeat = 60 / bpm;
const eventTime = this.tileStartTimes[floor] + (angleOffset / 180) * secPerBeat;
```

- `angleOffset` 单位是**度**，180° = 1 拍，可以为负（事件发生在砖之前）。
- `tileBPM[floor]` 已经折进了 `SetSpeed` 的倍率；`pitch` **不**在这里折算（它在事件解码和跟随速度里各折一次）。
- `floor` 是**事件宿主砖**，不是它"看起来该作用在"的砖。
- `floor 0` 且 `angleOffset = 0` 的事件 `t = tileStartTimes[0] = 0` = 倒计时开始（`timeInLevel` 0 =
  countdown 起点），所以 floor 0 的运镜往往在倒计时期间就演完了。

> 同一个"BPM 归属"在别处也出现过：`Flash`/`MoveTrack` 用它算时长，`SetSpeed` 改的是它。
> **事件时长用 `bpmFloor`，跟随速度用 `bpmCurrent`** —— 这两个不是同一个数（§5.1）。

### 2.3 排序与 `RepeatEvents`

```ts
// TimelineManager：先按 time 升序，时间差 < 0.1 ms 时按 event.id 升序
this._cameraEvents.sort((a, b) => {
    const dt = a.time - b.time;
    if (Math.abs(dt) < 0.0001) return (a.event.id ?? Infinity) - (b.event.id ?? Infinity);
    return dt > 0 ? 1 : -1;
});
```

- **同刻事件靠 `id` 定序**（不是数组顺序）。`id` 缺失时按 `Infinity` 排最后。
- `RepeatEvents` 的展开发生在**这里之前**（`TimelineManager` 的 `eventsToProcess`），所以
  `CameraController` 看到的是**已经展开完的每一次重复**。展开规则（对运镜要紧的三条）：
  - Beat 模式：事件留在**基砖**，`angleOffset += interval * rep * 180`（时间按拍平移）；
  - Floor 模式：`executeOnCurrentFloor` 时事件搬到目标砖、`angleOffset` 用 0；
    否则留在基砖，`angleOffset` 用**几何拍差**（`entryBeat` 之差 × 180），与路径上的变速无关；
  - 事件 `eventTag` 是**空格分隔的多个 tag**，逐个查表、命中第一个就 break；
  - `MoveCamera` **不在** `REPEAT_SOLO_TYPES` 里（那份表是 Twirl/Pause/Hold/… 只应用第一次），
    所以它的每一次重复都会真的重放一遍 —— 用重复做的运镜必须按"重复 N 次"算，别按一次算。

### 2.4 `buildCameraTimeline()` 是死代码 —— 别照着它移植

`CameraController.ts:181` 还留着 `buildCameraTimeline(tileCameraEvents: Map<number, any[]>)`，
它做的是旧版逻辑（按 floor 分组 + `angleOffset=0` 的事件加 0.1 ms 微增量）。**全库没有调用点**
（`grep buildCameraTimeline` 只命中它自己和 `.bak`），实际路径见 §2.1。

两者的差别不是风格问题：旧版**没有 `RepeatEvents` 展开**，也没有 TimelineManager 那套
"Beat/Floor 模式 + entryBeat 几何拍差"。照着旧版写会漏掉所有重复运镜。

---

## 3. 每帧 `update()` 的顺序（不能换）

```ts
update({ nowSeconds, deltaSeconds, bpm, pitch, planetWorldPos, currentTileIndex }) {
  // 1. 回退守卫：时间倒退到游标事件之前 → 整体重置（游标回 -1，下一帧重放全部）
  if (lastCameraTimelineIndex >= 0 && nowSeconds < timeline[lastCameraTimelineIndex].time)
      resetCameraState();

  // 2. 处理所有到期事件（可能一次跨好几个）
  while (next.time <= nowSeconds) {
      idx++;
      applyTweens(entry.time);        // ← 先把旧 tween 求值到"这个事件的时刻"
      startEffect(entry, floor, entry.time, floorPos, planetWorldPos,
                  tileBPM[floor] || bpm, pitch);
  }

  // 3. 切砖才更新跟随目标（不是每帧）
  if (currentTileIndex !== lastFollowTile) { lastFollowTile = currentTileIndex; updateFollowCam(planetWorldPos); }

  // 4. 跟随速度用"当前砖"的 bpm 与 pitch
  const camspeed = (60 / (bpm * pitch)) * 2;

  // 5. 计时器先加，再求值（切砖那一帧就吃掉一个 dt）
  timer += deltaSeconds;
  applyTweens(nowSeconds);

  // 6. 跟随层
  if (followMode) {
      const t = clamp01(timer / camspeed);
      pos = frompos + (topos + offset + holdOffset - frompos) * t;
  } else pos = {0,0};
}
```

四个"顺序敏感"的地方：

1. **事件先于切砖**：同一帧里"运镜到期"和"切砖"同时发生时，运镜先改 rig，切砖再改跟随目标。
2. **`applyTweens(entry.time)` 用的是事件时刻**，不是本帧时刻。所以一帧跨多个事件时，每个事件
   看到的"旧 tween 当前值"都是它自己该看到的那一刻 —— 这是"低帧率下运镜仍然按时间线演"的来源。
3. **`timer += dt` 在 lerp 之前**：切砖那一帧的 lerp 已经走了 `dt/camspeed`（探针 B' 实测：`dt=0.1`
   时第一帧就是 0.1）。想复现逐帧一致，这条不能"顺手改成先算后加"。
4. **`applyTweens` 每帧至少调两次**（事件循环里每个事件一次、最后统一一次），所以同一个 tween 在
   同一帧里会被求值多次（每次用不同的 `now`）—— 幂等，不影响结果，但别以为只算了一次；也别在
   `applyTweens` 里塞"有累计副作用"的东西。

---

## 4. `startEffect()`：一个事件怎么变成 4 条 tween

### 4.1 字段与单位

| 字段 | 单位 | 缺省 | 语义 |
|---|---|---|---|
| `duration` | **拍** | 0 | `0` = 瞬移（`ease` 无效）；>0 = tween |
| `ease` | 字符串 | `Linear` | 见 `Easing.ts`（32 个：In/Out/InOut × Sine/Quad/…/Bounce + `Linear`/`Unset`）；查不到就按 `Linear` |
| `position` | **砖**（`TILE_SIZE = 1.0`） | 无 | `[x,y]`，可单轴为 `null`（§4.3） |
| `rotation` | **度** | 无 | 绝对赋值（`LastPosition` 例外，§4.6） |
| `zoom` | **百分比** | 无 | 100 = 默认；`zoomSize = zoom/100` |
| `relativeTo` | 字符串或 `0..4` | 无 | `0 Player / 1 Tile / 2 Global / 3 LastPosition / 4 LastPositionNoRotation`；字符串不在表里 → `undefined` |
| `angleOffset` | 度 | 0 | **只用于计时**（§2.2），不进 `startEffect` 的数值 |
| `active` | bool/字符串 | true | `false` / `"Disabled"` → 整条跳过（`isEventActive`） |
| `editorOnly` | bool | false | 为 true 整条跳过 |
| `disabled.{position,rotation,zoom,relativeTo}` | bool | false | 为 true 表示**该字段不生效**（`isFieldEnabled`） |

> `disabled` 是编辑器的"字段级过滤器"，历史谱面里没有；`position: [null,null]` 是另一套机制（§4.3/4.4）。

### 4.2 Decode

```ts
const crotchet = 60 / (bpm * pitch);          // 一拍秒数
const duration = (Number(event.duration) || 0) * crotchet;
const ease = event.ease || 'Linear';

const pos = event.position;
const positionUsed = Array.isArray(pos) && isFieldEnabled(event,'position');
// 分量缺失 → NaN（不是 0）；positionUsed 为 false 时两轴都是 NaN
const component = (v) => (v !== null && v !== undefined) ? Number(v) * TILE_SIZE : NaN;
const targetPos = {
    x: positionUsed ? component(pos[0]) : NaN,
    y: positionUsed ? component(pos[1]) : NaN,
};
const rotationUsed = event.rotation != null && isFieldEnabled(event,'rotation');
const targetRot   = rotationUsed ? Number(event.rotation) : 0;
const zoomUsed    = event.zoom != null && isFieldEnabled(event,'zoom');
const targetZoom  = (zoomUsed ? Number(event.zoom) : 100) / 100;
```

**"字段没写" 与 "字段写了 0" 是两件事**：`rotationUsed=false` 时旋转完全不动；`rotation: 0` 时旋转被
tween 到 0。zoom 同理（`zoom: 100` ≠ 没写 zoom）。

### 4.3 逐轴 `NaN`

`position` 的两个分量各自可以是 `null`。未使用的分量保持 `NaN`，**后面所有 `startTween` 都被
`isNaN` 挡住**，于是该轴完全不动：

```ts
if (positionUsed || movementTypeUsed) {
    if (!isNaN(finalPos.x)) startTween(moveXTween, camParent.x, finalPos.x, …);
    if (!isNaN(finalPos.y)) startTween(moveYTween, camParent.y, finalPos.y, …);
}
```

这是 commit 13adbab 修的 bug：以前未使用的分量写 0，`position:[null,5]` 会把 X 拖回 0。

**但有一个例外**：只要事件同时带了 `relativeTo`（`movementTypeUsed === true`），NaN 会被**归零**
（`CameraController.ts:462`），于是该轴的目标变成 0。所以：

| 写法 | X 轴行为 |
|---|---|
| `position:[null,5]`（无 `relativeTo`） | 不动 |
| `position:[null,5]` + `relativeTo:"Global"` | 归零 |

### 4.4 去重（dedup）：让 `[null,null]` 真的"什么都不做"

在算目标之前有一段**条件取消**（`CameraController.ts:435`）：

```ts
if (movementTypeUsed
 && movementType ∉ {Global, LastPosition, LastPositionNoRotation}
 && positionUsed
 && (isNaN(targetPos.x) || isNaN(targetPos.y))
 && movementType === lastUsedMovementType
 && (movementType !== 'Tile' || floorIndex === lastTileCamFloor))
    movementTypeUsed = false;
```

效果（探针 K/L 验过，相机一动不动）：

- 连续两个 `Player` 事件，第二个写 `[null,null]` → 不归零到行星；
- 同一砖上连续两个 `Tile` 事件，第二个写 `[null,null]` → 不跳回砖心。

**它只在"和上一次是同一种模式"时生效**：换成别的模式时 `[null,null]` 仍会走该模式的目标计算
（例如 `Global` → `Player` + `[null,null]` 会真的把 rig 归零）。`Tile` 模式还额外要求**同一块砖**。

### 4.5 `Kill(complete: true)`：抢占是"跳到终点"，不是"从当前位置拐弯"

每个事件在写目标前，先把它会碰的轨道**结束掉**，结束方式是**直接赋成该 tween 的 `to`**：

```ts
killTween(tw) { if (tw.active) { setValue(tw.to); tw.active = false; } }

// 谁会被 kill：
if (positionUsed || movementTypeUsed) { kill(moveX, v => camParent.x = v); kill(moveY, …); }
if (rotationUsed || (movementTypeUsed && isLastPosition)) kill(rotation, v => camAngle = v);
if (zoomUsed) kill(zoom, v => zoomSize = v);
```

**x / y 是两条独立轨道**：只给 Y 的事件不会碰 X（探针 §8-A/F 里能看到 X 与 Y 各自插值）。
`kill` 之后 `camParentBefore` 才被取快照，新 tween 从"被杀后的值"起步。

探针 C 的实测（这是最容易看错的一条）：

```
Global: (0,0) → (10,0)，5 s；t=2.0 被"1 s 回 (0,0)"抢占
t=1.983 → x=3.967
t=2.000 → x=10.000   ← 瞬跳：旧 tween 先 complete
t=2.500 → x=5.000
t=3.000 → x=0.000
```

所以**"运镜中途被新事件打断"在 ADOFAI 里是可见的瞬跳**，不是平滑衔接。别"优化"成从当前位置续。

### 4.6 五种 `relativeTo`

`startEffect` 的骨架：先算 `vector`（事件目标，NaN 保留）与 `vector2`（实际送进 tween 的目标），
再按模式决定 `finalPos`。

```ts
const camParentBefore = {camParent.x, camParent.y};
const vector2 = positionUsed
    ? vector                          // 写了 position：用它
    : { lastEventRelativePosition.x - camParentBefore.x,   // 没写 position：怪路径（见下）
        lastEventRelativePosition.y - camParentBefore.y };
```

| 模式 | 进入时 | 目标 `finalPos` | 副作用 |
|---|---|---|---|
| `Player` | 若当前是 free：`camParent -= planet`，`pos = planet`，`followMode = true`，`frompos = topos = planet`，`timer = 0` | `vector2`（行星相对偏移） | 跟随层开始追行星 |
| `Tile` | 若当前是 follow：`camParent = 相机世界坐标`，`pos = 0`，`followMode = false` | `vector2 + 砖位(floor)` | `lastEventRelativePosition = 砖位`；`lastTileCamFloor = floor` |
| `Global` | 同上（若 follow 则转 free） | `vector2`（世界绝对） | `lastEventRelativePosition = {0,0}` |
| `LastPosition` | **不切模式、不换算** | `vector + camParentBefore`（累加；未写 `position` 则原地不动） | `rotationOffset = camAngle`（继承当前旋转） |
| `LastPositionNoRotation` | 同上 | 同上 | 不继承旋转 |

四点必须记住：

1. **`Tile` 的目标是"砖位 + position"，不是"当前相机 + position"**。同一砖上连写两个
   `Tile` 事件（都写了 `position`），第二个会覆盖成同一个绝对点（不是相对第一个再挪）。
2. **`Tile`/`Global` 的起点是"相机当前世界坐标"**，所以从跟随态切过去时画面不跳（探针 A：事件时刻
   相机还在追上一块砖，起点就是那个"落后"的位置，然后才开始运镜）。
3. **`LastPosition` 是唯一"累加"模式**，且继承了当前角度 —— 所以它适合做"从当前位置继续往前推"。
   它**不**把 `camParent` 从相对量换算成世界量，因此只在 free 模式下语义清晰；在 follow 模式里用它
   会得到一个"行星相对偏移的累加"。
4. **没写 `position` 但写了 `relativeTo` 的事件**走 `vector2` 的怪路径：
   `lastEventRelativePosition - camParentBefore`（然后 `Tile` 再加砖位）。探针 F 实测：

   ```
   t=1.0  Tile, position [2,1], floor 2   → 相机 (4,1)（= 砖位 (2,0)+(2,1)）
   t=2.0  Player, 无 position, floor 4    → 相机 (2,-1)   ← 不是"留在原地"，也不是 (4,0)
   ```

   这条路径在正常编辑器产出的谱里几乎不会出现（编辑器总会写 `position`），但**别假设它不存在**。

### 4.7 `legacyCamRelativeTo`

`settings.legacyCamRelativeTo: true` 且事件**没写 `relativeTo`** 时，走一条老语义：

```ts
finalPos = positionUsed ? vector + camParentBefore : camParentBefore;   // 累加！
```

也就是"事件里的 position 是**相对当前相机的增量**"，而且不切模式、不换算。探针 J（两个 `[3,0]`）：

```
legacy=false → x = 3      （绝对赋值，两次写同一个点）
legacy=true  → x = 6      （累加）
```

注意它**只在 `relativeTo` 缺失时**生效；写了 `relativeTo` 就正常走 §4.6。

### 4.8 四条 tween 的创建条件（一张表）

| 轨道 | kill 条件 | 创建条件 | 起点 | 终点 |
|---|---|---|---|---|
| moveX | `positionUsed \|\| movementTypeUsed`（且该轴目标非 NaN 或 movementTypeUsed） | 同 kill 条件 且 `finalPos.x` 非 NaN | `camParent.x`（kill 后） | `finalPos.x` |
| moveY | 同上 | 同上 | `camParent.y` | `finalPos.y` |
| rotation | `rotationUsed \|\| (movementTypeUsed && isLastPosition)` | 同 kill 条件 | `camAngle` | `rotation + rotationOffset` |
| zoom | `zoomUsed` | `zoomUsed` | `zoomSize` | `zoom/100` |

`startTween` 对 `duration > 0` 建活动 tween；`duration <= 0` 直接赋终点、tween 保持 inactive
（探针 I：`duration:0` 下 `ease:'InBack'` 也只是一步到位）。

---

## 5. 跟随层

### 5.1 `camspeed`、`timer`、lerp

```ts
const camspeed = (60 / (bpm * pitch)) * 2;     // 2 拍
timer += deltaSeconds;
const t = clamp01(timer / camspeed);           // 线性
pos = frompos + (topos + offset + holdOffset - frompos) * t;
```

- **线性，不是指数阻尼**：`camspeed` 到点就精确等于 `topos`，不会永远差一点。
- **2 拍是"追赶时长"**：BPM 120 → 1.000 s；BPM 240 → 0.5 s；`pitch = 150%` → 1.5 倍速（时长 ×2/3）。
- `bpm` 用的是**当前砖**的 `tileBPM[currentTileIndex]`，所以变速段里相机的跟随快慢也跟着变。
- `bpm`/`pitch` 都做了 `max(…, 1e-6)` 兜底（避免除零）。
- `num5`（`followMovingPlatforms` 那支）恒 1，见 §5.4。
- 探针 B'（`dt=0.1`，camspeed 1.0，目标 1 砖）：`t=0` 就已经 0.1，之后严格线性，`t=0.9` 到 1.0 并停住。

### 5.2 切砖才换目标

```ts
if (currentTileIndex !== lastFollowTile) {
    lastFollowTile = currentTileIndex;
    frompos = pos;              // 从当前（可能还没追上）的位置起步
    topos   = planetWorldPos;
    timer   = 0;
}
```

- `frompos = pos`（不是 `topos`）→ 连续、无跳变；每块砖都重新给满 2 拍。
- 只有**砖号变了**才重置。同一砖内 `planetWorldPos` 变了（贴砖移动、Hold 滑动）不会自动重新采样 ——
  Hold 靠 `holdOffset` 单独补（§5.3），贴砖移动靠 `followMovingPlatforms`（恒 false，§5.4）。
- `planetWorldPos` 是**枢轴行星（轨道圆心）**，不是那颗公转的球（§7.2）。

### 5.3 `holdOffset`（长按带的相机跟随）

`Player.updateCameraFollow` 每帧：

```ts
this.cameraController.setHoldOffset(this.holdSlideOffset.x, this.holdSlideOffset.y);
```

`holdSlideOffset = 长按段当前圆心 - 砖心`（`updatePlanetsPosition` 里算）。它被**加在 lerp 的目标上**
（`topos + offset + holdOffset`），所以：

- 长按期间目标随圆心平移，相机跟着长按带走，而不是钉在起点砖；
- 非长按砖 `holdSlideOffset = 0`，这项自动失效；
- 它**不重置 timer**，所以是"目标在动、追赶进度继续"。

> 这是 commit 3b13075 补的（原版 `scrPlanet` 每帧 `camy.SetHoldOffset(holdOffsetPos)`）。在此之前
> `holdOffset` 字段存在但没人写 —— 典型的"骨架搭好了但没接上"。

### 5.4 `followMovingPlatforms` 恒 false（已知缺口）

`resetCameraState()` 里 `this.followMovingPlatforms = false;` 之后**全库没有写入点**。
所以 `num5` 那段（距离 > 5 时把追赶速度提高最多 2 倍）**永远是死支**。

后果：`stickToFloors` + `MoveTrack` 让砖在运镜中移动时，相机的跟随目标只在**切砖那一刻**采样一次，
砖在这一砖内移动不会让相机逐帧重采样（Hold 除外，它有 `holdOffset`）。移植时要么照抄这个缺口，
要么明确做成"逐帧重采样"并接受与 Re_ADOJAS 不一致。

### 5.5 `pitch` 进入两处

`pitch = settings.pitch/100`（clamp ≥ 0.1），在两处生效：

- 事件 `duration` 的拍→秒（`60/(bpm*pitch)`）；
- 跟随 `camspeed`（`60/(bpm*pitch)*2`）。

探针 G（`pitch=1.5`，`duration=6` 拍 @120 BPM）：`6*60/(120*1.5) = 2.000 s`，实测 `t=2.0` 正好到 6.0。

---

## 6. `reset` / `seek` / 回退

### 6.1 `resetCameraState()`：把 `settings` 当成一个 duration=0 的合成事件

`MoveCamera` 事件不是相机的初始值来源 —— **`settings` 才是**。`resetCameraState()` 会构造一个
合成的 `MoveCamera` 并交给 `startEffect`：

```ts
const synthetic = {
    eventType: 'MoveCamera', duration: 0,
    position: Array.isArray(settings.position) ? settings.position : [0,0],
    rotation: settings.rotation ?? 0, zoom: settings.zoom ?? 100,
    relativeTo: parseMovementType(settings.relativeTo) ?? 'Player',
    ease: 'Linear', angleOffset: 0, floor: 0,
};
this.startEffect(synthetic, 0, 0, 砖位(0), {0,0}, tileBPM[0] || 100, 1);
```

探针 M（`t=1.0` 时枢轴在 (4,0)）：

| `settings` | 相机世界坐标 | 模式 |
|---|---|---|
| `position [0,0]`, `relativeTo Player` | (4, 0)（= 行星） | follow |
| `position [0,0]`, `relativeTo Tile` | (0, 0)（= floor 0 砖位） | free |
| `position [2,0]`, `relativeTo Global` | (2, 0) | free |
| `position [2,3]`, `rotation 45`, `zoom 250`, `Player` | (6, 3)，rot 45，zoom 250 | follow |

**`settings.relativeTo: "Tile"` 锚在 floor 0**，这是"开局相机为什么会停在谱面起点而不是球旁边"的原因。

`resetCameraState()` 同时把 `lastCameraTimelineIndex = -1`、`lastFollowTile = -1`、四条 tween 清空、
`offset`/`holdOffset`/`lastEventRelativePosition` 归零、`lastUsedMovementType = 'Player'`、
`legacyRelativeTo` 从 settings 重读。

### 6.2 `seek(t, planet, tileIndex, pitch)`

```ts
resetCameraState();
for (每个 entry，直到 entry.time > t) {
    applyTweens(entry.time);
    若 followMode：frompos = topos = pos = planet;      // 强制跟随层"已在稳态"
    startEffect(entry, floor, entry.time, 砖位(floor), planet, tileBPM[floor], pitch);
}
lastCameraTimelineIndex = 最后一个已处理的下标;
applyTweens(t);                                        // 把最后那条 tween 求值到 t

若 followMode：frompos = topos = pos = planet; timer = 1e9;   // 稳态，别让续播再追一遍
否则：        frompos = topos = pos = 0;       timer = 0;
lastFollowTile = currentTileIndex;
```

要点：

- **每次都从头重放**（`for i = 0`），复杂度 O(事件数)，与 t 无关；`seek` 频繁时是可见开销
  （事件数通常几十~几千，可接受）。
- 循环里每次 `startEffect` 前把跟随层钉在 `planet`，是为了让"free→follow 换算"看到确定的世界坐标。
- 结束后强制稳态（`timer = 1e9`），所以 **seek 之后的第一帧不会从旧位置追过来**；探针 H：
  `seek(4.0)` 直接落在最后一次事件给的 (1,1)、rot 90、zoom 150（150 是更早事件留下的，见 §6.3），
  续跑 0.2 s 位置不变（free 模式）。
- `seek` 传的 `planet` 是**目标时刻**的行星位置，而循环里的事件发生在更早的时刻（§6.4）。

### 6.3 状态是"累积"的，不会自己回退

`rotation` / `zoom` 没有事件时**保持不变**（不回到 100/0）。探针 H 里 `zoom=150` 就是 t=1 的事件留下的；
`seek(4.0)` 之后仍是 150。写测试时别指望"跳到没有运镜的段落 = 默认值"。

### 6.4 已知偏差：seek 与实播可能不同

`seek` 在整套循环里复用同一个 `planetWorldPos`（目标时刻的），而实播时每个事件用的是**那一刻**的行星。
只要谱面里存在 `free → Player` 的切换（`Tile`/`Global` 之后又回到 `Player`），换算出的 `camParent`
就会差一个行星位移。这是"seek 后画面和从头播不完全一致"的根因之一；本文只记录，**不建议**在没有
对拍的情况下改它（改了就不是 Re_ADOJAS 的行为了）。

### 6.5 回退守卫

```ts
if (lastCameraTimelineIndex >= 0 && nowSeconds < timeline[lastCameraTimelineIndex].time)
    resetCameraState();
```

时间倒退到"已处理事件之前"就整体重置（游标 -1 → 下一帧从第一个事件重放）。它只处理**大幅回退**；
1 帧内的抖动不会触发（`now` 仍在游标事件之后）。`update` 的游标只前进，从不后退。

---

## 7. `Player` 侧的集成

### 7.1 时间基准

```ts
const timeInLevel = this.elapsedTime / 1000 - this.getTimeOrigin();
```

- `timeInLevel` 与 `tileStartTimes` **同一条时间线**（§2.2 直接拿 `tileStartTimes[floor]` 比较）。
- `getTimeOrigin()` 把"墙钟/音乐时间"换成"关卡时间"：
  - `separateCountdownTime` 关：`origin = offset/1000`；
  - 开：`origin = countdownTicks * 60/bpm0 + offset/1000 - tileStartTimes[1]`（tile1 对齐判定线）。
- 倒计时期间 `timeInLevel < 0`：没有事件可触发，相机就在 follow 模式下朝 tile 0 的枢轴收敛。

### 7.2 `planetWorldPos` = 枢轴（轨道圆心），不是公转的球

`updateCameraFollow` 传的是 `currentPivotPosition`，它在 `updatePlanetsPosition` 里被设成**当前砖的砖心**
（`stickToFloors` 时用 live mesh 位置）：

```ts
this.currentPivotPosition.x = pivotPos.x;   // 砖心 / 移动后的砖心
```

而"那颗在转的球"是 `applyMultiPlanetLayout` 里 `mx = cx + cos(angle) * orbitR` 算出来的另一颗
（枢轴行星画在圆心 `px/py`，移动行星画在圆周 `mx/my`）。所以相机跟的是**"落点中心"**：
公转的那颗绕着屏幕中心转 —— 视觉上是"球绕着自己转"，不是"相机盯着球飞"。

推论：`Player` 模式的 `settings.position` 是"相对轨道圆心的偏移"；`Tile` 模式的锚点是**砖心**；
Hold 段圆心会沿长按带滑动，靠 `holdOffset` 补上（§5.3），而 `currentPivotPosition` 仍是砖心。

### 7.3 上屏：zoom / rotation / 视锥

```ts
const camPos = controller.getCameraPosition();       // camParent + pos
camera.position.x = camPos.x;  camera.position.y = camPos.y;

const adoZoom = controller.getCameraZoom();          // zoomSize * 100
zoom = 100 / adoZoom;
camera.zoom = zoom * zoomMultiplier;                 // zoomMultiplier = 用户滚轮（0.1~10）
camera.rotation.z = controller.getCameraRotation() * Math.PI / 180;
```

- **视野**：Re_ADOJAS 的 `baseFrustumSize = 5.47`（**全高**，砖单位）→ `adoZoom=100` 时可见高
  5.47 砖。代码注释：官方可视高 10 单位、砖距 1.71，折算约 5.85 砖，Re_ADOJAS 取 5.47。
  实际可见高 = `5.47 / camera.zoom = 5.47 * adoZoom/100 / zoomMultiplier`。
  我们的 `render/Camera` 内部是**放大倍数**（`halfH = 6/(zoom/100)`，100 → 可见高 12 砖），
  所以谱面给的 zoom（`settings.zoom` 与事件 zoom，**越大看得越远**）一律走
  `Camera::setAdoZoom()`：`本类 zoom = 120000/(5.47·adoZoom)`，等价于可见高 `5.47·adoZoom/100`。
  **两套口径方向相反**，混用会出现"zoom 200 反而放大 2 倍"（真踩过：Fledgling 的
  371 条运镜全反，用户第一反应是 Retina 问题）。`--capture-zoom` 与滚轮仍是本类口径。
- **旋转方向**：直接乘 π/180 交给 three.js。three.js 里 **正的 `camera.rotation.z` = 相机逆时针**，
  画面里的世界因此**顺时针**转。移植时用一张 `rotation: 90` 的谱拍帧对齐符号，不要凭"应该朝哪边"猜。
- **`zoom <= 0` 的守卫在 Player 侧**：`if (adoZoom > 0)` 才写 `camera.zoom`。controller 本身不防
  `zoom: 0`（会把 `zoomSize` 设成 0），只是上屏被跳过 → 视觉上"保持上一个 zoom"。
- **`zoom > 300`**：自动关掉砖块贴图（`setTileTextureEnabled(false)`），回到阈值以下再打开。
  这是纯性能优化，不影响几何。

### 7.4 屏幕震动（Shake）在相机之外

`ShakeScreen` 的偏移**只在 render 时**加到 `camera.position` 上，渲染完立刻减回去：

```ts
const unshakenX = camera.position.x; …
camera.position.x += shake.x; camera.position.y += shake.y;
// … 渲染 …
camera.position.x = unshakenX; camera.position.y = unshakenY;
```

所以 controller 里的 `camParent/pos` 永远是**未震**值。谁拿震后坐标要说清：

- **剔除 / 装饰**：用 `cameraPosition` 与未震的 `camera.position`（`updateDecorations` 在加 shake
  之前调用），所以它们看到的是**未震**坐标；
- **背景 / 视频 / 闪光**：`updateBackgroundTransform` 与 `FlashEffect.updateBG` 在**加完 shake 之后**
  调用，看到的是**震后**坐标 —— 代码注释写明这是为了避免"背景和相机差一帧、大幅运镜时露边"。

### 7.5 暂停 / 编辑器

`renderPlayer` 在 `!isPlaying || isPaused` 时：

```ts
syncCameraFromController(false);   // 只同步 zoom + rotation，不动 position
```

注释写明：**不覆盖 position**，因为编辑器里用户拖动视角，控制器位置只在 seek / 播放时应用。
所以停下来的画面位置由用户拖动决定，而缩放/旋转仍由 controller 说了算。

### 7.6 下游消费者（移植时别漏）

- **可见砖剔除**：`updateVisibleTiles` 用 `cameraPosition` + `camera.zoom` 算**轴对齐**视锥，外扩
  `margin = 2.0` 砖；早退条件只看"位置移动 < 0.1 且 zoom 变化 < 0.01"（**旋转不在签名里**）。
  按代码推算的风险：旋转 45°、zoom 100（16:9）时需要的垂直外扩约 `(2.735+4.86)*0.707 - 2.735 ≈ 2.6` 砖
  > margin 2 → 边缘可能缺砖；zoom 越大越明显。**未实测**，移植时按旋转扩边更稳。
- **砖上图标反旋**：`updateTileChildSpriteRotations` 用 `-cameraRotation` 抵消（只在旋转变化 > 1e-4
  时重算可见砖）。
- **背景/闪光/装饰**：都吃 `camera.rotation.z` / `camera.zoom` / `adoZoom`（装饰的 `relativeTo: Camera`
  用的是震后相机坐标）。

---

## 8. 实测（探针）

**怎么跑**（可复现，不改 Re_ADOJAS 仓库）：把 `CameraController.ts`、`Easing.ts`、`EventUtils.ts`
复制到一个临时目录，把三行 import 的扩展名补成 `.ts`，然后用 **Node ≥ 22 的 type-stripping 直接跑**
（`import type` 那行会被类型擦除丢掉，所以 `TileAccess` 不必存在；探针要传砖位时自己实现
`getX/getY` 就行）：

```bash
node probe.ts          # Node 26 默认就能跑 .ts（无需 tsx/ts-node）
```

驱动方式：固定 `dt = 1/60`（或 0.1），每帧调 `update({nowSeconds, deltaSeconds, bpm, pitch,
planetWorldPos, currentTileIndex})`；世界是 40 块砖沿 +x、间距 1 砖，BPM 120（1 砖 = 0.5 s）。

| # | 输入 | 结果（实测） | 说明 |
|---|---|---|---|
| A | `Tile`，`position [1,0]`，`duration 4` @120，floor 4（t=2.0） | t=3.0 → x=3.5625；t=4.0 → x=5.0 | 时长 2.000 s；起点 = 事件时刻相机实际世界坐标 ≈2.125（跟随层还在追），终点 = 砖位 4 + 1 |
| B' | 无事件，砖切到 1、目标 (1,0)，`dt=0.1` | t=0 → 0.1；t=0.9 → 1.0；之后恒定 | 切砖那帧就吃一个 dt；lerp 严格线性 |
| C | `Global` (0→10)/5 s，t=2.0 被 (0,0)/1 s 抢占 | 1.983→3.967；**2.000→10.000**；2.5→5.0；3.0→0 | `Kill(complete)` 的瞬跳 |
| D | rot 30 → `LastPosition` rot 0 → `LastPositionNoRotation` rot 0 | 30 → 30 → 0 | LastPosition 继承当前角，NoRotation 不继承 |
| E | zoom 200，再 zoom 50（带 `[null,null]`） | 200 → 50；位置不动 | zoom 百分比；`[null,null]` 不产生移动 |
| F | `Tile [2,1]` @floor2，然后 `Player` 无 position @floor4 | (4,1) → **(2,-1)** | §4.6 的怪路径，反直觉 |
| G | `pitch=1.5`，`duration 6` @120 | t=2.000 → x=6.000 | 拍→秒被 pitch 缩放 |
| H | `seek(4.0)`，事件在 1.0 / 3.0 | (1,1)，rot 90，zoom 150，free，游标 1，续跑不动 | seek 强制稳态；zoom 是累积值 |
| I | `duration 0`，`ease InBack`，pos (5,0)，rot 20，zoom 300 | 一步到 (5,0)/20/300 | duration=0 时 ease 无效 |
| J | 两个 `position [3,0]`（无 relativeTo） | legacy=false → 3；legacy=true → 6 | §4.7 累加 |
| K | `Player [2,3]`，再 `Player [null,null]` | (2,3) 保持不动 | dedup 生效 |
| L | `Tile [1,0]` @floor2 → (3,0)，同砖 `[null,null]` | (3,0) 保持不动 | dedup 的 Tile 分支要求同砖 |
| M | 四种 `settings`（见 §6.1 表） | 见 §6.1 | settings 合成事件的语义 |

---

## 9. 反直觉清单（移植前必读）

1. **`duration` 用事件所在砖的 BPM，跟随速度用当前砖的 BPM**。变速谱里这是两个数；把 `camspeed`
   也按 `bpmFloor` 算会让变奏段的跟随手感整个偏掉。
2. **`Kill(complete:true)` = 跳到终点**。运镜被打断会瞬跳（C 实测 3.97 → 10.00），这是"正确行为"。
3. **`Tile` 是绝对锚定**（砖位 + position），`LastPosition` 才是累加。写谱时把两者搞混，运镜会
   "永远回到同一块砖"。
4. **`[null,null]` + 同模式 = 什么都不做**（dedup）；但换成别的模式就会归零/落点。判据见 §4.4。
5. **只要带了 `relativeTo`，NaN 分量被归零**（§4.3）。"不给 position" 和 "给 `[null,null]`" 不等价。
6. **没写 `position` 又写了 `relativeTo`** 走 `lastEventRelativePosition - camParent` 的怪路径（F）。
7. **切砖那一帧的 lerp 已经走了 `dt`**（timer 先加后算，B'）。逐帧对拍要照抄这个顺序。
8. **`followMovingPlatforms` 恒 false**：贴砖移动的谱面，相机只按切砖采样（Hold 靠 `holdOffset` 例外）。
9. **`seek` 会强制跟随层稳态并重放全部事件**（O(事件数)），且用的是目标时刻的行星位置 → 与实播可能
   在 free↔follow 切换处不同（§6.4）。
10. **`rotation` / `zoom` 是累积状态**，没有事件就保持上一个值；`seek` 到空白段也不会回默认。
11. **`zoom:0` 不会把画面缩没**：controller 会设 `zoomSize=0`，但 Player 侧 `adoZoom > 0` 的守卫
    让上屏跳过（保持上一个 zoom）。
12. **`buildCameraTimeline()` 是死代码**，且缺 `RepeatEvents` 展开 —— 别照它写。
13. **`offset` 字段恒 0**（只读不写），别在移植时给它编语义。
14. **旧 `docs/Camera.md` 描述的 `CameraMode`/`CameraTransition`/`findRealRelativeTo` 已经不存在**；
    它描述的是 2026 年更早的一版实现（事件快照 + 过渡快照 + 回溯），别拿它当现状。
15. **`settings.relativeTo: "Tile"` 锚在 floor 0**，所以开局相机可能停在谱面原点而不是球旁边（M）。
16. **可见剔除不看旋转**（§7.6）—— 大 zoom + 大旋转时边缘可能缺砖（推算，未实测）。

---

## 10. 移植到 ADOCAO

> **2026-10-17：下面这套依赖链已经全部落地**（§12 有逐项状态与文件）。本节保留原文，
> 因为它解释的是"为什么按这个顺序做"以及每一层的验收口径。

### 10.1 现状差距（2026-10-16 核过）

| 能力 | Re_ADOJAS | ADOCAO 现状 |
|---|---|---|
| 相机跟随 | 两层 + 2 拍线性追赶 | `GameWindow` 每帧 `snapTo(当前砖砖心)`（瞬移，无追赶） |
| `MoveCamera` | 完整（5 种 relativeTo + 4 轨 tween） | **完全没有**：`eventTypeOf()` 把未知事件归 `Other` 并丢弃 |
| `angleOffset` | 事件计时用 | **没解析**（`grep angleOffset` 只在注释里） |
| `RepeatEvents` | 已展开 | **未实现** |
| 缓动函数 | 32 个（`Easing.ts`，名字 = 谱面 `ease` 字符串） | **一个都没有**（`core/util` 下没有 Easing 模块） |
| 相机旋转 | `camAngle` → `camera.rotation.z` | `render/Camera` 的 view 矩阵构造时固定 `lookAt`，**没有旋转** |
| 相机缩放事件 | `zoomSize` | `setZoom` 只有编辑器/CLI 用（`zoom=100 → halfH=6`） |
| 视野基准 | 5.47 砖全高 @100 | **12 砖全高 @100**（`halfH = 6/(zoom/100)`）→ 两套 zoom 不能混用 |
| 精度 | 全部 double（JS） | `Camera::m_targetX/Y` 是 double ✅，但 `zoom/aspect` 是 float |

### 10.2 依赖链（按这个顺序做，每步都能单独验收）

1. **`angleOffset` 进 action**：`ActionFields` 加一个 `double angleOffset`（缺省 0），`FastAction` 没地方
   放就挂在"少量事件"的新结构里（见第 4 步）。这一步不改行为，纯扩解析。
   注意它属于 `parseNumber` 的"非整数 action 字段"那一类 —— 快路径放弃的历史教训写在 AGENTS.md，
   加字段时别把快路径又打回慢路径。
2. **`RepeatEvents` 展开**：相机事件只关心 Beat/Floor 两模式 + tag 匹配 + `entryBeat` 几何拍差
   （§2.3）。没有它，所有用重复做的运镜都会缺。
3. **缓动模块（32 个函数）**：`Easing.ts` 的**名字就是契约**（谱面里的 `ease` 字符串直接查表），
   查不到时回退 `Linear`。建议做成 `name → id` 的静态表 + `switch`，别每帧拿字符串查表。
   验收方式同 `render/TileShape`：拿 `Easing.ts` 当参考实现逐点对拍（含 `InBack` 这种越界值，
   以及 `InBounce`/`OutBounce` 的分段常数 7.5625 / 2.75）。
4. **`MoveCamera` 富事件**：不要塞进 16 B 的 `FastAction`。运镜事件**与砖数无关**（一个事件一条），
   单独一个 `std::vector<CameraEvent>` 就够：`{floor, time, durationBeats, easeId, posX, posY,
   hasPos[2], rotation, hasRotation, zoom, hasZoom, relativeTo, angleOffset, disabledMask, active}`。
   解析可以走 DOM 路径（`actions` 数组里 `eventType == "MoveCamera"` 的少数对象），不必进快路径。
   **`disabled`/`angleOffset`/`position` 的单轴 null 都要保留**。
5. **时间线**：`time = timeline.tileStartTimes()[floor] + angleOffset/180 * (60/timeline.tileBPMs()[floor])`，
   按 `(time, id)` 排序。用 `double`。
6. **`CameraController`（新模块）**：照 §1.3 建状态、§3 建循环、§4 建 `startEffect`、§5 建跟随层。
   建议放 `app/CameraController`（现有那个是导航态机）或另开 `app/CameraTimeline`，避免与拖拽/滚轮
   逻辑纠缠 —— 现有 `CameraController` 的职责是"用户nav"，两者唯一交汇点是 `Camera::setTarget/setZoom`。
7. **`render/Camera` 加旋转**：`view = rotate(-angle) * translate(-target)`（或把旋转并进 proj），
   `frustumBounds` 要返回值给剔除用，**旋转后要按旋转矩形的 AABB 扩边**（别学 margin 2 近似，
   除非也接受同样的边缘缺砖）。
8. **剔除接旋转**：`TileMesh::draw` 收的是轴对齐 bounds；旋转后传 AABB 或加朝向参数。这是像素门槛
   会抓到的第一处回归。

### 10.3 验收（按 ADOCAO 的规矩）

- **逐位对拍优先**：给 `CameraController` 写纯函数式的用例（无 GL），输入事件表 + 每帧
  `(t, bpm, pitch, planet, tile)`，输出 `(camX, camY, rot, zoom)` 序列；用 §8 的探针数据做金标
  （A/C/F/J/K 覆盖 5 种模式与两个反直觉分支）。
- **像素门槛**：`scripts/capture-gate.sh store|check` 加一个带 `MoveCamera` 的谱面状态（至少覆盖
  `Tile` 位移 + `rotation` + `zoom` 各一帧），确认旋转/缩放的出图与手算一致。
- **别用"看起来对"验收**：本仓库有过"用直觉否掉 Re_ADOJAS"的事故（AGENTS.md 的反例），
  运镜的符号、方向、瞬跳语义全部以对拍为准。

### 10.4 风险

- 视野基准（5.47 vs 12 砖）一旦混用，所有 `zoom` 事件都会差一倍多；先定基准再写事件。
- `zoom/aspect` 是 float：大 zoom（1000+）时视锥边界的相对误差仍可接受，但 `zoomSize` 本身要 double
  （乘法链 `100/adoZoom*multiplier` 在 float 里会抖）。
- 相机位置大量是 `planet + camParent`：行星坐标在巨谱上是 1e7 量级（AGENTS.md 的 float 事故），
  这个和必须在 **double** 里算完再窄化一次。

---

## 11. 源码地图（Re_ADOJAS @ 6ccf519）

| 位置 | 内容 |
|---|---|
| `src/lib/Player/CameraController.ts:7` | 5 种 `CamMovementType` + 数字映射 + `TILE_SIZE` |
| `:42`–`:61` | `TweenState` / `evalTween`（clamp + ease 表） |
| `:139`–`:173` | 构造与访问器（`getCameraPosition` = `camParent + pos`） |
| `:181` | **死代码** `buildCameraTimeline` |
| `:218` | `resetCameraState`（settings 合成事件） |
| `:282` | `seek` |
| `:332` | `update`（§3 的 7 步） |
| `:400`–`:558` | `startEffect`（Decode / dedup / Kill / 5 模式 / 4 轨） |
| `:562`–`:610` | `killTween` / `startTween` / `applyTweens` |
| `:618`–`:632` | `setToFreeMode` / `updateFollowCam` |
| `:639` | `setHoldOffset` |
| `Player.ts:7033` | `updateCameraFollow`（喂参数、holdOffset、贴图自动关、剔除） |
| `Player.ts:7075` | `syncCameraFromController`（zoom/rotation 上屏） |
| `Player.ts:5726` | `onWindowResize`（`baseFrustumSize = 5.47`） |
| `Player.ts:2102` | `getTimeOrigin`（时间基准：倒计时 / separateCountdownTime / offset） |
| `TimelineManager.ts:~250` | `MoveCamera` 的 `time` 计算与 `_cameraEvents` |
| `TimelineManager.ts:~150` | `RepeatEvents` 表与展开（Beat/Floor） |
| `src/lib/Player/EventUtils.ts` | `isEventActive` / `isFieldEnabled` |
| `src/lib/Player/Easing.ts` | 32 个缓动（名称即事件里的 `ease` 字符串；含 `Linear`/`Unset`） |

---

## 12. ADOCAO 落地状态（2026-10-17）

八步依赖链已全部落地，且每一步都有独立可跑的验收。

| # | 内容 | 落点 | 验收 |
|---|---|---|---|
| ③ | 32 个缓动 | `core/util/Easing.{hpp,cpp}` | `tests/easing_test.cpp`：金标由 `Easing.ts` 直接跑出来（`tests/easing_gold.inc`），32×25 点，容差 1e-12（跨 JS/libm 的 ulp）；另外钉住 `InBack(0.5)<0`（不许 clamp） |
| ① | `angleOffset` | `LevelData::CameraEvent.angleOffset`（**不存进 16 B 的 `FastAction`** —— 只有运镜需要它，9M action 不该为此变 24 B） | 解析对拍（下面 ④ 的同一套） |
| ② | `RepeatEvents` 展开 | `core/camera/CameraTimeline.cpp`（Beat/Floor、`executeOnCurrentFloor`、`entryBeat` 的 -1 哨兵、多 tag、id tie-break） | `tests/camera_timeline_test.cpp`（8 组手算用例） |
| ④ | `MoveCamera` 富事件 | `LevelData::CameraEvent` + 四条解析路径（快路径/分块并行/流式窗口/DOM）+ `.adocao` 段 9 | `tests/level_parse_test.cpp`（新增 fixture `f30_camera_events.json`，四条路径 + 容器往返逐字段对拍） |
| ⑤ | 事件时间线 | `core/camera/CameraTimeline`（时间 = `tileStartTimes[floor] + angleOffset/180*60/bpm`） | 同上 |
| ⑥ | 两层控制器 | `core/camera/CameraDirector.{hpp,cpp}`（rig + 跟随层 + 4 条 tween + dedup + legacy + seek/回退） | `tests/camera_test.cpp`：**跨语言逐帧金标** —— 同一批事件喂给 Re_ADOJAS 的 `CameraController.ts` 与 C++，8 个场景 × 每 15 帧一个采样点，容差 1e-9 |
| ⑦ | 相机旋转 | `render/Camera::setRotation`（`proj = ortho · R(-θ)`） | 抓帧目检（`tests/charts/camera_basic.adofai`，rotation 90 那张轨道是竖的）+ 单元测试不动像素 |
| ⑧ | 剔除扩边 | `render/Camera::frustumBounds` 返回**旋转矩形的世界 AABB**（θ=0 时逐位退化） | 像素门槛：51 个旧基线（θ=0）逐字节不变 + 3 个新运镜状态（54/54 ✓） |
| ⑨ | **谱面 zoom 口径** | `render/Camera::setAdoZoom`（ADOFAI：越大看得越远；内部放大倍数不变，`--capture-zoom`/滚轮仍走本类口径） | `tests/camera_zoom_test.cpp`（方向 + 100/200/50/500 的可见高 + 非法值兜底 + 与 `setZoom` 的换算一致） |

**顺便补的两处口径**（不补就会静默错）：

* `settings.pitch` 与 `settings.legacyCamRelativeTo` 现在真的解析了（`readSettings`）；前者同时缩放
  事件时长与跟随速度，后者是 §4.7 的老语义开关。`.adocao` 的 Settings 记录**末尾追加**了这两个字段
  （尾部追加 = 双向兼容，`docs/adocao-format.md` §3.1）。
* `.adocao` 新增两段：`Camera`(9, 38 B/条) 与 `Repeat`(10, 19 B/条)。不写这两段的话，
  "打包成 .adocao 再看"会把运镜整段丢掉（这是本仓库"不许静默错误"的直接后果）。

### 12.1 应用侧怎么接的（GameWindow）

* 时间线在 **`LevelLoader` 里构建**，早于 `dropStartTimesArray()` / `releaseMemory()` ——
  它需要逐层起始时间与砖位，晚一步就只剩空数组（**真踩过**：时间线全 0，所有事件挤在 t=0）。
* 播放时每帧：`t = PlaybackClock::timeInLevel()`（与 `tileStartTimes` 同一基准），
  `planet = tilePositionAt(currentTileIndex)`（**枢轴砖心**，不是公转的球），
  `bpm = tileBPMs[currentTileIndex]`，`pitch = settings.pitch/100`。
* 跳转/回退/首帧走 `seek`；正常帧走 `update`（判据：`|Δt| > 0.25 s` 或时间倒退）。
  停下时把当前视角 `snapTo` 成导航基准，**保留旋转**（`CameraController` 的
  `screenToWorld`/拖动已按 `camera.rotation()` 反旋转，暂停时点选仍然准）。
* `--capture-camera`：抓帧也走关卡相机（`--capture-tile` 仍按砖定点）。**这条模式下 zoom 由相机决定**，
  `--capture-zoom` 被忽略 —— 否则运镜的 zoom 事件会被 CLI 覆盖，等于没测。
  默认不开：既有 51 个像素基线的口径不变（The Moon 的 `settings.relativeTo=Tile` 会整幅平移）。

### 12.2 还没做 / 已知限制

| 项 | 现状 | 影响 |
|---|---|---|
| ~~**视野基准**~~ | ✅ 已对齐：谱面 zoom 走 `Camera::setAdoZoom()`（100 → 5.47 砖，越大越远）。内部仍是"放大倍数"（`--capture-zoom` / 滚轮不变） | 两套口径的换算由 `tests/camera_zoom_test.cpp` 钉住（方向、100/200/50/500 的可见高、非法值兜底） |
| **Hold 的 `holdOffset`** | 控制器有接口（`setHoldOffset`），app 恒写 0 | ADOCAO 还没有 Hold 事件；等 Hold 落地后接上即可（docs/camera.md §5.3） |
| **`MoveTrack` 的砖位** | 相机锚点用**静态**砖位（与 Re_ADOJAS 的 `tilePositions` 一致） | 贴砖移动的谱面，相机不会跟着砖逐帧走（Re_ADOJAS 的 `followMovingPlatforms` 也是死支，行为一致） |
| **抓帧门槛的运镜状态** | ✅ 已进门槛：`tests/charts/camera_basic.adofai` 三个时刻（`camera_rot` / `camera_tween_mid` / `camera_tween_end`），基线已 store | 全量 `check` = **54/54 逐字节相同**（51 旧 + 3 新） |
| **`.adocao` 的其它未知事件** | `MoveCamera`/`RepeatEvents` 已无损；`MoveTrack`/`Decorations` 等仍在 `Preserved` 的待做清单里 | 与运镜无关，但同样是"打包后丢事件" |
| **砖上图标（Twirl/SetSpeed…）** | 我们是**烘进砖几何**的（一次 draw），相机旋转时图标跟着世界转；Re_ADOJAS 是 sprite，`baseRotation + tileZ - cameraRotation` 会**反向补偿**保持正立 | 只有 `rotation ≠ 0` 时可见（51 个旧基线全在 0°；`camera_rot` 这个新状态是 90°，但它只锁"现在的出图"，不是对齐 Re_ADOJAS 的图标朝向）。要对齐得把图标拆成独立绘制并传一个逆旋角 —— 与"一次 draw"的取舍见 `docs/rendering.md` |

### 12.3 回归怎么跑

```bash
ctest --test-dir build -R 'easing|camera|level_parse'    # 纯逻辑（无 GL）
./build/tests/adocao_camera_test                          # 跨语言逐帧金标
tests/gen_camera_gold.sh                                  # 重新生成金标（node ≥ 22 + Re_ADOJAS 克隆）
scripts/capture-gate.sh check                             # 像素（要没有别的 build/adocao 在跑）
```

## 交互：光标位置必须"按下前现取"（2026-10-22 实测）

GLFW **不会**在窗口创建时送一次 cursor-pos 事件；如果用户开窗后立刻按下（1 秒内），
`CameraController::m_cursorX/Y` 还是 `(0,0)`，`dragStart` 被记成 `(0,0)` →
第一次移动算出的 offset 是「整块屏幕」的量级 → 镜头被甩到**不在任何砖上**的空白处。

症状（用户原话）："一拖动镜头会传送，而且那个地方不在任何一块砖上"。

修法（两层）：
1. `glfwSetMouseButtonCallback` 里**先** `glfwGetCursorPos()` 再 `onCursorPos()`，最后才
   `onMouseButton()` —— 保证 dragStart 一定是真实位置；
2. `CameraController::update()` 的拖拽分支：`!m_cursorSeen` 时**不计算 offset**（防御未来其它入口）。

诊断用：`ADOCAO.log` 里 `mouse down at (0.0,0.0) seen=0` 就是踩中了这个坑。
