#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "core/level/ByteSource.hpp"   // 容器种类 / magic 判断 / WindowSource 接口都在 core
#include "Install.hpp"                 // install()：这个头**不含** lzma/zstd 类型，app 可以放心引

// 谱面容器识别与解压。
//
// 除了明文 .adofai，社区/工具还会用两种压缩容器存放同一份 JSON：
//   .adofai.xz   —— xz (LZMA2)
//   .adofai.zst  —— zstd
// 按 **magic** 判断而不是扩展名（../Song.adofai 那套 audio-as-chart 工具也是这么
// 自动识别的），所以改过名、或扩展名被抹掉的谱面照样能读。

namespace adofai {

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
class ArchiveStream : public WindowSource {
public:
    static constexpr size_t kDefaultHalf = 96u << 20;   // 96 MB（半窗）

    ArchiveStream();
    ~ArchiveStream() override;
    ArchiveStream(const ArchiveStream&) = delete;
    ArchiveStream& operator=(const ArchiveStream&) = delete;

    bool open(const char* data, size_t length, LevelArchiveKind kind,
              size_t halfSize = kDefaultHalf) override;
    bool next() override;              // 装填下一块；false = 结束（或出错/卡住）
    // 访问器都在 .cpp 里定义：Impl 在头里是不完整类型，内联实现没法解引用它。
    const char* data() const override;
    size_t size() const override;
    void consume(size_t completeBytes) override;   // 记录残缺尾部长度，下一块补在开头
    bool failed() const override;
    bool stuck() const override;       // 单个值 > 半窗，调用方退回整份解压
    bool eof() const override;
    size_t halfSize() const override;
    const std::string& error() const override;

private:
    // lzma_stream / ZSTD_DStream 等三方状态藏在 Impl 里（pimpl）：这个头因此**不含任何
    // lzma/zstd 类型**，只 include 它的消费者不必配压缩库的头文件路径。
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool pump(size_t carry);
    void release();
};

// archive 模块的入口：注册后端（core 通过 ArchiveBackend 钩子调用这里的实现）
namespace archive {

ArchiveBackend backend();   // decodeWhole + makeWindow（install() 的声明在 Install.hpp）

}  // namespace archive

}  // namespace adofai
