#pragma once

#include <vector>
#include <map>
#include <shared_mutex>
#include <memory>
#include <list>
#include <thread>
#include <condition_variable>
#include <cstddef>
#include <functional>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>

#include "Device.h"


namespace usbipdcpp {
class Session;

/**
 * @brief Thread purpose identifier, used in the pre-thread-creation callback.
 */
enum class ThreadPurpose {
    NetworkIO,      // Server network I/O thread
    SessionMain,    // Session main thread
    SessionSender   // Session sender thread
};

/**
 * @brief Server network configuration.
 */
struct ServerNetworkConfig {
    /// Socket receive buffer size in bytes; 0 means use the system default.
    std::size_t socket_recv_buffer_size = 128 * 1024;
    /// Socket send buffer size in bytes; 0 means use the system default.
    std::size_t socket_send_buffer_size = 128 * 1024;
    /// Whether to disable the Nagle algorithm (reduces small-packet latency).
    bool tcp_no_delay = true;
};

/**
 * @brief USB/IP server.
 *
 * @attention Thread-safety summary:
 *   - Constructor / start / stop / ~Server: lifecycle methods; must be called serially on the same thread.
 *   - add_device / has_bound_device / get_session_count / print_bound_devices / register_session_exit_callback:
 *     internally locked; safe from any thread.
 *   - get_available_devices / get_using_devices: not locked; caller must hold get_devices_mutex().
 *   - get_devices_mutex: always safe; only returns a mutex reference.
 *   - set_before_thread_create_callback / set_after_thread_create_callback: must be called before start().
 */
class USBIPDCPP_API Server final {
public:
    friend class Session;

    Server() = default;
    explicit Server(const ServerNetworkConfig &network_config);
    explicit Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config = {});
    Server(const Server &) = delete;
    Server(Server &&) = delete;
    /**
     * @brief Start the server non-blockingly; internally starts a thread to accept sockets.
     * add_device may be called before or after start.
     * @param ep Listening endpoint.
     *
     * @thread_safety Must not be called concurrently. Call at most once (call stop() before calling again).
     */
    void start(asio::ip::tcp::endpoint &ep);
    /**
     * @brief Internally closes each session's socket first, then closes the io_context.
     * The effect is equivalent to calling detach on every client.
     *
     * @thread_safety Must not be called concurrently. Must be called after start() and before destruction, at most once.
     */
    void stop();

    /**
     * @brief Add a device; thread-safe. May be called regardless of whether the server is started.
     * @param device The device to add.
     * @return The added device.
     *
     * @thread_safety Internally locked; safe from any thread.
     */
    std::shared_ptr<UsbDevice> add_device(std::shared_ptr<UsbDevice> &&device);

    /**
     * @thread_safety Internally locked; safe from any thread.
     */
    bool has_bound_device(const std::string &busid);

    /**
     * @thread_safety Internally locked; safe from any thread.
     */
    size_t get_session_count();

    /**
     * @thread_safety Internally locked; safe from any thread.
     */
    void print_bound_devices();

    /**
     * @brief Not thread-safe at all; call get_devices_mutex() yourself to acquire the lock.
     * @return
     *
     * @thread_safety Caller must hold a read or write lock from get_devices_mutex().
     */
    [[nodiscard]] std::vector<std::shared_ptr<UsbDevice>> &get_available_devices() {
        return available_devices;
    }

    /**
     * @brief Not thread-safe at all; call get_devices_mutex() yourself to acquire the lock.
     * @return
     *
     * @thread_safety Caller must hold a read or write lock from get_devices_mutex().
     */
    [[nodiscard]] std::map<std::string, std::shared_ptr<UsbDevice>> &get_using_devices() {
        return using_devices;
    }

    /**
     * @brief Call this function to obtain the lock before operating on device data.
     * @return
     *
     * @thread_safety Always safe (returns a reference only).
     */
    [[nodiscard]] std::shared_mutex &get_devices_mutex() const {
        return devices_mutex;
    }

    /**
     * @thread_safety Internally locked; safe from any thread.
     */
    void register_session_exit_callback(std::function<void()> &&callback);

    /**
     * @brief Set a pre-thread-creation callback, e.g., for setting thread core affinity on embedded platforms.
     * @param callback Callback function that receives the thread purpose identifier.
     *
     * @thread_safety Must be called before start().
     */
    void set_before_thread_create_callback(std::function<void(ThreadPurpose)> &&callback) {
        before_thread_create_callback = std::move(callback);
    }

    /**
     * @brief Set a post-thread-creation callback, e.g., for setting the thread name.
     * @param callback Callback function that receives the thread purpose identifier and a thread reference.
     *
     * @thread_safety Must be called before start().
     */
    void set_after_thread_create_callback(std::function<void(ThreadPurpose, std::thread&)> &&callback) {
        after_thread_create_callback = std::move(callback);
    }

    /**
     * @brief Remove the specified session and trigger on_session_exit.
     * @param session Pointer to the session to remove.
     *
     * @thread_safety Internally locked, but should only be called on the Session exit path.
     */
    void remove_session(Session *session);

    ~Server();

protected:
    asio::awaitable<void> do_accept(asio::ip::tcp::acceptor &acceptor);

    bool is_device_using(const std::string &busid);

    void try_moving_device_to_available(const std::string &busid);

    /**
     * @brief Try to move device to using_devices, and return this device,
     * return nullptr if there is no such device in available_devices or moved failed.
     * @param busid device busid
     * @return device or nullptr when error
     */
    std::shared_ptr<UsbDevice> try_moving_device_to_using(const std::string &busid);

    void print_devices();

    std::atomic_bool should_stop = false;

    ServerNetworkConfig network_config;

    // Pre-thread-creation callback
    std::function<void(ThreadPurpose)> before_thread_create_callback;
    // Post-thread-creation callback
    std::function<void(ThreadPurpose, std::thread&)> after_thread_create_callback;

    std::list<std::weak_ptr<Session>> sessions;
    mutable std::shared_mutex session_list_mutex;
    std::condition_variable_any all_sessions_closed_cv;

    // Use this io_context asynchronously for network communication
    asio::io_context asio_io_context;
    // All network communication must run on this thread; network communication must not run on other threads
    std::thread network_io_thread;

private:
    void on_session_exit();

    std::list<std::function<void()>> session_exit_callbacks;
    mutable std::shared_mutex exit_callbacks_mutex;

    // Devices available for import
    std::vector<std::shared_ptr<UsbDevice>> available_devices;
    // Devices currently in use; busid is the index key for lookup only, unrelated to the USBIP protocol
    std::map<std::string, std::shared_ptr<UsbDevice>> using_devices;
    // Guards both available_devices and using_devices
    mutable std::shared_mutex devices_mutex;
};
}
