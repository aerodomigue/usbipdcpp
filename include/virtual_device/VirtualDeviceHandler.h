#pragma once

#include "DeviceHandler/DeviceHandler.h"
#include "utils/StringPool.h"
#include "virtual_device/VirtualDeviceTransferOperator.h"

namespace usbipdcpp {

class USBIPDCPP_API VirtualDeviceHandler : public AbstDeviceHandler {
public:
    explicit VirtualDeviceHandler(UsbDevice &handle_device, StringPool &string_pool,
                                  const Version &usb_version = {2, 0, 0}) :
        AbstDeviceHandler(handle_device, std::make_unique<VirtualDeviceTransferOperator>()), string_pool(string_pool),
        usb_version(usb_version) {
        change_device_ep0_max_size_by_speed();

        string_configuration_value = string_pool.new_string(L"Default Configuration");
        string_manufacturer_value = string_pool.new_string(L"Usbipdcpp");
        string_product_value = string_pool.new_string(L"Usbipdcpp Virtual Device");
        string_serial_value = 0; // No serial number, matching physical UVC device behavior
    }

    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override;
    /**
     * @brief Called when a new client connects
     * @param current_session
     * @param ec Error code that occurred
     */
    void on_new_connection(Session &current_session, error_code &ec) override;
    /**
     * @brief Called when a transfer must be completely terminated due to errors, client detach, server shutdown, etc. No messages may be submitted after this call
     */
    void on_disconnection(error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    /**
     * @brief Set the device_handler pointer for all interface handlers
     * @note Should be called after creating UsbDevice and setting up all interface handlers
     */
    void setup_interface_handlers();

protected:
    void change_device_ep0_max_size_by_speed();

    // Called by receive_urb, dispatches to specific handle_xxx_transfer
    void dispatch_urb(const UsbIpCommand::UsbIpCmdSubmit &cmd, std::uint32_t seqnum, const UsbEndpoint &ep,
                      std::optional<UsbInterface> &interface, std::uint32_t transfer_flags,
                      std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet, usbipdcpp::error_code &ec);

    void handle_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                            std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                            TransferHandle transfer, std::error_code &ec);
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, UsbInterface &interface,
                              std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                              TransferHandle transfer, std::error_code &ec);
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, UsbInterface &interface,
                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                   TransferHandle transfer, std::error_code &ec);

    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, UsbInterface &interface,
                                     std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                     TransferHandle transfer, int num_iso_packets, std::error_code &ec);

    virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const SetupPacket &setup_packet, TransferHandle transfer,
                                                              std::error_code &ec) = 0;

    virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) = 0;

    virtual std::uint8_t request_get_configuration(std::uint32_t *p_status);

    virtual std::uint16_t request_get_status(std::uint32_t *p_status) = 0;
    virtual void request_set_address(std::uint16_t address, std::uint32_t *status) = 0;
    virtual void request_set_configuration(std::uint16_t configuration_value, std::uint32_t *p_status) = 0;
    virtual void request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t language_id,
                                        std::uint16_t descriptor_length, const data_type &descriptor,
                                        std::uint32_t *p_status) = 0;

    virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) = 0;

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status);
    std::uint8_t request_get_interface(std::uint16_t intf, std::uint32_t *p_status);
    void request_set_interface(std::uint16_t alternate_setting, std::uint16_t intf, std::uint32_t *p_status);


    virtual data_type get_device_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                            std::uint32_t *p_status);
    virtual data_type get_bos_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                         std::uint32_t *p_status);
    virtual data_type get_configuration_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                   std::uint32_t *p_status);
    virtual data_type get_string_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                            std::uint32_t *p_status);
    virtual data_type get_device_qualifier_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status);
    virtual data_type get_other_speed_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                                 std::uint32_t *p_status) = 0;

    /**
     * @brief If host wants a descriptor which is not in enum DescriptorType, then this function will be called
     * @param type descriptor tye
     * @param language_id which language
     * @param descriptor_length descriptor_length
     * @param p_status
     * @return descriptor
     */
    virtual data_type get_custom_descriptor(std::uint8_t type, std::uint8_t language_id,
                                            std::uint16_t descriptor_length, std::uint32_t *p_status);

    /// Handles special string indices (e.g. Microsoft OS 0xEE). Returns nullopt to continue with string_pool lookup
    virtual std::optional<data_type> get_special_string_descriptor(std::uint8_t string_index);

    virtual void set_descriptor(std::uint16_t configuration_value) = 0;

public:
    void change_string_configuration(const std::wstring &new_str) {
        string_pool.change_string(string_configuration_value, new_str);
    }

    void change_string_manufacturer(const std::wstring &new_str) {
        string_pool.change_string(string_manufacturer_value, new_str);
    }

    void change_string_product(const std::wstring &new_str) {
        string_pool.change_string(string_product_value, new_str);
    }

    void change_string_serial(const std::wstring &new_str) {
        string_pool.change_string(string_serial_value, new_str);
    }

    std::wstring get_string_manufacturer() const {
        return string_pool.get_string(string_manufacturer_value).value_or(L"");
    }

    std::wstring get_string_product() const {
        return string_pool.get_string(string_product_value).value_or(L"");
    }

    std::wstring get_string_serial() const {
        return string_pool.get_string(string_serial_value).value_or(L"");
    }

protected:
    std::uint8_t string_configuration_value;
    std::uint8_t string_manufacturer_value;
    std::uint8_t string_product_value;
    std::uint8_t string_serial_value;

    StringPool &string_pool;

    Version usb_version;
    std::shared_mutex data_mutex;
};
} // namespace usbipdcpp
