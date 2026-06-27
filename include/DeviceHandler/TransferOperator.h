#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>

#include <asio.hpp>

#include "Export.h"
#include "protocol.h"

namespace usbipdcpp {

/**
 * @brief Abstract base class for transfer operators
 *
 * Encapsulates the creation, reading/writing, and releasing of transfer_handles, delegated by AbstDeviceHandler.
 * Different scenarios have their own implementations: GenericTransferOperator (default), libusb, zero-copy storage, etc.
 */
class USBIPDCPP_API TransferOperator {
public:
    virtual ~TransferOperator() = default;

    virtual void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                        const SetupPacket &setup_packet) = 0;
    virtual void free_transfer_handle(void *handle) = 0;

    virtual std::size_t get_actual_length(void *handle) = 0;

    virtual UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) = 0;
    virtual void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) = 0;

    /**
     * @brief Send transfer data (IN direction: server → client)
     *
     * Called by RET_SUBMIT::to_socket() after writing the USBIP header.
     * The caller guarantees that handle was created by this operator's alloc_transfer_handle.
     *
     * Implementations must strictly follow these steps and must not read or write sock beyond them, to prevent protocol desynchronization:
     *
     * 1. If length > 0, send length bytes of data from the private transfer to sock.
     *    For IN transfers, length is actual_length; for OUT transfers, length is always 0 — skip this step.
     *
     * 2. Send N = num_iso_packets (passed to alloc_transfer_handle)
     *    UsbIpIsoPacketDescriptors. For each descriptor in a for loop, call to_bytes()
     *    to obtain network byte order and write to sock.
     *    For non-isochronous transfers, N is 0 or 0xFFFFFFFF — skip this step.
     *
     * All ISO packets are packed contiguously on the wire; no gaps may be inserted between the byte ranges described by descriptors.
     *
     * See GenericTransferOperator and LibusbTransferOperator for concrete implementations.
     */
    virtual void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                    std::error_code &ec) = 0;

    /**
     * @brief Receive transfer data (OUT direction: client → server, including IN isochronous descriptors)
     *
     * Called by CMD_SUBMIT::from_socket() after reading the USBIP header.
     * The caller guarantees that handle was created by this operator's alloc_transfer_handle.
     *
     * Implementations must strictly follow these steps and must not read or write sock beyond them, to prevent protocol desynchronization:
     *
     * 1. If length > 0, read length bytes from sock and write them into the private transfer.
     *    For OUT transfers, length is transfer_buffer_length; for IN transfers, length is always 0 — skip this step.
     *
     * 2. Continue reading N = num_iso_packets (passed to alloc_transfer_handle)
     *    UsbIpIsoPacketDescriptors from sock. In a for loop, use
     *    UsbIpIsoPacketDescriptor::from_socket(sock) to read each one,
     *    and write it into the iso packet descriptor section of the private transfer via set_iso_descriptor.
     *    For non-isochronous transfers, N is 0 or 0xFFFFFFFF — skip this step.
     *
     * All ISO packets are packed contiguously on the wire; no gaps may be inserted between the byte ranges described by descriptors.
     *
     * See GenericTransferOperator and LibusbTransferOperator for concrete implementations.
     */
    virtual void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                    std::error_code &ec) = 0;

    /**
     * @brief Returns the leaf TransferOperator for the specified endpoint
     *
     * Used for endpoint routing in from_socket: a routing-layer op (e.g. VirtualDeviceTransferOperator) uses this method
     * to return the final leaf op for a given ep; the caller then performs alloc / I/O directly on the leaf op without any map lookup.
     * Non-routing-layer ops simply return this.
     */
    virtual TransferOperator *get_operator_for_ep(std::uint8_t ep) {
        return this;
    }
};

/**
 * @brief Default transfer operator based on GenericTransfer
 *
 * Fully consistent with AbstDeviceHandler's original default implementation: creates a GenericTransfer,
 * stores data in a vector, and uses asio for one-shot reads/writes in send/recv.
 */
class USBIPDCPP_API GenericTransferOperator : public TransferOperator {
public:
    void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                const SetupPacket &setup_packet) override;
    void free_transfer_handle(void *handle) override;

    std::size_t get_actual_length(void *handle) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override;
    void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override;

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
    void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
};

} // namespace usbipdcpp
