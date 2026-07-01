#pragma once

#include <atomic>
#include <shared_mutex>
#include <unordered_map>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "DeviceHandler/DeviceHandler.h"
#include "LibusbHandler/tools.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"
#include "utils/ObjectPool.h"

namespace usbipdcpp {
class USBIPDCPP_API LibusbDeviceHandler : public AbstDeviceHandler {
    friend class LibusbServer;

public:
    /**
     * @brief Normal mode constructor (lazy binding)
     *
     * The device is only opened when a client connects (on_new_connection).
     *
     * @param handle_device The UsbDevice this handler is attached to.
     * @param native_device The libusb device (not yet opened). The handler takes ownership of this reference.
     */
    explicit LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device);

    /**
     * @brief Android mode constructor
     *
     * Uses a system device file descriptor. libusb_wrap_sys_device is called to wrap the fd on each client connection.
     * The handle is closed on disconnection and re-wrapped on the next connection, supporting reconnection.
     *
     * @param handle_device The UsbDevice this handler is attached to.
     * @param fd A valid file descriptor opened on the device node.
     *           The fd must remain valid until the handler is destroyed.
     */
    explicit LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd);

    ~LibusbDeviceHandler() override;
    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    bool is_device_removed() const override {
        return device_removed;
    }

    void on_device_removed() override {
        device_removed = true;
    }

public:
    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override;

    int tweak_clear_halt_cmd(const SetupPacket &setup_packet);
    int tweak_set_interface_cmd(const SetupPacket &setup_packet);
    int tweak_set_configuration_cmd(const SetupPacket &setup_packet);
    int tweak_reset_device_cmd(const SetupPacket &setup_packet);

    /**
     * @brief Handle special control requests
     * @param setup_packet
     * @return -1: no tweak needed, should submit transfer
     *          0: tweak succeeded, no need to submit transfer
     *         >0: tweak failed (libusb error code), no need to submit transfer
     */
    int tweak_special_requests(const SetupPacket &setup_packet);

    static uint8_t get_libusb_transfer_flags(uint32_t in);

    static void masking_bogus_flags(bool is_out, struct libusb_transfer *trx);

    static int trxstat2error(enum libusb_transfer_status trxstat);
    static enum libusb_transfer_status error2trxstat(int e);

    struct libusb_callback_args {
        LibusbDeviceHandler *handler = nullptr;
        std::uint32_t seqnum; // seqnum of CMD_SUBMIT
        bool is_out;
        TransferHandle transfer; // Owns the libusb_transfer*
        bool unlinking = false; // unlink is in progress
        std::uint32_t unlink_cmd_seqnum = 0; // seqnum of the corresponding CMD_UNLINK

        void reset() {
            handler = nullptr;
            seqnum = 0;
            is_out = false;
            transfer.reset();
            unlinking = false;
            unlink_cmd_seqnum = 0;
        }
    };

    struct CallbackArgsReset {
        static void reset(libusb_callback_args &args) {
            args.reset();
        }
    };

    static void LIBUSB_CALL transfer_callback(libusb_transfer *trx);

    // Object pool: 256 slots; reset() is called automatically on alloc to clear stale data
    using CallbackArgsPool =
            ObjectPool<libusb_callback_args, 256, true, detail::DefaultLM<libusb_callback_args>, CallbackArgsReset>;
    CallbackArgsPool callback_args_pool_;

    // Used to wait for all transfers to complete
    std::mutex transfer_complete_mutex_;
    std::condition_variable transfer_complete_cv_;

    // Once this flag is true, communication should stop immediately; all variables tracking communication state become invalid
    std::atomic_bool client_disconnection = false;
    std::atomic_bool device_removed = false;

    // Ongoing transfers: seqnum → callback_args*
    std::shared_mutex transfers_mutex_;
    std::unordered_map<std::uint32_t, libusb_callback_args *> transfers_;
    std::atomic<std::size_t> pending_count_{0};

    // Device handle
    // - Normal mode: assigned during on_new_connection
    // - Android mode: created via libusb_wrap_sys_device during on_new_connection
    libusb_device_handle *native_handle = nullptr;

    // Device reference (used only in normal mode)
    // Distinguish normal mode from Android mode by checking native_device_ != nullptr
    libusb_device *native_device_ = nullptr;

    // Android mode: system device file descriptor
    intptr_t wrapped_fd_ = -1;

    bool interfaces_claimed_ = false; // Whether interfaces have been claimed
    int claimed_interface_count_ = 0; // Number of interfaces claimed; stored so release_and_close_device
                                      // can iterate correctly even if the device is already removed and
                                      // libusb_get_active_config_descriptor() would fail.

    /**
     * @brief Open device and claim interfaces (normal mode).
     * Called on client connection.
     * @return true on success, false on failure.
     */
    bool open_and_claim_device();

    /**
     * @brief Wrap fd and claim interfaces (Android mode).
     * Called on client connection.
     * @return true on success, false on failure.
     */
    bool wrap_fd_and_claim_interfaces();

    /**
     * @brief Release interfaces and close device.
     * Called on client disconnection.
     */
    void release_and_close_device();

    // Must not have a timeout, because a timeout means the device data is not ready (not an error),
    // and a rep_submit would still be submitted on timeout, but since the device has not responded, a submission should not occur
    static constexpr int timeout_milliseconds = 0;
};

} // namespace usbipdcpp
