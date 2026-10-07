#pragma once

// PNG 写出用的 deflate 层：统一走 **zlib**。
//
// 为什么不是 miniz：`mz_deflate` 在 MZ_NO_FLUSH 下会吞进整行却不吐输出（1024 行 >120s），
// 而 `PngBand`/`PngStream` 都是流式用法；core/CMakeLists.txt 在系统没有 zlib 时会
// FetchContent 取一份，所以这里直接硬要求它 —— 硬错误好过静默写坏 PNG。
//
// 这个头把 zlib 的名字映射成 miniz 风格的 `mz_*`，于是两个写出器的源码不用改。
#if !defined(ADOCAO_HAVE_ZLIB)
  #error "PNG 写出需要 zlib（core/CMakeLists.txt 在系统没有时会 FetchContent 取一份）。"
#endif

#include <zlib.h>

using mz_ulong = uLong;
#define mz_stream               z_stream
#define mz_deflateInit          deflateInit
#define mz_deflateInit2         deflateInit2
#define mz_deflate              deflate
#define mz_deflateEnd           deflateEnd
#define mz_adler32              adler32
#define mz_crc32                crc32
#define MZ_OK                   Z_OK
#define MZ_STREAM_END           Z_STREAM_END
#define MZ_BUF_ERROR            Z_BUF_ERROR
#define MZ_DEFLATED             Z_DEFLATED
#define MZ_NO_FLUSH             Z_NO_FLUSH
#define MZ_FULL_FLUSH           Z_FULL_FLUSH
#define MZ_FINISH               Z_FINISH
#define MZ_DEFAULT_COMPRESSION  Z_DEFAULT_COMPRESSION
#define MZ_DEFAULT_STRATEGY     Z_DEFAULT_STRATEGY
#define MZ_CRC32_INIT           crc32(0L, Z_NULL, 0)
#define MZ_DEFAULT_WINDOW_BITS  15
