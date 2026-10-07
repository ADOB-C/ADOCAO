#pragma once

#include <cstddef>
#include <functional>
#include <string>

// 谱面容器识别 + "解压字节从哪来"的**接口**（2026-10 解耦）。
//
// core 只认这份文件：容器种类与 magic 判断在这里（纯字节比较），真正的解压由 **archive 模块**
// 实现并通过下面的后端钩子注册进来（依赖倒置）。于是：
//   * core 不 include lzma/zstd，只想解析明文谱的消费者不用链压缩库；
//   * 没注册后端时：明文照常解析，压缩容器给出明确错误（而不是链不上或静默失败）。

namespace adofai {

enum class LevelArchiveKind { Plain, Xz, Zstd, Adocao };

// `.adocao` 的 magic（"ADO1"）：**唯一来源** —— core 的 sniff 与 archive 的读写都用它，
// archive/AdocaoFormat.hpp 用 static_assert 钉住一致。
inline constexpr char kAdocaoMagic[4] = {'A', 'D', 'O', '1'};

struct LevelData;

// `.adocao` 解码钩子：core 只按 magic 分派，真正的解析在 archive 模块（依赖倒置）。
// 失败时**必须**给出原因；不许静默回退到别的路径（文件坏了就说坏了）。
using AdocaoDecoder = bool (*)(const char* data, size_t length, LevelData& out, std::string& reason);

// 按 **magic** 判断容器种类（不看扩展名，所以改过名/抹掉扩展名的谱面照样能读）
LevelArchiveKind sniffLevelArchive(const char* data, size_t length);

// 窗口式解压源：不把整份解压结果摊在内存里，而是交替使用两块固定地址的"半窗"
// （实现见 archive/LevelArchive.cpp 的 ArchiveStream，那儿有完整的使用说明与内存实测）。
class WindowSource {
public:
    virtual ~WindowSource() = default;

    virtual bool open(const char* data, size_t length, LevelArchiveKind kind, size_t halfSize) = 0;
    virtual bool next() = 0;                                  // 装填下一块；false = 结束/出错/卡住
    virtual const char* data() const = 0;
    virtual size_t size() const = 0;
    virtual void consume(size_t completeBytes) = 0;            // 残缺尾部留到下一块补
    virtual bool failed() const = 0;
    virtual bool stuck() const = 0;                            // 单个值 > 半窗 → 调用方退回整份解压
    virtual bool eof() const = 0;
    virtual size_t halfSize() const = 0;
    virtual const std::string& error() const = 0;
};

// 整份解压到 out（覆盖写）。失败时返回 false，reason 里是原因。
using WholeDecoder = bool (*)(const char* data, size_t length, LevelArchiveKind kind,
                              std::string& out, std::string& reason,
                              const std::function<void(float)>& onProgress);

// 后端能力：三个都可以为空（= 该能力不可用）。由 archive 模块注册。
struct ArchiveBackend {
    WholeDecoder decodeWhole = nullptr;
    WindowSource* (*makeWindow)() = nullptr;
    AdocaoDecoder decodeAdocao = nullptr;
};

void setArchiveBackend(const ArchiveBackend& backend);   // archive 模块在启动时调用
const ArchiveBackend& archiveBackend();

}  // namespace adofai
