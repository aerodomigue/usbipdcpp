#pragma once

#include <cstdint>
#include <unordered_map>

#include "DeviceHandler/TransferOperator.h"

namespace usbipdcpp {

/**
 * @brief Virtual device layer transfer operator, routes to interface-level TransferOperator by endpoint
 *
 * Maintains an endpoint→operator registry (ep_operators_); alloc_transfer_handle routes by header.ep.
 * After routing, the caller stores the leaf op in TransferHandle; subsequent I/O operations call the leaf op directly
 * without going through this class's map lookup, so no handle→operator mapping or lock is needed.
 */
class USBIPDCPP_API VirtualDeviceTransferOperator : public TransferOperator {
public:
    /**
     * @brief Register an endpoint→operator mapping
     * @param ep Endpoint address (e.g. 0x02 for OUT, 0x81 for IN)
     * @param op Interface-level TransferOperator (e.g. StorageTransferOperator)
     */
    void register_endpoint_operator(std::uint8_t ep, TransferOperator *op) {
        ep_operators_[ep] = op;
    }

    /**
     * @brief Returns the leaf TransferOperator corresponding to ep
     *
     * from_socket retrieves the leaf op via this method and operates directly on it,
     * eliminating the need for a handle→operator mapping.
     */
    TransferOperator *get_operator_for_ep(std::uint8_t ep) override;

    // ========== TransferOperator interface ==========

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

private:
    GenericTransferOperator generic_op_;
    std::unordered_map<std::uint8_t, TransferOperator *> ep_operators_;
};

} // namespace usbipdcpp
