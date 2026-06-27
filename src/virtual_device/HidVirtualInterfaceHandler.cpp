#include "virtual_device/HidVirtualInterfaceHandler.h"

#include <algorithm>

#include "Session.h"
#include "constant.h"
#include "protocol.h"

// ========== Interrupt transfer handling ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                      std::uint32_t transfer_flags,
                                                                      std::uint32_t transfer_buffer_length,
                                                                      TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // Interrupt IN: host requests input report
        // Let subclass generate report on the spot (pull model); subclass can call send_input_report()
        // to push data into pending_input_reports_, then follow the normal push flow
        on_input_report_requested(transfer_buffer_length);


        // Lock both mutexes simultaneously to avoid race conditions
        std::lock(input_mutex_, endpoint_requests_mutex_);
        std::lock_guard lock1(input_mutex_, std::adopt_lock);
        std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

        // If queue is empty and there are pending reports, respond immediately
        if (endpoint_requests_.empty(ep.address) && !pending_input_reports_.empty()) {
            auto &front_report = pending_input_reports_.front();
            auto *trx = GenericTransfer::from_handle(transfer.get());
            auto send_len = std::min(front_report.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(front_report.begin(), front_report.begin() + send_len);
            trx->actual_length = send_len;
            pending_input_reports_.pop_front();
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, static_cast<std::uint32_t>(send_len), std::move(transfer)));
        }
        else {
            // Enqueue the request; wait for send_input_report() response
            endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
        }
    }
    else {
        // Interrupt OUT: host sends output report
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        on_output_report_received(asio::buffer(trx->data));

        // TransferHandle is released automatically on destruction
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
    }
}

// ========== Send input report ==========

void usbipdcpp::HidVirtualInterfaceHandler::send_input_report(asio::const_buffer data) {
    // Lock both mutexes simultaneously
    std::lock(input_mutex_, endpoint_requests_mutex_);
    std::lock_guard lock1(input_mutex_, std::adopt_lock);
    std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

    // Dequeue from any endpoint that has a request
    auto req_opt = endpoint_requests_.dequeue_any();

    if (req_opt.has_value()) {
        // Queued request exists; respond to it
        auto &[ep_addr, req] = req_opt.value();

        auto *trx = GenericTransfer::from_handle(req.transfer.get());
        auto send_len = std::min(data.size(), static_cast<std::size_t>(req.length));
        trx->data.assign(static_cast<const std::uint8_t *>(data.data()),
                         static_cast<const std::uint8_t *>(data.data()) + send_len);
        trx->actual_length = send_len;

        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                req.seqnum, static_cast<std::uint32_t>(send_len), std::move(req.transfer)));
    }
    else {
        // No request; queue the report to wait
        pending_input_reports_.emplace_back(static_cast<const std::uint8_t *>(data.data()),
                                            static_cast<const std::uint8_t *>(data.data()) + data.size());
    }
}

// ========== Callback default implementations ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_input_report_requested(std::uint16_t length) {
    // Default empty implementation; subclasses may override
}

void usbipdcpp::HidVirtualInterfaceHandler::on_output_report_received(asio::const_buffer data) {
    // Default empty implementation; subclasses may override
}

// ========== Connection lifecycle ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(input_mutex_);
        pending_input_reports_.clear();
    }
    {
        std::lock_guard lock(endpoint_requests_mutex_);
        // TransferHandle is released automatically on destruction
        endpoint_requests_.clear();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

// ========== UNLINK handling ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum,
                                                                 std::uint32_t cmd_seqnum) {
    std::lock_guard lock(endpoint_requests_mutex_);
    endpoint_requests_.cancel_by_seqnum(unlink_seqnum);
    // Return success regardless of whether it was found
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(cmd_seqnum));
}

// ========== Control request handling ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    switch (type) {
        case RequestType::Class: {
            auto request = static_cast<HIDRequest>(setup_packet.request);
            std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
            if (!setup_packet.is_out()) {
                data_type result;
                switch (request) {
                    case HIDRequest::GetIdle: {
                        result = request_get_idle(setup_packet.value >> 8, setup_packet.value, setup_packet.length,
                                                  &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    case HIDRequest::GetProtocol: {
                        auto ret = request_get_protocol(&status);
                        vector_append_to_net(result, ret);
                        break;
                    }
                    case HIDRequest::GetReport: {
                        result = request_get_report(setup_packet.value >> 8, setup_packet.value, setup_packet.length,
                                                    &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    default: {
                        SPDLOG_ERROR("Unknown HID request 0x{:x}", setup_packet.request);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    }
                }
                // Write data into transfer_handle
                trx->data = std::move(result);
                trx->actual_length = trx->data.size();
                trx->data_offset = 0;

                session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                        seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            }
            else {
                // Get OUT data from transfer_handle
                data_type out_data(trx->data.begin(), trx->data.begin() + transfer_buffer_length);
                switch (request) {
                    case HIDRequest::SetIdle: {
                        request_set_idle(setup_packet.value >> 8, &status);
                        break;
                    }
                    case HIDRequest::SetProtocol: {
                        request_set_protocol(setup_packet.value, &status);
                        break;
                    }
                    case HIDRequest::SetReport: {
                        request_set_report(setup_packet.value >> 8, setup_packet.value, setup_packet.length, out_data,
                                           &status);
                        break;
                    }
                    default: {
                        SPDLOG_ERROR("Unknown HID request 0x{:x}", setup_packet.request);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    }
                }
                // TransferHandle is released automatically on destruction
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
            }
            break;
        }
        default: {
            handle_non_hid_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                                    std::move(transfer), ec);
        }
    }
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_descriptor(std::uint8_t type,
                                                                                   std::uint8_t language_id,
                                                                                   std::uint16_t descriptor_length,
                                                                                   std::uint32_t *p_status) {
    auto hid_type = static_cast<HidDescriptorType>(type);
    switch (hid_type) {
        case HidDescriptorType::Report: {
            return get_report_descriptor();
        }
        default: {
            SPDLOG_ERROR("Unimplement descriptor type: {:x}", static_cast<std::uint32_t>(hid_type));
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
            return {};
        }
    }
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::get_class_specific_descriptor() {
    auto report_descriptor_size = get_report_descriptor_size();
    return {
            0x09, // bLength
            HidDescriptorType::Hid, // bDescriptorType: HID
            0x11,
            0x01, // bcdHID 1.11
            0x00, // bCountryCode
            0x01, // bNumDescriptors
            HidDescriptorType::Report, // bDescriptorType[0] HID
            static_cast<std::uint8_t>(report_descriptor_size),
            static_cast<std::uint8_t>(report_descriptor_size >> 8), // wDescriptorLength[0]
    };
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id,
                                                                             std::uint16_t length,
                                                                             std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

void usbipdcpp::HidVirtualInterfaceHandler::request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

// ========== Non-HID request default implementations ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_hid_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// ========== Report request default implementations ==========

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_report(std::uint8_t type,
                                                                               std::uint8_t report_id,
                                                                               std::uint16_t length,
                                                                               std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_get_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

void usbipdcpp::HidVirtualInterfaceHandler::request_set_report(std::uint8_t type, std::uint8_t report_id,
                                                               std::uint16_t length, const data_type &data,
                                                               std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
