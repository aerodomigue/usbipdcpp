#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "Export.h"
#include "virtual_device/storage_backends/StorageBackend.h"

namespace usbipdcpp {

/**
 * @brief Raw disk image file backend (memory-mapped implementation, cross-platform)
 *
 * Maps the entire image file into the process address space via mmap / MapViewOfFile;
 * reads and writes are done via direct memcpy on the mapped memory, with the OS handling asynchronous write-back to disk.
 *
 * If the file does not exist, it is automatically created and zero-filled to initial_blocks size.
 * If the file already exists, the block count is estimated from the actual file size (file size / 512).
 */
class USBIPDCPP_API RawImageBackend : public StorageBackend {
public:
    /**
     * @param path           Image file path
     * @param initial_blocks Number of blocks when creating a new file; ignored when opening an existing file
     * @param block_size     Bytes per block (default 512)
     */
    explicit RawImageBackend(std::string path, std::uint64_t initial_blocks = 2048, std::uint32_t block_size = 512);
    ~RawImageBackend() override;

    std::size_t read(std::uint64_t lba, std::uint16_t count, void *buffer) override;
    std::size_t write(std::uint64_t lba, std::uint16_t count, const void *data) override;
    void punch_hole(std::uint64_t lba, std::uint64_t count) override;
    void *get_direct_buffer(std::uint64_t lba) override;
    bool send_direct(std::uint64_t lba, std::size_t offset, std::size_t length, intptr_t sock_fd,
                     std::error_code &ec) override;
    bool recv_direct(std::uint64_t lba, std::size_t offset, std::size_t length, intptr_t sock_fd,
                     std::error_code &ec) override;

    std::uint64_t block_count() const override {
        return block_count_;
    }

    std::uint32_t block_size() const override {
        return block_size_;
    }

    const std::string &path() const {
        return path_;
    }

    bool is_valid() const {
        return mapped_data_ != nullptr;
    }

private:
    std::string path_; // File path
    std::uint64_t block_count_; // Total block count
    std::uint32_t block_size_ = 512; // Bytes per block
    void *mapped_data_ = nullptr; // Base address of the mapped memory
    std::size_t mapped_size_ = 0; // Total size of the mapping in bytes
    mutable std::mutex mutex_; // Protects concurrent reads and writes

#ifdef _WIN32
    void *file_handle_ = nullptr; // HANDLE returned by CreateFile
    void *mapping_handle_ = nullptr; // HANDLE returned by CreateFileMapping
#else
    int fd_ = -1; // File descriptor returned by open
    int fs_block_size_ = 4096; // Filesystem block size, used for punch_hole alignment
#ifdef __linux__
    int splice_pipe_[2] = {-1, -1}; // Pipe used for splice
#endif
#endif
};

} // namespace usbipdcpp
