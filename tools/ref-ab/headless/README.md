# headless：在 Node 里直接跑 Re_ADOJAS 的 `TimelineManager`

用途：**秒级**验证「动画/时间窗」类语义（AnimateTrack、appear/disappear、MoveTrack 时间轴…），
不需要起 vite + Chromium。要看整帧构图才用上一层的 `scripts/ref-ab.sh`。

## 用法

```bash
# 1) 我们这边把"同一套网格"导出（否则比的是时间轴而不是动画逻辑）
ADOCAO_TRACKVIS_DUMP=2 ./build/adocao <chart.adofai> --capture /tmp/x.png --capture-time 1 \
    --capture-camera --width 640 --height 360 --no-hitsound
#    → build/trackvis_grid.csv（floor,startTime,bpm）

# 2) 打包 R 的 TimelineManager（vite 虚拟模块 virtual:wasm-easing 用空模块桩掉，
#    WasmEasing 会自动回落到 JS 缓速）
cd "$ADOJAS_REF"   # 默认 ~/Repos/Re_ADOJAS；ADOCAO=本仓库根
node -e '
const esbuild = require("esbuild");
esbuild.build({entryPoints:["$ADOCAO/tools/ref-ab/headless/harness.ts"], bundle:true,
  platform:"node", format:"esm", outfile:"/tmp/r-harness.mjs", external:["three"], logLevel:"error",
  plugins:[{name:"w",setup(b){
    b.onResolve({filter:/^virtual:wasm-easing$/},()=>({path:"w",namespace:"s"}));
    b.onLoad({filter:/.*/,namespace:"s"},()=>({contents:"export default \"\";",loader:"js"}));}}]});
'

# 3) 跑：参数 = <谱> <网格 csv>
node /tmp/r-harness.mjs <chart.adofai> $ADOCAO/build/trackvis_grid.csv
```

输出的 `sample('tile:N','opacity',t)` 与我们的 `Timeline::animAt` / `ADOCAO_TRACKVIS_DUMP`
逐值对照。2026-10-21 用它确认了：floor 1 的 `trackAnimation: None` 让 appear 关闭
（R 返回 undefined = 无动画），floor 20 的 `disappear=Fade, beatsBehind=0` 在
entry+0 开始淡出（0.95 / 0.80 @ +5 ms / +20 ms，和我们窗口算出的值一致）。
