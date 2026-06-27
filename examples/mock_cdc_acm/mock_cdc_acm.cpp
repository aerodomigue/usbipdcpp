#include "mock_cdc_acm.h"

using namespace usbipdcpp;

// ==================== MockCdcAcmCommunicationInterfaceHandler ====================

MockCdcAcmCommunicationInterfaceHandler::MockCdcAcmCommunicationInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmCommunicationInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    SPDLOG_INFO("Line coding set: baud={}, data_bits={}, stop_bits={}, parity={}",
                coding.dwDTERate, coding.bDataBits, coding.bCharFormat, coding.bParityType);
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    SPDLOG_INFO("Control line state: DTR={}, RTS={}", state.dtr, state.rts);

    // When DTR goes high, send DCD and DSR signals
    if (state.dtr && get_data_handler()) {
        // Send status notification: DCD and DSR asserted
        send_serial_state_notification(static_cast<std::uint16_t>(CdcAcmSerialState::DCD) |
                                       static_cast<std::uint16_t>(CdcAcmSerialState::DSR));
    }
}

// ==================== MockCdcAcmDataInterfaceHandler ====================

MockCdcAcmDataInterfaceHandler::MockCdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmDataInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmDataInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
    CdcAcmDataInterfaceHandler::on_new_connection(current_session, ec);
    should_immediately_stop = false;
}

void MockCdcAcmDataInterfaceHandler::on_disconnection(error_code &ec) {
    should_immediately_stop = true;
    CdcAcmDataInterfaceHandler::on_disconnection(ec);
}

void MockCdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    if (should_immediately_stop) {
        return;
    }

    // Echo: send received data back as-is using blocking send to ensure no data is lost
    SPDLOG_DEBUG("Echo {} bytes", data.size());
    send_data_blocking(std::move(data));
}
