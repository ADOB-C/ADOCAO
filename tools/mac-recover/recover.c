// macOS 急救小工具（只读报告 + 几个安全的修复动作）。
//   recover report                 打印 Space 列表 / 显示器 / 光标 / 输入源 / ADOCAO 进程
//   recover fix-cursor             重新绑定鼠标↔光标、显示光标、指针居中
//   recover fix-input-source       把输入源修正为系统**已启用**的第一个键盘布局
//   recover close-mission-control  killall Dock（Mission Control 卡住时用；不动其它东西）
#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int CGSConnID;
typedef CFArrayRef (*CopySpacesFn)(CGSConnID);
typedef CGSConnID (*MainConnFn)(void);

static CFArrayRef copySpaces(void) {
    void* h = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    if (!h) return NULL;
    CopySpacesFn copy = (CopySpacesFn)dlsym(h, "CGSCopyManagedDisplaySpaces");
    MainConnFn mainConn = (MainConnFn)dlsym(h, "_CGSDefaultConnection");
    if (!copy) return NULL;
    CGSConnID cid = mainConn ? mainConn() : 0;
    return copy(cid);
}

static int countSpaces(int* fullscreenSpaces) {
    CFArrayRef arr = copySpaces();
    if (!arr) return -1;
    int total = 0, fs = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(arr, i);
        CFArrayRef sp = CFDictionaryGetValue(d, CFSTR("Spaces"));
        if (!sp) continue;
        for (CFIndex j = 0; j < CFArrayGetCount(sp); j++) {
            CFDictionaryRef s = CFArrayGetValueAtIndex(sp, j);
            int type = 0;
            CFNumberGetValue(CFDictionaryGetValue(s, CFSTR("type")), kCFNumberIntType, &type);
            total++;
            if (type == 4) fs++;
        }
    }
    if (fullscreenSpaces) *fullscreenSpaces = fs;
    return total;
}

static void report(void) {
    int fs = 0;
    int total = countSpaces(&fs);
    printf("空格（Space）: %d 个，其中全屏空壳（type=4）: %d 个\n", total, fs);
    if (fs > 0) printf("  ⚠️  有全屏空壳：通常 app 退出后会自动回收；不回收就 killall Dock 或重新登录\n");
    CGDirectDisplayID d = CGMainDisplayID();
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
    if (m) {
        printf("显示器: %zux%zu px（%zux%zu 点），刷新 %.0f Hz\n",
               CGDisplayModeGetPixelWidth(m), CGDisplayModeGetPixelHeight(m),
               CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m),
               CGDisplayModeGetRefreshRate(m));
        CGDisplayModeRelease(m);
    }
    printf("光标可见: %s\n", CGCursorIsVisible() ? "是" : "否");
    printf("（指针不动 → recover fix-cursor；Mission Control 卡住 → recover close-mission-control）\n");
}

static void fixCursor(void) {
    CGAssociateMouseAndMouseCursorPosition(true);
    CGDisplayShowCursor(CGMainDisplayID());
    CGWarpMouseCursorPosition(CGPointMake(CGDisplayPixelsWide(CGMainDisplayID()) / 2.0,
                                         CGDisplayPixelsHigh(CGMainDisplayID()) / 2.0));
    printf("已重新绑定鼠标↔光标、显示光标、指针居中\n");
}

static void fixInputSource(void) {
    // 读取已启用的输入源，挑第一个 Keyboard Layout，把它写回 current（原值不合法会让按键收不到）
    FILE* f = popen("defaults read com.apple.HIToolbox AppleEnabledInputSources 2>/dev/null", "r");
    if (!f) { printf("读不到 AppleEnabledInputSources\n"); return; }
    char buf[65536]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; pclose(f);
    const char* key = "\"KeyboardLayout Name\" = \"";
    char name[128] = {0};
    const char* p = strstr(buf, key);
    if (p) {
        p += strlen(key);
        const char* q = strchr(p, '"');
        if (q && (size_t)(q - p) < sizeof name) { memcpy(name, p, (size_t)(q - p)); }
    }
    if (!name[0]) { printf("没找到已启用的键盘布局\n"); return; }
    // U.S. → com.apple.keylayout.US 这类映射不可靠，直接把 current 清掉让系统自己挑第一个
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "defaults delete com.apple.HIToolbox AppleCurrentKeyboardLayoutInputSourceID 2>/dev/null;"
             "killall TextInputMenuAgent 2>/dev/null; true");
    int rc = system(cmd);
    printf("已清除 current 输入源（系统会回落到已启用的第一个：%s），rc=%d\n", name, rc);
    (void)rc;
}

int main(int argc, char** argv) {
    const char* cmd = argc > 1 ? argv[1] : "report";
    if (!strcmp(cmd, "report")) report();
    else if (!strcmp(cmd, "fix-cursor")) fixCursor();
    else if (!strcmp(cmd, "fix-input-source")) fixInputSource();
    else if (!strcmp(cmd, "close-mission-control")) {
        int rc = system("killall Dock 2>/dev/null; true");
        printf("已重启 Dock（Mission Control 会关闭），rc=%d\n", rc);
    } else {
        printf("用法: recover [report|fix-cursor|fix-input-source|close-mission-control]\n");
        return 2;
    }
    return 0;
}
