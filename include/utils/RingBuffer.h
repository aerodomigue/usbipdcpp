#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

#include "Export.h"

namespace usbipdcpp {

/**
 * @brief Ring buffer with support for lazy allocation
 */
class USBIPDCPP_API RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity = 64 * 1024);

    /**
     * @brief Write data
     * @param data Data pointer
     * @param size Data size
     * @return Actual number of bytes written
     */
    std::size_t write(const std::uint8_t *data, std::size_t size);

    /**
     * @brief Read data
     * @param data Data buffer
     * @param max_size Maximum number of bytes to read
     * @return Actual number of bytes read
     */
    std::size_t read(std::uint8_t *data, std::size_t max_size);

    /**
     * @brief Peek at data (without removing)
     * @param data Data buffer
     * @param max_size Maximum number of bytes to peek
     * @return Actual number of bytes peeked
     */
    std::size_t peek(std::uint8_t *data, std::size_t max_size) const;

    /**
     * @brief Get the current amount of data
     * @return Number of bytes currently used in the buffer
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Get the buffer capacity
     * @return Total buffer capacity
     */
    [[nodiscard]] std::size_t capacity() const;

    /**
     * @brief Get the remaining buffer space
     * @return Available bytes in the buffer
     */
    [[nodiscard]] std::size_t available() const;

    /**
     * @brief Check whether the buffer is empty
     * @return true if empty
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief Check whether the buffer is full
     * @return true if full
     */
    [[nodiscard]] bool full() const;

    /**
     * @brief Clear the buffer
     */
    void clear();

    /**
     * @brief Resize the buffer capacity
     * @param new_capacity New capacity
     */
    void resize(std::size_t new_capacity);

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t capacity_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

}
