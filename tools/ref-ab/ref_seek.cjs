// Re_ADOJAS `/editor`：**闭环 seek**（播放 → 轮询 __adojasState().elapsedMs → 到点按空格暂停 → 截图）。
// 之前"按空格再死等 N 秒"根本没对上时间（2026-10-21 用户指出）。
//   REF_URL=http://127.0.0.1:5202/ node tools/ref-ab/ref_seek.cjs <chart> <out.png> <levelSec>
const { chromium } = require('playwright')
const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const URL = process.env.REF_URL || 'http://127.0.0.1:5199/'
const [chart, out, secArg] = process.argv.slice(2)
const targetMs = parseFloat(secArg || '0') * 1000
const W = 1280, H = 720

;(async () => {
  const browser = await chromium.launch({ executablePath: CHROME, headless: true })
  const page = await browser.newPage({ viewport: { width: W, height: H }, deviceScaleFactor: 1 })
  await page.goto(URL, { waitUntil: 'domcontentloaded' })
  await page.waitForTimeout(2500)
  await page.getByText('打开播放编辑器').click().catch(() => {})
  await page.waitForTimeout(6000)
  const inputs = await page.$$('input[type=file]')
  if (!inputs.length) { console.log('FAIL: 没有 file input'); process.exit(1) }
  await inputs[0].setInputFiles(chart)
  await page.waitForTimeout(20000)

  await page.keyboard.press('Space')                     // 开始播放
  let state = null
  for (let i = 0; i < 1200; ++i) {                       // 最多轮询 ~12 s
    state = await page.evaluate(() => (window).__adojasState ? (window).__adojasState() : null)
    if (state && typeof state.elapsedMs === 'number' && state.elapsedMs >= targetMs) break
    await page.waitForTimeout(10)
  }
  // 不暂停（第二次 Space 是"重新播放"）：到点立刻截图，误差 = 一帧
  state = await page.evaluate(() => (window).__adojasState ? (window).__adojasState() : null)
  console.log('ref state:', JSON.stringify({ elapsedMs: state?.elapsedMs, tile: state?.tile, playing: state?.playing }))
  await page.screenshot({ path: out })
  await browser.close()
})()
