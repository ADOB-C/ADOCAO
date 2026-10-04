#pragma once

#include <cstdint>
#include <functional>
#include <cstddef>
#include <string>

// 分带并行写一张 PNG（pigz 的做法）：
//   把行按 band 切开 → 每个 band 用**独立的 raw deflate** 压缩（无 zlib 头/尾）→
//   band 之间以 FULL_FLUSH 断开（字典重置）→ 主线程按序把压缩块拼成同一条 zlib 流，
//   最后写上 adler32。于是压缩小事化了：各 band 完全独立、可并行。
//
// produceRow(y, packed)：把第 y 行打包成 packed（(width+7)/8 字节、MSB-first、1 表示墨）。
//   会被多个 worker 并***发***调用，所以实现必须线程安全（写入各自的 packed 缓冲即可）。
// 内存上限：最多 maxPending 个已完成的 band 压在内存里（默认 threads*2）。
// threadInit(i)：第 i 个 worker 线程启动时调用一次。core 里不碰平台 API，落核策略
//   （macOS 的 QoS → P/E 核）由调用方在 app 层决定。
bool writePngParallel(const std::string& path, uint32_t width, uint32_t height,
                      int bitDepth, int colorType, const uint8_t* palette, size_t paletteLen,
                      int bandRows, int threads,
                      const std::function<void(long long, uint8_t*)>& produceRow,
                      int maxPending = 0,
                      std::function<void(int)> threadInit = {});

// 自检用：adler32 拼接（与 mz_adler32 对同一段数据的结果必须一致）
uint32_t adler32Combine(uint32_t s1a, uint32_t s2a, size_t lenB, uint32_t s1b, uint32_t s2b);
