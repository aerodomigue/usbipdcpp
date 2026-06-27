#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"

#include "DeviceHandler/DeviceHandler.h"

#include <algorithm>
#include "Session.h"

namespace usbipdcpp {
// ==================== CdcAcmCommunicationInterfaceHandler ====================

CdcAcmCommunicationInterfaceHandler::CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface,
                                                                         StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void CdcAcmCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<CdcAcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN request
            auto *trx = GenericTransfer::from_handle(transfer.get());
            switch (request) {
                case CdcAcmRequest::GetLineCoding: {
                    auto bytes = line_coding_.to_bytes();
                    trx->data.assign(bytes.begin(), bytes.end());
                    if (setup_packet.length < trx->data.size()) {
                        trx->data.resize(setup_packet.length);
                    }
                    trx->actual_length = trx->data.size();
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM IN request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    trx->actual_length = 0;
                }
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, status, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // OUT request
            auto *trx = GenericTransfer::from_handle(transfer.get());
            auto &out_data = trx->data;
            switch (request) {
                case CdcAcmRequest::SetLineCoding: {
                    auto new_coding = LineCoding::from_bytes(out_data);
                    on_set_line_coding(new_coding);
                    line_coding_ = new_coding;
                    SPDLOG_DEBUG("SET_LINE_CODING: baud={}, data_bits={}, stop_bits={}, parity={}",
                                 line_coding_.dwDTERate, line_coding_.bDataBits, line_coding_.bCharFormat,
                                 line_coding_.bParityType);
                    break;
                }
                case CdcAcmRequest::SetControlLineState: {
                    auto state = ControlSignalState::from_uint16(setup_packet.value);
                    on_set_control_line_state(state);
                    control_signal_state_ = state;
                    SPDLOG_DEBUG("SET_CONTROL_LINE_STATE: DTR={}, RTS={}", state.dtr, state.rts);
                    // Notify data interface of RTS state change
                    if (data_handler_) {
                        data_handler_->on_rts_changed(state.rts);
                    }
                    break;
                }
                case CdcAcmRequest::SendBreak: {
                    auto duration = setup_packet.value;
                    on_send_break(duration);
                    SPDLOG_DEBUG("SEND_BREAK: duration={}", duration);
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM OUT request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                }
            }
            // TransferHandle is released automatically on destruction
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
        }
    }
    else {
        // Non-CDC class request; delegate to subclass
        handle_non_cdc_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                                std::move(transfer), ec);
    }
}

void CdcAcmCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                    std::uint32_t transfer_flags,
                                                                    std::uint32_t transfer_buffer_length,
                                                                    TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // Lock both mutexes simultaneously to avoid race conditions
        std::lock(notification_mutex_, endpoint_requests_mutex_);
        std::lock_guard lock1(notification_mutex_, std::adopt_lock);
        std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

        if (!pending_notification_.empty() && endpoint_requests_.empty(ep.address)) {
            // Pending notification exists and no queued requests; respond immediately
            auto *trx = GenericTransfer::from_handle(transfer.get());
            trx->data = std::move(pending_notification_);
            trx->actual_length = trx->data.size();
            pending_notification_.clear();
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // Enqueue the request; wait for processing
            endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
        }
    }
    else {
        // Interrupt OUT: CDC ACM does not normally use this
        // TransferHandle is released automatically on destruction
        SPDLOG_WARN("CDC ACM communication interface received unexpected interrupt OUT");
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type CdcAcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // CDC ACM class-specific descriptor
    // Header Functional Descriptor
    data_type descriptor = {
            0x05, // bLength
            0x24, // bDescriptorType: CS_INTERFACE
            0x00, // bDescriptorSubtype: Header
            0x10, 0x01 // bcdCDC: 1.10
    };

    // Call Management Functional Descriptor
    descriptor.insert(descriptor.end(), {
                                                0x05, // bLength
                                                0x24, // bDescriptorType: CS_INTERFACE
                                                0x01, // bDescriptorSubtype: Call Management
                                                0x00, // bmCapabilities
                                                0x01 // bDataInterface: Interface 1
                                        });

    // ACM Functional Descriptor
    descriptor.insert(descriptor.end(),
                      {
                              0x04, // bLength
                              0x24, // bDescriptorType: CS_INTERFACE
                              0x02, // bDescriptorSubtype: ACM
                              0x02 // bmCapabilities: support Set_Line_Coding, Set_Control_Line_State, Send_Break
                      });

    // Union Functional Descriptor
    descriptor.insert(descriptor.end(), {
                                                0x05, // bLength
                                                0x24, // bDescriptorType: CS_INTERFACE
                                                0x06, // bDescriptorSubtype: Union
                                                0x00, // bMasterInterface: Interface 0
                                                0x01 // bSlaveInterface0: Interface 1
                                        });

    return descriptor;
}

void CdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    // Default empty implementation; subclasses may override
}

void CdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    // Default empty implementation; subclasses may override
}

void CdcAcmCommunicationInterfaceHandler::on_send_break(std::uint16_t duration) {
    // Default empty implementation; subclasses may override
}

void CdcAcmCommunicationInterfaceHandler::handle_non_cdc_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Returns error by default; subclasses may override to handle non-CDC requests
    SPDLOG_WARN("Unhandled request type 0x{:x} in CDC ACM communication interface", setup_packet.calc_request_type());
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// ==================== CdcAcmCommunicationInterfaceHandler default implementations ====================

void CdcAcmCommunicationInterfaceHandler::request_clear_feature(std::uint16_t feature_selector,
                                                                std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmCommunicationInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector,
                                                                         std::uint8_t ep_address,
                                                                         std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t CdcAcmCommunicationInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void CdcAcmCommunicationInterfaceHandler::request_set_interface(std::uint16_t alternate_setting,
                                                                std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t CdcAcmCommunicationInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t CdcAcmCommunicationInterfaceHandler::request_endpoint_get_status(std::uint8_t ep_address,
                                                                               std::uint32_t *p_status) {
    return 0;
}

void CdcAcmCommunicationInterfaceHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmCommunicationInterfaceHandler::request_endpoint_set_feature(std::uint16_t feature_selector,
                                                                       std::uint8_t ep_address,
                                                                       std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmCommunicationInterfaceHandler::send_serial_state_notification(std::uint16_t state_bits) {
    SerialStateNotification notification;
    notification.data = state_bits;

    // Lock both mutexes simultaneously to avoid race conditions
    std::lock(notification_mutex_, endpoint_requests_mutex_);
    std::lock_guard lock1(notification_mutex_, std::adopt_lock);
    std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

    pending_notification_ = notification.to_bytes();

    // If there are queued interrupt requests, respond to the first one
    auto req_opt = endpoint_requests_.dequeue_any();
    if (req_opt.has_value()) {
        auto &[ep_addr, req] = req_opt.value();

        auto *trx = GenericTransfer::from_handle(req.transfer.get());
        trx->data = std::move(pending_notification_);
        trx->actual_length = trx->data.size();
        pending_notification_.clear();
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                req.seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(req.transfer)));
    }
}

void CdcAcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(notification_mutex_);
        pending_notification_.clear();
    }
    {
        std::lock_guard lock(endpoint_requests_mutex_);
        // TransferHandle is released automatically on destruction
        endpoint_requests_.clear();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    std::lock_guard lock(endpoint_requests_mutex_);
    endpoint_requests_.cancel_by_seqnum(unlink_seqnum);
    // Return success regardless of whether it was found
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(cmd_seqnum));
}

// ==================== CdcAcmDataInterfaceHandler ====================

CdcAcmDataInterfaceHandler::CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void CdcAcmDataInterfaceHandler::on_new_connection(Session &current_session, std::error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    std::lock_guard lock(tx_mutex_);
    disconnected_ = false;
}

void CdcAcmDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(tx_mutex_);
        disconnected_ = true;
        tx_buffer_.clear();
    }
    {
        std::lock_guard lock(endpoint_requests_mutex_);
        // TransferHandle is released automatically on destruction
        endpoint_requests_.clear();
    }
    tx_cv_.notify_all();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    std::lock_guard lock(endpoint_requests_mutex_);
    endpoint_requests_.cancel_by_seqnum(unlink_seqnum);
    // Return success regardless of whether it was found
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(cmd_seqnum));
}

void CdcAcmDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                      std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN: host requests data
        // Lock both tx_mutex_ and endpoint_requests_mutex_ simultaneously to avoid race conditions
        std::lock(tx_mutex_, endpoint_requests_mutex_);
        std::lock_guard lock1(tx_mutex_, std::adopt_lock);
        std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

        if (tx_buffer_.empty() && endpoint_requests_.empty(ep.address)) {
            // Buffer empty and no queued requests; invoke subclass callback to get data
            auto data = on_data_requested(transfer_buffer_length);
            if (!data.empty()) {
                if (data.size() <= transfer_buffer_length) {
                    auto *trx = GenericTransfer::from_handle(transfer.get());
                    trx->data = std::move(data);
                    trx->actual_length = trx->data.size();
                    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                            seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
                }
                else {
                    // Data exceeds requested length; move entire data, send partial, write remainder to buffer
                    auto *trx = GenericTransfer::from_handle(transfer.get());
                    trx->data = std::move(data);
                    trx->actual_length = transfer_buffer_length;

                    // Write remaining data to buffer
                    tx_buffer_.write(trx->data.data() + transfer_buffer_length,
                                     trx->data.size() - transfer_buffer_length);

                    // Truncate to send length
                    trx->data.resize(transfer_buffer_length);

                    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                            seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
                }
            }
            else {
                // No data to send; enqueue the request
                endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
            }
        }
        else {
            // Buffer has data or there are queued requests; enqueue the request
            endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
            // Try to send
            try_send_pending_locked();
        }
    }
    else {
        // Bulk OUT: receive data from host; invoke subclass callback directly
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        on_data_received(std::move(trx->data));

        // TransferHandle is released automatically on destruction
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
    }
}

void CdcAcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Data interface does not normally handle class-specific control requests
    SPDLOG_WARN("CDC ACM data interface received unexpected control request");
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type CdcAcmDataInterfaceHandler::get_class_specific_descriptor() {
    // Data interface has no class-specific descriptor
    return {};
}

void CdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    // Default empty implementation; subclasses may override
}

data_type CdcAcmDataInterfaceHandler::on_data_requested(std::uint16_t length) {
    // Returns empty by default; subclasses may override
    return {};
}

void CdcAcmDataInterfaceHandler::on_rts_changed(bool rts) {
    // Default empty implementation; subclasses may override
}

// ===== Internal functions =====

void CdcAcmDataInterfaceHandler::send_from_tx_buffer_locked(std::uint32_t seqnum, std::uint32_t max_length,
                                                            TransferHandle transfer) {
    // Caller must already hold tx_mutex_ and ensure tx_buffer_ is not empty
    // Read data from TX buffer and send
    std::size_t send_len = std::min(tx_buffer_.size(), static_cast<std::size_t>(max_length));
    auto *trx = GenericTransfer::from_handle(transfer.get());
    trx->data.resize(send_len);
    tx_buffer_.read(trx->data.data(), send_len);
    trx->actual_length = send_len;

    // Notify blocked sender: buffer has space now
    tx_cv_.notify_one();

    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
            seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));

    // Check low watermark
    if (tx_buffer_.size() <= tx_low_watermark_) {
        // Can notify subclass or set CTS here
    }
}

// ===== send_data implementation =====

std::size_t CdcAcmDataInterfaceHandler::send_data(const std::uint8_t *data, std::size_t size) {
    // Lock both mutexes simultaneously
    std::lock(tx_mutex_, endpoint_requests_mutex_);
    std::lock_guard lock1(tx_mutex_, std::adopt_lock);
    std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

    // Write to TX buffer (non-blocking; only writes available space if full)
    std::size_t written = tx_buffer_.write(data, size);

    // Try to send pending data (both mutexes already held)
    try_send_pending_locked();

    return written;
}

std::size_t CdcAcmDataInterfaceHandler::send_data(const data_type &data) {
    return send_data(data.data(), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data(data_type &&data) {
    return send_data(data.data(), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data(std::string_view data) {
    return send_data(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const std::uint8_t *data, std::size_t size,
                                                           std::uint32_t timeout_ms) {
    std::size_t total_written = 0;
    std::size_t offset = 0;

    while (offset < size) {
        // Phase 1: wait for buffer space
        {
            std::unique_lock tx_lock(tx_mutex_);

            // Check if already disconnected
            if (disconnected_) {
                return total_written;
            }

            // If buffer is full, try sending first
            if (tx_buffer_.available() == 0) {
                // Lock queue and send
                {
                    std::lock_guard queue_lock(endpoint_requests_mutex_);
                    try_send_pending_locked();
                }

                // Check disconnection again
                if (disconnected_) {
                    return total_written;
                }

                // If still full, wait on condition variable
                while (tx_buffer_.available() == 0 && !disconnected_) {
                    if (timeout_ms == 0) {
                        // Wait indefinitely
                        tx_cv_.wait(tx_lock);
                    }
                    else {
                        // Wait with timeout
                        auto result = tx_cv_.wait_for(tx_lock, std::chrono::milliseconds(timeout_ms));
                        if (result == std::cv_status::timeout) {
                            // Timed out; return amount written so far
                            return total_written;
                        }
                    }
                }

                // Check disconnection after wakeup
                if (disconnected_) {
                    return total_written;
                }
            }

            // Write data
            std::size_t written = tx_buffer_.write(data + offset, size - offset);
            total_written += written;
            offset += written;
        }

        // Phase 2: try to send (lock both mutexes)
        {
            std::lock(tx_mutex_, endpoint_requests_mutex_);
            std::lock_guard lock1(tx_mutex_, std::adopt_lock);
            std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);
            try_send_pending_locked();
        }
    }

    return total_written;
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const data_type &data, std::uint32_t timeout_ms) {
    return send_data_blocking(data.data(), data.size(), timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(data_type &&data, std::uint32_t timeout_ms) {
    return send_data_blocking(data.data(), data.size(), timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(std::string_view data, std::uint32_t timeout_ms) {
    return send_data_blocking(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(), timeout_ms);
}

// ===== Buffer configuration =====

void CdcAcmDataInterfaceHandler::set_tx_buffer_capacity(std::size_t capacity) {
    std::lock_guard lock(tx_mutex_);
    tx_buffer_.resize(capacity);
    tx_high_watermark_ = capacity * 3 / 4;
    tx_low_watermark_ = capacity / 4;
}

void CdcAcmDataInterfaceHandler::set_tx_watermarks(std::size_t high, std::size_t low) {
    tx_high_watermark_ = high;
    tx_low_watermark_ = low;
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_size() const {
    std::lock_guard lock(tx_mutex_);
    return tx_buffer_.size();
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_available() const {
    std::lock_guard lock(tx_mutex_);
    return tx_buffer_.available();
}

// ===== Flow control state =====

void CdcAcmDataInterfaceHandler::set_cts(bool cts) {
    if (comm_handler_) {
        std::uint16_t state = cts ? static_cast<std::uint16_t>(CdcAcmSerialState::CTS) : 0;
        comm_handler_->send_serial_state_notification(state);
    }
}

bool CdcAcmDataInterfaceHandler::get_rts() const {
    if (comm_handler_) {
        return comm_handler_->get_control_signal_state().rts;
    }
    return true; // Allow sending by default
}

void CdcAcmDataInterfaceHandler::set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler) {
    comm_handler_ = handler;
}

void CdcAcmDataInterfaceHandler::try_send_pending_locked() {
    // Caller must already hold tx_mutex_ and endpoint_requests_mutex_

    // Check if there are queued requests and data to send
    while (!tx_buffer_.empty()) {
        auto req_opt = endpoint_requests_.dequeue_any();
        if (!req_opt.has_value()) {
            break;
        }

        auto &[ep_addr, req] = req_opt.value();
        // Read from buffer and send
        send_from_tx_buffer_locked(req.seqnum, req.length, std::move(req.transfer));
    }
}

// ==================== CdcAcmDataInterfaceHandler default implementations ====================

void CdcAcmDataInterfaceHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmDataInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                                std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t CdcAcmDataInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void CdcAcmDataInterfaceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t CdcAcmDataInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t CdcAcmDataInterfaceHandler::request_endpoint_get_status(std::uint8_t ep_address,
                                                                      std::uint32_t *p_status) {
    return 0;
}

void CdcAcmDataInterfaceHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmDataInterfaceHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                              std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
} // namespace usbipdcpp
