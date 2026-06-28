#include "LibusbHandler/LibusbDeviceHandler.h"


#include "Endpoint.h"
#include "LibusbHandler/LibusbTransferOperator.h"
#include "Session.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"

using namespace usbipdcpp;

// Normal mode constructor
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device) :
    AbstDeviceHandler(handle_device, std::make_unique<LibusbTransferOperator>()), native_device_(native_device) {
    // display_name is populated by bind_host_device() before taking the devices_mutex,
    // so no blocking I/O happens here inside the constructor.
}

// Android mode constructor
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd) :
    AbstDeviceHandler(handle_device, std::make_unique<LibusbTransferOperator>()), wrapped_fd_(fd) {
    // fd will be wrapped via libusb_wrap_sys_device in on_new_connection
}

usbipdcpp::LibusbDeviceHandler::~LibusbDeviceHandler() {
    if (native_device_) {
        libusb_unref_device(native_device_);
        native_device_ = nullptr;
    }
}

void usbipdcpp::LibusbDeviceHandler::on_new_connection(Session &current_session, error_code &ec) {
    AbstDeviceHandler::on_new_connection(current_session, ec);

    if (native_device_) {
        // Normal mode: need to open device
        if (!open_and_claim_device()) {
            SPDLOG_ERROR("Failed to open device");
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }
    else if (wrapped_fd_ >= 0) {
        // Android mode: wrap fd and claim interfaces
        if (!wrap_fd_and_claim_interfaces()) {
            SPDLOG_ERROR("Failed to wrap fd");
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }

    // Mark client as connected
    client_disconnection = false;
}

void usbipdcpp::LibusbDeviceHandler::on_disconnection(error_code &ec) {
    client_disconnection = true;
    // Do not check device_removed; libusb correctly triggers callbacks on device removal (LIBUSB_TRANSFER_NO_DEVICE)

    // Cancel all pending transfers
    {
        std::shared_lock lock(transfers_mutex_);
        for (auto &[seqnum, cb]: transfers_) {
            auto err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err) {
                SPDLOG_ERROR("libusb_cancel_transfer failed on seqnum {}: {}", cb->seqnum, libusb_strerror(err));
            }
        }
    }

    // Wait for all transfers to complete
    {
        std::unique_lock lock(transfer_complete_mutex_);
        transfer_complete_cv_.wait(lock, [this]() { return pending_count_.load(std::memory_order_acquire) == 0; });
    }

    // Prepare for next connection: reset the object pool state
    callback_args_pool_.reset();

    // Release interfaces and close device on disconnection
    if (interfaces_claimed_) {
        release_and_close_device();
    }

    AbstDeviceHandler::on_disconnection(ec);
}

void usbipdcpp::LibusbDeviceHandler::receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep,
                                                 std::optional<UsbInterface> interface, usbipdcpp::error_code &ec) {

    if (device_removed) [[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    auto seqnum = cmd.header.seqnum;
    auto transfer_flags = cmd.transfer_flags;
    auto transfer_buffer_length = cmd.transfer_buffer_length;
    const auto &setup_packet = cmd.setup;

    // Dispatch based on endpoint type
    if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control)) [[unlikely]] {
        // Control transfer
        auto tweak_ret = tweak_special_requests(setup_packet);
        if (tweak_ret < 0) [[likely]] {
            // No tweak needed; submit transfer
            SPDLOG_DEBUG("Control transfer {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            auto *trx = static_cast<libusb_transfer *>(cmd.transfer.get());

            // Fill setup packet into the start of the buffer
            libusb_fill_control_setup(trx->buffer, setup_packet.request_type, setup_packet.request, setup_packet.value,
                                      setup_packet.index, setup_packet.length);

            auto *callback_args = callback_args_pool_.alloc();
            if (!callback_args) [[unlikely]] {
                callback_args = new libusb_callback_args{};
            }
            callback_args->handler = this;
            callback_args->seqnum = seqnum;
            callback_args->is_out = setup_packet.is_out();
            callback_args->transfer = std::move(cmd.transfer); // Transfer ownership

            libusb_fill_control_transfer(trx, native_handle, trx->buffer, LibusbDeviceHandler::transfer_callback,
                                         callback_args, timeout_milliseconds);
            trx->flags = get_libusb_transfer_flags(transfer_flags);
            masking_bogus_flags(setup_packet.is_out(), trx);

            {
                std::lock_guard lock(transfers_mutex_);
                transfers_.emplace(seqnum, callback_args);
                pending_count_.fetch_add(1, std::memory_order_release);
            }

            LATENCY_TRACK(session->latency_tracker, seqnum, "LibusbDeviceHandler::receive_urb libusb_submit_transfer");
            auto err = libusb_submit_transfer(trx);

            if (err < 0) [[unlikely]] {
                SPDLOG_ERROR("Control transfer to device failed: {}", libusb_strerror(err));
                {
                    std::lock_guard lock(transfers_mutex_);
                    if (transfers_.erase(seqnum)) {
                        pending_count_.fetch_sub(1, std::memory_order_release);
                    }
                }
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
                    device_removed = true;
                    ec = make_error_code(ErrorType::NO_DEVICE);
                }
            }
        }
        else {
            // Whether tweak succeeded or failed, do not submit transfer
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, transfer_buffer_length));
        }
    }
    else if (interface.has_value()) [[likely]] {
        bool is_out = !ep.is_in();

        auto *trx = static_cast<libusb_transfer *>(cmd.transfer.get());

        auto *callback_args = callback_args_pool_.alloc();
        if (!callback_args) [[unlikely]] {
            callback_args = new libusb_callback_args{};
        }
        callback_args->handler = this;
        callback_args->seqnum = seqnum;
        callback_args->is_out = is_out;
        callback_args->transfer = std::move(cmd.transfer); // Transfer ownership

        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) [[likely]] {
            LATENCY_TRACK(session->latency_tracker, seqnum, "LibusbDeviceHandler::receive_urb bulk");

            libusb_fill_bulk_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                      LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_DEBUG("Interrupt transfer {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            libusb_fill_interrupt_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                           LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            int num_iso_packets = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                                          ? static_cast<int>(cmd.number_of_packets)
                                          : 0;
            SPDLOG_DEBUG("Isochronous transfer {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            libusb_fill_iso_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                     num_iso_packets, LibusbDeviceHandler::transfer_callback, callback_args,
                                     timeout_milliseconds);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("Unknown transfer type for endpoint {:02x}: {}", ep.address, ep.attributes);
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        trx->flags = get_libusb_transfer_flags(transfer_flags);
        masking_bogus_flags(is_out, trx);

        {
            std::lock_guard lock(transfers_mutex_);
            transfers_.emplace(seqnum, callback_args);
            pending_count_.fetch_add(1, std::memory_order_release);
        }

        auto err = libusb_submit_transfer(trx);
        if (err < 0) [[unlikely]] {
            SPDLOG_ERROR("Transfer failed: {}", libusb_strerror(err));
            {
                std::lock_guard lock(transfers_mutex_);
                if (transfers_.erase(seqnum)) {
                    pending_count_.fetch_sub(1, std::memory_order_release);
                }
            }
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
                device_removed = true;
                ec = make_error_code(ErrorType::NO_DEVICE);
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
    }
    else [[unlikely]] {
        SPDLOG_ERROR("Non-control transfer but no target interface exists");
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    if (device_removed) [[unlikely]]
        return;

    {
        std::shared_lock lock(transfers_mutex_);
        auto it = transfers_.find(unlink_seqnum);
        if (it != transfers_.end()) {
            auto *cb = it->second;
            // Transfer still in tracker; mark unlinking and cancel under lock.
            // When the callback runs it will see unlinking==true, take the RET_UNLINK branch, and wake the sender.
            cb->unlinking = true;
            cb->unlink_cmd_seqnum = cmd_seqnum;

            lock.unlock();

            int err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err == LIBUSB_ERROR_NOT_FOUND) [[unlikely]] {
                // Transfer completed at the libusb layer but callback not yet executed. unlinking is already set;
                // the callback will take the unlink branch, enqueue RET_UNLINK (with actual status), and wake the sender.
            }
            else if (err) [[unlikely]] {
                SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
            }
        }
        else {
            SPDLOG_DEBUG("handle_unlink: transfer NOT found, enqueue RET_UNLINK({})", cmd_seqnum);
            // Transfer already removed from the map by transfer_callback, meaning the callback has already
            // decided to send RET_SUBMIT or RET_UNLINK and enqueued it. The host is about to receive (or
            // has already received) the completion notification for this transfer, so this CMD_UNLINK is
            // irrelevant — the transfer has already completed and the unlink arrived too late.
            // Therefore RET_UNLINK(0) does not need to be sent immediately; just enqueue it without waking
            // the sender. It will be sent along with the next callback's wakeup_sender(), or during session cleanup.
            session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(cmd_seqnum, 0));
            lock.unlock();
        }
    }
}

int usbipdcpp::LibusbDeviceHandler::tweak_clear_halt_cmd(const SetupPacket &setup_packet) {
    auto target_endp = setup_packet.index;
    SPDLOG_DEBUG("tweak_clear_halt_cmd");

    auto err = libusb_clear_halt(native_handle, target_endp);
    if (err) [[unlikely]] {
        SPDLOG_ERROR("libusb_clear_halt() error: endp {} returned {}", target_endp, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG("libusb_clear_halt() done: endp {}", target_endp);
    }
    return err; // Returns 0 on success, positive value on error
}

int usbipdcpp::LibusbDeviceHandler::tweak_set_interface_cmd(const SetupPacket &setup_packet) {
    uint16_t alternate = setup_packet.value;
    uint16_t interface = setup_packet.index;

    SPDLOG_INFO("set_interface: inf {} alt {}", interface, alternate);
    int err = libusb_set_interface_alt_setting(native_handle, interface, alternate);
    if (err) [[unlikely]] {
        SPDLOG_ERROR("{}: usb_set_interface error: inf {} alt {} err {}",
                     get_device_busid(libusb_get_device(native_handle)), interface, alternate, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG("{}: usb_set_interface done: inf {} alt {}", get_device_busid(libusb_get_device(native_handle)),
                     interface, alternate);

        // Switch current_altsetting (endpoint data already pre-filled at bind time)
        if (interface < handle_device.interfaces.size()) {
            auto &dev_intf = handle_device.interfaces[interface];
            if (alternate < dev_intf.endpoints.size()) {
                dev_intf.current_altsetting = static_cast<std::uint8_t>(alternate);
                SPDLOG_DEBUG("Switched interface {} to alt {}, endpoint count: {}", interface, alternate,
                             dev_intf.current_endpoints().size());
            }
        }
    }
    return err; // Returns 0 on success, positive value on error
}

int usbipdcpp::LibusbDeviceHandler::tweak_set_configuration_cmd(const SetupPacket &setup_packet) {
    SPDLOG_INFO("tweak_set_configuration_cmd");

    // uint16_t config = libusb_le16_to_cpu(setup_packet.value);

    // auto err = libusb_set_configuration(native_handle, config);
    // if (err) {
    //     SPDLOG_ERROR(
    //             "{}: libusb_set_configuration error: config {} ret {}",
    //             get_device_busid(libusb_get_device(native_handle)), config, libusb_strerror(err));
    // }
    // else {
    //     SPDLOG_DEBUG(
    //             "{}: libusb_set_configuration done: config {}",
    //             get_device_busid(libusb_get_device(native_handle)), config);
    // }
    // return err;

    // Cannot call set_configuration; it would result in device_busy
    //  usbipd-libusb returns -1, indicating this command is not handled; the transfer is submitted normally
    //  The device will receive the set_configuration command
    return -1;
}

int usbipdcpp::LibusbDeviceHandler::tweak_reset_device_cmd(const SetupPacket &setup_packet) {
    SPDLOG_INFO("{}: usb_queue_reset_device", get_device_busid(libusb_get_device(native_handle)));

    // Following usbipd-libusb: do not call libusb_reset_device
    // A reset may cause the device to re-enumerate, breaking the connection
    // libusb_reset_device(native_handle);
    return 0;
}

int usbipdcpp::LibusbDeviceHandler::tweak_special_requests(const SetupPacket &setup_packet) {
    // Return values:
    // -1: no tweak needed; transfer should be submitted
    //  0: tweak succeeded; no transfer submission needed
    // >0: tweak failed (libusb error code); no transfer submission needed
    // Special requests are rare; in most cases returns -1 (no tweak needed)
    if (setup_packet.is_clear_halt_cmd()) [[unlikely]] {
        return tweak_clear_halt_cmd(setup_packet);
    }
    else if (setup_packet.is_set_interface_cmd()) [[unlikely]] {
        return tweak_set_interface_cmd(setup_packet);
    }
    else if (setup_packet.is_set_configuration_cmd()) [[unlikely]] {
        return tweak_set_configuration_cmd(setup_packet);
    }
    else if (setup_packet.is_reset_device_cmd()) [[unlikely]] {
        return tweak_reset_device_cmd(setup_packet);
    }
    SPDLOG_DEBUG("No packet adjustment needed");
    return -1; // No tweak needed
}

uint8_t usbipdcpp::LibusbDeviceHandler::get_libusb_transfer_flags(uint32_t in) {
    uint8_t flags = 0;

    if (in & static_cast<std::uint32_t>(TransferFlag::URB_SHORT_NOT_OK))
        flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    if (in & static_cast<std::uint32_t>(TransferFlag::URB_ZERO_PACKET))
        flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;

    return flags;
}

void usbipdcpp::LibusbDeviceHandler::masking_bogus_flags(bool is_out, struct libusb_transfer *trx) {
    std::uint32_t allowed = 0;
    /* enforce simple/standard policy */
    switch (trx->type) {
        case LIBUSB_TRANSFER_TYPE_BULK:
            if (is_out)
                allowed |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        /* FALLTHROUGH */
        case LIBUSB_TRANSFER_TYPE_CONTROL:
            /*allowed |= URB_NO_FSBR; */ /* only affects UHCI */
            /* FALLTHROUGH */
        default: /* all non-iso endpoints */
            if (!is_out)
                allowed |= LIBUSB_TRANSFER_SHORT_NOT_OK;
            break;
    }
    trx->flags &= allowed;
}

int usbipdcpp::LibusbDeviceHandler::trxstat2error(enum libusb_transfer_status trxstat) {
    // Specific values copied from Linux
    switch (trxstat) {
        case LIBUSB_TRANSFER_COMPLETED:
            return static_cast<int>(UrbStatusType::StatusOK);
        case LIBUSB_TRANSFER_CANCELLED:
            return static_cast<int>(UrbStatusType::StatusECONNRESET);
        case LIBUSB_TRANSFER_ERROR:
        case LIBUSB_TRANSFER_STALL:
        case LIBUSB_TRANSFER_TIMED_OUT:
        case LIBUSB_TRANSFER_OVERFLOW:
            return static_cast<int>(UrbStatusType::StatusEPIPE);
        case LIBUSB_TRANSFER_NO_DEVICE:
            return static_cast<int>(UrbStatusType::StatusESHUTDOWN);
        default:
            return static_cast<int>(UrbStatusType::StatusENOENT);
    }
}


enum libusb_transfer_status usbipdcpp::LibusbDeviceHandler::error2trxstat(int e) {
    switch (e) {
        case static_cast<int>(UrbStatusType::StatusOK):
            return LIBUSB_TRANSFER_COMPLETED;
        case static_cast<int>(UrbStatusType::StatusENOENT):
            return LIBUSB_TRANSFER_ERROR;
        case static_cast<int>(UrbStatusType::StatusECONNRESET):
            return LIBUSB_TRANSFER_CANCELLED;
        case static_cast<int>(UrbStatusType::StatusETIMEDOUT):
            return LIBUSB_TRANSFER_TIMED_OUT;
        case static_cast<int>(UrbStatusType::StatusEPIPE):
            return LIBUSB_TRANSFER_STALL;
        case static_cast<int>(UrbStatusType::StatusESHUTDOWN):
            return LIBUSB_TRANSFER_NO_DEVICE;
        case static_cast<int>(UrbStatusType::StatusEEOVERFLOW): // EOVERFLOW
            return LIBUSB_TRANSFER_OVERFLOW;
        default:
            return LIBUSB_TRANSFER_ERROR;
    }
}

void LIBUSB_CALL usbipdcpp::LibusbDeviceHandler::transfer_callback(libusb_transfer *trx) {
    auto &callback_arg = *static_cast<libusb_callback_args *>(trx->user_data);

    // SPDLOG_WARN("callback: seqnum={} type={} num_iso={} actual_length={} is_out={}", callback_arg.seqnum,
    //             static_cast<int>(trx->type), trx->num_iso_packets, trx->actual_length, callback_arg.is_out);

    LATENCY_TRACK(callback_arg.handler->session->latency_tracker, callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback invoked");

    // If disconnected, clean up and return immediately (do not send a response)
    // This is checked after transfer completion; disconnection is an exceptional case
    if (callback_arg.handler->client_disconnection) [[unlikely]] {
        auto *handler = callback_arg.handler;
        handler->transfers_mutex_.lock();
        handler->transfers_.erase(callback_arg.seqnum);
        handler->pending_count_.fetch_sub(1, std::memory_order_release);
        handler->transfers_mutex_.unlock();
        callback_arg.transfer.reset(); // Release libusb_transfer to avoid deferring until next alloc
        if (!handler->callback_args_pool_.free(&callback_arg)) {
            delete &callback_arg;
        }
        handler->transfer_complete_cv_.notify_one();
        return;
    }

    // Status check
    switch (trx->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            /* OK */
            break;
        case LIBUSB_TRANSFER_ERROR:
            if (!(trx->flags & LIBUSB_TRANSFER_SHORT_NOT_OK)) {
                dev_err(libusb_get_device(trx->dev_handle), "error on endpoint {}", trx->endpoint);
            }
            else {
                // Tweaking status to complete as we received data, but all
                trx->status = LIBUSB_TRANSFER_COMPLETED;
            }
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            dev_dbg(libusb_get_device(trx->dev_handle), "unlinked by a call to usb_unlink_urb()");
            break;
        case LIBUSB_TRANSFER_STALL:
            dev_err(libusb_get_device(trx->dev_handle), "endpoint {} is stalled", trx->endpoint);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            dev_info(libusb_get_device(trx->dev_handle), "device removed?");
            callback_arg.handler->device_removed = true;
            break;
        default:
            dev_warn(libusb_get_device(trx->dev_handle), "urb completion with unknown status {}",
                     static_cast<int>(trx->status));
            break;
    }
    SPDLOG_DEBUG("libusb transferred {} bytes", trx->actual_length);

    // Calculate actual length for ISO transfers
    std::uint32_t actual_length = trx->actual_length;
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && !callback_arg.is_out) {
        // ISO IN transfer: sum the actual_length of all iso packets
        size_t iso_actual_length = 0;
        for (int i = 0; i < trx->num_iso_packets; i++) {
            iso_actual_length += trx->iso_packet_desc[i].actual_length;
        }
        actual_length = static_cast<std::uint32_t>(iso_actual_length);
    }

    // Under lock: check unlinking, enqueue response, remove from tracker — three operations completed atomically.
    // If handle_unlink_seqnum runs before this, it will find the entry in the map and set unlinking;
    // if it runs after this, it won't find it and will enqueue RET_UNLINK(0) itself.
    bool unlinking = false;
    std::uint32_t unlink_cmd_seqnum = 0;

    {
        auto *handler = callback_arg.handler;
        std::unique_lock lock(handler->transfers_mutex_);
        unlinking = callback_arg.unlinking;
        unlink_cmd_seqnum = callback_arg.unlink_cmd_seqnum;
        handler->transfers_.erase(callback_arg.seqnum);
        handler->pending_count_.fetch_sub(1, std::memory_order_release);

        if (unlinking) [[unlikely]] {
            // URB was cancelled by unlink; enqueue RET_UNLINK (with actual transfer status)
            callback_arg.handler->session->enqueue_ret_unlink(
                    UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(unlink_cmd_seqnum, trxstat2error(trx->status)));
            // Unlink case: release transfer
            callback_arg.transfer.reset();
        }
        else {
            // Send ret_submit
            // OUT transfer: no data needs to be sent back to the client, only the header (no transfer_handle)
            // IN transfer: send header + data (with transfer_handle)
            UsbIpResponse::UsbIpRetSubmit ret;
            if (callback_arg.is_out) {
                // OUT transfer: no data phase
                ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                        callback_arg.seqnum, trxstat2error(trx->status), actual_length);
                // OUT transfer: release transfer
                callback_arg.transfer.reset();
            }
            else {
                // IN transfer: has data; transfer ownership
                ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(callback_arg.seqnum, trxstat2error(trx->status),
                                                                       actual_length,
                                                                       0, // start_frame
                                                                       trx->num_iso_packets,
                                                                       std::move(callback_arg.transfer) // Transfer ownership
                );
            }
            callback_arg.handler->session->enqueue_ret_submit(std::move(ret));
        }
    }

    SPDLOG_DEBUG("libusb transfer actual_length is {} bytes", actual_length);

    LATENCY_TRACK(callback_arg.handler->session->latency_tracker, callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback submit_ret_submit");

    // Enqueue complete; wake sender thread to transmit
    callback_arg.handler->session->wakeup_sender();

    // Release libusb_transfer then return callback_arg to pool (value fields zeroed by Reset at alloc time)
    auto *handler = callback_arg.handler;
    callback_arg.transfer.reset();
    if (!handler->callback_args_pool_.free(&callback_arg)) {
        delete &callback_arg;
    }
    // Unconditional notify covers the race between the normal path and on_disconnection.
    // The CV predicate pending_count_==0 ensures only the last callback actually wakes the wait.
    handler->transfer_complete_cv_.notify_one();
}

bool usbipdcpp::LibusbDeviceHandler::open_and_claim_device() {
    // This function is for normal mode only
    if (!native_device_) {
        SPDLOG_ERROR("native_device_ is null; cannot open device");
        return false;
    }

    int err = libusb_open(native_device_, &native_handle);
    if (err) {
        SPDLOG_ERROR("Cannot open device: {}", libusb_strerror(err));
        return false;
    }

    // Get configuration descriptor
    struct libusb_config_descriptor *active_config_desc = nullptr;
    err = libusb_get_active_config_descriptor(native_device_, &active_config_desc);
    if (err) {
        SPDLOG_ERROR("Cannot get configuration descriptor: {}", libusb_strerror(err));
        libusb_close(native_handle);
        native_handle = nullptr;
        return false;
    }

    int num_interfaces = active_config_desc->bNumInterfaces;
    SPDLOG_DEBUG("Device has {} interfaces", num_interfaces);

    // Detach kernel drivers and claim all interfaces
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        err = libusb_detach_kernel_driver(native_handle, intf_i);
        if (err && err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("Cannot detach kernel driver for interface {}: {}", intf_i, libusb_strerror(err));
        }

        err = libusb_claim_interface(native_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("Cannot claim interface {}: {}", intf_i, libusb_strerror(err));
            // Roll back already-claimed interfaces
            for (int j = 0; j < intf_i; j++) {
                libusb_release_interface(native_handle, j);
                libusb_attach_kernel_driver(native_handle, j);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    // Ensure all interfaces are on alt 0, consistent with current_altsetting
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        libusb_set_interface_alt_setting(native_handle, intf_i, 0);
        if (intf_i < static_cast<int>(handle_device.interfaces.size()))
            handle_device.interfaces[intf_i].current_altsetting = 0;
    }

    libusb_free_config_descriptor(active_config_desc);
    interfaces_claimed_ = true;
    SPDLOG_INFO("Successfully opened device and claimed {} interfaces", num_interfaces);
    return true;
}

bool usbipdcpp::LibusbDeviceHandler::wrap_fd_and_claim_interfaces() {
    // This function is for Android mode only
    if (wrapped_fd_ < 0) {
        SPDLOG_ERROR("wrapped_fd_ is invalid");
        return false;
    }

    int err = libusb_wrap_sys_device(nullptr, wrapped_fd_, &native_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device failed: {}", libusb_strerror(err));
        return false;
    }

    // Note: the device obtained by wrap is destroyed after libusb_close
    // Therefore it must be re-wrapped on every connection
    libusb_device *wrapped_device = libusb_get_device(native_handle);

    // Get configuration descriptor
    struct libusb_config_descriptor *active_config_desc = nullptr;
    err = libusb_get_active_config_descriptor(wrapped_device, &active_config_desc);
    if (err) {
        SPDLOG_ERROR("Cannot get configuration descriptor: {}", libusb_strerror(err));
        libusb_close(native_handle);
        native_handle = nullptr;
        return false;
    }

    int num_interfaces = active_config_desc->bNumInterfaces;
    SPDLOG_DEBUG("Device has {} interfaces", num_interfaces);

    // Attempt to detach kernel driver before claiming interfaces
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        int detach_err = libusb_detach_kernel_driver(native_handle, intf_i);
        if (detach_err && detach_err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("Android mode: failed to detach kernel driver for interface {}: {}", intf_i, libusb_strerror(detach_err));
        }
        err = libusb_claim_interface(native_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("Cannot claim interface {}: {}", intf_i, libusb_strerror(err));
            // Roll back already-claimed interfaces
            for (int j = 0; j < intf_i; j++) {
                libusb_release_interface(native_handle, j);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    // Ensure all interfaces are on alt 0, consistent with current_altsetting
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        libusb_set_interface_alt_setting(native_handle, intf_i, 0);
        if (intf_i < static_cast<int>(handle_device.interfaces.size()))
            handle_device.interfaces[intf_i].current_altsetting = 0;
    }

    libusb_free_config_descriptor(active_config_desc);
    interfaces_claimed_ = true;
    SPDLOG_INFO("Successfully wrapped fd and claimed {} interfaces", num_interfaces);
    return true;
}

void usbipdcpp::LibusbDeviceHandler::release_and_close_device() {
    if (!native_handle) {
        return;
    }

    // Get device to query interface count
    libusb_device *device = libusb_get_device(native_handle);
    struct libusb_config_descriptor *active_config_desc = nullptr;
    int num_interfaces = 0;
    if (device && libusb_get_active_config_descriptor(device, &active_config_desc) == 0) {
        num_interfaces = active_config_desc->bNumInterfaces;
        libusb_free_config_descriptor(active_config_desc);
    }

    // Release all interfaces
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        int err = libusb_release_interface(native_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("Error releasing interface {}: {}", intf_i, libusb_strerror(err));
        }

        // Re-attach kernel driver
        err = libusb_attach_kernel_driver(native_handle, intf_i);
        if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED) {
            SPDLOG_WARN("Failed to re-attach kernel driver (interface {}): {}", intf_i, libusb_strerror(err));
        }
    }

    interfaces_claimed_ = false;

    // Close handle
    // Both normal mode and Android mode require calling libusb_close
    libusb_close(native_handle);
    native_handle = nullptr;

    SPDLOG_INFO("Device interfaces released");
}
