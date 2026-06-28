#pragma once

#include <atomic>
#include <functional>
#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "Server.h"

namespace usbipdcpp {

/**
 * @brief Result of device bind/unbind operations.
 */
enum class DeviceOperationResult {
    Success,                ///< Operation completed successfully
    DeviceNotFound,         ///< Device was not found in the device list
    DeviceInUse,            ///< Device is currently in use and cannot be modified
    DeviceOpenFailed,       ///< Failed to open the device
    GetDescriptorFailed,    ///< Failed to get device descriptor
    GetConfigFailed,        ///< Failed to get configuration descriptor
    ClaimInterfaceFailed    ///< Failed to claim interface
};

/**
 * @brief USB/IP server based on libusb
 *
 * @attention Thread safety summary:
 *   - Constructor / start / stop / ~LibusbServer: lifecycle methods, must be called serially on the same thread
 *   - bind / unbind / try_remove_dead_device / notify_device_removed: internally locked, safe from any thread
 *   - print_device / list_host_devices: internally locked, safe from any thread
 *   - get_device_names / find_by_busid: static methods operating on independent libusb device lists, safe from any thread
 *   - get_server: always safe (returns a reference only), but calls on the returned object must comply with Server's own thread constraints
 *   - set_hotplug_enabled / is_hotplug_enabled: must be called/read before start()
 */
class USBIPDCPP_API LibusbServer {
public:
    LibusbServer();

    /**
     * @brief Bind a physical USB device to make it available for export (normal mode).
     *
     * This function retrieves device information and adds it to the available devices list.
     * The device is not opened until a client connects (lazy binding).
     *
     * @param dev The libusb device to bind. Must not be nullptr.
     *            The function takes ownership of the device reference.
     * @return DeviceOperationResult::Success on success, or an appropriate error code.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult bind_host_device(libusb_device *dev);

    /**
     * @brief Bind a physical USB device with a system file descriptor (Android mode).
     *
     * This is for Android where the device is accessed via a file descriptor obtained
     * from UsbManager.openDevice(). The fd is wrapped via libusb_wrap_sys_device()
     * on each client connection, supporting reconnection after disconnection.
     *
     * @param fd A valid file descriptor opened on the device node.
     *           The fd must remain valid until the device is unbound or the server is stopped.
     * @return DeviceOperationResult::Success on success, or an appropriate error code.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult bind_host_device_with_wrapped_fd(intptr_t fd);

    /**
     * @brief Unbind a previously bound physical USB device.
     *
     * Releases all interfaces, reattaches kernel drivers, closes the device handle,
     * and removes the device from the available devices list. The device reference
     * will be released.
     *
     * @param device The libusb device to unbind. The function takes ownership of this reference.
     * @return DeviceOperationResult::Success on success,
     *         DeviceOperationResult::DeviceNotFound if not in available devices,
     *         DeviceOperationResult::DeviceInUse if currently in use.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult unbind_host_device(libusb_device *device);

    /**
     * @brief Unbind a previously bound device by its file descriptor (Android mode).
     *
     * Finds and removes the device that was bound with the specified fd.
     *
     * @param fd The file descriptor used when binding the device.
     * @return DeviceOperationResult::Success on success,
     *         DeviceOperationResult::DeviceNotFound if not in available devices,
     *         DeviceOperationResult::DeviceInUse if currently in use.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult unbind_host_device_by_fd(intptr_t fd);

    /**
     * @brief Remove a dead device from the device lists.
     *
     * This function should not be called with a busid that is still in use.
     * It only removes libusb devices, not other device types.
     *
     * @param busid The bus ID of the device to remove.
     * @return DeviceOperationResult::Success if found and removed,
     *         DeviceOperationResult::DeviceNotFound if not found.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult try_remove_dead_device(const std::string &busid);

    /**
     * @brief Notify that a device has been physically removed (Android mode).
     *
     * This should be called when the system detects a USB device has been detached.
     * If the device is currently in use, it will trigger disconnection and stop the session.
     *
     * @param busid The bus ID of the removed device.
     * @return DeviceOperationResult::Success if found and handled,
     *         DeviceOperationResult::DeviceNotFound if not found.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    DeviceOperationResult notify_device_removed(const std::string &busid);

    /**
     * @brief Start the server listening on the specified endpoint.
     *
     * Also starts the hotplug monitor if enabled and supported.
     *
     * @param ep The TCP endpoint to listen on.
     *
     * @thread_safety Cannot be called concurrently. May be called at most once (must call stop() first to call again).
     */
    void start(asio::ip::tcp::endpoint &ep);

    /**
     * @brief Stop the server.
     *
     * Stops the hotplug monitor, closes all device handles, releases interfaces,
     * and reattaches kernel drivers.
     *
     * @thread_safety Cannot be called concurrently. Must be called after start() and before destruction.
     */
    void stop();

    ~LibusbServer();

    /**
     * @brief Print detailed information about a USB device to stdout.
     *
     * Displays device name, bus ID, VID/PID, USB version, device class, and speed.
     * Also shows whether the device is exported, available, or unbound.
     *
     * @param dev The libusb device to print information about.
     *
     * @thread_safety Internally locked for reading device state, safe from any thread.
     */
    void print_device(libusb_device *dev);

    /**
     * @brief List all USB devices connected to the host.
     *
     * Prints detailed information about each device to stdout.
     *
     * @thread_safety Uses the libusb global device list (libusb_get_device_list), safe from any thread.
     */
    void list_host_devices();

    /**
     * @brief Get the underlying Server instance.
     *
     * @return Reference to the internal Server object.
     *
     * @thread_safety Always safe (returns a reference only). Operations on the returned object must comply with Server's thread constraints.
     */
    Server &get_server() {
        return server;
    }

    /**
     * @brief Get the manufacturer and product names of a USB device.
     *
     * @param device The libusb device to get names from.
     * @return A pair containing {manufacturer, product} names. Returns "Unknown Manufacturer"
     *         or "Unknown Product" if the corresponding string descriptor is not available.
     *
     * @thread_safety Static method, opens an independent libusb handle, safe from any thread.
     */
    static std::pair<std::string, std::string> get_device_names(libusb_device *device);

    /**
     * @brief Find a libusb device by its bus ID.
     *
     * @param busid The bus ID to search for (e.g., "1-2.3").
     * @return A libusb_device pointer if found, nullptr otherwise. The caller must call
     *         libusb_unref_device() when done with the device.
     *
     * @thread_safety Static method, uses the libusb global device list, safe from any thread.
     */
    static libusb_device *find_by_busid(const std::string &busid);

    /**
     * @brief Set whether hotplug monitoring is enabled.
     *
     * Hotplug monitoring is enabled by default. On Android without root privileges,
     * hotplug is not supported and should be disabled before starting the server.
     *
     * @param enabled true to enable hotplug monitoring, false to disable.
     *
     * @thread_safety Must be called before start().
     */
    void set_hotplug_enabled(bool enabled) {
        hotplug_enabled_by_user_ = enabled;
    }

    /**
     * @brief Check if hotplug monitoring is enabled.
     *
     * @return true if hotplug monitoring is enabled, false otherwise.
     *
     * @thread_safety Reads an atomic variable, safe from any thread. However, results are only meaningful after start().
     */
    bool is_hotplug_enabled() const {
        return hotplug_enabled_by_user_;
    }

protected:
    Server server;

    std::atomic<bool> should_exit_libusb_event_thread = false;

    // Must not send network packets on this thread
    std::thread libusb_event_thread;

    // Hotplug related
    libusb_hotplug_callback_handle hotplug_handle_ = 0;
    bool hotplug_enabled_ = false;
    bool hotplug_enabled_by_user_ = true;  // User-controlled switch, enabled by default
    std::atomic<int> pending_bind_threads_{0};  // tracks detached bind worker threads

    void start_hotplug_monitor();
    void stop_hotplug_monitor();
    void bind_existing_devices();

    void handle_device_arrived(libusb_device *device);
    void handle_device_left(const std::string &busid);

    static int LIBUSB_CALL hotplug_callback(
        libusb_context *ctx,
        libusb_device *device,
        libusb_hotplug_event event,
        void *user_data);
};
}
