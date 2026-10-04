#include "FileDialogs.hpp"

#include <tinyfiledialogs.h>
#include <filesystem>

#ifdef _WIN32
#include <shobjidl.h>
#include <cstring>
#include <vector>

namespace wizard {
namespace {

std::string cleanupWin32(std::string r, bool comInitialized) {
    if (comInitialized) CoUninitialize();
    return r;
}

std::string wideToUtf8(const wchar_t* rawPath) {
    int u8len = WideCharToMultiByte(CP_UTF8, 0, rawPath, -1, nullptr, 0, nullptr, nullptr);
    std::string result(u8len ? u8len - 1 : 0, '\0');
    if (u8len > 1)
        WideCharToMultiByte(CP_UTF8, 0, rawPath, -1, &result[0], u8len, nullptr, nullptr);
    return result;
}

} // namespace

std::string openFileDialog(const char* title, const std::vector<std::string>& patterns,
                           const char* description) {
    bool comInitialized = (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK);

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                 IID_IFileOpenDialog, (void**)&dlg)))
        return cleanupWin32({}, comInitialized);

    std::wstring wtitle(title, title + strlen(title));
    // COMDLG accepts several patterns in one filter entry, separated by ';'
    std::string joined;
    for (size_t i = 0; i < patterns.size(); i++) {
        if (i) joined += ';';
        joined += patterns[i];
    }
    std::wstring wpatterns(joined.begin(), joined.end());
    std::wstring wdesc(description, description + strlen(description));
    COMDLG_FILTERSPEC spec{wdesc.c_str(), wpatterns.c_str()};
    if (!patterns.empty())
        dlg->SetFileTypes(1, &spec);

    dlg->SetTitle(wtitle.c_str());

    if (FAILED(dlg->Show(nullptr))) { dlg->Release(); return cleanupWin32({}, comInitialized); }

    IShellItem* item = nullptr;
    if (FAILED(dlg->GetResult(&item))) { dlg->Release(); return cleanupWin32({}, comInitialized); }

    wchar_t* rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
        item->Release(); dlg->Release(); return cleanupWin32({}, comInitialized);
    }

    std::string result = wideToUtf8(rawPath);
    CoTaskMemFree(rawPath);
    item->Release();
    dlg->Release();
    return cleanupWin32(result, comInitialized);
}

std::string selectFolderDialog(const char* title, const std::string& initialDir) {
    bool comInitialized = (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK);

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                 IID_IFileOpenDialog, (void**)&dlg)))
        return cleanupWin32({}, comInitialized);
    DWORD opts;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);

    std::wstring wtitle(title, title + strlen(title));
    dlg->SetTitle(wtitle.c_str());

    if (!initialDir.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, initialDir.c_str(), -1, nullptr, 0);
        std::wstring wdir(wlen ? wlen - 1 : 0, L'\0');
        if (wlen > 1)
            MultiByteToWideChar(CP_UTF8, 0, initialDir.c_str(), -1, &wdir[0], wlen);
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wdir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (FAILED(dlg->Show(nullptr))) { dlg->Release(); return cleanupWin32({}, comInitialized); }
    IShellItem* item = nullptr;
    if (FAILED(dlg->GetResult(&item))) { dlg->Release(); return cleanupWin32({}, comInitialized); }
    wchar_t* rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
        item->Release(); dlg->Release(); return cleanupWin32({}, comInitialized);
    }
    std::string result = wideToUtf8(rawPath);
    CoTaskMemFree(rawPath);
    item->Release();
    dlg->Release();
    return cleanupWin32(result, comInitialized);
}

} // namespace wizard

#else

namespace wizard {

std::string openFileDialog(const char* title, const std::vector<std::string>& patterns,
                           const char* description) {
    std::vector<const char*> raw;
    raw.reserve(patterns.size());
    for (const std::string& p : patterns) raw.push_back(p.c_str());
#ifdef __APPLE__
    // macOS：tinyfd 走 osascript 的 `choose file`，**一旦给了类型表**，名字与类型匹配的
    // 文件夹就会被当成文件 —— 选得中、但进不去。本工程的谱恰恰都放在 `<名字>.adofai/`
    // 这种文件夹里（如 Charts/Song.adofai/），所以这里不传过滤器，保证文件夹永远能进去。
    // 选错路径由 resolveLevelPath() + 加载失败提示兜底。
    (void)raw;
    const char* path = tinyfd_openFileDialog(title, "", 0, nullptr, description, 0);
#else
    const char* path = tinyfd_openFileDialog(title, "", (int)raw.size(),
                                             raw.empty() ? nullptr : raw.data(),
                                             description, 0);
#endif
    return path ? std::string(path) : std::string();
}

std::string selectFolderDialog(const char* title, const std::string& initialDir) {
    const char* path = tinyfd_selectFolderDialog(title, initialDir.c_str());
    return path ? std::string(path) : std::string();
}

} // namespace wizard

#endif

namespace wizard {

std::string detectMusicFile(const std::string& levelPath) {
    namespace fs = std::filesystem;
    fs::path lvl(levelPath);
    fs::path dir = lvl.parent_path();
    if (dir.empty()) dir = ".";
    std::string stem = lvl.stem().string();
    // "X.adofai.xz" -> stem "X.adofai" -> 再剥一层，音乐名才和谱面同名
    if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, ".adofai") == 0)
        stem.resize(stem.size() - 7);
    for (const char* ext : {".ogg",".OGG",".mp3",".MP3",".wav",".WAV",".flac",".FLAC",".m4a",".M4A"}) {
        fs::path cand = dir / (stem + ext);
        if (fs::exists(cand)) return cand.string();
    }
    return "";
}

} // namespace wizard
