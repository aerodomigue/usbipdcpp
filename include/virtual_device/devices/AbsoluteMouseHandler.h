#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {
/**
 * @brief Absolute coordinate mouse virtual device handler
 *
 * Provides a mouse operation API based on absolute coordinates. Uses screen coordinates (pixels) or HID raw coordinates.
 *
 * HID raw coordinate range: [0, 32767]
 * Screen coordinate range: set via set_screen_bounds
 */
class USBIPDCPP_API AbsoluteMouseHandler : public HidVirtualInterfaceHandler {
public:
    /// HID coordinate maximum value
    static constexpr std::int16_t HID_MAX = 32767;

    /**
     * @brief Constructor
     * @param handle_interface USB interface
     * @param string_pool String pool
     * @param screen_width Screen width in pixels (default 1920)
     * @param screen_height Screen height in pixels (default 1080)
     */
    AbsoluteMouseHandler(UsbInterface &handle_interface, StringPool &string_pool, int screen_width = 1920,
                         int screen_height = 1080);

    ~AbsoluteMouseHandler() override = default;

    // ========== HidVirtualInterfaceHandler interface implementation ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    // ========== Screen configuration ==========

    /**
     * @brief Set the screen size
     * @param width Screen width in pixels
     * @param height Screen height in pixels
     */
    void set_screen_size(int width, int height);

    /**
     * @brief Set the screen coordinate bounds
     * @param x1 Top-left X coordinate
     * @param y1 Top-left Y coordinate
     * @param x2 Bottom-right X coordinate
     * @param y2 Bottom-right Y coordinate
     */
    void set_screen_bounds(int x1, int y1, int x2, int y2);

    int get_screen_width() const;
    int get_screen_height() const;
    int get_screen_x1() const;
    int get_screen_y1() const;
    int get_screen_x2() const;
    int get_screen_y2() const;

    // ========== Mouse position API (screen coordinates) ==========

    /**
     * @brief Set the mouse position (screen coordinates)
     * @param x X coordinate in pixels
     * @param y Y coordinate in pixels
     */
    void set_position(int x, int y);

    /**
     * @brief Smooth move (screen coordinates)
     * @param from_x Start X coordinate in pixels
     * @param from_y Start Y coordinate in pixels
     * @param to_x End X coordinate in pixels
     * @param to_y End Y coordinate in pixels
     * @param duration_ms Total movement time in milliseconds
     * @param callback Optional per-frame callback (current screen coordinates)
     */
    void move(int from_x, int from_y, int to_x, int to_y, int duration_ms,
              std::function<void(int, int)> callback = nullptr);

    /**
     * @brief Humanized move (simulates human mouse movement)
     * @param from_x Start X coordinate in pixels
     * @param from_y Start Y coordinate in pixels
     * @param to_x End X coordinate in pixels
     * @param to_y End Y coordinate in pixels
     * @param duration_ms Total movement time in milliseconds
     * @param callback Optional per-frame callback (current screen coordinates)
     *
     * Features: Bezier curve, random speed, path jitter, random pauses
     */
    void humanized_move(int from_x, int from_y, int to_x, int to_y, int duration_ms,
                        std::function<void(int, int)> callback = nullptr);

    // ========== Mouse position API (HID raw coordinates) ==========

    /**
     * @brief Set the mouse position (HID raw coordinates)
     * @param x HID X coordinate (0-32767)
     * @param y HID Y coordinate (0-32767)
     */
    void set_position_raw(std::int16_t x, std::int16_t y);

    /**
     * @brief Smooth move (HID raw coordinates)
     * @param from_x Start HID X coordinate (0-32767)
     * @param from_y Start HID Y coordinate (0-32767)
     * @param to_x End HID X coordinate (0-32767)
     * @param to_y End HID Y coordinate (0-32767)
     * @param duration_ms Total movement time in milliseconds
     * @param callback Optional per-frame callback (current HID coordinates)
     */
    void move_raw(std::int16_t from_x, std::int16_t from_y, std::int16_t to_x, std::int16_t to_y, int duration_ms,
                  std::function<void(std::int16_t, std::int16_t)> callback = nullptr);

    // ========== Button API ==========

    /**
     * @brief Set the left button state
     * @param pressed true = pressed, false = released
     */
    void set_left_button(bool pressed);

    /**
     * @brief Set the right button state
     * @param pressed true = pressed, false = released
     */
    void set_right_button(bool pressed);

    /**
     * @brief Set the middle button state
     * @param pressed true = pressed, false = released
     */
    void set_middle_button(bool pressed);

    /**
     * @brief Set the scroll wheel
     * @param delta Scroll amount (-127 to 127)
     */
    void set_wheel(std::int8_t delta);

    /**
     * @brief Left-click
     * @param x Click position X coordinate in pixels
     * @param y Click position Y coordinate in pixels
     * @param delay_ms Delay between press and release in milliseconds
     */
    void left_click(int x, int y, int delay_ms = 50);

    /**
     * @brief Right-click
     * @param x Click position X coordinate in pixels
     * @param y Click position Y coordinate in pixels
     * @param delay_ms Delay between press and release in milliseconds
     */
    void right_click(int x, int y, int delay_ms = 50);

    /**
     * @brief Middle-click
     * @param x Click position X coordinate in pixels
     * @param y Click position Y coordinate in pixels
     * @param delay_ms Delay between press and release in milliseconds
     */
    void middle_click(int x, int y, int delay_ms = 50);

    /**
     * @brief Double left-click
     * @param x Click position X coordinate in pixels
     * @param y Click position Y coordinate in pixels
     * @param delay_ms Delay between the two clicks in milliseconds
     */
    void double_click(int x, int y, int delay_ms = 100);

    // ========== Drag API ==========

    /**
     * @brief Drag (move while holding left button)
     * @param from_x Start X coordinate in pixels
     * @param from_y Start Y coordinate in pixels
     * @param to_x End X coordinate in pixels
     * @param to_y End Y coordinate in pixels
     * @param duration_ms Total movement time in milliseconds
     * @param callback Optional per-frame callback (current screen coordinates)
     */
    void drag(int from_x, int from_y, int to_x, int to_y, int duration_ms,
              std::function<void(int, int)> callback = nullptr);

    /**
     * @brief Humanized drag (simulates human dragging)
     * @param from_x Start X coordinate in pixels
     * @param from_y Start Y coordinate in pixels
     * @param to_x End X coordinate in pixels
     * @param to_y End Y coordinate in pixels
     * @param duration_ms Total movement time in milliseconds
     * @param callback Optional per-frame callback (current screen coordinates)
     */
    void humanized_drag(int from_x, int from_y, int to_x, int to_y, int duration_ms,
                        std::function<void(int, int)> callback = nullptr);

    // ========== Coordinate conversion ==========

    /**
     * @brief Convert screen coordinates to HID coordinates
     */
    std::pair<std::int16_t, std::int16_t> screen_to_hid(int screen_x, int screen_y) const;

    /**
     * @brief Convert HID coordinates to screen coordinates
     */
    std::pair<int, int> hid_to_screen(std::int16_t hid_x, std::int16_t hid_y) const;

    // ========== State query ==========

    /**
     * @brief Get the current button state (position is not stored; position is tracked by the caller)
     */
    struct ButtonState {
        bool left_button = false;
        bool right_button = false;
        bool middle_button = false;
        std::int8_t wheel = 0;
    };

    ButtonState get_button_state() const;

    /**
     * @brief Wait for a client to connect
     * @param timeout_ms Timeout in milliseconds; negative means wait indefinitely
     * @return true if a client connected, false if timed out
     */
    bool wait_for_client(int timeout_ms = -1);

private:
    int screen_x1_ = 0; ///< Screen top-left X coordinate
    int screen_y1_ = 0; ///< Screen top-left Y coordinate
    int screen_x2_ = 1920; ///< Screen bottom-right X coordinate
    int screen_y2_ = 1080; ///< Screen bottom-right Y coordinate
    int screen_width_; ///< Screen width (x2 - x1)
    int screen_height_; ///< Screen height (y2 - y1)

    std::int16_t hid_x_ = 0; ///< Current HID X coordinate
    std::int16_t hid_y_ = 0; ///< Current HID Y coordinate
    bool left_button_ = false;
    bool right_button_ = false;
    bool middle_button_ = false;
    std::int8_t wheel_ = 0;

    mutable std::mutex state_mutex_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::condition_variable state_cv_;
    bool state_changed_{false};

    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;

    data_type report_descriptor_;

    void send_current_state();
    void notify_state_change();
};
} // namespace usbipdcpp
