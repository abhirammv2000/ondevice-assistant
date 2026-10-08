#include "assist/mapped_file.hpp"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace assist {

#if defined(_WIN32)

std::shared_ptr<const MappedFile> MappedFile::open(const std::filesystem::path& path, std::string* error) {
    auto fail = [&](const char* what) -> std::shared_ptr<const MappedFile> {
        if (error) *error = std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    };
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail("cannot open file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return fail("cannot get file size");
    }
    if (size.QuadPart <= 0) {
        CloseHandle(file);
        if (error) *error = "file is empty";
        return nullptr;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        return fail("cannot create file mapping");
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        return fail("cannot map view of file");
    }
    std::shared_ptr<MappedFile> m(new MappedFile());
    m->data_ = static_cast<const std::byte*>(view);
    m->size_ = static_cast<std::size_t>(size.QuadPart);
    m->file_ = file;
    m->mapping_ = mapping;
    return m;
}

MappedFile::~MappedFile() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_) CloseHandle(file_);
}

#else

std::shared_ptr<const MappedFile> MappedFile::open(const std::filesystem::path& path, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = std::string("cannot open file: ") + std::strerror(errno);
        return nullptr;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        if (error) *error = std::string("cannot stat file: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    if (st.st_size <= 0) {
        if (error) *error = "file is empty";
        ::close(fd);
        return nullptr;
    }
    void* view = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // the mapping stays valid after the descriptor is closed
    if (view == MAP_FAILED) {
        if (error) *error = std::string("cannot map file: ") + std::strerror(errno);
        return nullptr;
    }
    std::shared_ptr<MappedFile> m(new MappedFile());
    m->data_ = static_cast<const std::byte*>(view);
    m->size_ = static_cast<std::size_t>(st.st_size);
    return m;
}

MappedFile::~MappedFile() {
    if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
}

#endif

}  // namespace assist
