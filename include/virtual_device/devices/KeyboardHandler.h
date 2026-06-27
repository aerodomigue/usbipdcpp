#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/// Keyboard modifier key masks
namespace KeyboardModifier {
    constexpr std::uint8_t LeftCtrl = 0x01;
    constexpr std::uint8_t LeftShift = 0x02;
    constexpr std::uint8_t LeftAlt = 0x04;
    constexpr std::uint8_t LeftGUI = 0x08;
    constexpr std::uint8_t RightCtrl = 0x10;
    constexpr std::uint8_t RightShift = 0x20;
    constexpr std::uint8_t RightAlt = 0x40;
    constexpr std::uint8_t RightGUI = 0x80;
} // namespace KeyboardModifier

/// Keyboard LED state masks
namespace KeyboardLED {
    constexpr std::uint8_t NumLock = 0x01;
    constexpr std::uint8_t CapsLock = 0x02;
    constexpr std::uint8_t ScrollLock = 0x04;
    constexpr std::uint8_t Compose = 0x08;
    constexpr std::uint8_t Kana = 0x10;
} // namespace KeyboardLED

/**
 * @brief USB HID keyboard virtual device handler (with Consumer Control media keys)
 *
 * Implements the standard USB HID boot keyboard protocol + Consumer Page media control, using Report IDs to separate two independent reports:
 * - Report ID 1: Standard keyboard 8-byte report (compatible with Boot Protocol)
 * - Report ID 2: Consumer Control 2-byte report (auto-cleared after a single trigger)
 *
 * Supports: up to 6 keys + 8 modifier keys, media keys, LED status reception.
 *
 * @note Endpoint max_packet_size must be at least 9 bytes (full Report ID 1 report)
 */
class USBIPDCPP_API KeyboardHandler : public HidVirtualInterfaceHandler {
public:
    /// Maximum number of simultaneously pressed keys (excluding modifier keys)
    static constexpr std::size_t MAX_KEYS = 6;
    static constexpr std::uint8_t REPORT_ID_KEYBOARD = 1;
    static constexpr std::uint8_t REPORT_ID_CONSUMER = 2;

    KeyboardHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~KeyboardHandler() override = default;

    // ========== HidVirtualInterfaceHandler interface implementation ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;
    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length, const data_type &data,
                            std::uint32_t *p_status) override;
    data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;

    // ========== Key API ==========

    /**
     * @brief Press a key
     * @param keycode USB HID key code (e.g. 0x04 = A, 0x28 = Enter)
     *
     * Ignored if the key is already pressed or 6 keys are already held.
     */
    void press_key(std::uint8_t keycode);

    /**
     * @brief Release a key
     * @param keycode USB HID key code
     */
    void release_key(std::uint8_t keycode);

    /// Release all keys and modifier keys
    void release_all();

    /**
     * @brief Press multiple keys simultaneously
     * @param keycodes List of key codes
     *
     * If more than MAX_KEYS are specified, only the first 6 are used.
     */
    void press_keys(std::initializer_list<std::uint8_t> keycodes);

    /// Query whether a specific key is currently pressed
    [[nodiscard]] bool is_key_pressed(std::uint8_t keycode) const;

    // ========== Modifier key API ==========

    /// Set modifier keys (bitwise OR of KeyboardModifier constants)
    void set_modifier(std::uint8_t mask);

    /// Clear specified modifier keys
    void clear_modifier(std::uint8_t mask);

    /// Get the current modifier key state
    [[nodiscard]] std::uint8_t get_modifier() const;

    // ========== Media key API (Consumer Control) ==========

    /**
     * @brief Send a media key; automatically released after a single trigger
     * @param usage USB Consumer Page Usage ID (e.g. HIDConsumer::PlayPause)
     */
    void press_media_key(std::uint16_t usage);

    // ========== LED status ==========

    /// Get the LED status set by the host (bitwise AND with KeyboardLED constants)
    [[nodiscard]] std::uint8_t get_led_status() const;

    /// Wait for a client to connect; timeout_ms negative means wait indefinitely
    bool wait_for_client(int timeout_ms = -1);

private:
    struct KeyboardState {
        std::uint8_t modifier = 0;
        std::array<std::uint8_t, MAX_KEYS> keys{};
        std::uint16_t consumer_usage = 0; // Consumer Page Usage ID, 0 = no action

        bool operator==(const KeyboardState &) const = default;
    };

    data_type report_descriptor_;
    std::atomic<std::int16_t> idle_speed_{1};
    std::atomic<std::uint8_t> led_status_{0};

    KeyboardState current_state_;
    KeyboardState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
