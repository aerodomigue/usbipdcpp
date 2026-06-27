#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string_view>
#include <vector>
#include "CdcAcmConstants.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"
#include "utils/RingBuffer.h"
#include "virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {
/**
 * @brief CDC ACM line coding structure
 */
struct LineCoding {
    std::uint32_t dwDTERate = 115200; // Baud rate
    std::uint8_t bCharFormat = 0; // Stop bits: 0=1 bit, 1=1.5 bits, 2=2 bits
    std::uint8_t bParityType = 0; // Parity: 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space
    std::uint8_t bDataBits = 8; // Data bits: 5, 6, 7, 8, 16

    [[nodiscard]] std::array<std::uint8_t, 7> to_bytes() const {
        return {{static_cast<std::uint8_t>(dwDTERate & 0xFF), static_cast<std::uint8_t>((dwDTERate >> 8) & 0xFF),
                 static_cast<std::uint8_t>((dwDTERate >> 16) & 0xFF),
                 static_cast<std::uint8_t>((dwDTERate >> 24) & 0xFF), bCharFormat, bParityType, bDataBits}};
    }

    static LineCoding from_bytes(const std::vector<std::uint8_t> &data) {
        LineCoding coding{};
        if (data.size() >= 7) {
            coding.dwDTERate = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            coding.bCharFormat = data[4];
            coding.bParityType = data[5];
            coding.bDataBits = data[6];
        }
        return coding;
    }
};

/**
 * @brief CDC ACM control signal state
 */
struct ControlSignalState {
    bool dtr = false; // Data Terminal Ready
    bool rts = false; // Request To Send

    [[nodiscard]] std::uint16_t to_uint16() const {
        std::uint16_t value = 0;
        if (dtr)
            value |= static_cast<std::uint16_t>(CdcAcmControlSignal::DTR);
        if (rts)
            value |= static_cast<std::uint16_t>(CdcAcmControlSignal::RTS);
        return value;
    }

    static ControlSignalState from_uint16(std::uint16_t value) {
        ControlSignalState state;
        state.dtr = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::DTR)) != 0;
        state.rts = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::RTS)) != 0;
        return state;
    }
};

/**
 * @brief CDC ACM serial state notification
 */
struct SerialStateNotification {
    std::uint8_t bmRequestType = 0xA1; // Class-specific, interface, IN
    std::uint8_t bNotification = 0x20; // SERIAL_STATE
    std::uint16_t wValue = 0;
    std::uint16_t wIndex = 0; // Interface number
    std::uint16_t wLength = 2;
    std::uint16_t data = 0; // State bits

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> result;
        result.push_back(bmRequestType);
        result.push_back(bNotification);
        result.push_back(wValue & 0xFF);
        result.push_back((wValue >> 8) & 0xFF);
        result.push_back(wIndex & 0xFF);
        result.push_back((wIndex >> 8) & 0xFF);
        result.push_back(wLength & 0xFF);
        result.push_back((wLength >> 8) & 0xFF);
        result.push_back(data & 0xFF);
        result.push_back((data >> 8) & 0xFF);
        return result;
    }
};

// Forward declaration
class CdcAcmDataInterfaceHandler;

/**
 * @brief CDC ACM communication interface handler (handles control requests and status notifications)
 *
 * Used to handle the communication interface of a CDC ACM device, responding to control requests and sending status notifications.
 */
class USBIPDCPP_API CdcAcmCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== Standard request default implementations ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;

    // ========== Optional overridable callbacks for subclasses ==========

    /**
     * @brief Callback when the host sets the line coding
     * @param coding The new line coding parameters
     */
    virtual void on_set_line_coding(const LineCoding &coding);

    /**
     * @brief Callback when the host sets the control line state
     * @param state Control signal state (DTR, RTS)
     */
    virtual void on_set_control_line_state(const ControlSignalState &state);

    /**
     * @brief Callback when the host requests to send a break signal
     * @param duration Break duration
     */
    virtual void on_send_break(std::uint16_t duration);

    /**
     * @brief Handles control transfers for non-CDC ACM class requests; subclasses can override to extend functionality
     */
    virtual void handle_non_cdc_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet, TransferHandle transfer,
                                                         std::error_code &ec);

    // ========== Status query API ==========

    /**
     * @brief Get the current line coding
     */
    [[nodiscard]] const LineCoding &get_line_coding() const {
        return line_coding_;
    }

    /**
     * @brief Get the current control signal state
     */
    [[nodiscard]] const ControlSignalState &get_control_signal_state() const {
        return control_signal_state_;
    }

    // ========== Send notification API ==========

    /**
     * @brief Send a serial state notification to the host
     * @param state_bits State bits (e.g. CTS, DSR, etc.)
     */
    void send_serial_state_notification(std::uint16_t state_bits);

    // ========== Interface association API ==========

    /**
     * @brief Associate the data interface handler
     */
    void set_data_handler(CdcAcmDataInterfaceHandler *handler) {
        data_handler_ = handler;
    }

    /**
     * @brief Get the associated data interface handler
     */
    CdcAcmDataInterfaceHandler *get_data_handler() const {
        return data_handler_;
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    LineCoding line_coding_;
    ControlSignalState control_signal_state_;

    /**
     * @brief Pending status notification data to be sent
     */
    std::vector<std::uint8_t> pending_notification_;

    /**
     * @brief Mutex protecting pending_notification_
     */
    std::mutex notification_mutex_;

    /**
     * @brief Associated data interface handler
     */
    CdcAcmDataInterfaceHandler *data_handler_ = nullptr;
};

/**
 * @brief CDC ACM data interface handler (handles data transfers)
 *
 * Used to handle the data interface of a CDC ACM device, processing bulk data transfers.
 */
class USBIPDCPP_API CdcAcmDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== Standard request default implementations ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;

    // ========== Optional overridable callbacks for subclasses ==========

    /**
     * @brief Callback when data sent by the host is received
     * @param data The received data
     */
    virtual void on_data_received(data_type &&data);

    /**
     * @brief Callback when the host requests data, used for on-demand data generation
     * @param length The data length requested by the host
     * @return Returns the data to be sent; if empty, waits for a send_data call
     */
    virtual data_type on_data_requested(std::uint16_t length);

    /**
     * @brief Callback when the host RTS state changes
     * @param rts RTS state, true = host is willing to receive data
     */
    virtual void on_rts_changed(bool rts);

    // ========== Send data API ==========

    /**
     * @brief Non-blocking send data to the host
     * @param data Data pointer
     * @param size Data size
     * @return Actual bytes written to the buffer; may be less than requested when the buffer is full
     */
    std::size_t send_data(const std::uint8_t *data, std::size_t size);
    std::size_t send_data(const data_type &data);
    std::size_t send_data(data_type &&data);
    std::size_t send_data(std::string_view data);

    /**
     * @brief Blocking send data to the host, waiting for buffer space
     * @param data Data pointer
     * @param size Data size
     * @param timeout_ms Timeout in milliseconds; 0 means wait indefinitely
     * @return Actual bytes written to the buffer; may be less than requested on timeout
     */
    std::size_t send_data_blocking(const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(const data_type &data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(data_type &&data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(std::string_view data, std::uint32_t timeout_ms = 0);

    // ========== Buffer configuration API ==========

    /**
     * @brief Set the TX buffer capacity
     * @param capacity Buffer size in bytes
     */
    void set_tx_buffer_capacity(std::size_t capacity);

    /**
     * @brief Set the TX watermarks
     * @param high High watermark; flow control is recommended when the buffer exceeds this value
     * @param low Low watermark; transmission may resume when the buffer falls below this value
     */
    void set_tx_watermarks(std::size_t high, std::size_t low);

    /**
     * @brief Get the current amount of data in the TX buffer
     * @return Number of bytes currently used in the buffer
     */
    [[nodiscard]] std::size_t get_tx_buffer_size() const;

    /**
     * @brief Get the remaining space in the TX buffer
     * @return Available bytes in the buffer
     */
    [[nodiscard]] std::size_t get_tx_buffer_available() const;

    // ========== Flow control state API ==========

    /**
     * @brief Set the CTS state to notify the host
     * @param cts CTS state, true = device can receive data
     */
    void set_cts(bool cts);

    /**
     * @brief Get the current RTS state (from the host)
     * @return RTS state, true = host is willing to receive data
     */
    [[nodiscard]] bool get_rts() const;

    /**
     * @brief Associate the communication interface handler
     * @param handler Pointer to the communication interface handler
     */
    void set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(Session &current_session, std::error_code &ec) override;
    void on_disconnection(std::error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief TX buffer (device → host)
     */
    RingBuffer tx_buffer_;

    std::size_t tx_high_watermark_ = 48 * 1024;
    std::size_t tx_low_watermark_ = 16 * 1024;

    /**
     * @brief Mutex protecting tx_buffer_
     */
    mutable std::mutex tx_mutex_;

    /**
     * @brief Condition variable used to wait for buffer space during blocking sends
     */
    std::condition_variable tx_cv_;

    /**
     * @brief Disconnection flag, used to make blocking sends return early
     */
    bool disconnected_ = true;

    /**
     * @brief Associated communication interface handler
     */
    CdcAcmCommunicationInterfaceHandler *comm_handler_ = nullptr;

    /**
     * @brief Read data from the TX buffer and send it
     * @param seqnum Request sequence number
     * @param max_length Maximum send length
     * @param transfer Transfer handle
     * @note Caller must already hold tx_mutex_ and endpoint_requests_mutex_, and ensure tx_buffer_ is not empty
     */
    void send_from_tx_buffer_locked(std::uint32_t seqnum, std::uint32_t max_length, TransferHandle transfer);

    /**
     * @brief Try to send pending data
     * @note Caller must already hold tx_mutex_ and endpoint_requests_mutex_
     */
    void try_send_pending_locked();
};
} // namespace usbipdcpp
