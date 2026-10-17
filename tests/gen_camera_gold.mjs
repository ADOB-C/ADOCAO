// 生成 tests/camera_gold.inc：跨语言金标（Re_ADOJAS 的 CameraController.ts ↔ 我们的
// core/camera/CameraDirector）。**不要直接在这个目录跑** —— 这个脚本假设自己旁边有一份
// `CameraController.ts`（Re_ADOJAS 原文，import 的扩展名改成了 .ts）。用 wrapper：
//
//     tests/gen_camera_gold.sh            # 需要 node ≥ 22（类型擦除）
//
// 之所以要"复制 + 改 import"：Re_ADOJAS 的 ESM import 不带扩展名，Node 的类型擦除只认
// 显式的 `.ts`。wrapper 会把 Easing.ts / EventUtils.ts / CameraController.ts 拷到临时目录、
// 改好 import、再把本文件也拷过去跑。
import { CameraController } from './CameraController.ts';
import { writeFileSync } from 'fs';

const BPM = 120, SPB = 0.5, N = 40, DT = 1.0 / 60.0;
const xs = Array.from({ length: N }, (_, i) => i);
const ys = new Array(N).fill(0);
const positions = { getX: i => xs[i] ?? 0, getY: i => ys[i] ?? 0 };
const tileStartTimes = Array.from({ length: N }, (_, i) => i * SPB);
const tileBPM = new Array(N).fill(BPM);
const tileAt = t => Math.min(N - 1, Math.max(0, Math.floor(t / SPB)));
const planetAt = t => ({ x: tileAt(t), y: 0 });

const FLAG = { hasPos: 1, hasX: 2, hasY: 4, hasRot: 8, hasZoom: 16, inactive: 32, editorOnly: 64, easeW: 128 };
const EASE = { Linear:0, InSine:1, OutSine:2, InOutSine:3, InQuad:4, OutQuad:5, InOutQuad:6, InCubic:7, OutCubic:8, InOutCubic:9,
  InQuart:10, OutQuart:11, InOutQuart:12, InQuint:13, OutQuint:14, InOutQuint:15, InExpo:16, OutExpo:17, InOutExpo:18,
  InCirc:19, OutCirc:20, InOutCirc:21, InBack:22, OutBack:23, InOutBack:24, InElastic:25, OutElastic:26, InOutElastic:27,
  InBounce:28, OutBounce:29, InOutBounce:30, Unset:31 };
const REL = { Player:0, Tile:1, Global:2, LastPosition:3, LastPositionNoRotation:4 };

// 场景定义（C++ 侧不再重复写一遍：键和采样都由这里生成）
const scenarios = [
  { name: 'tile_move', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 4, time: 2.0, duration: 4, position: [1,0], relativeTo: 'Tile', ease: 'Linear' } ], frames: 360 },
  { name: 'kill_complete', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 0, time: 0.0, duration: 10, position: [10,0], relativeTo: 'Global', ease: 'Linear' },
              { floor: 4, time: 2.0, duration: 2, position: [0,0], relativeTo: 'Global', ease: 'Linear' } ], frames: 240 },
  { name: 'rotation_modes', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 2, time: 1.0, duration: 0, rotation: 30 },
              { floor: 4, time: 2.0, duration: 0, rotation: 0, relativeTo: 'LastPosition' },
              { floor: 6, time: 3.0, duration: 0, rotation: 0, relativeTo: 'LastPositionNoRotation' } ], frames: 210 },
  { name: 'zoom_dedup', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 2, time: 1.0, duration: 0, zoom: 200 },
              { floor: 4, time: 2.0, duration: 0, zoom: 50, position: [null,null] } ], frames: 180 },
  { name: 'no_position_player', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 2, time: 1.0, duration: 0, position: [2,1], relativeTo: 'Tile' },
              { floor: 4, time: 2.0, duration: 0, relativeTo: 'Player' } ], frames: 180 },
  { name: 'legacy_relative', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100, legacy: true },
    events: [ { floor: 2, time: 1.0, duration: 0, position: [3,0] },
              { floor: 4, time: 2.0, duration: 0, position: [3,0] } ], frames: 150 },
  { name: 'nan_zero_global', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 2, time: 1.0, duration: 0, position: [null,5], relativeTo: 'Global' } ], frames: 120 },
  { name: 'ease_outback', settings: { relativeTo: 'Player', position: [0,0], rotation: 0, zoom: 100 },
    events: [ { floor: 2, time: 1.0, duration: 4, position: [3,-2], relativeTo: 'Global', ease: 'OutBack', zoom: 300 } ], frames: 240 },
];

function flagsOf(ev) {
  let f = 0;
  if (Array.isArray(ev.position)) f |= FLAG.hasPos;
  if (Array.isArray(ev.position) && ev.position[0] !== null && ev.position[0] !== undefined) f |= FLAG.hasX;
  if (Array.isArray(ev.position) && ev.position[1] !== null && ev.position[1] !== undefined) f |= FLAG.hasY;
  if (ev.rotation !== undefined && ev.rotation !== null) f |= FLAG.hasRot;
  if (ev.zoom !== undefined && ev.zoom !== null) f |= FLAG.hasZoom;
  if (ev.ease !== undefined) f |= FLAG.easeW;
  return f;
}

let out = '// 自动生成 —— 跨语言金标：同一批事件喂给 Re_ADOJAS 的 CameraController.ts 与我们的\n';
out += '// core/camera/CameraDirector，逐帧比较 (x, y, rot, zoom, followMode)。\n';
out += '// 生成器：tests/gen_camera_gold.mjs（node 直接 import 那份 .ts，类型擦除）。\n';
out += '// 采样：dt = 1/60，每 15 帧一个点（t = k * (1.0/60.0)，两边同一算式）。\n';
out += 'struct GoldKey { double time, ax, ay; float bpm; int32_t floor; float x, y, rot, zoom, dur; uint8_t ease, relTo, flags; };\n';
out += 'struct GoldSample { int k; double x, y, rot, zoom; int follow; };\n';
out += 'struct GoldScenario { const char* name; float posX, posY, rot, zoom; const char* relTo; bool legacy; const GoldKey* keys; int keyCount; const GoldSample* samples; int sampleCount; };\n\n';

for (const sc of scenarios) {
  const levelData = { settings: { position: sc.settings.position, rotation: sc.settings.rotation, zoom: sc.settings.zoom,
                                  relativeTo: sc.settings.relativeTo, legacyCamRelativeTo: !!sc.settings.legacy } };
  const ctrl = new CameraController(levelData, tileStartTimes, tileBPM, positions);
  const timeline = sc.events.map(e => ({ time: e.time, event: { ...e, floor: e.floor } }));
  ctrl.loadCameraTimeline(timeline);
  // 键：事件时刻的锚点/ BPM 与"已展开"的最终字段
  const keys = sc.events.map(e => ({ ...e }));
  let keysOut = '';
  for (const e of keys) {
    const ax = xs[e.floor], ay = ys[e.floor];
    const flags = flagsOf(e);
    keysOut += `  {${e.time}, ${ax}, ${ay}, ${BPM}, ${e.floor}, ${e.position?.[0] ?? 0}, ${e.position?.[1] ?? 0}, ${e.rotation ?? 0}, ${e.zoom ?? 100}, ${e.duration ?? 0}, ${EASE[e.ease ?? 'Linear']}, ${e.relativeTo === undefined ? 255 : REL[e.relativeTo]}, ${flags}},\n`;
  }
  // 跑
  const samples = [];
  for (let k = 0; k <= sc.frames; k++) {
    const t = k * DT;
    const p = planetAt(t);
    ctrl.update({ nowSeconds: t, deltaSeconds: DT, bpm: BPM, pitch: 1, planetWorldPos: p, currentTileIndex: tileAt(t) });
    if (k % 15 === 0) {
      const q = ctrl.getCameraPosition();
      samples.push({ k, x: q.x, y: q.y, rot: ctrl.getCameraRotation(), zoom: ctrl.getCameraZoom(), follow: ctrl.isFollowMode() ? 1 : 0 });
    }
  }
  let samplesOut = '';
  for (const s of samples)
    samplesOut += `  {${s.k}, ${s.x.toPrecision(17)}, ${s.y.toPrecision(17)}, ${s.rot.toPrecision(17)}, ${s.zoom.toPrecision(17)}, ${s.follow}},\n`;
  out += `static const GoldKey kGoldKeys_${sc.name}[] = {\n${keysOut}};\n`;
  out += `static const GoldSample kGoldSamples_${sc.name}[] = {\n${samplesOut}};\n\n`;
}

out += 'static const GoldScenario kGoldScenarios[] = {\n';
for (const sc of scenarios) {
  out += `  {"${sc.name}", ${sc.settings.position[0]}, ${sc.settings.position[1]}, ${sc.settings.rotation}, ${sc.settings.zoom}, "${sc.settings.relativeTo}", ${sc.settings.legacy ? 'true' : 'false'}, kGoldKeys_${sc.name}, ${sc.events.length}, kGoldSamples_${sc.name}, `;
  const n = Math.floor(sc.frames / 15) + 1;
  out += `${n}},\n`;
}
out += '};\n';
writeFileSync(process.env.GOLD_OUT || 'tests/camera_gold.inc', out);   // 由 wrapper 从仓库根调用
console.log('scenarios', scenarios.length, 'bytes', out.length);
