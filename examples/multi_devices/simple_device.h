#pragma once

#include <cstdint>

#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"
#include "protocol.h"

/**
 * @brief Simple virtual HID device interface handler
 * Used to create basic virtual input devices
 */
class SimpleHidInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    explicit SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool);

    std::uint16_t get_report_descriptor_size() override;
    usbipdcpp::data_type get_report_descriptor() override;

private:
    // Simple HID report descriptor - one button
    static const usbipdcpp::data_type report_descriptor_;
};

/**
 * @brief Device handler for creating a simple virtual device
 */
class SimpleDeviceHandler : public usbipdcpp::SimpleVirtualDeviceHandler {
public:
    SimpleDeviceHandler(usbipdcpp::UsbDevice &handle_device, usbipdcpp::StringPool &string_pool);
};
