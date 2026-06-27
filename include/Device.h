#pragma once

#include <filesystem>
#include <variant>
#include <memory>

#include "Version.h"
#include "SetupPacket.h"
#include "constant.h"
#include "network.h"
#include "Interface.h"
#include "type.h"
#include "Export.h"


namespace usbipdcpp {
class Session;

class AbstDeviceHandler;

struct UsbIpIsoPacketDescriptor;

namespace UsbIpCommand {
    struct OpReqDevlist;
    struct OpReqImport;
    struct UsbIpCmdSubmit;
    struct UsbIpCmdUnlink;
    using AllCmdVariant = std::variant<OpReqDevlist, OpReqImport, UsbIpCmdSubmit, UsbIpCmdUnlink>;
}

struct USBIPDCPP_API UsbDevice {
    std::filesystem::path path{};
    std::string busid{};
    std::uint32_t bus_num;
    std::uint32_t dev_num;
    std::uint32_t speed;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    Version device_bcd{0, 0, 0};
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t configuration_value;
    std::uint8_t num_configurations;
    std::vector<UsbInterface> interfaces{};


    UsbEndpoint ep0_in;
    UsbEndpoint ep0_out;

    /**
     * @brief Device handler.
     * Must be set before calling Server::add_device; the with_handler function is recommended.
     * If handler is null at call time, the behavior is undefined.
     */
    std::shared_ptr<AbstDeviceHandler> handler;

    /**
     * @brief Create and set the handler.
     * This function is the recommended way to set the handler.
     * @tparam T Handler type.
     * @tparam args Arguments forwarded to the handler constructor.
     * @return The created handler.
     */
    template<typename T, typename... Args>
    std::shared_ptr<T> with_handler(Args &&... args) {
        auto new_handler = std::make_shared<T>(*this, std::forward<Args>(args)...);
        handler = std::static_pointer_cast<AbstDeviceHandler>(new_handler);
        return new_handler;
    }

    static constexpr std::size_t bytes_without_interfaces_num = calculate_total_size_with_array<
        array_data_type<256>,
        array_data_type<32>,
        decltype(bus_num),
        decltype(dev_num),
        decltype(speed),
        decltype(vendor_id),
        decltype(product_id),
        std::uint16_t,
        decltype(device_class),
        decltype(device_subclass),
        decltype(device_protocol),
        decltype(configuration_value),
        decltype(num_configurations),
        std::uint8_t
    >();

    [[nodiscard]] std::vector<std::uint8_t> to_bytes_with_interfaces() const;
    [[nodiscard]] array_data_type<bytes_without_interfaces_num> to_bytes_without_interfaces() const;

    // Interface info is sent for devlist requests; it is not sent for import requests
    [[nodiscard]] array_data_type<bytes_without_interfaces_num> to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);

    std::optional<std::pair<UsbEndpoint, std::optional<UsbInterface>>> find_ep(std::uint8_t ep);

    bool operator==(const UsbDevice &other) const {
        return path == other.path &&
               busid == other.busid &&
               bus_num == other.bus_num &&
               dev_num == other.dev_num &&
               speed == other.speed &&
               vendor_id == other.vendor_id &&
               product_id == other.product_id &&
               device_bcd == other.device_bcd &&
               device_class == other.device_class &&
               device_subclass == other.device_subclass &&
               device_protocol == other.device_protocol &&
               configuration_value == other.configuration_value &&
               num_configurations == other.num_configurations &&
               interfaces == other.interfaces;
    }
};

static_assert(Serializable<UsbDevice>);
}
