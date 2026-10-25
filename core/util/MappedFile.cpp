#include "core/util/MappedFile.hpp"

#include <cstdio>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace adofai {

MappedFile::~MappedFile() { close(); }

void MappedFile::close() {
#if !defined(_WIN32)
    if (m_mapped && m_data) {
        ::munmap(const_cast<char*>(m_data), m_size);
    }
#endif
    m_data = nullptr;
    m_size = 0;
    m_mapped = false;
    m_fallback.clear();
}

bool MappedFile::open(const std::string& path) {
    close();
#if !defined(_WIN32)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
    void* p = ::mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return false;
    m_data = (const char*)p;
    m_size = (size_t)st.st_size;
    m_mapped = true;
    return true;
#else
    // Windows：回退成整份读（映射在这里收益有限，且要处理 CreateFileMapping 的句柄生命周期）
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    m_fallback.resize((size_t)n);
    const size_t got = std::fread(m_fallback.data(), 1, m_fallback.size(), f);
    std::fclose(f);
    if (got != m_fallback.size()) { m_fallback.clear(); return false; }
    m_data = m_fallback.data();
    m_size = m_fallback.size();
    return true;
#endif
}

}  // namespace adofai
