#pragma once

#include <string>

// 把"用户给的 level 路径"解析成真正的谱文件：
//   * 是文件（或不存在）→ 原样返回，交给调用方报错；
//   * 是目录 → 直接子文件里**唯一**的谱就用它；没有就看下一层子目录
//     （`Charts/Song.adofai/<chart>/<name>.adofai.xz` 那种结构）；多于一个就原样返回。
// 唯一命中才生效、绝不猜 —— 文件名以 .adofai / .adofai.xz / .adofai.zst 结尾（大小写不敏感）。
// 动机：macOS 的文件对话框按"名字后缀"匹配，于是名字以 .adofai 结尾的**文件夹**也能被选中；
// 而本工程的谱一律放在同名文件夹里，所以直接把"选中文件夹"变成可用输入。
std::string resolveLevelPath(const std::string& path);
