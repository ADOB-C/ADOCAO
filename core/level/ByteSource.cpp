#include "ByteSource.hpp"

#include <cstring>

namespace adofai {

namespace {
// xz: FD 37 7A 58 5A 00 ; zstd: 28 B5 2F FD
constexpr unsigned char kXzMagic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
constexpr unsigned char kZstdMagic[] = {0x28, 0xB5, 0x2F, 0xFD};

ArchiveBackend g_backend;
}  // namespace

LevelArchiveKind sniffLevelArchive(const char* data, size_t length) {
    if (!data || length == 0) return LevelArchiveKind::Plain;
    if (length >= sizeof(kXzMagic) && std::memcmp(data, kXzMagic, sizeof(kXzMagic)) == 0)
        return LevelArchiveKind::Xz;
    if (length >= sizeof(kZstdMagic) && std::memcmp(data, kZstdMagic, sizeof(kZstdMagic)) == 0)
        return LevelArchiveKind::Zstd;
    if (length >= sizeof(kAdocaoMagic) && std::memcmp(data, kAdocaoMagic, sizeof(kAdocaoMagic)) == 0)
        return LevelArchiveKind::Adocao;
    return LevelArchiveKind::Plain;
}

void setArchiveBackend(const ArchiveBackend& backend) { g_backend = backend; }
const ArchiveBackend& archiveBackend() { return g_backend; }

}  // namespace adofai
