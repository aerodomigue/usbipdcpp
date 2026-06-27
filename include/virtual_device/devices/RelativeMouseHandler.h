#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief Relative coordinate mouse virtual device handler
 *
 * Provides a mouse operation API based on relative offsets. Movement amounts accumulate until the host reads them;
 * multiple move() calls are automatically summed and reset to zero after the report is sent.
 *
 * HID report format (6 bytes):
 *   [0]    Buttons (bit0:left, bit1:right, bit2:middle, bit3:side, bit4:extra) + 3 bits padding
 *   [1-2]  X-axis relative movement (-32767~32767, little-endian)
 *   [3-4]  Y-axis relative movement (-32767~32767, little-endian)
 *   [5]    Scroll wheel (-127~127)
 */
class USBIPDCPP_API RelativeMouseHandler : public HidVirtualInterfaceHandler {
public:
    RelativeMouseHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~RelativeMouseHandler() override = default;

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    /// Accumulate relative movement offset, clamped to [-32767, 32767]; keeps accumulating until the host reads it
    void move(std::int16_t dx, std::int16_t dy);
    /// Accumulate scroll wheel offset, clamped to [-127, 127]
    void set_wheel(std::int8_t delta);

    void set_left_button(bool pressed);
    void set_right_button(bool pressed);
    void set_middle_button(bool pressed);
    void set_side_button(bool pressed);
    void set_extra_button(bool pressed);

    /// Press → delay → release
    void left_click(int delay_ms = 50);
    void right_click(int delay_ms = 50);
    void middle_click(int delay_ms = 50);
    void double_click(int delay_ms = 100);

    struct ButtonState {
        bool left = false;
        bool right = false;
        bool middle = false;
        bool side = false;
        bool extra = false;

        bool operator==(const ButtonState &) const = default;
    };
    ButtonState get_button_state() const;

    struct RelativeDataState {
        std::int16_t dx = 0;
        std::int16_t dy = 0;
        std::int8_t wheel = 0;

        bool operator==(const RelativeDataState &) const = default;

        [[nodiscard]] bool all_zeros() const {
            return dx == 0 && dy == 0 && wheel == 0;
        }

        void reset() {
            dx = 0;
            dy = 0;
            wheel = 0;
        }
    };
    RelativeDataState get_relative_data_state() const;

    /// Wait for a client to connect
    bool wait_for_client(int timeout_ms = -1);

private:
    struct State {
        ButtonState button;
        RelativeDataState relative_data;

        bool operator==(const State &) const = default;

        [[nodiscard]] const ButtonState &get_button_state() const {
            return button;
        }

        [[nodiscard]] const RelativeDataState &get_relative_data_state() const {
            return relative_data;
        }
    };

    void notify();
    // Does not change button state, pure send
    void send_report();

    data_type report_descriptor;

    State current;
    State last;

    mutable std::mutex state_mutex;
    std::condition_variable state_cv;
    std::thread send_thread;
    std::atomic_bool should_stop{false};

    std::atomic_bool client_connected{false};
    std::mutex connect_mutex;
    std::condition_variable connect_cv;
};

} // namespace usbipdcpp
