#include "MacBorderless.hpp"

#import <Cocoa/Cocoa.h>

#include <initializer_list>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace adofai {
namespace {

// 过场结束时刻（只用于"过场期间别改窗口几何"这条自我保护，不是系统状态）
double g_transitionUntil = 0.0;

void beginTransition() { g_transitionUntil = glfwGetTime() + 0.8; }

// **进 Space 之后把焦点抢回来**。`toggleFullScreen:` 是带过场的异步操作：过场期间窗口
// 会先 resign key，动作完成后再变成 key —— 我们在 toggle 之后立刻 activate 是**无效的**
// （用户实测：窗口化能拿到焦点，全屏（进 Space）拿不到）。
// 所以在过场结束后再 activate + makeKey 一次（两次，覆盖慢动画）。
void activateAndFocus(NSWindow* w) {
    if (!w) return;
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    [w makeKeyAndOrderFront:nil];
    // 键盘事件走 first responder；无边框窗口 makeKey 之后视图不一定是第一响应者
    if (NSView* v = [w contentView]) [w makeFirstResponder:v];
    NSLog(@"[ADOCAO] focus: key=%d main=%d active=%d firstResponder=%@",
          (int)[w isKeyWindow], (int)[w isMainWindow],
          (int)[[NSApplication sharedApplication] isActive], [[w firstResponder] class]);
}
void focusAfterTransition(NSWindow* w) {
    [w retain];
    for (double delay : {0.35, 1.10}) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                           activateAndFocus(w);
                           [w release];
                       });
    }
}

bool isFullScreen(NSWindow* w) { return ([w styleMask] & NSWindowStyleMaskFullScreen) != 0; }

// **P0（2026-10-19 审计）**：GLFW 给 undecorated 窗口的 styleMask 只有
// Miniaturizable|Borderless，**没有 Resizable ⇒ `toggleFullScreen:` 会被静默拒绝**。
// SDL 在 `windowWillEnterFullScreen` 里补 Titled|Closable|Miniaturizable|Resizable
// （SDL_cocoawindow.m:1416，注释说明 Titled 是 fullscreen 下能收到键鼠事件所必需）。
void ensureFullscreenCapable(NSWindow* w) {
    const NSWindowStyleMask need = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    if (([w styleMask] & need) != need) [w setStyleMask:([w styleMask] | need)];
    [w setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
}

}  // namespace

bool macosFullscreenTransitionActive() { return glfwGetTime() < g_transitionUntil; }

void macosWindowedChrome(GLFWwindow* window) {
    NSWindow* w = glfwGetCocoaWindow(window);
    if (!w) return;
    // GLFW 的 Titled 窗口默认带 关闭/最小化/缩放 三个按钮。这里只留关闭：
    // styleMask 去掉 Resizable/Miniaturizable，并把黄/绿按钮藏起来。
    [w setStyleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)];
    [w setCollectionBehavior:NSWindowCollectionBehaviorFullScreenNone];
    [[w standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
    [[w standardWindowButton:NSWindowZoomButton] setHidden:YES];
}

void macosBorderlessEnter(GLFWwindow* window) {
    NSWindow* w = glfwGetCocoaWindow(window);
    if (!w) return;
    // 只做"进自己的 Space"（= osu! 的 Borderless / SDL_WINDOW_FULLSCREEN_DESKTOP）。
    // 不切显示模式、不改 presentation options、不碰系统 defaults —— 2026-10-19 的事故
    // 全部来自那几样东西（见 docs/mac-recovery.md 的禁令清单）。
    ensureFullscreenCapable(w);
    if (!isFullScreen(w)) {
        beginTransition();
        [w toggleFullScreen:nil];
        focusAfterTransition(w);   // 过场结束后再抢焦点（立刻抢没用）
    } else {
        activateAndFocus(w);
    }
    [w setLevel:NSNormalWindowLevel];
}

void macosBorderlessExit(GLFWwindow* window) {
    NSWindow* w = glfwGetCocoaWindow(window);
    if (!w) return;
    if (isFullScreen(w)) {
        beginTransition();
        [w toggleFullScreen:nil];
    }
}

}  // namespace adofai
