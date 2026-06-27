#pragma once

#include <bit>
#include <cstdint>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>

#include "type.h"

namespace usbipdcpp {

constexpr bool is_little_endian() {
    if consteval {
        return std::endian::native == std::endian::little;
    }
    else {
        std::uint16_t tmp = 0x1234u;
        return *reinterpret_cast<std::uint8_t *>(&tmp) != 0x12u;
    }
}

template<std::unsigned_integral T>
constexpr T ntoh(T num) {
    if (is_little_endian()) {
        return std::byteswap(num);
    }
    return num;
}

template<std::unsigned_integral T>
constexpr T hton(T num) {
    return ntoh<T>(num);
}


template<typename T>
concept SerializableFromSocket = requires(const T &t, asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec) {
    { t.to_socket(sock, ec) } -> std::same_as<void>;
    { std::declval<T &>().from_socket(sock) } -> std::same_as<void>;
};

template<typename T>
concept Serializable = requires(const T &t, asio::ip::tcp::socket &sock) {
    { t.to_bytes() } -> supported_data_type;
    { std::declval<T &>().from_socket(sock) } -> std::same_as<void>;
};

template<typename T>
concept is_serializable_can_be_array = requires(T &&t1) {
    requires Serializable<T>;
    requires is_array_data_type<decltype(t1.to_bytes())>;
};

template<typename T>
struct is_serializable_vector_t : std::false_type {};

template<Serializable T>
struct is_serializable_vector_t<std::vector<T>> : std::true_type {};

template<typename T>
concept serializable_vector = is_serializable_vector_t<T>::value;

/**
 * @brief Calculates the total size using sizeof for all types.
 * @tparam Args Types.
 * @return Total size.
 */
template<std::unsigned_integral... Args>
constexpr std::size_t calculate_unsigned_integral_total_size() {
    return (sizeof(std::remove_reference_t<Args>) + ...);
}

/**
 * @brief Can only handle unsigned_integral types and array_data_type types.
 * Integer types are measured with sizeof; array types are measured with std::size.
 * @tparam Args Types.
 * @return Total size.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
constexpr std::size_t calculate_total_size_with_array() {
    auto calc_type_size = []<typename T>() -> std::size_t {
        using RawType = std::remove_cvref_t<T>;
        if constexpr (is_array_data_type<RawType>) {
            return std::size(RawType{});
        }
        else {
            return sizeof(RawType);
        }
    };
    return (calc_type_size.template operator()<Args>() + ...);
}

/**
 * @brief Direct-argument type-deduction version of calculate_total_size_with_array.
 * Can only handle unsigned_integral types and array_data_type types.
 * Integer types are measured with sizeof; array types are measured with std::size.
 * @param args Arguments.
 * @return Total size.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
constexpr std::size_t calculate_data_total_size_with_array(const Args &...args) {
    return calculate_data_total_size_with_array<Args...>();
}

/**
 * @brief Can only handle unsigned_integral types and supported_data_type types.
 * Integer types are measured with sizeof; supported_data_type types are measured with std::size.
 * @param arg Arguments.
 * @return Total size.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
constexpr std::size_t calculate_data_total_size_with_range(const Args &...arg) {
    auto calc_type_size = []<typename T>(const T &t) -> std::size_t {
        if constexpr (supported_data_type<T>) {
            return std::size(t);
        }
        else {
            return sizeof(T);
        }
    };
    return (calc_type_size(arg) + ...);
}

/**
 * @brief Read integers from a socket all at once and assign them. ntoh is called after reading.
 * @param sock Target socket.
 * @param args Integers to read into.
 */
template<std::unsigned_integral... Args>
void unsigned_integral_read_from_socket(asio::ip::tcp::socket &sock, Args &...args) {
    constexpr auto total_size = calculate_unsigned_integral_total_size<Args...>();

    std::array<std::uint8_t, total_size> buffer;
    asio::read(sock, asio::buffer(buffer));
    std::size_t offset = 0;

    auto process = [&](auto &arg) {
        using RawType = std::remove_reference_t<decltype(arg)>;
        RawType tmp;
        std::memcpy(&tmp, &buffer[offset], sizeof(tmp));
        offset += sizeof(tmp);

        arg = ntoh(tmp);
    };

    (process(args), ...);
}

/**
 * @brief Can only handle unsigned_integral types and supported_data_type types.
 * Integer types have ntoh called after reading; supported_data_type types are read directly.
 * Reads and writes each argument one by one.
 * @tparam Args All types.
 * @param sock Target socket.
 * @param args Data to read into.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void data_read_from_socket(asio::ip::tcp::socket &sock, Args &...args) {
    auto process = [&](auto &arg) -> void {
        using RawType = std::remove_reference_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            asio::read(sock, asio::buffer(arg));
        }
        else {
            RawType tmp;
            asio::read(sock, asio::buffer(&tmp, sizeof(tmp)));
            arg = ntoh(tmp);
        }
    };
    (process(args), ...);
}

inline std::uint64_t read_u64(asio::ip::tcp::socket &sock) {
    std::uint64_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

inline std::uint32_t read_u32(asio::ip::tcp::socket &sock) {
    std::uint32_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

inline std::uint16_t read_u16(asio::ip::tcp::socket &sock) {
    std::uint16_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

inline std::uint8_t read_u8(asio::ip::tcp::socket &sock) {
    std::uint8_t result;
    asio::read(sock, asio::buffer(&result, sizeof(result)));
    return ntoh(result);
}

/**
 * @brief Read integers and arrays from a socket all at once and assign them.
 * Integer types have ntoh called after reading; array types are copied directly.
 * @tparam padding Number of trailing padding bytes.
 * @tparam Args Parameter types.
 * @param sock Target socket.
 * @param args Data to read into (integer references or array references).
 */
template<std::size_t padding = 0, typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void unsigned_integral_and_array_read_from_socket(asio::ip::tcp::socket &sock, Args &...args) {
    constexpr auto total_size = calculate_total_size_with_array<Args...>() + padding;

    std::array<std::uint8_t, total_size> buffer;
    asio::read(sock, asio::buffer(buffer));
    std::size_t offset = 0;

    auto process = [&](auto &arg) {
        using RawType = std::remove_reference_t<decltype(arg)>;
        if constexpr (is_array_data_type<RawType>) {
            std::memcpy(arg.data(), &buffer[offset], arg.size());
            offset += arg.size();
        }
        else {
            RawType tmp;
            std::memcpy(&tmp, &buffer[offset], sizeof(tmp));
            offset += sizeof(tmp);
            arg = ntoh(tmp);
        }
    };

    (process(args), ...);
    // padding bytes are read but ignored
}

/**
 * @brief Add padding to an array.
 * @tparam padding Number of padding bytes to add.
 * @param array Source array.
 * @return Padded array.
 */
template<std::size_t padding, is_array_data_type Array,
         std::size_t total_size = calculate_total_size_with_array<Array>() + padding>
constexpr array_data_type<total_size> array_add_padding(const Array &array) {
    array_data_type<total_size> result{};
    std::memcpy(result.data(), array.data(), std::size(array));
    return result;
}

/**
 * @brief Convert a vector of serializable objects into a serialized vector.
 * @return The converted vector.
 */
template<serializable_vector T>
    requires is_serializable_can_be_array<typename T::value_type>
data_type serializable_array_range_to_network_data(const T &vec) {
    constexpr std::size_t serializable_size = decltype(vec.begin()->to_bytes()){}.size();
    std::size_t total_size = serializable_size * vec.size();
    data_type ret(total_size, 0u);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        is_array_data_type auto as_bytes = vec[i].to_bytes();
        std::memcpy(ret.data() + offset, as_bytes.data(), as_bytes.size());
        offset += as_bytes.size();
    }
    return ret;
}

/**
 * @brief Can only handle unsigned_integral types and array_data_type types.
 * Integer types are stored in network byte order; array types are copied directly. hton is called for integers.
 * @tparam Args Types of the input data.
 * @param args Data to convert.
 * @return Created array.
 */
template<typename... Args, std::size_t total_size = calculate_total_size_with_array<Args...>()>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || is_array_data_type<std::remove_cvref_t<Args>>) &&
             ...)
array_data_type<total_size> to_network_array(const Args &...args) {
    // Create buffer
    array_data_type<total_size> buffer;

    // Process each argument
    std::size_t offset = 0;
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;

        if constexpr (is_array_data_type<RawType>) {
            std::memcpy(buffer.data() + offset, arg.data(), arg.size());
            offset += arg.size();
        }
        else {
            const RawType net_value = hton(arg);
            // Copy data into buffer
            std::memcpy(buffer.data() + offset, &net_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };
    // Expand all arguments
    (process(args), ...);

    return buffer;
}

/**
 * @brief Can only handle unsigned_integral types and supported_data_type types.
 * Integer types are stored in network byte order; range types are copied directly. hton is called for integers.
 * @tparam Args Types of the input data.
 * @param args Data to convert.
 * @return Created vector.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
std::vector<std::uint8_t> to_network_data(const Args &...args) {
    // Calculate total buffer size
    std::size_t total_size = calculate_data_total_size_with_range(args...);

    // Create buffer
    data_type buffer;
    buffer.resize(total_size);

    // Process each argument
    std::size_t offset = 0;
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;

        if constexpr (supported_data_type<RawType>) {
            memcpy(buffer.data() + offset, arg.data(), arg.size());
            offset += std::size(arg);
        }
        else {
            const RawType net_value = hton(arg);
            // Copy data into buffer
            std::memcpy(buffer.data() + offset, &net_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };

    // Expand all arguments
    (process(args), ...);

    return buffer;
}

/**
 * @brief Can only handle unsigned_integral types and supported_data_type types.
 * Integer types are stored in host byte order; range types are copied directly.
 * @tparam Args
 * @param vec Target vector.
 * @param args Integer references.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void vector_mem_order_append(data_type &vec, const Args &...args) {
    // Calculate total buffer size
    std::size_t total_size = calculate_data_total_size_with_range(args...);

    // Expand the buffer
    std::size_t offset = vec.size();
    vec.resize(vec.size() + total_size);

    // Process each argument
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            memcpy(vec.data() + offset, arg.data(), arg.size());
            offset += std::size(arg);
        }
        else {
            // Copy data into buffer
            std::memcpy(vec.data() + offset, &arg, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };

    // Expand all arguments
    (process(args), ...);
}

/**
 * @brief Can only handle unsigned_integral types and supported_data_type types.
 * Integer types are stored in network byte order; range types are copied directly. hton is called for integers.
 * @tparam Args
 * @param vec Target vector.
 * @param args Integer references.
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void vector_append_to_net(data_type &vec, const Args &...args) {
    // Calculate total buffer size
    std::size_t total_size = calculate_data_total_size_with_range(args...);

    // Expand the buffer
    std::size_t offset = vec.size();
    vec.resize(vec.size() + total_size);

    // Process each argument
    auto process = [&](auto &&arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            memcpy(vec.data() + offset, arg.data(), arg.size());
            offset += std::size(arg);
        }
        else {
            const RawType net_value = hton(arg);
            // Copy data into buffer
            std::memcpy(vec.data() + offset, &net_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };

    // Expand all arguments
    (process(args), ...);
}


} // namespace usbipdcpp
