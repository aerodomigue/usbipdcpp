#pragma once

#include <atomic>
#include <unordered_map>
#include <shared_mutex>

#include <chrono>
#include <thread>
#include <condition_variable>
#include <deque>

#include <asio/ip/tcp.hpp>

#include "Export.h"
#include "utils/LatencyTracker.h"
#include "protocol.h"
#include "type.h"

namespace usbipdcpp {
class Server;
class AbstDeviceHandler;

/**
 * @brief Manages its own lifetime; one Session is created per connection, and the server relinquishes control of the Session after creation.
 * Ensure that the Server is not destroyed while the Session is still alive; otherwise the behavior is undefined.
 */
class USBIPDCPP_API Session final : public std::enable_shared_from_this<Session> {
    friend class Server;

public:
    explicit Session(Server &server);
    Session(const Session &) = delete;
    Session(Session &&) = delete;

    /**
     * @brief This function is asynchronous and non-blocking. It submits tasks directly to the asio context internally, so no locking is needed. Internally thread-safe.
     * Ensure that a return packet is submitted for every URB.
     * @param unlink
     */
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);

    /**
     * @brief This function is asynchronous and non-blocking. It submits tasks directly to the asio context internally, so no locking is needed. Internally thread-safe.
     * Ensure that a return packet is submitted for every URB.
     * @param submit
     */
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief Enqueue into write_buffer only, without waking the sender.
     * Use this when multiple responses need to be enqueued consecutively before a single wakeup.
     */
    void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);
    void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief Wake up the sender thread without enqueuing any data.
     * Use together with enqueue_ret_*: enqueue consecutively first, then call wakeup_sender once at the end.
     */
    void wakeup_sender();

    /**
     * @brief Set the stop flag and close the socket. May only be called by Server and AbstDeviceHandler::trigger_session_stop.
     * Does not close the thread internally; only notifies the thread to shut down.
     */
    void immediately_stop();

    ~Session();

    LATENCY_TRACKER_MEMBER(latency_tracker);

private:
    /**
     * @brief Called by Server when a new Session is created.
     */
    void run();

    // Double-buffered queue: producer writes to write_buffer, consumer reads from read_buffer.
    // A brief lock is held during the swap, greatly reducing lock contention.
    std::deque<UsbIpResponse::RetVariant> write_buffer;
    std::deque<UsbIpResponse::RetVariant> read_buffer;
    mutable std::mutex swap_mutex;
    std::condition_variable data_available_cv;
    std::atomic_bool has_data{false};

    void parse_op();

    /**
     * @brief Continuously transfer URBs.
     * @param transferring_ec Error code encountered during URB transfer.
     */
    void transfer_loop(usbipdcpp::error_code &transferring_ec);
    void receiver(usbipdcpp::error_code &receiver_ec);
    void sender(usbipdcpp::error_code &ec);
    std::optional<UsbIpResponse::RetVariant> sender_get_data(usbipdcpp::error_code &ec);

    std::atomic_bool should_immediately_stop = false;

    // Whether currently in the ret_submit transfer phase
    std::atomic_bool cmd_transferring = false;

    // Must not be null during transfer; no writes allowed during transfer. Must not be read from non-network threads without locking.
    std::optional<std::string> current_import_device_id = std::nullopt;
    // Must not be null during transfer; no writes allowed during transfer. Must not be read from non-network threads without locking.
    std::shared_ptr<UsbDevice> current_import_device = nullptr;
    // Holds the handler directly to avoid indirection through device
    std::shared_ptr<AbstDeviceHandler> current_handler = nullptr;
    // Lock for the above variables
    std::shared_mutex current_import_device_data_mutex;

    Server &server;
    asio::io_context session_io_context{};
    asio::ip::tcp::socket socket;


    // Automatically destructs this object when this thread finishes
    std::thread run_thread;
};
}
