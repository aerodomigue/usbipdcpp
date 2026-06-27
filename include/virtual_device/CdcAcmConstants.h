#pragma once

#include <cstdint>

namespace usbipdcpp {
// CDC ACM class-specific descriptor subtypes
enum class CdcAcmDescriptorSubtype {
    Header = 0x00,
    CallManagement = 0x01,
    ACM = 0x02,
    DirectLineManagement = 0x03,
    TelephoneRinger = 0x04,
    TelephoneCallStateReporting = 0x05,
    Union = 0x06,
    CountrySelection = 0x07,
    TelephoneOperatingModes = 0x08,
    USBTerminal = 0x09,
    NetworkChannelTerminal = 0x0A,
    ProtocolUnit = 0x0B,
    ExtensionUnit = 0x0C,
    MultiChannelManagement = 0x0D,
    CAPIControlManagement = 0x0E,
    EthernetNetworking = 0x0F,
    ATMNetworking = 0x10,
};

// CDC ACM control request codes
enum class CdcAcmRequest {
    SendEncapsulatedCommand = 0x00,
    GetEncapsulatedResponse = 0x01,
    SetCommFeature = 0x02,
    GetCommFeature = 0x03,
    ClearCommFeature = 0x04,
    SetLineCoding = 0x20,
    GetLineCoding = 0x21,
    SetControlLineState = 0x22,
    SendBreak = 0x23,
};

// CDC ACM control signal bits
enum class CdcAcmControlSignal : std::uint16_t {
    DTR = 0x01, // Data Terminal Ready
    RTS = 0x02, // Request To Send
};

// CDC ACM serial state bits
enum class CdcAcmSerialState : std::uint16_t {
    DCD = 0x01, // Data Carrier Detect
    DSR = 0x02, // Data Set Ready
    Break = 0x04, // Break signal
    Ring = 0x08, // Ring signal
    FramingError = 0x10, // Framing error
    ParityError = 0x20, // Parity error
    OverrunError = 0x40, // Overrun error
    CTS = 0x80, // Clear To Send
};
} // namespace usbipdcpp
