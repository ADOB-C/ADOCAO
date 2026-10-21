// Re_ADOJAS：打开谱 → 播放 → **轮询 HUD 的 Tiles 计数**，到指定砖立刻截图。
//   node tools/ref-ab/ref_tile_play.cjs <chart> <tile> <out.png>
const { chromium } = require('playwright')
const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const URL = process.env.REF_URL || 'http://127.0.0.1:5199/'
const [chart, tileArg, out] = process.argv.slice(2)
const target = parseInt(tileArg, 10)
const W = 1280, H = 720
const hud = async page => {
  const t = await page.evaluate(() => document.body.innerText)
  const m = t.match(/Tiles\s*\|\s*(\d+)\s*\/\s*(\d+)/)
  const mt = t.match(/Map Time\s*\|\s*([0-9:.]+)~([0-9:.]+)/)
  return { tile: m ? +m[1] : -1, map: mt ? mt[1] : '?' }
}
;(async () => {
  const browser = await chromium.launch({ executablePath: CHROME, headless: true })
  const page = await browser.newPage({ viewport: { width: W, height: H }, deviceScaleFactor: 1 })
  page.on('pageerror', e => console.log('[pageerror]', e.message))
  await page.goto(URL, { waitUntil: 'domcontentloaded' })
  await page.waitForTimeout(2500)
  await page.getByText('打开播放编辑器').click().catch(() => {})
  await page.waitForTimeout(7000)
  const inputs = await page.$$('input[type=file]')
  if (!inputs.length) { console.log('NO FILE INPUT'); process.exit(1) }
  await inputs[0].setInputFiles(chart)
  await page.waitForTimeout(25000)
  await page.keyboard.press('Space')
  const t0 = Date.now()
  let last = null
  for (let i = 0; i < 4000; i++) {
    last = await hud(page)
    if (last.tile >= target) break
    await page.waitForTimeout(100)
  }
  await page.screenshot({ path: out })
  console.log(`hit tile=${last.tile} map=${last.map} wall=${((Date.now() - t0) / 1000).toFixed(1)}s -> ${out}`)
  await browser.close()
})().catch(e => { console.error('FAIL', e.message); process.exit(1) })
