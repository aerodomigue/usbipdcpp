#pragma once


#include <deque>
#include <mutex>

#include <asio.hpp>

#include "HidConstants.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"
#include "virtual_device/VirtualInterfaceHandler.h"


namespace usbipdcpp {
/**
 * @brief HID device interface handler base class
 *
 * Provides a default implementation for interrupt transfers; users only need to implement the report descriptor and control request handling.
 */
class USBIPDCPP_API HidVirtualInterfaceHandler : public VirtualInterfaceHandler {
public:
    HidVirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    /**
     * @brief Handle interrupt transfers (default implementation)
     *
     * Interrupt IN: host requests an input report, calls on_input_report_requested() to get data
     * Interrupt OUT: host sends an output report, calls on_output_report_received()
     */
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    virtual void handle_non_hid_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet, TransferHandle transfer,
                                                         std::error_code &ec);

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== Virtual functions that subclasses must implement ==========

    /**
     * @brief Get the HID report descriptor
     * @return Report descriptor data
     */
    virtual data_type get_report_descriptor() = 0;

    /**
     * @brief Get the HID report descriptor size
     * @return Descriptor length in bytes
     */
    virtual std::uint16_t get_report_descriptor_size() = 0;

    // ========== Send data API ==========

    /**
     * @brief Send an input report (zero-copy)
     *
     * If there are queued requests, immediately respond to the first one; otherwise store the data and wait for the next request.
     *
     * @param data Report data (can use stack-allocated std::array + asio::buffer)
     */
    void send_input_report(asio::const_buffer data);

    // ========== Optional overridable callbacks for subclasses ==========

    /**
     * @brief Callback when the host requests an input report
     *
     * @warning This function is called every time the host polls the interrupt endpoint. Inside this function,
     *          since no locks are held, you can directly call send_input_report and similar functions.
     *          If send_input_report() is called on every invocation, the host will immediately retrieve the data
     *          and poll again, causing very high CPU usage. Unless in special circumstances, do not call
     *          send_input_report() every time this function is invoked.
     *
     * @param length The data length requested by the host
     */
    virtual void on_input_report_requested(std::uint16_t length);

    /**
     * @brief Callback when an output report is received
     *
     * @param data Output report data
     */
    virtual void on_output_report_received(asio::const_buffer data);

    // ========== HID class-specific requests (optional override for subclasses) ==========

    /**
     * @brief Rarely implemented, this is optional for unbooted devices
     * @param p_status
     * @return
     */
    virtual std::uint8_t request_get_protocol(std::uint32_t *p_status) {
        SPDLOG_WARN("unhandled request_get_protocol");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return 0;
    };

    /**
     * @brief Rarely implemented, this is optional for unbooted devices
     * @param type
     * @param p_status
     */
    virtual void request_set_protocol(std::uint16_t type, std::uint32_t *p_status) {
        SPDLOG_WARN("unhandled request_set_protocol");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    };

    virtual data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                         std::uint32_t *p_status);
    virtual void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                    const data_type &data, std::uint32_t *p_status);

    virtual data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                       std::uint32_t *p_status);
    virtual void request_set_idle(std::uint8_t speed, std::uint32_t *p_status);

    // ========== Standard request default implementations ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief Mutex protecting pending_input_report_
     */
    mutable std::mutex input_mutex_;

    /**
     * @brief Queue of pending input reports to be sent
     */
    std::deque<data_type> pending_input_reports_;

    bool has_pending_input_reports() const {
        std::lock_guard<std::mutex> lock(input_mutex_);
        return !pending_input_reports_.empty();
    }
};
} // namespace usbipdcpp
