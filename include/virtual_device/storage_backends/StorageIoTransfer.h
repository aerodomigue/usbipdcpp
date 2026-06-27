#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbipdcpp {

/**
 * @brief MSC storage I/O dedicated Transfer, used with StorageTransferOperator
 *
 * Does not allocate internal GenericTransfer::data; all I/O goes directly to the target address through this structure's members.
 * Supports two data paths: zero-copy (mmap file memory) and fallback (staging_data_ / fallback_data).
 *
 * === IN direction (device → host, READ / CSW) ===
 * external_buf and actual_length are set in handle_bulk_transfer; the sender thread sends via
 * send_transfer_data. Prefers sendfile / TransmitFile (direct_io), falls back to asio::write.
 * - READ mmap: external_buf points to file memory mapped by RawImageBackend, direct_io = true
 * - READ fallback: external_buf points into staging_data_
 * - CSW: external_buf points to fallback_data in this structure, direct_io = false
 *
 * === OUT direction (host → device, CBW / WRITE / UNMAP) ===
 * prepare_out_buffer sets external_buf at alloc_transfer_handle time;
 * recv_transfer_data prefers splice to write directly to file (direct_io), falls back to asio::read into external_buf,
 * then on_out_data_received parses the CBW or accumulates write data.
 * - CBW: external_buf is nullptr, uses fallback_data, direct_io = false
 * - WRITE mmap: external_buf points to mmap file memory, direct_io = true
 * - WRITE fallback / UNMAP: external_buf points to reserved space at the tail of staging_data_
 */
struct StorageIoTransfer {
    // ===== Set by MscBulkOnlyHandler =====

    /**
     * @brief Target address for direct reads/writes
     *
     * IN(READ mmap): points to file memory mapped by RawImageBackend;
     * IN(READ staging): points to an offset within staging_data_;
     * IN(CSW): points to the CSW content in fallback_data;
     * OUT(WRITE mmap): points to mmap memory, read directly from socket or written via splice;
     * OUT(WRITE staging): points to reserved space at the tail of staging_data_;
     * OUT(Idle): nullptr, CBW is read into fallback_data.
     */
    void *external_buf = nullptr;

    /**
     * @brief Number of bytes pending to send in the IN direction
     *
     * Set by MscBulkOnlyHandler in handle_bulk_transfer DataIn/Status;
     * this is the length parameter for send_transfer_data. Not used in the OUT direction.
     */
    std::size_t actual_length = 0;

    /**
     * @brief Starting LBA for file I/O
     *
     * send_direct / recv_direct use this together with block_size to calculate the file offset.
     * IN(READ): set to read_lba_ by handle_bulk_transfer DataIn;
     * OUT(WRITE): set to write_lba_ by prepare_out_buffer;
     * CSW / CBW: stays 0 (this path is not triggered when direct_io = false).
     */
    std::uint64_t file_lba = 0;

    /**
     * @brief Byte offset within the LBA
     *
     * Used together with file_lba to determine the exact file position.
     * IN(READ): equals staging_offset_;
     * OUT(WRITE): equals write_accumulated_.
     */
    std::size_t file_offset = 0;

    /**
     * @brief Whether to use the zero-copy sendfile / splice path
     *
     * Set to true only when external_buf points to the mmap file memory of RawImageBackend:
     * - IN(READ mmap): set to true in handle_bulk_transfer DataIn;
     * - OUT(WRITE mmap): set to true in prepare_out_buffer.
     *
     * CSW / CBW / staging fallback paths are all false; send_transfer_data / recv_transfer_data
     * use this to skip send_direct / recv_direct and go directly to asio reads/writes.
     */
    bool direct_io = false;

    // ===== Buffer used internally by this structure =====

    /**
     * @brief Fallback buffer when external_buf is null
     *
     * - OUT(Idle): CBW (31 bytes) is read here, then memcpy'd to current_cbw_ by on_out_data_received
     *   to prevent an overly long CBW from corrupting the stack;
     * - IN(Status): CSW (13 bytes) is constructed here, and external_buf points to its data().
     */
    std::vector<std::uint8_t> fallback_data;

    /** Reset all fields for object pool reuse */
    void reset() {
        external_buf = nullptr;
        actual_length = 0;
        file_lba = 0;
        file_offset = 0;
        direct_io = false;
        fallback_data.clear();
    }

    static StorageIoTransfer *from_handle(void *handle) {
        return static_cast<StorageIoTransfer *>(handle);
    }
};

} // namespace usbipdcpp
