#include "virtual_device/VirtualInterfaceHandler.h"

#include "Session.h"
#include "protocol.h"

using namespace usbipdcpp;

void VirtualInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                   TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("Virtual interface default bulk transfer implementation for endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                        std::uint32_t transfer_flags,
                                                        std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                        std::error_code &ec) {
    SPDLOG_TRACE("Virtual interface default interrupt transfer implementation for endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                          int num_iso_packets, std::error_code &ec) {
    SPDLOG_TRACE("Virtual interface default isochronous transfer implementation for endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("Virtual interface default non-standard control transfer implementation for endpoint {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("Default non-standard control transfer implementation for recipient endpoint address {:04x}", ep.address);
    // TransferHandle is released automatically on destruction
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type VirtualInterfaceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    *p_status = static_cast<uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}
