#pragma once

// 打拍音类型表：CLI 的 `--force-hitsound` 与向导的 Override 下拉**共用这一份**。
// 以前两边各写一份，加 "raw-pcm" 时漏了逗号 → `"Sizzle" "raw-pcm"` 被 C++ 当字符串拼接，
// 数组少一个元素、末位变成 nullptr，GUI 里显示成 `*Unknown item*`。
#include <array>
#include <cstddef>

inline constexpr std::array<const char*, 29> kHitsoundTypes = {
    "Kick","KickHouse","KickChroma","KickRupture",
    "Snare","SnareHouse","SnareVapor","Clap","ClapHit","ClapHitEcho",
    "Hat","HatHouse","Chuck","Hammer","Shaker","ShakerLoud",
    "Sidestick","Stick","ReverbClack","ReverbClap","Squareshot",
    "FireTile","IceTile","PowerUp","PowerDown","VehiclePositive",
    "VehicleNegative","Sizzle","raw-pcm",   // raw-pcm: 直通（audio-as-chart）   // 直通：把逐层音量当 PCM 播（audio-as-chart 谱面）
};

// 编译期守卫：非空元素数必须等于声明的大小。
// 漏逗号 → 相邻字面量被拼接 → 少一个元素 → 这里直接编译失败（而不是 GUI 里静默变成
// `*Unknown item*`）。
inline constexpr std::size_t kHitsoundTypeCount = [] {
    std::size_t n = 0;
    for (const char* p : kHitsoundTypes) if (p != nullptr) n++;
    return n;
}();
static_assert(kHitsoundTypeCount == kHitsoundTypes.size(),
              "kHitsoundTypes 元素数不对：多半是漏了逗号（相邻字符串字面量会被拼接）");
