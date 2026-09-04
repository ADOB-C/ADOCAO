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

// Parses "*.adofai\0*.json\0\0" into COMDLG_FILTERSPEC pairs
std::vector<COMDLG_FILTERSPEC> parseFilters(const wchar_t* filters) {
    std::vector<COMDLG_FILTERSPEC> specs;
    std::vector<std::wstring> specStrs;
    std::wstring filterStr(filters);
    size_t start = 0;
    while (start < filterStr.length()) {
        size_t end = filterStr.find(L'\0', start);
        if (end == std::wstring::npos) break;
        std::wstring pat = filterStr.substr(start, end - start);
        std::wstring desc = pat + L" files";
        specStrs.push_back(desc);
        specStrs.push_back(pat);
        specs.push_back({specStrs[specStrs.size()-2].c_str(),
                         specStrs[specStrs.size()-1].c_str()});
        start = end + 1;
        if (filterStr[start] == L'\0') break;
    }
    return specs;
}

std::string wideToUtf8(const wchar_t* rawPath) {
    int u8len = WideCharToMultiByte(CP_UTF8, 0, rawPath, -1, nullptr, 0, nullptr, nullptr);
    std::string result(u8len ? u8len - 1 : 0, '\0');
    if (u8len > 1)
        WideCharToMultiByte(CP_UTF8, 0, rawPath, -1, &result[0], u8len, nullptr, nullptr);
    return result;
}

} // namespace

std::string openFileDialog(const char* title, const char* filterStr) {
    bool comInitialized = (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK);

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                 IID_IFileOpenDialog, (void**)&dlg)))
        return cleanupWin32({}, comInitialized);

    std::wstring wtitle(title, title + strlen(title));
    std::wstring wfilter(filterStr, filterStr + strlen(filterStr));
    auto specs = parseFilters(wfilter.c_str());
    if (!specs.empty())
        dlg->SetFileTypes((UINT)specs.size(), specs.data());

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

std::string openFileDialog(const char* title, const char* filterStr) {
    const char* path = tinyfd_openFileDialog(title, "", 0, nullptr, filterStr, 0);
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
    for (const char* ext : {".ogg",".OGG",".mp3",".MP3",".wav",".WAV",".flac",".FLAC",".m4a",".M4A"}) {
        fs::path cand = dir / (stem + ext);
        if (fs::exists(cand)) return cand.string();
    }
    return "";
}

} // namespace wizard
