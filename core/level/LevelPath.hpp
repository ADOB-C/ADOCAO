#pragma once

#include <string>
#include <vector>

// 把"用户给的 level 路径"解析成真正的谱文件：
//   * 是文件 → 原样返回；
//   * 不存在 → 试 `<path> + 已知后缀`（.adofai / .adofai.xz / .adofai.zst / .adocao），
//     **唯一命中**才用它（macOS 的 Finder 默认隐藏扩展名，用户看到的路径往往没有后缀）；
//   * 是目录 → 直接子文件里**唯一**的谱就用它；没有就看下一层子目录
//     （`Charts/Song.adofai/<chart>/<name>.adofai.xz` 那种结构）；多于一个就原样返回。
// 唯一命中才生效、绝不猜 —— 文件名以 .adofai / .adofai.xz / .adofai.zst 结尾（大小写不敏感）。
// 动机：macOS 的文件对话框按"名字后缀"匹配，于是名字以 .adofai 结尾的**文件夹**也能被选中；
// 而本工程的谱一律放在同名文件夹里，所以直接把"选中文件夹"变成可用输入。

namespace adofai {

std::string resolveLevelPath(const std::string& path);

// 供 UI 用：这个目录里能当谱文件的有哪些（先看直接子文件，没有再往下看一层子目录；
// 直接子文件有命中时就不会去翻子目录）。非目录 / 不存在 → 空。
std::vector<std::string> listLevelCandidates(const std::string& dir);

}  // namespace adofai
