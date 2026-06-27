#pragma once

#include <string>
#include <format>

#include <spdlog/spdlog.h>
#include <libusb-1.0/libusb.h>
#include <constant.h>
#include "Export.h"


namespace usbipdcpp {
inline std::string get_device_busid(libusb_device *device) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(device, ports, 8);
    auto busid = std::to_string(libusb_get_bus_number(device));
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            busid += (i == 0 ? "-" : ".");
            busid += std::to_string(ports[i]);
        }
    } else {
        // Topology information unavailable (e.g. Android wrap_sys_device); use bus-addr:port format.
        // addr is unique on the bus, and colons do not appear in normal topology paths, so there is no conflict with any device.
        busid += "-" + std::to_string(libusb_get_device_address(device))
              + ":" + std::to_string(libusb_get_port_number(device));
    }
    return busid;
}

USBIPDCPP_API UsbSpeed libusb_speed_to_usb_speed(int speed);
}


#define dev_pfmt(dev, fmt) "dev {}: " fmt, ::usbipdcpp::get_device_busid((dev))

#define dev_info(dev,fmt,...) \
SPDLOG_INFO(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
#define dev_dbg(dev,fmt,...) \
SPDLOG_DEBUG(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
#define dev_warn(dev,fmt,...) \
SPDLOG_WARN(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
#define dev_err(dev,fmt,...) \
SPDLOG_ERROR(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
