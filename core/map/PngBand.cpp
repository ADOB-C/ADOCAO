#include "core/map/PngBand.hpp"

#include "core/util/Logger.hpp"

// 有系统 zlib 就用它 —— 已验证：同一份数据 zlib 2.0s 跑完且逐字节正确，
// 而 miniz 的 mz_deflate 在 NO_FLUSH 下会吞入整行却不吐输出（1024 行 >120s）。
// 没有 zlib 时退回 miniz（CI 的 Windows 就是这种情形，可编译即可）。
#if defined(ADOCAO_HAVE_ZLIB)
  #include <zlib.h>
  using mz_ulong = uLong;
  #define mz_stream               z_stream
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
#else
  #error "分带并行 PNG 写出需要系统 zlib（core/CMakeLists.txt 在找不到时会 FetchContent 取一份）。\
 miniz 的 mz_deflate/mz_inflate 在这条用法下会吞行、也解不开自己写的流 —— 硬错误好过静默写坏 PNG。"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const uint32_t kBase = 65521;
const uint8_t  kSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

bool writeChunk(std::FILE* f, const char* type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> hdr; put32(hdr, (uint32_t)len);
    if (std::fwrite(hdr.data(), 1, hdr.size(), f) != hdr.size()) return false;
    if (std::fwrite(type, 1, 4, f) != 4) return false;
    if (len && std::fwrite(data, 1, len, f) != len) return false;
    uint8_t t[4]; std::memcpy(t, type, 4);
    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, t, 4);
    if (len) crc = mz_crc32(crc, data, len);
    uint8_t out[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    return std::fwrite(out, 1, 4, f) == 4;
}

}  // namespace

uint32_t adler32Combine(uint32_t s1a, uint32_t s2a, size_t lenB, uint32_t s1b, uint32_t s2b) {
    // adler(A||B): s1 = s1a + s1b - 1;  s2 = s2a + s2b + lenB*(s1a - 1)
    // （对着 zlib 的 adler32_combine 校准过：s2 这里**不能**再减 1）
    const uint64_t s1 = ((uint64_t)s1a + s1b - 1) % kBase;
    const uint64_t s2 = ((uint64_t)s2a + s2b + (uint64_t)lenB * (s1a - 1)) % kBase;
    return (uint32_t)((s2 << 16) | s1);
}

bool writePngParallel(const std::string& path, uint32_t width, uint32_t height,
                      int bitDepth, int colorType, const uint8_t* palette, size_t paletteLen,
                      int bandRows, int threads,
                      const std::function<void(long long, uint8_t*)>& produceRow,
                      int maxPending, std::function<void(int)> threadInit) {
    if (width == 0 || height == 0 || bandRows <= 0) return false;
    const size_t rowBytes = (bitDepth == 1 && colorType == 3) ? ((size_t)width + 7) / 8
                                                              : (size_t)width * 4;
    const long long NB = (height + bandRows - 1) / bandRows;
    const int K = std::max(1, threads > 0 ? threads : (int)std::thread::hardware_concurrency());
    const int pendingCap = maxPending > 0 ? maxPending : K * 2;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { LOG_E("PngBand: 无法写入 %s", path.c_str()); return false; }
    if (std::fwrite(kSig, 1, 8, f) != 8) { std::fclose(f); return false; }
    {
        std::vector<uint8_t> ihdr;
        put32(ihdr, width); put32(ihdr, height);
        ihdr.push_back((uint8_t)bitDepth); ihdr.push_back((uint8_t)colorType);
        ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
        if (!writeChunk(f, "IHDR", ihdr.data(), ihdr.size())) { std::fclose(f); return false; }
        if (colorType == 3 && palette && paletteLen && !writeChunk(f, "PLTE", palette, paletteLen)) {
            std::fclose(f); return false;
        }
        std::fflush(f);          // 头部立刻落盘：外部能马上看到文件"开始了"（~53 字节）
    }

    struct Band { std::vector<uint8_t> z; uint32_t s1 = 1, s2 = 0; size_t rawBytes = 0; bool done = false; };
    std::vector<Band> bands((size_t)NB);
    std::mutex m; std::condition_variable cv;
    long long nextBand = 0; size_t pending = 0;
    bool failed = false;

    auto worker = [&]() {
        for (;;) {
            long long b;
            { std::unique_lock<std::mutex> lk(m);
              // 注意 third clause：没活可领时也必须让等待结束，否则 worker 永远 park → join() 死锁
              cv.wait(lk, [&] { return failed || nextBand >= NB || pending < (size_t)pendingCap; });
              if (failed) return;
              if (nextBand >= NB) return;
              b = nextBand++; pending++; }
            const long long y0 = b * bandRows;
            const long long rows = std::min<long long>(bandRows, (long long)height - y0);
            std::vector<uint8_t> packed(rowBytes, 0), prev(rowBytes, 0), line(1 + rowBytes, 0);
            std::vector<uint8_t> out(1u << 18);
            mz_stream zs; std::memset(&zs, 0, sizeof zs);
            if (mz_deflateInit2(&zs, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                                -MZ_DEFAULT_WINDOW_BITS, 8, MZ_DEFAULT_STRATEGY) != MZ_OK) {
                std::lock_guard<std::mutex> lk(m); failed = true; cv.notify_all(); return;
            }
            Band& bd = bands[(size_t)b];
            uint32_t adler = 1;
            bd.rawBytes = (size_t)rows * (1 + rowBytes);
            for (long long r = 0; r < rows; ++r) {
                produceRow(y0 + r, packed.data());
                line[0] = (r == 0) ? 0 : 2;          // band 的首行必须用 None（前面那行不属于本 band）
                for (size_t i = 0; i < rowBytes; ++i)
                    line[1 + i] = (uint8_t)(packed[i] - prev[i]);
                std::memcpy(prev.data(), packed.data(), rowBytes);
                adler = (uint32_t)mz_adler32(adler, line.data(), line.size());
                zs.next_in = line.data(); zs.avail_in = (unsigned)line.size();
                while (zs.avail_in) {
                    zs.next_out = out.data(); zs.avail_out = (unsigned)out.size();
                    const int st = mz_deflate(&zs, MZ_NO_FLUSH);
                    const size_t produced = out.size() - zs.avail_out;
                    if (produced) bd.z.insert(bd.z.end(), out.data(), out.data() + produced);
                    if (st != MZ_OK && st != MZ_BUF_ERROR) { mz_deflateEnd(&zs); std::lock_guard<std::mutex> lk(m); failed = true; cv.notify_all(); return; }
                }
            }
            {   // band 收尾：中间 band 用 FULL_FLUSH（重置字典，后续 band 才能独立起跑）
                const int flush = (b == NB - 1) ? MZ_FINISH : MZ_FULL_FLUSH;
                for (;;) {
                    zs.next_in = nullptr; zs.avail_in = 0;
                    zs.next_out = out.data(); zs.avail_out = (unsigned)out.size();
                    const int st = mz_deflate(&zs, flush);
                    const size_t produced = out.size() - zs.avail_out;
                    if (produced) bd.z.insert(bd.z.end(), out.data(), out.data() + produced);
                    if (st == MZ_STREAM_END) break;
                    if (st == MZ_BUF_ERROR) break;        // 没有更多可输出 = flush 完成（不是错误）
                    if (st != MZ_OK) { mz_deflateEnd(&zs); std::lock_guard<std::mutex> lk(m); failed = true; cv.notify_all(); return; }
                    if (produced == 0) break;
                }
            }
            mz_deflateEnd(&zs);
            bd.s1 = adler & 0xFFFF; bd.s2 = adler >> 16;
            { std::lock_guard<std::mutex> lk(m); bd.done = true; cv.notify_all(); }
        }
    };

    std::vector<std::thread> pool;
    for (int t = 0; t < K; ++t)
        pool.emplace_back([&worker, &threadInit, t] {
            if (threadInit) threadInit(t);      // 落核策略交给调用方（core 不碰平台 API）
            worker();
        });

    // 主线程按序写出：zlib 头 → 各 band 的压缩块 → adler32 → IEND
    {   // zlib 头也必须装在 IDAT 里（裸写会让 PNG 非法）
        const uint8_t zh[2] = { 0x78, 0x01 };
        if (!writeChunk(f, "IDAT", zh, 2)) failed = true;
    }
    uint32_t s1 = 1, s2 = 0;
    for (long long b = 0; b < NB && !failed; ++b) {
        { std::unique_lock<std::mutex> lk(m);
          cv.wait(lk, [&] { return failed || bands[(size_t)b].done; });   // 必须等"这一个"band
          if (failed) break; }
        const Band& bd = bands[(size_t)b];
        for (size_t off = 0; off < bd.z.size(); off += (1u << 20)) {
            const size_t n = std::min<size_t>(1u << 20, bd.z.size() - off);
            if (!writeChunk(f, "IDAT", bd.z.data() + off, n)) { failed = true; break; }
        }
        // 累加 adler（按写出的顺序），写完即可释放该 band 的压缩数据
        const uint32_t comb = adler32Combine(s1, s2, bd.rawBytes, bd.s1, bd.s2);
        s1 = comb & 0xFFFF; s2 = comb >> 16;
        { std::lock_guard<std::mutex> lk(m); bands[(size_t)b].z.clear(); bands[(size_t)b].z.shrink_to_fit();
          --pending; cv.notify_all(); }
    }
    { std::lock_guard<std::mutex> lk(m); cv.notify_all(); }   // 唤醒 park 的 worker
    for (auto& th : pool) th.join();
    bool ok = !failed;
    if (ok) {
        // adler32 是 zlib 流的一部分，必须装在 IDAT 里 —— 裸写 4 字节会让 PNG 非法
        const uint8_t tail[4] = { (uint8_t)(s2 >> 8), (uint8_t)s2, (uint8_t)(s1 >> 8), (uint8_t)s1 };
        ok = writeChunk(f, "IDAT", tail, 4) && writeChunk(f, "IEND", nullptr, 0);
    }
    std::fclose(f);
    if (!ok) LOG_E("PngBand: 写出失败 %s", path.c_str());
    return ok;
}
