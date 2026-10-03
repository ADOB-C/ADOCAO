#pragma once
// 只读内存映射。放在 app 层而不是 core/：core/ 必须保持零平台头
// （scripts/check-core-purity.sh 禁 unistd.h 等），映射属于平台代码。
//
// 为什么值得单独做：1.5 GB 的谱用 ifstream 读进 std::string 要 ~250 ms，而且多占
// 1.5 GB 常驻内存（匿名脏页）；mmap 之后解析器直接在页缓存上扫描，这几百毫秒和
// 那份副本都省掉了。core 里的 LevelData::loadFromFile 保留可移植读法给测试和嵌入
// 方用，app 走 FileMap + LevelData::loadFromBuffer。

#include <cstddef>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

class FileMap {
public:
    FileMap() = default;
    ~FileMap() { close(); }
    FileMap(const FileMap&) = delete;
    FileMap& operator=(const FileMap&) = delete;

    bool open(const std::string& filepath) {
        close();
#ifdef _WIN32
        // UTF-8 路径转宽字符
        int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       filepath.c_str(), -1, nullptr, 0);
        UINT cp = CP_UTF8;
        if (wlen <= 0) {
            cp = CP_ACP;
            wlen = MultiByteToWideChar(CP_ACP, 0, filepath.c_str(), -1, nullptr, 0);
        }
        if (wlen <= 0) return false;
        std::wstring wpath((size_t)wlen, L'\0');
        MultiByteToWideChar(cp, 0, filepath.c_str(), -1, &wpath[0], wlen);

        HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
            CloseHandle(hFile);
            return false;
        }
        HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMapping) { CloseHandle(hFile); return false; }
        const char* p = (const char*)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (!p) { CloseHandle(hMapping); CloseHandle(hFile); return false; }
        m_file = hFile;
        m_mapping = hMapping;
        m_data = p;
        m_size = (size_t)fileSize.QuadPart;
        return true;
#else
        int fd = ::open(filepath.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
        void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return false; }
        m_fd = fd;
        m_data = (const char*)p;
        m_size = (size_t)st.st_size;
        return true;
#endif
    }

    void close() {
        if (!m_data && m_size == 0) return;
#ifdef _WIN32
        if (m_data) UnmapViewOfFile((LPCVOID)m_data);
        if (m_mapping) CloseHandle((HANDLE)m_mapping);
        if (m_file) CloseHandle((HANDLE)m_file);
        m_file = m_mapping = nullptr;
#else
        if (m_data) munmap((void*)m_data, m_size);
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
#endif
        m_data = nullptr;
        m_size = 0;
    }

    const char* data() const { return m_data; }
    size_t size() const { return m_size; }

private:
    const char* m_data = nullptr;
    size_t m_size = 0;
#ifdef _WIN32
    void* m_file = nullptr;
    void* m_mapping = nullptr;
#else
    int m_fd = -1;
#endif
};
