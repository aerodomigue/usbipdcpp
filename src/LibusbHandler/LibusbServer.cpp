#include "LibusbHandler/LibusbServer.h"

#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "LibusbHandler/LibusbDeviceHandler.h"
#include "LibusbHandler/tools.h"

using namespace usbipdcpp;

static constexpr int UDP_NOTIFY_PORT = 3241;

static void udp_broadcast_notify(const std::string &busid) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_NOTIFY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    std::string msg = "ATTACH " + busid + "\n";
    sendto(sock, msg.c_str(), msg.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    close(sock);
    SPDLOG_DEBUG("UDP broadcast: {}", msg);
}

namespace {
void log_device_state(Server &server) {
    std::lock_guard lock(server.get_devices_mutex());
    auto &available = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    std::string avail_busids;
    for (auto &d: available) {
        if (!avail_busids.empty())
            avail_busids += ", ";
        avail_busids += d->busid;
        if (!d->display_name.empty())
            avail_busids += " (" + d->display_name + ")";
    }
    std::string using_busids;
    for (auto &[busid, d]: using_devices) {
        if (!using_busids.empty())
            using_busids += ", ";
        using_busids += busid;
        if (!d->display_name.empty())
            using_busids += " (" + d->display_name + ")";
    }

    SPDLOG_INFO("Device status: available {} [{}] | in use {} [{}]", available.size(), avail_busids, using_devices.size(),
                using_busids);
}
} // namespace

LibusbServer::LibusbServer() {
    server.register_session_exit_callback([this]() {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_available_devices = server.get_available_devices();
        for (auto it = server_available_devices.begin(); it != server_available_devices.end();) {
            bool removed = false;
            if (auto libusb_handle = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                if (libusb_handle->device_removed) {
                    it = server_available_devices.erase(it);
                    removed = true;
                }
            }
            if (!removed) {
                ++it;
            }
        }
    });
}

std::pair<std::string, std::string> LibusbServer::get_device_names(libusb_device *device) {
    libusb_device_handle *handle = nullptr;
    libusb_device_descriptor desc;
    int ret = libusb_get_device_descriptor(device, &desc);
    if (ret < 0) {
        return {"Unknown", "Unknown"};
    }

    // Try to open the device to get string descriptors
    char manufacturer[256] = {0};
    char product[256] = {0};

    if (libusb_open(device, &handle) == 0) {
        if (desc.iManufacturer) {
            libusb_get_string_descriptor_ascii(handle, desc.iManufacturer,
                                               reinterpret_cast<unsigned char *>(manufacturer), sizeof(manufacturer));
        }

        if (desc.iProduct) {
            libusb_get_string_descriptor_ascii(handle, desc.iProduct, reinterpret_cast<unsigned char *>(product),
                                               sizeof(product));
        }

        libusb_close(handle);
    }

    return {(manufacturer[0] ? manufacturer : "Unknown Manufacturer"), (product[0] ? product : "Unknown Product")};
}

void LibusbServer::print_device(libusb_device *dev) {
    libusb_device_descriptor desc{};
    // Get device descriptor
    auto err = libusb_get_device_descriptor(dev, &desc);
    if (err) {
        SPDLOG_ERROR("Cannot get device descriptor: {}", libusb_strerror(err));
        return;
    }
    // Print device information
    auto device_name = get_device_names(dev);
    auto busid = get_device_busid(dev);
    bool is_used = false;
    bool is_available = false;
    {
        std::shared_lock lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        if (server_using_devices.contains(busid)) {
            is_used = true;
        }
        for (auto it = server_available_devices.begin(); it != server_available_devices.end(); ++it) {
            if ((*it)->busid == busid) {
                is_available = true;
            }
        }
    }
    std::cout << std::format("Device name: {}-{} ({})", device_name.first, device_name.second,
                             is_used ? "exported" : (is_available ? "available" : "unbinded"))
              << std::endl;
    std::cout << std::format("busid: {}", busid) << std::endl;
    std::cout << std::format("  VID: 0x{:2x}", desc.idVendor) << std::endl;
    std::cout << std::format("  PID: 0x{:2x}", desc.idProduct) << std::endl;
    auto version = Version(desc.bcdUSB);
    std::cout << std::format("  USB version: {}.{}.{}", version.major, version.minor, version.patch) << std::endl;
    std::cout << std::format("  Class: 0x{:2x}", static_cast<int>(desc.bDeviceClass)) << std::endl;
    std::cout << "  Speed: ";
    // Parse device speed
    switch (libusb_get_device_speed(dev)) {
        case LIBUSB_SPEED_LOW:
            std::cout << "1.5 Mbps (Low)";
            break;
        case LIBUSB_SPEED_FULL:
            std::cout << "12 Mbps (Full)";
            break;
        case LIBUSB_SPEED_HIGH:
            std::cout << "480 Mbps (High)";
            break;
        case LIBUSB_SPEED_SUPER:
            std::cout << "5 Gbps (Super)";
            break;
        case LIBUSB_SPEED_SUPER_PLUS:
            std::cout << "10 Gbps (Super+)";
            break;
        default:
            std::cout << "Unknown speed";
    }
    std::cout << std::endl;
}

void LibusbServer::list_host_devices() {
    libusb_device **devs;
    auto dev_nums = libusb_get_device_list(nullptr, &devs);
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        print_device(devs[dev_i]);
        std::cout << std::endl;
    }
    libusb_free_device_list(devs, 1);
}

libusb_device *LibusbServer::find_by_busid(const std::string &busid) {
    libusb_device **devs;
    int dev_nums = libusb_get_device_list(nullptr, &devs);
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        if (get_device_busid(devs[dev_i]) == busid) {
            auto ret_dev = libusb_ref_device(devs[dev_i]);
            libusb_free_device_list(devs, 1);
            return ret_dev;
        }
    }
    libusb_free_device_list(devs, 1);
    return nullptr;
}

DeviceOperationResult LibusbServer::bind_host_device(libusb_device *dev) {
    if (dev == nullptr) {
        SPDLOG_ERROR("dev cannot be null");
        return DeviceOperationResult::DeviceNotFound;
    }

    // Get device descriptor
    libusb_device_descriptor device_descriptor{};
    int err = libusb_get_device_descriptor(dev, &device_descriptor);
    if (err) {
        SPDLOG_WARN("Cannot get device descriptor: {}", libusb_strerror(err));
        libusb_unref_device(dev);
        return DeviceOperationResult::GetDescriptorFailed;
    }

    // Get configuration descriptor
    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(dev, &active_config_desc);
    if (err) {
        SPDLOG_WARN("Cannot get current device configuration descriptor: {}", libusb_strerror(err));
        libusb_unref_device(dev);
        return DeviceOperationResult::GetConfigFailed;
    }

    // Build interface information
    SPDLOG_DEBUG("This device has {} interfaces", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("Interface {} has {} altsettings", intf_i, intf.num_altsetting);

        std::vector<std::vector<UsbEndpoint>> endpoints;
        endpoints.reserve(intf.num_altsetting);
        for (auto alt_i = 0; alt_i < intf.num_altsetting; alt_i++) {
            auto &intf_desc = intf.altsetting[alt_i];
            std::vector<UsbEndpoint> alt_endpoints;
            alt_endpoints.reserve(intf_desc.bNumEndpoints);
            for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                alt_endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                           intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                           intf_desc.endpoint[ep_i].wMaxPacketSize, intf_desc.endpoint[ep_i].bInterval);
            }
            endpoints.push_back(std::move(alt_endpoints));
        }
        interfaces.emplace_back(UsbInterface{.interface_class = intf.altsetting[0].bInterfaceClass,
                                             .interface_subclass = intf.altsetting[0].bInterfaceSubClass,
                                             .interface_protocol = intf.altsetting[0].bInterfaceProtocol,
                                             .endpoints = std::move(endpoints)});
    }

    // Create UsbDevice and LibusbDeviceHandler
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto current_device = std::make_shared<UsbDevice>(UsbDevice{
                .path = std::format("/sys/bus/{}/{}/{}", libusb_get_bus_number(dev), libusb_get_device_address(dev),
                                    libusb_get_port_number(dev)),
                .busid = get_device_busid(dev),
                .bus_num = libusb_get_bus_number(dev),
                .dev_num = libusb_get_port_number(dev),
                .speed = (std::uint32_t) libusb_speed_to_usb_speed(libusb_get_device_speed(dev)),
                .vendor_id = device_descriptor.idVendor,
                .product_id = device_descriptor.idProduct,
                .device_bcd = device_descriptor.bcdDevice,
                .device_class = device_descriptor.bDeviceClass,
                .device_subclass = device_descriptor.bDeviceSubClass,
                .device_protocol = device_descriptor.bDeviceProtocol,
                .configuration_value = active_config_desc->bConfigurationValue,
                .num_configurations = device_descriptor.bNumConfigurations,
                .interfaces = std::move(interfaces),
                .ep0_in = UsbEndpoint::get_ep0_in(device_descriptor.bMaxPacketSize0),
                .ep0_out = UsbEndpoint::get_ep0_out(device_descriptor.bMaxPacketSize0),
        });

        // Normal mode: pass device reference (handler holds reference ownership)
        current_device->with_handler<LibusbDeviceHandler>(dev);
        server.get_available_devices().emplace_back(std::move(current_device));
    }

    libusb_free_config_descriptor(active_config_desc);
    SPDLOG_INFO("Device {} added to available list", get_device_busid(dev));
    log_device_state(server);
    udp_broadcast_notify(get_device_busid(dev));
    return DeviceOperationResult::Success;
}

DeviceOperationResult LibusbServer::bind_host_device_with_wrapped_fd(intptr_t fd) {
    if (fd < 0) {
        SPDLOG_ERROR("fd is invalid");
        return DeviceOperationResult::DeviceNotFound;
    }

    // Android mode: temporarily wrap fd to obtain device info, then close
    libusb_device_handle *temp_handle = nullptr;
    int err = libusb_wrap_sys_device(nullptr, fd, &temp_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device failed: {}", libusb_strerror(err));
        return DeviceOperationResult::DeviceOpenFailed;
    }

    // Get device info from temporary handle
    libusb_device *device_for_info = libusb_get_device(temp_handle);
    if (!device_for_info) {
        SPDLOG_ERROR("libusb_get_device returns nullptr");
        libusb_close(temp_handle);
        return DeviceOperationResult::DeviceNotFound;
    }

    // Get device descriptor
    libusb_device_descriptor device_descriptor{};
    err = libusb_get_device_descriptor(device_for_info, &device_descriptor);
    if (err) {
        SPDLOG_WARN("Cannot get device descriptor: {}", libusb_strerror(err));
        libusb_close(temp_handle);
        return DeviceOperationResult::GetDescriptorFailed;
    }

    // Get configuration descriptor
    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(device_for_info, &active_config_desc);
    if (err) {
        SPDLOG_WARN("Cannot get current device configuration descriptor: {}", libusb_strerror(err));
        libusb_close(temp_handle);
        return DeviceOperationResult::GetConfigFailed;
    }

    // Build interface information
    SPDLOG_DEBUG("This device has {} interfaces", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("Interface {} has {} altsettings", intf_i, intf.num_altsetting);

        std::vector<std::vector<UsbEndpoint>> endpoints;
        endpoints.reserve(intf.num_altsetting);
        for (auto alt_i = 0; alt_i < intf.num_altsetting; alt_i++) {
            auto &intf_desc = intf.altsetting[alt_i];
            std::vector<UsbEndpoint> alt_endpoints;
            alt_endpoints.reserve(intf_desc.bNumEndpoints);
            for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                alt_endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                           intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                           intf_desc.endpoint[ep_i].wMaxPacketSize, intf_desc.endpoint[ep_i].bInterval);
            }
            endpoints.push_back(std::move(alt_endpoints));
        }
        interfaces.emplace_back(UsbInterface{.interface_class = intf.altsetting[0].bInterfaceClass,
                                             .interface_subclass = intf.altsetting[0].bInterfaceSubClass,
                                             .interface_protocol = intf.altsetting[0].bInterfaceProtocol,
                                             .endpoints = std::move(endpoints)});
    }

    // Save device information
    auto busid = get_device_busid(device_for_info);
    auto bus_num = libusb_get_bus_number(device_for_info);
    auto dev_addr = libusb_get_device_address(device_for_info);
    auto dev_num = libusb_get_port_number(device_for_info);
    auto speed = (std::uint32_t) libusb_speed_to_usb_speed(libusb_get_device_speed(device_for_info));
    auto configuration_value = active_config_desc->bConfigurationValue;

    // Close temporary handle
    libusb_free_config_descriptor(active_config_desc);
    libusb_close(temp_handle);

    // Create UsbDevice and LibusbDeviceHandler
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto current_device = std::make_shared<UsbDevice>(UsbDevice{
                .path = std::format("/sys/bus/{}/{}/{}", bus_num, dev_addr, dev_num),
                .busid = busid,
                .bus_num = bus_num,
                .dev_num = dev_num,
                .speed = speed,
                .vendor_id = device_descriptor.idVendor,
                .product_id = device_descriptor.idProduct,
                .device_bcd = device_descriptor.bcdDevice,
                .device_class = device_descriptor.bDeviceClass,
                .device_subclass = device_descriptor.bDeviceSubClass,
                .device_protocol = device_descriptor.bDeviceProtocol,
                .configuration_value = configuration_value,
                .num_configurations = device_descriptor.bNumConfigurations,
                .interfaces = std::move(interfaces),
                .ep0_in = UsbEndpoint::get_ep0_in(device_descriptor.bMaxPacketSize0),
                .ep0_out = UsbEndpoint::get_ep0_out(device_descriptor.bMaxPacketSize0),
        });

        // Android mode: pass fd (re-wrapped on every connection)
        current_device->with_handler<LibusbDeviceHandler>(fd);
        server.get_available_devices().emplace_back(std::move(current_device));
    }

    SPDLOG_INFO("Device {} added to available list (fd={})", busid, fd);
    log_device_state(server);
    return DeviceOperationResult::Success;
}

DeviceOperationResult LibusbServer::unbind_host_device(libusb_device *device) {
    auto target_busid = get_device_busid(device);
    auto result = DeviceOperationResult::DeviceNotFound;
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if ((*i)->busid == target_busid) {
                auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler);
                if (libusb_device_handler) {
                    // If interfaces are claimed, release them
                    if (libusb_device_handler->interfaces_claimed_) {
                        libusb_device_handler->release_and_close_device();
                    }
                    // Release device reference
                    if (libusb_device_handler->native_device_) {
                        libusb_unref_device(libusb_device_handler->native_device_);
                        libusb_device_handler->native_device_ = nullptr;
                    }
                }
                server_available_devices.erase(i);
                libusb_unref_device(device);
                spdlog::info("Successfully unbound");
                result = DeviceOperationResult::Success;
                break;
            }
        }
        if (result == DeviceOperationResult::DeviceNotFound) {
            SPDLOG_WARN("Target device not found among available devices");

            if (server_using_devices.contains(target_busid)) {
                SPDLOG_WARN("Cannot unbind a device that is currently in use");
                libusb_unref_device(device);
                result = DeviceOperationResult::DeviceInUse;
            }
            else {
                libusb_unref_device(device);
            }
        }
    }
    log_device_state(server);
    return result;
}

DeviceOperationResult LibusbServer::unbind_host_device_by_fd(intptr_t fd) {
    auto result = DeviceOperationResult::DeviceNotFound;
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();

        // Check available_devices first
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (libusb_device_handler->wrapped_fd_ == fd) {
                    // If interfaces are claimed, release them
                    if (libusb_device_handler->interfaces_claimed_) {
                        libusb_device_handler->release_and_close_device();
                    }
                    // Android mode has no native_device_; no reference to release

                    auto busid = (*i)->busid;
                    server_available_devices.erase(i);
                    SPDLOG_INFO("Successfully unbound device {} (fd={})", busid, fd);
                    result = DeviceOperationResult::Success;
                    break;
                }
            }
        }

        // Then check using_devices (device currently in use)
        if (result != DeviceOperationResult::Success) {
            for (auto it = server_using_devices.begin(); it != server_using_devices.end(); ++it) {
                if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(it->second->handler)) {
                    if (libusb_device_handler->wrapped_fd_ == fd) {
                        SPDLOG_WARN("Device is in use; cannot unbind (fd={})", fd);
                        result = DeviceOperationResult::DeviceInUse;
                        break;
                    }
                }
            }
        }

        if (result == DeviceOperationResult::DeviceNotFound) {
            SPDLOG_WARN("Device with fd={} not found", fd);
        }
    }
    log_device_state(server);
    return result;
}

DeviceOperationResult LibusbServer::try_remove_dead_device(const std::string &busid) {
    std::lock_guard lock(server.get_devices_mutex());
    auto &server_using_devices = server.get_using_devices();
    auto &server_available_devices = server.get_available_devices();
    for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
        if ((*i)->busid == busid) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                // Device may not be open
                if (libusb_device_handler->native_handle) {
                    libusb_close(libusb_device_handler->native_handle);
                    libusb_device_handler->native_handle = nullptr;
                }
                // Release device reference
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
                server_available_devices.erase(i);
                spdlog::info("Removed {} from available devices", busid);
                return DeviceOperationResult::Success;
            }
        }
    }
    if (auto device = server_using_devices.find(busid); device != server_using_devices.end()) {
        if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*device).second->handler)) {
            if (libusb_device_handler->native_handle) {
                libusb_close(libusb_device_handler->native_handle);
                libusb_device_handler->native_handle = nullptr;
            }
            if (libusb_device_handler->native_device_) {
                libusb_unref_device(libusb_device_handler->native_device_);
                libusb_device_handler->native_device_ = nullptr;
            }
            server_using_devices.erase(device);
            spdlog::info("Removed {} from in-use devices", busid);
            return DeviceOperationResult::Success;
        }
    }
    SPDLOG_WARN("Cannot find device with busid {}", busid);
    return DeviceOperationResult::DeviceNotFound;
}

DeviceOperationResult LibusbServer::notify_device_removed(const std::string &busid) {
    SPDLOG_INFO("Device removed: {}", busid);

    std::lock_guard lock(server.get_devices_mutex());
    auto &available_devices = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    // 1. Remove from available_devices
    for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
        if ((*it)->busid == busid) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                if (handler->native_handle) {
                    libusb_close(handler->native_handle);
                    handler->native_handle = nullptr;
                }
                if (handler->native_device_) {
                    libusb_unref_device(handler->native_device_);
                    handler->native_device_ = nullptr;
                }
            }
            available_devices.erase(it);
            SPDLOG_INFO("Removed from bound device list: {}", busid);
            return DeviceOperationResult::Success;
        }
    }

    // 2. If in use, trigger disconnection
    if (auto it = using_devices.find(busid); it != using_devices.end()) {
        if (auto handler = it->second->handler) {
            handler->on_device_removed();
            SPDLOG_WARN("Device in use was removed; forcibly closing Session: {}", busid);
            handler->trigger_session_stop();
        }
        return DeviceOperationResult::Success;
    }

    SPDLOG_WARN("Device not found: {}", busid);
    return DeviceOperationResult::DeviceNotFound;
}

void LibusbServer::start_hotplug_monitor() {
    if (!hotplug_enabled_by_user_) {
        SPDLOG_DEBUG("Hotplug monitoring disabled by user");
        return;
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        SPDLOG_WARN("Current libusb does not support hotplug");
        return;
    }

    int ret = libusb_hotplug_register_callback(
            nullptr,
            static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_callback, this, &hotplug_handle_);

    if (ret == 0) {
        hotplug_enabled_ = true;
        SPDLOG_INFO("Hotplug monitoring started");
    }
    else {
        SPDLOG_ERROR("Failed to register hotplug callback: {}", libusb_strerror(ret));
    }
}

void LibusbServer::stop_hotplug_monitor() {
    if (hotplug_enabled_) {
        libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);
        hotplug_enabled_ = false;
        SPDLOG_INFO("Hotplug monitoring stopped");
    }
}

int LIBUSB_CALL LibusbServer::hotplug_callback(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event,
                                               void *user_data) {
    auto *server = static_cast<LibusbServer *>(user_data);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        server->handle_device_arrived(device);
    }
    else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        auto busid = get_device_busid(device);
        server->handle_device_left(busid);
    }

    return 0;
}

void LibusbServer::handle_device_arrived(libusb_device *device) {
    auto busid = get_device_busid(device);

    // Check if already bound
    {
        std::shared_lock lock(server.get_devices_mutex());
        for (const auto &dev: server.get_available_devices()) {
            if (dev->busid == busid) {
                SPDLOG_DEBUG("Device {} is already in the bound list", busid);
                return;
            }
        }
        if (server.get_using_devices().contains(busid)) {
            SPDLOG_DEBUG("Device {} is currently in use", busid);
            return;
        }
    }

    // Skip hubs (class 0x09) and internal Ethernet (0424:ec00)
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(device, &desc) == 0) {
        if (desc.bDeviceClass == 0x09) return;
        if (desc.idVendor == 0x0424 && desc.idProduct == 0xec00) return;
    }

    SPDLOG_INFO("Auto-binding new device: {}", busid);
    auto ref = libusb_ref_device(device);
    bind_host_device(ref);
}

void LibusbServer::handle_device_left(const std::string &busid) {
    SPDLOG_INFO("Device removal detected: {}", busid);

    std::lock_guard lock(server.get_devices_mutex());
    auto &available_devices = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    // 1. Remove from available_devices
    for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
        if ((*it)->busid == busid) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                // Device may not be open
                if (handler->native_handle) {
                    libusb_close(handler->native_handle);
                    handler->native_handle = nullptr;
                }
                if (handler->native_device_) {
                    libusb_unref_device(handler->native_device_);
                    handler->native_device_ = nullptr;
                }
            }
            available_devices.erase(it);
            SPDLOG_INFO("Removed from bound device list: {}", busid);
            return;
        }
    }

    // 2. If in use, trigger disconnection
    if (auto it = using_devices.find(busid); it != using_devices.end()) {
        if (auto handler = it->second->handler) {
            // Notify via AbstDeviceHandler interface (backend-agnostic)
            handler->on_device_removed();
            // Forcibly close Session
            SPDLOG_WARN("Device in use was removed; forcibly closing Session: {}", busid);
            handler->trigger_session_stop();
        }
    }
}

void LibusbServer::bind_existing_devices() {
    libusb_device **devs;
    int dev_nums = libusb_get_device_list(nullptr, &devs);
    for (int i = 0; i < dev_nums; i++) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) continue;
        if (desc.bDeviceClass == 0x09) continue;
        if (desc.idVendor == 0x0424 && desc.idProduct == 0xec00) continue;
        auto busid = get_device_busid(devs[i]);
        SPDLOG_INFO("Auto-binding existing device: {}", busid);
        bind_host_device(libusb_ref_device(devs[i]));
    }
    libusb_free_device_list(devs, 1);
}

void LibusbServer::start(asio::ip::tcp::endpoint &ep) {
    start_hotplug_monitor();
    bind_existing_devices();

    should_exit_libusb_event_thread = false;

    libusb_event_thread = std::thread([this]() {
        try {
            SPDLOG_INFO("Starting libusb event loop thread for libusb device handle");
            while (!should_exit_libusb_event_thread) {
                auto ret = libusb_handle_events(nullptr);

                if (ret == LIBUSB_ERROR_INTERRUPTED && should_exit_libusb_event_thread) [[unlikely]] {
                    SPDLOG_INFO("libusb event loop received interrupt signal and exited normally");
                    break;
                }
                if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED) [[unlikely]] {
                    SPDLOG_ERROR("Event handling error: {}\n", libusb_strerror(ret));
                    break;
                }
            }
            SPDLOG_TRACE("Exiting libusb event loop");
        } catch (const std::exception &e) {
            SPDLOG_ERROR("An unexpected exception occurs in libusb handler thread: {}", e.what());
            std::exit(1);
        }
    });
    server.start(ep);
}

void LibusbServer::stop() {
    stop_hotplug_monitor();
    server.stop();
    SPDLOG_INFO("usbip server stopped");

    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (libusb_device_handler->interfaces_claimed_) {
                    libusb_device_handler->release_and_close_device();
                }
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
            }
        }
        server_available_devices.clear();

        for (auto using_dev_i = server_using_devices.begin(); using_dev_i != server_using_devices.end();
             ++using_dev_i) {
            if (auto libusb_device_handler =
                        std::dynamic_pointer_cast<LibusbDeviceHandler>(using_dev_i->second->handler)) {
                if (libusb_device_handler->interfaces_claimed_) {
                    libusb_device_handler->release_and_close_device();
                }
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
            }
        }
        server_using_devices.clear();
    }

    should_exit_libusb_event_thread = true;
    libusb_interrupt_event_handler(nullptr);
    spdlog::info("Waiting for libusb event thread to finish");
    libusb_event_thread.join();
    spdlog::info("libusb event thread finished");
}

// void usbipcpp::LibusbServer::add_device(std::shared_ptr<UsbDevice> &&device) {
//     Server::add_device(std::move(device));
// }
//
// bool usbipcpp::LibusbServer::remove_device(const std::string &busid) {
//     return Server::remove_device(busid);
// }

LibusbServer::~LibusbServer() {
}
