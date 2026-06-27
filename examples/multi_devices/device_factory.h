#pragma once

#include <memory>
#include <string>

#include "Device.h"
#include "utils/StringPool.h"

/**
 * @brief Device factory class for creating virtual USB devices
 */
class DeviceFactory {
public:
    /**
     * @brief Create a simple virtual HID device
     * @param index Device index (1-10)
     * @param string_pool Reference to the string pool
     * @return Shared pointer to the created device
     */
    static std::shared_ptr<usbipdcpp::UsbDevice> create_simple_device(int index, usbipdcpp::StringPool &string_pool);

    /**
     * @brief Create multiple virtual devices
     * @param count Number of devices
     * @param string_pool Reference to the string pool
     * @return Device list
     */
    static std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> create_devices(int count,
                                                                             usbipdcpp::StringPool &string_pool);

private:
    /**
     * @brief Generate the device busid
     * @param index Device index
     * @return busid string, e.g. "1-1", "1-2"
     */
    static std::string generate_busid(int index);

    /**
     * @brief Generate the device path
     * @param index Device index
     * @return Device path
     */
    static std::string generate_path(int index);

    /**
     * @brief Generate the vendor ID
     * @param index Device index
     * @return Vendor ID
     */
    static std::uint16_t generate_vendor_id(int index);

    /**
     * @brief Generate the product ID
     * @param index Device index
     * @return Product ID
     */
    static std::uint16_t generate_product_id(int index);
};
