#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <system_error>
#include <vector>

#include <asio.hpp>

#include "Device.h"
#include "DeviceHandler/TransferOperator.h"
#include "protocol.h"
#include "type.h"


namespace usbipdcpp {
struct UsbEndpoint;
struct UsbInterface;
struct SetupPacket;

class Session;

/**
 * @brief Abstract base class for USB device handling.
 *
 * Each imported device corresponds to one DeviceHandler instance. Transfer data reads and writes are done through
 * TransferOperator, obtained via get_transfer_operator().
 * Uses GenericTransferOperator by default; subclasses may replace it.
 */
class USBIPDCPP_API AbstDeviceHandler {
public:
    explicit AbstDeviceHandler(UsbDevice &handle_device, std::unique_ptr<TransferOperator> op = nullptr) :
        handle_device(handle_device), transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {
    }

    AbstDeviceHandler(AbstDeviceHandler &&other) noexcept;

    /**
     * @brief Unified entry point for handling URB requests
     * @param cmd Complete CMD_SUBMIT command
     * @param ep Endpoint information
     * @param interface Optional interface information (required for non-control transfers)
     * @param ec Error code
     */
    virtual void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                             usbipdcpp::error_code &ec) = 0;

    /**
     * @brief Called when a new client connects; may block. Subclasses should call this at the start of their implementation
     * @param current_session Store the communication session yourself
     * @param ec Error code that occurred
     */
    virtual void on_new_connection(Session &current_session, error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = &current_session;
    }

    /**
     * @brief Called when a transfer must be completely terminated due to errors, etc. After this call, submitting messages or using the Session object is prohibited.\n
     * May block to handle all pending work. Subclasses should call this at the end of their implementation
     */
    virtual void on_disconnection(error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = nullptr;
    }

    /**
     * @brief Check whether the device has been removed
     * @return true if the device has been physically unplugged
     */
    virtual bool is_device_removed() const {
        return false; // Default implementation
    }

    /**
     * @brief Called when the device is physically removed
     */
    virtual void on_device_removed() {
    }

    /**
     * @brief Stop the Session in a thread-safe manner
     */
    void trigger_session_stop();

    /**
     * @brief Handle USBIP_CMD_UNLINK (client requests cancellation of a pending transfer).
     *
     * Protocol contract:
     * - Each CMD_UNLINK must have a corresponding RET_UNLINK with a USBIP error code as status.
     * - If the target transfer has already completed normally, the RET_UNLINK status must be 0 (URB completed).
     * - RET_SUBMIT must be sent before RET_UNLINK. The client receives the actual URB result first,
     *   then receives the unlink confirmation — not the other way around. If an implementation may send
     *   both RET_SUBMIT and RET_UNLINK for the same transfer, RET_SUBMIT must arrive at the client before RET_UNLINK.
     *
     * @param unlink_seqnum The seqnum of the CMD_SUBMIT to cancel
     * @param cmd_seqnum    The seqnum of CMD_UNLINK itself (echoed back in RET_UNLINK)
     *
     * The target transfer may be in one of the following states when unlink arrives; each must be handled separately:
     *
     * State 1 — Transfer has not yet started or is still in progress:
     *   Cancel the transfer. If cancellation succeeds, send RET_UNLINK(cmd_seqnum, -ECONNRESET)
     *   when the transfer completes (indicating the URB was cancelled), and do not send RET_SUBMIT.
     *
     * State 2 — Transfer has completed but RET_SUBMIT has not yet been sent:
     *   Send RET_UNLINK(cmd_seqnum, actual_completion_status); 0 if no error.
     *   Do not send RET_SUBMIT.
     *
     * State 3 — RET_SUBMIT has already been sent:
     *   Send RET_UNLINK(cmd_seqnum, 0) directly. RET_SUBMIT has already been sent first; order is correct.
     *
     * State 4 — Transfer has been fully processed (both RET_SUBMIT and any RET_UNLINK have been sent):
     *   Send RET_UNLINK(cmd_seqnum, 0) directly.
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) = 0;

    // ========== TransferOperator interface ==========

    /**
     * @brief Get the transfer operator (subclasses may override to replace the default implementation)
     */
    TransferOperator *get_transfer_operator() const {
        return transfer_op_.get();
    }

    /**
     * @brief Replace the transfer operator (ownership is transferred)
     */
    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        transfer_op_ = std::move(op);
    }

    virtual ~AbstDeviceHandler() = default;

protected:
    UsbDevice &handle_device;
    Session *session = nullptr;
    mutable std::mutex session_mutex_;
    std::unique_ptr<TransferOperator> transfer_op_ = std::make_unique<GenericTransferOperator>();
};
} // namespace usbipdcpp
