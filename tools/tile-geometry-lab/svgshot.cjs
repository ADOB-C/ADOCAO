// 把本地 SVG/HTML 用**系统无头 Chromium**拍成 PNG（给"实际看图"用）。
//
//   NODE_PATH=$(npm root -g) node svgshot.cjs <in.svg> <out.png> [宽] [高]
//
// Chromium 路径可用环境变量覆盖（默认 macOS 的 /Applications/Chromium.app）：
//   CHROME=/path/to/chrome NODE_PATH=$(npm root -g) node svgshot.cjs in.svg out.png
//
// 注意：一定要用 headless 的 page.screenshot（非 fullPage）—— 试过 fullPage:true，
// 在 SVG 文档上会卡到 30 s 超时。
let chromium
try {
  ({ chromium } = require('playwright'))
} catch {
  console.error('需要 playwright：NODE_PATH=$(npm root -g) node svgshot.cjs …（或 npm i -g playwright）')
  process.exit(2)
}

const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const [src, out, w, h] = process.argv.slice(2)
if (!src || !out) {
  console.error('用法：node svgshot.cjs <in.svg> <out.png> [宽] [高]')
  process.exit(2)
}

;(async () => {
  const browser = await chromium.launch({ executablePath: CHROME, headless: true })
  const page = await browser.newPage({ viewport: { width: +(w || 1200), height: +(h || 900) }, deviceScaleFactor: 2 })
  page.on('pageerror', e => console.log('[pageerror]', e.message))
  await page.goto('file://' + src, { waitUntil: 'domcontentloaded' })
  await page.waitForTimeout(1200)
  await page.screenshot({ path: out, animations: 'disabled', caret: 'hide' })
  await browser.close()
  console.log('ok', out)
})().catch(e => { console.error('FAIL', e.message); process.exit(1) })
