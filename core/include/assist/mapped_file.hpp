// A read-only memory-mapped file, on POSIX (mmap) and on Windows (CreateFileMapping).
//
// Why map the model instead of reading it into a buffer: the kernel loads only the pages that are touched, so a
// model whose weights are mostly unused for a given utterance costs only the pages it uses. Pages come from the
// page cache, so a second process that maps the same file shares the same physical memory, and the kernel can drop
// clean pages under memory pressure instead of swapping them out, because they can always be read back from the file.
// A heap copy gets none of that.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace assist {

class MappedFile {
public:
    // Returns null and fills `error` if the file cannot be opened or mapped, or is empty.
    static std::shared_ptr<const MappedFile> open(const std::filesystem::path& path, std::string* error);

    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::byte* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    MappedFile() = default;

    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
#if defined(_WIN32)
    void* file_ = nullptr;     // HANDLE
    void* mapping_ = nullptr;  // HANDLE
#endif
};

}  // namespace assist
