#include "core/level/TileColor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace adofai {
namespace {

constexpr double kSixth[5] = {1.0 / 6.0, 1.0 / 3.0, 1.0 / 2.0, 2.0 / 3.0, 5.0 / 6.0};

// Re_ADOJAS 的 fmod 是"正模"（a - b*floor(a/b)），不是 std::fmod 的截断语义。
double posMod(double a, double b) {
    if (b == 0.0) return 0.0;
    return a - b * std::floor(a / b);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

std::uint8_t toByte(double v) {
    // JS 是 Math.round（.5 向上），与 std::lround 一致；越界钳制（见 rainbow 的边界说明）。
    const long r = std::lround(v);
    return (std::uint8_t)std::clamp<long>(r, 0, 255);
}

double Rchange(double p) {
    if (p > 0 && p < kSixth[0]) return 1;
    if (p < kSixth[1]) return 1 - (p - kSixth[0]) / kSixth[0];
    if (p < kSixth[3]) return 0;
    if (p < kSixth[4]) return (p - kSixth[3]) / kSixth[0];
    return 1;
}
double Gchange(double p) {
    if (p > 0 && p < kSixth[0]) return p / kSixth[0];
    if (p < kSixth[2]) return 1;
    if (p < kSixth[3]) return 1 - (p - kSixth[2]) / kSixth[0];
    return 0;
}
double Bchange(double p) {
    if (p > 0 && p < kSixth[1]) return 0;
    if (p < kSixth[2]) return (p - kSixth[1]) / kSixth[0];
    if (p < kSixth[4]) return 1;
    return 1 - (p - kSixth[4]) / kSixth[0];
}

struct RgbF {
    double r = 0, g = 0, b = 0;
};

// Re_ADOJAS `ColorType._rainbow()` 的逐行移植（RAINBOW_PROCESS + R/G/Bchange）。
RgbF rainbow(Rgb8 c, double percent) {
    const double cr = c.r, cg = c.g, cb = c.b;
    char maxCh = 'r', minCh = 'r';
    if (cg > cr) maxCh = 'g';
    if (cb > (maxCh == 'r' ? cr : (maxCh == 'g' ? cg : cb))) maxCh = 'b';
    if (cg < cr) minCh = 'g';
    if (cb < (minCh == 'r' ? cr : (minCh == 'g' ? cg : cb))) minCh = 'b';
    const double maxVal = (maxCh == 'r') ? cr : (maxCh == 'g' ? cg : cb);
    const double minVal = (minCh == 'r') ? cr : (minCh == 'g' ? cg : cb);
    if (maxVal == minVal) return {cr, cg, cb};

    // RAINBOW_PROCESS: {信道, 方向, 基准}
    struct Deal { char ch; int dir; double base; };
    Deal deal{};
    const std::string key = std::string(1, (char)toupper((unsigned char)maxCh)) +
                            std::string(1, (char)toupper((unsigned char)minCh));
    if (key == "RB") deal = {'G', 1, 0.0};
    else if (key == "GB") deal = {'R', -1, 0.16666666};
    else if (key == "GR") deal = {'B', 1, 0.3333333333};
    else if (key == "BR") deal = {'G', -1, 0.5};
    else if (key == "BG") deal = {'R', 1, 0.66666666666};
    else if (key == "RG") deal = {'B', -1, 0.83333333333};
    else return {cr, cg, cb};

    const double range = maxVal - minVal;
    const double dealVal = (deal.ch == 'R') ? cr : (deal.ch == 'G' ? cg : cb);
    const double midVal = minVal;
    double per;
    if (deal.dir == 1) per = deal.base + (dealVal - midVal) / range / 6.0;
    else               per = deal.base + (maxVal - dealVal) / range / 6.0;
    per = posMod(per + percent, 1.0);
    const double base = minVal;
    return {base + range * Rchange(per), base + range * Gchange(per), base + range * Bchange(per)};
}

double pulsePercent(const TileColorConfig& cfg, double timeSec, int floor) {
    const double anim = (double)cfg.animDuration;
    const double len = (double)cfg.pulseLength;
    const double span = anim != 0.0 ? anim : 1.0;
    const double t = posMod(timeSec - cfg.startTime, span) / span;
    if (cfg.pulse == "Forward" || cfg.pulse == "Backward") {
        const double spanLen = len != 0.0 ? len : 1.0;
        const double f = posMod((double)floor - (double)cfg.startFloor, spanLen) / spanLen;
        return posMod(cfg.pulse == "Forward" ? t - f : t + f, 1.0);
    }
    return posMod(t, 1.0);   // None（以及未知值）→ pulseNone
}

// COLOR_FUNCS：从本体色算"基色"。percent 语义见各分支。
Rgb8 baseColor(const TileColorConfig& cfg, double percent, float amplitude) {
    const Rgb8 c1 = parseColorHex(cfg.color);
    const Rgb8 c2 = parseColorHex(cfg.secondary);
    const std::string& t = cfg.colorType;
    auto lerp = [](std::uint8_t a, std::uint8_t b, double p) {
        return toByte((double)a + ((double)b - (double)a) * p);
    };
    if (t == "Stripes" || t == "Switch") {
        return percent < 0.5 ? c1 : c2;
    }
    if (t == "Glow") {
        const double pp = 1.0 - std::fabs(1.0 - 2.0 * percent);
        return {lerp(c1.r, c2.r, pp), lerp(c1.g, c2.g, pp), lerp(c1.b, c2.b, pp), c1.a};
    }
    if (t == "Blink") {
        return {lerp(c1.r, c2.r, percent), lerp(c1.g, c2.g, percent), lerp(c1.b, c2.b, percent), c1.a};
    }
    if (t == "Volume") {
        const double amp = std::clamp((double)amplitude, 0.0, 1.0);
        return {lerp(c1.r, c2.r, amp), lerp(c1.g, c2.g, amp), lerp(c1.b, c2.b, amp), c1.a};
    }
    if (t == "Rainbow") {
        const RgbF f = rainbow(c1, percent);
        return {toByte(f.r), toByte(f.g), toByte(f.b), c1.a};
    }
    return c1;   // Single（以及未知类型）→ 本体色
}

}  // namespace

bool isTimeDrivenColorType(std::string_view type) {
    return type == "Glow" || type == "Blink" || type == "Rainbow" || type == "Volume";
}

Rgb8 parseColorHex(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
    Rgb8 out{};
    auto at = [&](size_t i) -> int { return i < hex.size() ? hexVal(hex[i]) : 0; };
    out.r = (std::uint8_t)(at(0) * 16 + at(1));
    out.g = (std::uint8_t)(at(2) * 16 + at(3));
    out.b = (std::uint8_t)(at(4) * 16 + at(5));
    out.a = hex.size() >= 8 ? (std::uint8_t)(at(6) * 16 + at(7)) : (std::uint8_t)255;
    return out;
}

void applyTrackStyle(std::string_view style, std::uint8_t baseR, std::uint8_t baseG,
                     std::uint8_t baseB, std::uint8_t outFill[3], std::uint8_t outStroke[3]) {
    auto mul = [](std::uint8_t c, double k) { return toByte((double)c * k); };
    if (style == "Neon") {
        outFill[0] = outFill[1] = outFill[2] = 0;
        outStroke[0] = baseR; outStroke[1] = baseG; outStroke[2] = baseB;
    } else if (style == "NeonLight") {
        outFill[0] = mul(baseR, 0.5); outFill[1] = mul(baseG, 0.5); outFill[2] = mul(baseB, 0.5);
        outStroke[0] = baseR; outStroke[1] = baseG; outStroke[2] = baseB;
    } else if (style == "Basic") {
        outFill[0] = baseR; outFill[1] = baseG; outFill[2] = baseB;
        outStroke[0] = outStroke[1] = outStroke[2] = 0;
    } else if (style == "Minimal") {
        outFill[0] = outStroke[0] = baseR;
        outFill[1] = outStroke[1] = baseG;
        outFill[2] = outStroke[2] = baseB;
    } else {
        // Standard / Gems（以及未知值 → Standard）
        outFill[0] = baseR; outFill[1] = baseG; outFill[2] = baseB;
        outStroke[0] = mul(baseR, 0.7); outStroke[1] = mul(baseG, 0.7); outStroke[2] = mul(baseB, 0.7);
    }
}

TileColors computeTileColors(const TileColorConfig& cfg, double timeSec, int floor, float amplitude) {
    const double percent = (cfg.colorType == "Stripes")
        ? (((floor - cfg.startFloor) % 2 + 2) % 2 == 1 ? 1.0 : 0.0)
        : pulsePercent(cfg, timeSec, floor);
    const Rgb8 base = baseColor(cfg, percent, amplitude);
    TileColors out;
    applyTrackStyle(cfg.style, base.r, base.g, base.b, out.fill, out.stroke);
    out.opacity = (float)parseColorHex(cfg.color).a / 255.0f;
    return out;
}

}  // namespace adofai
