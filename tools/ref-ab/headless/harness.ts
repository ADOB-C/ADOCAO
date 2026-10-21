// 在 Node 里直接跑 Re_ADOJAS 的 TimelineManager（不启浏览器），用于和 ADOCAO 的
// AnimateTrack 结果逐值对拍。`virtual:wasm-easing` 是 vite 虚拟模块，这里由 esbuild 插件桩掉
// （WasmEasing 会回落到 JS 的 EasingFunctions）。
import fs from 'node:fs';
import { TimelineManager } from './src/lib/Player/TimelineManager';

const chartPath = process.argv[2];
const raw = fs.readFileSync(chartPath, 'utf8').replace(/^\uFEFF/, '');
const chart = JSON.parse(raw);
// 网格由 ADOCAO 导出（ADOCAO_TRACKVIS_DUMP=2 → build/trackvis_grid.csv），
// 这样两边的 tileStartTimes/tileBPM **逐位相同**，比较的纯粹是 AnimateTrack 逻辑。
const rows = fs.readFileSync(process.argv[3], 'utf8').trim().split('\n').slice(1);
const n = rows.length;
const tileStartTimes = new Float64Array(n);
const tileBPM = new Float32Array(n);
for (const line of rows) {
  const [f, t, b] = line.split(',');
  tileStartTimes[Number(f)] = Number(t);
  tileBPM[Number(f)] = Number(b);
}
const bases = {
  posX: new Float64Array(n), posY: new Float64Array(n),
  rotRad: (_i: number) => 0, scaleX: (_i: number) => 1, scaleY: (_i: number) => 1, opacity: (_i: number) => 1,
};
const tm: any = new TimelineManager(chart.actions ?? [], tileStartTimes, tileBPM, bases as any, n, chart.settings);
// 打印几个关键点：tile 5 的 opacity（floor1 把 appear 设成 None → 应该恒为 1，不该淡入）
const t5 = tileStartTimes[6];   // 我们的网格里 tile i 的起点是 startTimes[i]，R 里 entryTime 用同值
for (const t of [t5 - 1.0, t5 - 0.5, t5 - 0.1, t5, t5 + 0.1]) {
  const op = tm.sample('tile:5', 'opacity', t);
  console.log(`R tile:5 opacity t=${t.toFixed(3)} -> ${op === undefined ? 'undefined(无动画=1)' : op.toFixed(4)}`);
}
// tile 20：disappear Fade 从 nextEntry 开始
const t20 = tileStartTimes[21];
for (const t of [t20 - 0.05, t20 + 0.005, t20 + 0.02]) {
  const op = tm.sample('tile:20', 'opacity', t);
  console.log(`R tile:20 opacity t=${t.toFixed(3)} -> ${op === undefined ? 'undefined(无动画=1)' : op.toFixed(4)}`);
}
