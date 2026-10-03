#pragma once
#include <cstddef>
#include <functional>
#include <string>

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
