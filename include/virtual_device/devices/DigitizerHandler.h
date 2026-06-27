#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief USB HID touchscreen virtual device handler (single-point touch)
 *
 * Report format (6 bytes):
 *   [0]    bit0=Tip Switch, bit1=In Range, rest reserved
 *   [1-2]  X coordinate (uint16 LE)
 *   [3-4]  Y coordinate (uint16 LE)
 *   [5]    Touch pressure (0~255)
 */
class USBIPDCPP_API DigitizerHandler : public HidVirtualInterfaceHandler {
public:
    static constexpr std::size_t REPORT_SIZE = 6;
    static constexpr std::uint16_t DEFAULT_MAX = 32767;

    /**
     * @param handle_interface USB interface
     * @param string_pool String pool
     * @param x_max X-axis maximum value (default 32767)
     * @param y_max Y-axis maximum value (default 32767)
     */
    explicit DigitizerHandler(UsbInterface &handle_interface, StringPool &string_pool,
                              std::uint16_t x_max = DEFAULT_MAX, std::uint16_t y_max = DEFAULT_MAX);

    ~DigitizerHandler() override = default;

    // ========== HidVirtualInterfaceHandler interface implementation ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    // ========== Touch API ==========

    /// Touch press (or move), coordinate range [0, max], pressure range 0~255 (default 128)
    void touch(std::uint16_t x, std::uint16_t y, std::uint8_t pressure = 128);

    /// Move (same as touch)
    void move(std::uint16_t x, std::uint16_t y, std::uint8_t pressure = 128) {
        touch(x, y, pressure);
    }

    /// Lift (release touch)
    void release();

    /// Whether a touch is currently active
    [[nodiscard]] bool is_touching() const;

    /// Get the current coordinates
    [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> get_position() const;

    /// Get the coordinate maximum values
    [[nodiscard]] std::uint16_t get_x_max() const {
        return x_max_;
    }
    [[nodiscard]] std::uint16_t get_y_max() const {
        return y_max_;
    }

    /// Wait for a client to connect
    bool wait_for_client(int timeout_ms = -1);

private:
    struct TouchState {
        bool touching = false;
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint8_t pressure = 0;

        bool operator==(const TouchState &) const = default;
    };

    std::uint16_t x_max_;
    std::uint16_t y_max_;
    data_type report_descriptor_;

    TouchState current_state_;
    TouchState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
