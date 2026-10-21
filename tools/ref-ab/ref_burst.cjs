// Re_ADOJAS：打开谱 → 播放 → 在给定的一组墙钟秒数上各截一帧（用来命中某一砖所在的瞬间）
//   node tools/ref-ab/ref_burst.cjs <chart> <outPrefix> 41.5 42.0 42.5 43.0
const { chromium } = require('playwright')
const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const URL = process.env.REF_URL || 'http://127.0.0.1:5199/'
const [chart, prefix, ...secs] = process.argv.slice(2)
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
  const marks = secs.map(Number).sort((a, b) => a - b)
  for (const m of marks) {
    const wait = m * 1000 - (Date.now() - t0)
    if (wait > 0) await page.waitForTimeout(wait)
    const f = `${prefix}_${m.toFixed(1)}.png`
    await page.screenshot({ path: f })
    console.log(`shot ${f} wall=${((Date.now() - t0) / 1000).toFixed(2)}s`)
  }
  await browser.close()
})().catch(e => { console.error('FAIL', e.message); process.exit(1) })
