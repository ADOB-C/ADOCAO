#pragma once
// macOS 窗口：只做**窗口级**操作，不碰任何系统状态。
//
// 2026-10-19 的事故与禁令（写在这里，防止再犯）：
//   ✗ `NSApplicationPresentationFullScreen | Hide*`：会禁用 Mission Control / ⌘Tab。
//     app 一卡住，用户就被困在它的全屏 Space 里出不来（只能在别的 app 里写字求救）。
//   ✗ `CGDisplayCapture`：把显示器从 WindowServer 合成里摘出去 → 我们自己的窗口也不上屏（黑屏）。
//   ✗ `CGDisplaySetDisplayMode` / 切显示模式：桌面从 2x 掉成 1x、所有窗口重排（"退出卡一下、桌面缩放还动"）。
//   ✗ `CGAssociateMouseAndMouseCursorPosition(false)` / `CGDisplayHideCursor`：**系统级**，
//     会把整台机器的鼠标解绑/光标藏掉（"鼠标自己都用不了了"）。
//   ✓ `toggleFullScreen:` 本身**可以**用（osu! 的 Borderless 就是靠它进自己的 Space），
//     但必须：① 先补 styleMask 的 Resizable/Titled（否则被静默拒绝，GLFW 的 undecorated
//     窗口没有这两位）；② 过场期间不要改窗口尺寸/位置（会被当成"用户拖窗口"→ 把全屏踢掉）。
//   ✗ 写 `com.apple.HIToolbox AppleCurrentKeyboardLayoutInputSourceID`：写成未启用的布局会
//     让"Keyboard Viewer 能看到按键、App 收不到，只有 Command 有效"。
//
// 现在只做：无边框窗口 + `toggleFullScreen:` 进**自己的 Space**（= osu! 的 Borderless）。
// **不切显示模式、不改 presentation options、不碰系统 defaults**。
// （2026-10-19 用户拍板：删掉 Exclusive，全屏一律用 osu! 的 Borderless 语义。）
struct GLFWwindow;
namespace adofai {
// 窗口态：**只留关闭按钮**（红），去掉最小化/缩放；窗口模式在启动时定死，运行期不改。
void macosWindowedChrome(GLFWwindow* window);
void macosBorderlessEnter(GLFWwindow* window);
void macosBorderlessExit(GLFWwindow* window);
// 全屏 Space 的过场是否还在进行（期间不要改窗口尺寸/位置）
bool macosFullscreenTransitionActive();
}  // namespace adofai
