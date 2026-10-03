#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <lzma.h>
#include <zstd.h>

// 谱面容器识别与解压。
//
// 除了明文 .adofai，社区/工具还会用两种压缩容器存放同一份 JSON：
//   .adofai.xz   —— xz (LZMA2)
//   .adofai.zst  —— zstd
// 按 **magic** 判断而不是扩展名（../Song.adofai 那套 audio-as-chart 工具也是这么
// 自动识别的），所以改过名、或扩展名被抹掉的谱面照样能读。
enum class LevelArchiveKind { Plain, Xz, Zstd };

LevelArchiveKind sniffLevelArchive(const char* data, size_t length);

// 解压到 out（覆盖写）。可选进度回调，参数 0..1。失败时返回 false，reason 里是原因。
bool decompressLevelArchive(const char* data, size_t length, LevelArchiveKind kind,
                            std::string& out, std::string& reason,
                            const std::function<void(float)>& onProgress = nullptr);

// 流式解压：不把整份解压结果摊在内存里，而是交替使用两块固定地址的"半窗"。
//
// 为什么：一张 10 GB 文本的 .xz 谱面，整份解压就是 10 GB 匿名内存 —— 物理内存装不下，
// 系统只能压缩/写 swap，每次访问再解压回来（实测吞吐 1.35 GB/s → 0.12 GB/s，还要写盘）。
// 半窗方案把它变成 2 × halfSize 的常驻缓冲，解压与解析可以交替进行。
//
// 消费方（解析器）的使用方式：
//   stream.open(...);  stream.next();
//   while (true) {
//       size_t complete = 在 data()/size() 里能处理完的完整字节数;
//       stream.consume(complete);          // 剩下的残缺值留到下一块补上
//       if (!stream.next()) break;
//   }
// 于是任何跨窗的 JSON 值都能连续（carry 会把残缺部分拷到下一块开头）。
// 单个值比半窗还大时会卡住：next() 返回 false 且 stuck() 为真，调用方应退回整份解压。
class ArchiveStream {
public:
    static constexpr size_t kDefaultHalf = 96u << 20;   // 96 MB（半窗）

    ArchiveStream() = default;
    ~ArchiveStream();
    ArchiveStream(const ArchiveStream&) = delete;
    ArchiveStream& operator=(const ArchiveStream&) = delete;

    bool open(const char* data, size_t length, LevelArchiveKind kind,
              size_t halfSize = kDefaultHalf);
    bool next();                       // 装填下一块；false = 结束（或出错/卡住）
    const char* data() const { return m_buf[m_cur]; }
    size_t size() const { return m_len; }
    void consume(size_t completeBytes);   // 记录残缺尾部长度，下一块补在开头
    bool failed() const { return m_failed; }
    bool stuck() const { return m_stuck; }   // 单个值 > 半窗，调用方退回整份解压
    bool eof() const { return m_eof; }
    size_t halfSize() const { return m_half; }
    const std::string& error() const { return m_error; }

private:
    bool pump(size_t carry);
    void release();

    std::vector<char> m_store;
    char* m_buf[2] = {nullptr, nullptr};
    size_t m_half = 0, m_len = 0, m_carry = 0, m_cur = 0;
    bool m_eof = false, m_failed = false, m_stuck = false, m_started = false;
    bool m_finalDelivered = false;   // 只含 carry 的尾块是否已经交付过
    std::string m_error;
    const char* m_in = nullptr;
    size_t m_inLen = 0;
    LevelArchiveKind m_kind = LevelArchiveKind::Plain;
    lzma_stream m_strm = LZMA_STREAM_INIT;
    ZSTD_DStream* m_ds = nullptr;
    ZSTD_inBuffer m_zin{nullptr, 0, 0};
    bool m_frameDone = false;
};
