// Re_ADOJAS：播放到指定墙钟秒 → 点 #play-button 暂停 → 滚轮放大 → 截图
//   node tools/ref-ab/ref_pause_zoom.cjs <chart> <out.png> <墙钟秒> [滚轮格数]
const { chromium } = require('playwright')
const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const URL = process.env.REF_URL || 'http://127.0.0.1:5199/'
const [chart, out, secArg, ticksArg] = process.argv.slice(2)
const target = parseFloat(secArg || '42'), ticks = parseInt(ticksArg || '7', 10)
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
  if (!inputs.length) { console.log('NO FILE INPUT'); process.exit(1) }
  await inputs[0].setInputFiles(chart)
  await page.waitForTimeout(25000)
  await page.keyboard.press('Space')
  const t0 = Date.now()
  const wait = target * 1000 - (Date.now() - t0)
  if (wait > 0) await page.waitForTimeout(wait)
  await page.click('#play-button')             // 暂停
  await page.waitForTimeout(600)
  await page.mouse.move(W / 2, H / 2)
  for (let i = 0; i < ticks; i++) { await page.mouse.wheel(0, -240); await page.waitForTimeout(120) }
  await page.waitForTimeout(1200)
  await page.screenshot({ path: out })
  console.log('paused wall=', ((Date.now() - t0) / 1000).toFixed(2), '->', out)
  await browser.close()
})().catch(e => { console.error('FAIL', e.message); process.exit(1) })
