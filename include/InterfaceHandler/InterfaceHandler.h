#pragma once

#include "Export.h"
#include "type.h"
#include "utils/StringPool.h"

namespace usbipdcpp {
struct UsbIpIsoPacketDescriptor;
struct UsbEndpoint;
struct SetupPacket;
struct UsbInterface;

class Session;
class TransferHandle;


/**
 * @brief Inherit VirtualInterfaceHandler instead; do not inherit this class directly
 */
class USBIPDCPP_API AbstInterfaceHandler {
public:
    explicit AbstInterfaceHandler(UsbInterface &handle_interface) :
        handle_interface(handle_interface) {
    }

    /**
     * @brief Called when a new client connects
     * @param session
     * @param ec Error code that occurred
     */
    virtual void on_new_connection(Session &session, error_code &ec) =0;

    /**
     * @brief Called when a transfer must be completely terminated due to errors, client detach, server shutdown, etc. No messages may be submitted after this call
     */
    virtual void on_disconnection(error_code &ec) =0;
    /**
     * @brief Called for all seqnums; make sure to only process your own seqnum
     * @param unlink_seqnum The sequence number of the packet to cancel
     * @param cmd_seqnum The sequence number of the CMD_UNLINK command (used to construct RET_UNLINK)
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum);

    virtual ~AbstInterfaceHandler() = default;

protected:
    UsbInterface &handle_interface;
};

}
