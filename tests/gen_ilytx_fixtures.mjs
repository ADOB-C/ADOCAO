// 生成 `.ilytx` 测试样本 —— **用 Re_ADOJAS 自己的编码器**（`src/lib/ilytx/ilybin.ts` + `tar.ts`），
// 不在这边手抄格式：自己写、自己读的对拍永远是绿的（AGENTS.md 的"对拍测试"铁律）。
//
// 产物（`tests/level_fixtures/`）：
//   ilytx_ref.adofai            参照谱：源谱 + 样本里那 3 个 decoration（逐位比对的基准）
//   ilytx_object.ilytx          tileMode=0（对象模式，direction 走 f64）→ 必须与参照谱**逐位相同**
//   ilytx_compact.ilytx         tileMode=1（紧凑模式，direction 走 f32）→ 角度值有 f32 量化误差
//   ilytx_missing_level.ilytx   tar 里没有 level.ilybin        → 必须干净失败
//   ilytx_truncated.ilytx       tar 在 ilybin 中间被截断        → 必须干净失败
//   ilytx_badmagic.ilytx        level.ilybin 的 magic 被改坏     → 必须干净失败
//   ilytx_frac_ref.adofai       第二组：**小数角度**谱（f32/f64 往返与缩放编码的真正考验）
//   ilytx_frac_object.ilytx     对象模式 → 与 frac 参照谱**逐位**相同
//   ilytx_frac_compact.ilytx    紧凑模式 → 出现 f32 量化误差（< 1e-3 度），其余仍相同
//
// 用法（需要本机的 Re_ADOJAS 克隆；默认 ~/Repos/Re_ADOJAS，可用 RE_ADOJAS_DIR 覆盖）：
//   node tests/gen_ilytx_fixtures.mjs
//
// 成员的顺序故意不一致（object 版 level.ilybin 在中间、compact 版在最后），用来覆盖
// "按名字找成员"而不是"按位置找"。

import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const RE = process.env.RE_ADOJAS_DIR ?? path.join(process.env.HOME, 'Repos/Re_ADOJAS')
const here = path.dirname(fileURLToPath(import.meta.url))
const fixtures = path.join(here, 'level_fixtures')
const src = path.join(here, 'charts', 'angles360.adofai')

const ilybinMod = await import(`file://${RE}/src/lib/ilytx/ilybin.ts`)
const tarMod = await import(`file://${RE}/src/lib/ilytx/tar.ts`)
const typesMod = await import(`file://${RE}/src/lib/ilytx/types.ts`)
const { encodeIlybin, TILE_MODE_OBJECT, TILE_MODE_COMPACT } = ilybinMod
const { buildTar } = tarMod
const { MEMBER_LEVEL, MEMBER_MANIFEST, ILYTX_VERSION } = typesMod

// ---- 源谱 → ilybin 的四个载荷 ----
const raw = JSON.parse(fs.readFileSync(src, 'utf8'))
const angleData = raw.angleData
if (!Array.isArray(angleData) || angleData.length === 0) throw new Error('源谱没有 angleData')

// settings 段 = 文件里**除掉** angleData / actions / decorations 的部分（原样，含嵌套的 "settings"）
const settings = { ...raw }
delete settings.angleData
delete settings.actions
delete settings.decorations

// 计数型 twirl：累计 Twirl 事件数（ilybin 存的是累计值，不是奇偶）
const twirlEvents = (raw.actions ?? [])
  .filter((a) => a.eventType === 'Twirl')
  .map((a) => a.floor)
  .sort((x, y) => x - y)
const twirl = new Float64Array(angleData.length)
for (let i = 0, k = 0; i < angleData.length; i++) {
  while (k < twirlEvents.length && twirlEvents[k] <= i) k++
  twirl[i] = k
}

// 样本装饰（原谱一个都没有 —— 加进来才能测 decorations 段的透传）
const decorations = [
  { floor: 10, eventType: 'AddDecoration', decorationImage: 'd1.png', position: [1, 2], scale: [100, 100] },
  { floor: 40, eventType: 'AddDecoration', decorationImage: 'd2.png', relativeTo: 'Player' },
  { floor: 5000, eventType: 'AddDecoration', decorationImage: 'd3.png', tag: 't' },
]
const actions = raw.actions ?? []

// 参照谱：源谱 + 上面那 3 个装饰（单行紧凑 JSON，和源谱同样的排版）
const ref = { ...raw, decorations }
fs.writeFileSync(path.join(fixtures, 'ilytx_ref.adofai'), JSON.stringify(ref))

// ---- 打包 ----
function tarWith(ilybin, { levelLast }) {
  const manifest = {
    format: 'ilytx',
    version: ILYTX_VERSION,
    generator: 'ADOCAO tests/gen_ilytx_fixtures.mjs',
    created: 0,
    tileCount: angleData.length,
  }
  const entries = [
    { name: MEMBER_MANIFEST, data: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: 'audio/song.ogg', data: new Uint8Array([0x4f, 0x67, 0x67, 0x53, 0, 0, 0, 0]) },
    { name: 'decor/d1.png', data: new Uint8Array([0x89, 0x50, 0x4e, 0x47]) },
  ]
  const level = { name: MEMBER_LEVEL, data: ilybin }
  // levelLast=false → level.ilybin 夹在中间（后面还有成员），覆盖"按名字找、不按位置"
  return buildTar(levelLast ? [...entries, level] : [entries[0], level, ...entries.slice(1)])
}

function writeXz(name, tarBytes) {
  const tmp = path.join(fixtures, `.${name}.tar`)
  fs.writeFileSync(tmp, tarBytes)
  execFileSync('xz', ['-9', '-c', tmp], { stdio: ['ignore', fs.openSync(path.join(fixtures, name), 'w'), 'inherit'] })
  fs.unlinkSync(tmp)
  console.log(`${name}: ${fs.statSync(path.join(fixtures, name)).size} B`)
}

const payload = (direction, twirlArr, tileMode, extra = {}) => ({
  tileMode,
  tileCount: angleData.length,
  settings,
  direction,
  angle: new Float64Array(angleData.length), // v1 遗留字段；编码器 v2 起不写
  twirl: twirlArr,
  actions,
  decorations,
  extraProps: null,
  ...extra,
})

const objBin = encodeIlybin(payload(Float64Array.from(angleData), twirl, TILE_MODE_OBJECT))
const cmpBin = encodeIlybin(payload(Float32Array.from(angleData), twirl, TILE_MODE_COMPACT))

writeXz('ilytx_object.ilytx', tarWith(objBin, { levelLast: false }))
writeXz('ilytx_compact.ilytx', tarWith(cmpBin, { levelLast: true }))

// ---- 三个"必须干净失败"的样本 ----
writeXz('ilytx_missing_level.ilytx', buildTar([
  { name: MEMBER_MANIFEST, data: new TextEncoder().encode('{"format":"ilytx","version":2}') },
]))
writeXz('ilytx_truncated.ilytx', tarWith(objBin.subarray(0, Math.floor(objBin.length / 2)), { levelLast: false }))
{
  const bad = Uint8Array.from(objBin)
  bad[0] = 0x58; bad[1] = 0x58; bad[2] = 0x58; bad[3] = 0x58   // "XXXX" 取代 "ILYB"
  writeXz('ilytx_badmagic.ilytx', tarWith(bad, { levelLast: false }))
}

console.log('ilytx_ref.adofai:', fs.statSync(path.join(fixtures, 'ilytx_ref.adofai')).size, 'B')

// ---- 第二组：小数角度（angles360 全是整数，f32 恰好精确 → 量化误差恒 0，测不到 f32 那条路）----
{
  const fracSrc = JSON.parse(fs.readFileSync(path.join(here, 'charts', 'diag_duet.adofai'), 'utf8'))
  const SPECIAL = new Set([999, 555, 666, 777, 888])
  // 只扰动**普通角度**：999/555/666/777/888 是标记，改了语义就变了
  const frac = fracSrc.angleData.map((v) => (SPECIAL.has(v) ? v : (v + 0.123456789) % 360))
  const fracSettings = { ...fracSrc }
  delete fracSettings.angleData
  delete fracSettings.actions
  delete fracSettings.decorations
  const fracActs = fracSrc.actions ?? []
  const fracTwirl = new Float64Array(frac.length)   // 这支谱没有 Twirl 事件
  const fracRef = { ...fracSrc, angleData: frac, decorations: [] }
  fs.writeFileSync(path.join(fixtures, 'ilytx_frac_ref.adofai'), JSON.stringify(fracRef))

  const mk = (direction, mode) => ({
    tileMode: mode,
    tileCount: frac.length,
    settings: fracSettings,
    direction,
    angle: new Float64Array(frac.length),
    twirl: fracTwirl,
    actions: fracActs,
    decorations: [],
    extraProps: null,
  })
  writeXz('ilytx_frac_object.ilytx',
          tarWith(encodeIlybin(mk(Float64Array.from(frac), TILE_MODE_OBJECT)), { levelLast: true }))
  writeXz('ilytx_frac_compact.ilytx',
          tarWith(encodeIlybin(mk(Float32Array.from(frac), TILE_MODE_COMPACT)), { levelLast: true }))
  console.log('ilytx_frac_ref.adofai:', fs.statSync(path.join(fixtures, 'ilytx_frac_ref.adofai')).size, 'B')
}
