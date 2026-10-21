# macOS 急救（`scripts/mac-recover.sh`）

ADOCAO 在 macOS 上碰过三处**系统级状态**，出过两次真事故。代码里的对应调用已经删掉/降级，
这里记录"如果再出现，怎么收拾"，以及**为什么**不能再这么写。

```bash
scripts/mac-recover.sh          # 只报告：Space 数量/全屏空壳、显示器模式、光标可见、ADOCAO 进程
scripts/mac-recover.sh fix      # 杀进程 → 重置光标 → 修输入源 → 重启 Dock → 复查
```

## 三个坑（都在代码里禁用了）

| 调用 | 后果（实测） | 现在的做法 |
|---|---|---|
| `NSApplicationPresentationFullScreen \| HideDock \| HideMenuBar` | 连带禁用 Mission Control / ⌘Tab。app 一旦卡住或被杀，用户**被困在它的全屏 Space 里**（只能在别的 app 里写字求救） | 只用 `AutoHideDock \| AutoHideMenuBar`（`app/MacBorderless.mm`） |
| `CGDisplayCapture` | 把显示器从 **WindowServer 合成**里摘出去 → **我们自己的窗口也不上屏**（黑屏） | 不用；"全屏观感"靠无边框窗口 + presentation options |
| `CGAssociateMouseAndMouseCursorPosition(false)` / `CGDisplayHideCursor` | **系统级**：解绑整台机器的鼠标、藏光标；进程异常退出还可能恢复不了（"鼠标自己都用不了了"） | 光标只用 GLFW 的窗口级 `GLFW_CURSOR_HIDDEN`，窗口销毁自动还原 |
| 写 `com.apple.HIToolbox AppleCurrentKeyboardLayoutInputSourceID` 成**未启用**的布局 | 按键进不了 App（Keyboard Viewer 能看到、只有 Command 有效） | **不要再写这个键**；诊断只读 |
| `toggleFullScreen:`（进自己的 Space） | **本身安全**（osu! 的 Borderless 就这么做），但 GLFW 的无边框窗口缺 `Resizable｜Titled` 时会被静默拒绝；过场期间改窗口几何会被当成"拖窗口"→ 踢掉全屏 | 允许使用，但必须先补 `styleMask`、过场期间不动窗口几何（`macosFullscreenTransitionActive()`） |

## 用户侧自救（脚本之外）

- 空的 ADOCAO Space：`⌃←` / `⌃→` 切走，或调度中心点别的桌面；不消失就 `killall Dock`；
- 键盘只在个别 App 失效：切到终端敲 `echo hi` 判断是否全局；全局失效就 `⌃⌘Q` 退出登录；
- 不要 `killall WindowServer`（等于注销，会丢未保存内容）。
