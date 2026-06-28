#include "Session.h"

#include <asio.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <variant>

#include "DeviceHandler/DeviceHandler.h"

#include "utils/utils.h"
#include "Device.h"
#include "Server.h"
#include "network.h"
#include "protocol.h"

usbipdcpp::Session::Session(Server &server) : server(server), socket(session_io_context) {
}

void usbipdcpp::Session::enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(submit));
}

void usbipdcpp::Session::enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(unlink));
}

void usbipdcpp::Session::wakeup_sender() {
    has_data.store(true, std::memory_order_release);
    data_available_cv.notify_one();
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    enqueue_ret_unlink(std::move(unlink));
    wakeup_sender();
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    enqueue_ret_submit(std::move(submit));
    wakeup_sender();
}

usbipdcpp::Session::~Session() {
    SPDLOG_TRACE("Session destructor called");
}

void usbipdcpp::Session::run() {
    // Acquire self pointer first to prevent destruction by smart pointer
    auto self = shared_from_this();
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionMain);
    }
    run_thread = std::thread([self = std::move(self)]() {
        self->parse_op();

        // After processing ends, automatically remove self from server and trigger exit callback
        self->server.remove_session(self.get());
        // Detach the current thread to prevent errors from self-destruction inside the thread
        self->run_thread.detach();
    });
    if (server.after_thread_create_callback) {
        server.after_thread_create_callback(ThreadPurpose::SessionMain, run_thread);
    }
}

void usbipdcpp::Session::parse_op() {
    usbipdcpp::error_code ec;
    SPDLOG_TRACE("Attempting to read OP");
    auto op = UsbIpCommand::get_op_from_socket(socket, ec);
    if (ec) {
        SPDLOG_DEBUG("Error getting op from socket: {}", ec.message());
        if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
            SPDLOG_DEBUG("Connection closed");
        }
        else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
            SPDLOG_DEBUG("Socket error occurred");
        }

        goto close_socket;
    }
    std::visit(
            [&, this](auto &&cmd) {
                using T = std::remove_cvref_t<decltype(cmd)>;
                if constexpr (std::is_same_v<UsbIpCommand::OpReqDevlist, T>) {
                    SPDLOG_TRACE("Received OpReqDevlist packet");
                    data_type to_be_sent;
                    {
                        std::shared_lock lock(server.devices_mutex);
                        to_be_sent =
                                UsbIpResponse::OpRepDevlist::create_from_devices(server.available_devices).to_bytes();
                    }
                    asio::write(socket, asio::buffer(to_be_sent), ec);
                    if (!ec) [[likely]]
                        SPDLOG_TRACE("Successfully sent OpRepDevlist packet");
                    else
                        SPDLOG_TRACE("Error sending OpRepDevlist packet: {}", ec.message());
                }
                else if constexpr (std::is_same_v<UsbIpCommand::OpReqImport, T>) {
                    SPDLOG_TRACE("Received OpReqImport packet");
                    spdlog::info("Device attach request from {}", socket.remote_endpoint().address().to_string());
                    auto wanted_busid = std::string(reinterpret_cast<char *>(cmd.busid.data()));
                    UsbIpResponse::OpRepImport op_rep_import{};
                    SPDLOG_TRACE("Client wants to connect to device with busid {}", wanted_busid);

                    bool target_device_is_using = false;
                    bool open_device_failed = false;
                    // Devices already in use do not support export
                    if (server.is_device_using(wanted_busid)) {
                        spdlog::warn("Device currently in use cannot be exported");
                        // See kernel source tools/usbip/src/usbipd.c function recv_request_import
                        // The source shows that NA should be returned instead of DevBusy
                        op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                static_cast<std::uint32_t>(OperationStatuType::NA));
                        target_device_is_using = true;
                    }
                    else {
                        if (auto using_device = server.try_moving_device_to_using(wanted_busid)) {
                            std::lock_guard lock(current_import_device_data_mutex);
                            spdlog::info("Successfully moved device into the in-use device list");
                            current_import_device_id = wanted_busid;
                            // Point the current in-use device to this device
                            current_import_device = using_device;
                            current_handler = using_device->handler;
                            spdlog::info("Successfully cached the in-use device");

                            // Try to open the device before sending OpRepImport
                            usbipdcpp::error_code open_ec;
                            current_handler->on_new_connection(*this, open_ec);
                            if (open_ec) {
                                SPDLOG_ERROR("Failed to open device: {}", open_ec.message());
                                open_device_failed = true;
                                // Move device back to available list
                                server.try_moving_device_to_available(wanted_busid);
                                current_import_device.reset();
                                current_handler.reset();
                                current_import_device_id.reset();
                            }
                        }
                    }

                    if (!target_device_is_using && !open_device_failed) {
                        std::shared_lock lock(current_import_device_data_mutex);
                        if (current_import_device) {
                            spdlog::info("Target device found, can be imported");
                            op_rep_import = UsbIpResponse::OpRepImport::create_on_success(current_import_device);
                            cmd_transferring = true;
                        }
                        else {
                            spdlog::info("Target device does not exist, cannot be imported");
                            op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                    static_cast<std::uint32_t>(OperationStatuType::NoDev));
                        }
                    }
                    else if (open_device_failed) {
                        op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                static_cast<std::uint32_t>(OperationStatuType::NA));
                    }

                    auto to_be_sent = op_rep_import.to_bytes();
                    SPDLOG_TRACE("About to send {} to server, total {} bytes", get_every_byte(to_be_sent), to_be_sent.size());
                    [[maybe_unused]] auto size = asio::write(socket, asio::buffer(to_be_sent), ec);
                    if (!ec) [[likely]]
                        SPDLOG_TRACE("Successfully sent OpRepImport packet", size);
                    else
                        SPDLOG_TRACE("Error sending OpRepImport packet: {}", ec.message());

                    if (cmd_transferring) {
                        usbipdcpp::error_code transferring_ec;
                        // Enter communication state
                        transfer_loop(transferring_ec);
                        if (transferring_ec) {
                            SPDLOG_ERROR("Error occurred during transferring : {}", transferring_ec.message());
                            ec = transferring_ec;
                        }

                        // on_disconnection and device cleanup are already handled in receiver
                    }
                }
                else {
                    // Ensure all possible types are handled
                    static_assert(!std::is_same_v<T, T>);
                }
            },
            op);

close_socket:
    std::error_code ignore_ec;
    SPDLOG_DEBUG("Attempting to close socket");
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);
}

void usbipdcpp::Session::immediately_stop() {
    should_immediately_stop = true;

    std::error_code ignore_ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    SPDLOG_INFO("Shutdown called successfully");
}

void usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec) {
    // on_new_connection has already been called in parse_op, do not call it again here

    error_code receiver_ec;
    error_code sender_ec;
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionSender);
    }
    std::thread sender_thread([&, this]() { sender(sender_ec); });
    if (server.after_thread_create_callback) {
        server.after_thread_create_callback(ThreadPurpose::SessionSender, sender_thread);
    }

    receiver(receiver_ec);
    SPDLOG_INFO("Receiver exited");
    sender_thread.join();
    SPDLOG_INFO("Sender thread exited");

    // Clear the queue while handler is alive to ensure handler is still valid when TransferHandle is destroyed
    write_buffer.clear();
    read_buffer.clear();

    if (sender_ec) {
        SPDLOG_ERROR("An error occur during sending: {}", sender_ec.message());
        transferring_ec = sender_ec;
    }
    // Generally receiver_ec is more important, so it will override
    else if (receiver_ec) {
        SPDLOG_ERROR("An error occur during receiving: {}", receiver_ec.message());
        transferring_ec = receiver_ec;
    }
    cmd_transferring = false;
}

std::optional<usbipdcpp::UsbIpResponse::RetVariant> usbipdcpp::Session::sender_get_data(usbipdcpp::error_code &ec) {
    // If read_buffer is empty, try to swap
    if (read_buffer.empty()) [[likely]] {
        std::unique_lock lock(swap_mutex);
        data_available_cv.wait(
                lock, [this]() { return has_data.load(std::memory_order_acquire) || should_immediately_stop; });
        if (!write_buffer.empty()) {
            read_buffer.swap(write_buffer);
            has_data.store(false, std::memory_order_release);
        }
    }
    if (!read_buffer.empty()) [[likely]] {
        usbipdcpp::UsbIpResponse::RetVariant ret_v = std::move(read_buffer.front());
        read_buffer.pop_front();
        return ret_v;
    }
    else {
        return std::nullopt;
    }
}

void usbipdcpp::Session::receiver(usbipdcpp::error_code &receiver_ec) {
    // spdlog::info("should_immediately_stop: {}", should_immediately_stop.load());
    while (!should_immediately_stop) {
        usbipdcpp::error_code ec;

        auto command = UsbIpCommand::get_cmd_from_socket(socket, current_handler.get(), ec);
        if (ec) [[unlikely]] {
            if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
                SPDLOG_DEBUG("Connection closed");
            }
            else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
                SPDLOG_DEBUG("Socket error occurred");
            }
            else {
                SPDLOG_ERROR("Error getting command from socket: {}", ec.message());
            }
            break;
        }
        if (should_immediately_stop) [[unlikely]]
            break;
        std::visit(
                [&, this](auto &&cmd) {
                    using T = std::remove_cvref_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdSubmit, T>) {
                        UsbIpCommand::UsbIpCmdSubmit &cmd2 = cmd;
                        LATENCY_TRACK_START(latency_tracker, cmd2.header.seqnum);
                        SPDLOG_TRACE("Received UsbIpCmdSubmit packet, seqnum: {}", cmd2.header.seqnum);
                        auto out = cmd2.header.direction == UsbIpDirection::Out;
                        SPDLOG_TRACE("Usbip transfer direction: {}", out ? "out" : "in");
                        std::uint8_t real_ep = out ? static_cast<std::uint8_t>(cmd2.header.ep)
                                                   : (static_cast<std::uint8_t>(cmd2.header.ep) | 0x80);
                        SPDLOG_TRACE("Real transfer endpoint: {:02x}", real_ep);
                        [[maybe_unused]] auto current_seqnum = cmd2.header.seqnum;

                        auto ep_find_ret = current_import_device->find_ep(real_ep);
                        if (ep_find_ret.has_value()) [[likely]] {
                            auto &ep = ep_find_ret->first;
                            auto &intf = ep_find_ret->second;

                            SPDLOG_TRACE("->endpoint {0:02x}", ep.address);
                            SPDLOG_TRACE("->setup data {}", get_every_byte(cmd2.setup.to_bytes()));


                            usbipdcpp::error_code ec_during_handling_urb;
                            // start_processing_urb();
                            LATENCY_TRACK(latency_tracker, cmd2.header.seqnum, "Preparing to call device receive_urb");
                            current_handler->receive_urb(std::move(cmd2), ep, std::move(intf), ec_during_handling_urb);

                            if (ec_during_handling_urb) [[unlikely]] {
                                SPDLOG_ERROR("Error during handling urb : {}", ec_during_handling_urb.message());
                                // An error means communication can no longer continue
                                receiver_ec = ec_during_handling_urb;
                                should_immediately_stop = true;
                                return;
                            }
                        }
                        else {
                            SPDLOG_WARN("Cannot find endpoint {}", real_ep);
                            UsbIpResponse::UsbIpRetSubmit ret_submit =
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                                            cmd2.header.seqnum, 0);
                            ret_submit.to_socket(socket, ec);
                            SPDLOG_TRACE("Successfully sent UsbIpRetSubmit packet");
                        }
                    }
                    else if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdUnlink, T>) {
                        UsbIpCommand::UsbIpCmdUnlink &cmd2 = cmd;
                        SPDLOG_TRACE("Received UsbIpCmdUnlink packet, seqnum: {}", cmd2.header.seqnum);

                        current_handler->handle_unlink_seqnum(cmd2.unlink_seqnum, cmd2.header.seqnum);
                    }
                    else if constexpr (std::is_same_v<std::monostate, T>) {
                        SPDLOG_ERROR("Received unknown packet");
                        receiver_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                    }
                    else {
                        // Ensure all possible types are handled
                        static_assert(!std::is_same_v<T, T>);
                    }
                    return;
                },
                command);
    }
    // Notify the device of disconnection, telling it not to send any more messages
    current_handler->on_disconnection(receiver_ec);
    // Then shut down the sender thread, to avoid the device reporting errors because it was closed before being notified
    should_immediately_stop = true;
    data_available_cv.notify_one();

    /* Marking as available first here is valid because:
     * 1. The device's on_disconnection must block and complete all disconnection tasks
     * 2. This session is about to be destroyed so the two current_import_device variables won't be used again
     * Therefore it's safe to mark as available before clearing the state of these two variables
     */
    if (current_handler->is_device_removed()) {
        // Device has been physically removed, erase it directly from using_devices
        SPDLOG_INFO("Device physically removed, will not be moved back to available list");
        std::lock_guard lock(server.get_devices_mutex());
        server.get_using_devices().erase(*current_import_device_id);
    }
    else {
        server.try_moving_device_to_available(*current_import_device_id);
    }
    current_import_device_id.reset();
    current_import_device.reset();
    SPDLOG_TRACE("Clearing the busid of the current imported device");
}

void usbipdcpp::Session::sender(usbipdcpp::error_code &ec) {
    // RET_SUBMIT and RET_UNLINK share a single write_buffer queue; the enqueue order is the send order,
    // so plain FIFO sending works. Unlike the kernel/usbipd-libusb which splits into priv_tx and unlink_tx
    // two separate queues with no way to distinguish ordering and must manually send SUBMIT before UNLINK.
    //
    // Taking the libusb backend as an example: transfer_callback atomically completes
    // "remove from map -> enqueue RET_SUBMIT" while holding transfers_mutex_.
    // handle_unlink_seqnum must wait for the lock to be released before it can intervene.
    // By the time it acquires the lock the transfer is already gone from the map, so the
    // enqueued RET_UNLINK naturally falls after RET_SUBMIT — order is correct.
    while (!should_immediately_stop) {
        auto send_data_opt = sender_get_data(ec);
        if (ec || should_immediately_stop) [[unlikely]] {
            break;
        }
        if (!send_data_opt.has_value()) [[unlikely]] {
            // Spurious wakeup (after separating enqueue and notify, has_data is true but data was already consumed in a previous round).
            // Only exit when should_immediately_stop is set, otherwise go back and keep waiting.
            if (should_immediately_stop)
                break;
            continue;
        }
        auto send_data = std::move(send_data_opt.value());

        SPDLOG_TRACE("Channel received message");
        error_code sending_ec;
        std::visit(
                [&](auto &&cmd) {
                    using T = std::remove_cvref_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetSubmit, T>) {
                        cmd.to_socket(socket, sending_ec);
                        LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket call completed");
                    }
                    else if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetUnlink, T>) {
                        cmd.to_socket(socket, sending_ec);
                        LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket call completed");
                    }
                    else if constexpr (std::is_same_v<std::monostate, T>) {
                        SPDLOG_ERROR("Received unknown packet");
                        sending_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                    }
                    else {
                        static_assert(!std::is_same_v<T, T>);
                    }
                },
                send_data);

        if (sending_ec) {
            // Errors during sending are not propagated
            // ec = sending_ec;
            break;
        }
    }
}
