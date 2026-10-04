// JSON（TileGeometry dump）→ SVG 面板图，俯视正交。读图用。
// 用法：node json2svg.mjs /tmp/midspin-lab/geo.json > /tmp/midspin-lab/geo.svg
import { readFileSync } from 'node:fs'

const cases = JSON.parse(readFileSync(process.argv[2], 'utf8'))
const PANEL = 300, PAD = 56, PX = 260

function panel(c, i) {
  const cx = PAD + PANEL / 2, cy = PAD + PANEL / 2
  const X = x => cx + x * PX, Y = y => cy - y * PX
  let g = ''
  for (let k = -2; k <= 2; k++) {
    g += `<line x1="${X(k / 10)}" y1="${cy - PX / 2}" x2="${X(k / 10)}" y2="${cy + PX / 2}" stroke="#232c3a" stroke-width="1"/>`
    g += `<line x1="${cx - PX / 2}" y1="${Y(k / 10)}" x2="${cx + PX / 2}" y2="${Y(k / 10)}" stroke="#232c3a" stroke-width="1"/>`
  }
  g += `<circle cx="${cx}" cy="${cy}" r="${0.5 * PX}" fill="none" stroke="#41324f" stroke-width="1" stroke-dasharray="4 4"/>`
  g += `<line x1="${cx - PX / 2}" y1="${cy}" x2="${cx + PX / 2}" y2="${cy}" stroke="#3b4757" stroke-width="1.5"/>`
  g += `<line x1="${cx}" y1="${cy - PX / 2}" x2="${cx}" y2="${cy + PX / 2}" stroke="#3b4757" stroke-width="1.5"/>`

  const tri = (a, b, cc, fill, stroke) =>
    `<polygon points="${[[a, b, cc].map(k => [c.verts[k * 3], c.verts[k * 3 + 1]])].flat().map(([x, y]) => `${X(x)},${Y(y)}`).join(' ')}" fill="${fill}" stroke="${stroke}" stroke-width="1.1"/>`

  for (let t = 0; t < c.indices.length; t += 3) {
    const [a, b, cc] = [c.indices[t], c.indices[t + 1], c.indices[t + 2]]
    if (c.types[a] < 0.5) g += tri(a, b, cc, 'rgba(96,116,146,0.35)', '#8399b3')
  }
  for (let t = 0; t < c.indices.length; t += 3) {
    const [a, b, cc] = [c.indices[t], c.indices[t + 1], c.indices[t + 2]]
    if (c.types[a] > 0.5) g += tri(a, b, cc, 'rgba(70,140,255,0.8)', '#d5e3ff')
  }
  // 顶点编号（只标描边层的 7 个）
  let labels = ''
  const seen = new Set()
  for (let t = 0; t < c.indices.length; t += 3) {
    for (const k of [c.indices[t], c.indices[t + 1], c.indices[t + 2]]) {
      if (seen.has(k) || c.types[k] > 0.5) continue
      seen.add(k)
      labels += `<text x="${X(c.verts[k * 3]) + 6}" y="${Y(c.verts[k * 3 + 1]) - 4}" fill="#9fb2c8" font-size="10" font-family="monospace">${k}</text>`
    }
  }
  return `<g transform="translate(${(i % 3) * (PANEL + 2 * PAD)},${Math.floor(i / 3) * (PANEL + 2 * PAD)})">
    <rect x="${PAD - 12}" y="${PAD - 44}" width="${PANEL + 24}" height="${PANEL + 96}" rx="10" fill="#111722" stroke="#26303f"/>
    <text x="${PAD}" y="${PAD - 24}" fill="#e8eef7" font-size="15" font-family="monospace">${c.label}</text>
    <text x="${PAD}" y="${PAD - 6}" fill="#8fa2ba" font-size="11" font-family="monospace">${c.note}</text>
    ${g}${labels}
  </g>`
}

const cols = 3, rows = Math.ceil(cases.length / cols)
const W = cols * (PANEL + 2 * PAD), H = rows * (PANEL + 2 * PAD) + 16
console.log(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}"><rect width="${W}" height="${H}" fill="#0b0f16"/>
${cases.map(panel).join('\n')}
</svg>`)
