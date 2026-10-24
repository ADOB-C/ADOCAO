// Re_ADOJAS（参考实现）驱动：打开一张谱 → 空格播放 → 到指定秒数 → 截图。
// 由 scripts/ref-ab.sh 调用；单独用也可以：
//   ADOJAS_REF=~/Repos/Re_ADOJAS NODE_PATH=$(npm root -g) \
//     node tools/ref-ab/ref_play.cjs <chart> <out.png> <秒>
//
// 注意：必须用**没有被 hardened runtime 签名**的 node（macOS 的 homebrew node 可以）；
// DSH 自带的 node 带 runtime 标志 → rolldown 的 .node 原生绑定会因 Team ID 不同被 dyld 拒绝
// （`Cannot find native binding` 那一坨）。浏览器默认系统 Chromium。
const { chromium } = require('playwright')
const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const URL = process.env.REF_URL || 'http://127.0.0.1:5199/'
const [chart, out, secArg, mapArg] = process.argv.slice(2)
const target = parseFloat(secArg || '10')
// **按页面上的 Map Time 对齐**（可选第 4 个参数）：
// 我们的 `--capture-time` 是"关卡时间"（tile 1 过判定线为 0），而页面显示的 Map Time
// 从歌曲开头算（含谱面 offset 与倒计时），两者差一个固定 preroll（MM2 约 4.9 s）。
// 只按墙钟 sleep 会让 A/B 差好几秒 —— 大谱上就是"完全不同的两段路"。
const extraWait = mapArg ? parseFloat(mapArg) : 0   // 第 4 个参数 = 额外等待秒数（= Map Time - 关卡时间）
function parseMap(text) {
  // 宽松：标签和数值之间可能有 '|'、换行、空格
  const m = text.match(/Map Time[\s\S]{0,16}?([0-9]+):([0-9]+(?:\.[0-9]+)?)/)
  if (!m) return null
  return parseInt(m[1], 10) * 60 + parseFloat(m[2])
}
const W = 1280, H = 720

;(async () => {
  const browser = await chromium.launch({ executablePath: CHROME, headless: true })
  const page = await browser.newPage({ viewport: { width: W, height: H }, deviceScaleFactor: 1 })
  page.on('pageerror', e => console.log('[pageerror]', e.message))
  await page.goto(URL, { waitUntil: 'domcontentloaded' })
  await page.waitForTimeout(2500)
  await page.getByText('打开播放编辑器').click().catch(() => {})
  await page.waitForTimeout(7000)
  const inputs = await page.$$('input[type=file]')
  if (!inputs.length) { console.log('FAIL: 编辑器里没有 file input'); process.exit(1) }
  await inputs[0].setInputFiles(chart)
  await page.waitForTimeout(25000)          // 大谱（3.4 MB / 1 万砖）解析
  await page.keyboard.press('Space')         // 空格开始播放
  const t0 = Date.now()
  await page.waitForTimeout((target + extraWait) * 1000)
  await page.screenshot({ path: out })
  const txt = await page.evaluate(() => document.body.innerText)
  const m = txt.match(/Tiles\s*\|\s*(\d+)\s*\/\s*(\d+)/)
  const mt = txt.match(/Map Time\s*\|\s*([0-9:.]+)~([0-9:.]+)/)
  console.log(`ref: wall=${((Date.now() - t0) / 1000).toFixed(1)}s tile=${m ? m[1] : '?'}/${m ? m[2] : '?'} mapTime=${mt ? mt[1] : '?'}`)
  await browser.close()
})().catch(e => { console.error('FAIL', e.message); process.exit(1) })
