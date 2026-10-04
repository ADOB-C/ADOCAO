// 把一张谱塞进 Re_ADOJAS（参考播放器）的编辑器里并截图 —— 用来"两边对拍同一个形状"。
//
//   NODE_PATH=$(npm root -g) node adojas_chart.cjs <谱.adofai> [基址] [输出.png]
//   例：NODE_PATH=$(npm root -g) node adojas_chart.cjs midspin-sample.adofai
//
// 前置：Re_ADOJAS 的 dev server 开着（在它的仓库里 `pnpm dev`，默认 http://127.0.0.1:3144）。
// 坑：Re_ADOJAS 前端是 **HashRouter**，页面路径必须写 `#/editor`；写 `/editor` 只会给你首页，
// 表现出来是"载入成功但画面是首页"。
let chromium
try {
  ({ chromium } = require('playwright'))
} catch {
  console.error('需要 playwright：NODE_PATH=$(npm root -g) node adojas_chart.cjs …')
  process.exit(2)
}

const CHROME = process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium'
const chart = process.argv[2]
const base = process.argv[3] || 'http://127.0.0.1:3144'
const out = process.argv[4] || 'adojas-editor.png'
if (!chart) {
  console.error('用法：node adojas_chart.cjs <谱.adofai> [基址] [输出.png]')
  process.exit(2)
}

;(async () => {
  const browser = await chromium.launch({ executablePath: CHROME, headless: true })
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 }, deviceScaleFactor: 2 })
  page.on('pageerror', e => console.log('[pageerror]', e.message.slice(0, 200)))
  page.on('console', m => { if (m.type() === 'error') console.log('[err]', m.text().slice(0, 200)) })

  await page.goto(base + '/#/editor', { waitUntil: 'domcontentloaded' })
  await page.waitForTimeout(4000)
  const inputs = await page.locator('input[type=file]').count()
  console.log('file inputs:', inputs, '（第一个是 .adofai/.json/.zip）')
  if (!inputs) { console.error('没找到文件输入框 —— 页面没到编辑器？'); process.exit(1) }

  await page.locator('input[type=file]').first().setInputFiles(chart)
  await page.waitForTimeout(6000)
  await page.screenshot({ path: out })
  console.log('body:', (await page.evaluate(() => document.body.innerText)).slice(0, 200).replace(/\n+/g, ' | '))
  await browser.close()
  console.log('ok', out)
})().catch(e => { console.error('FAIL', e.message); process.exit(1) })
