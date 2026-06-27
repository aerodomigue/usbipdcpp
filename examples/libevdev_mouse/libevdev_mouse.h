#pragma once

#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "protocol.h"

#include <mutex>
#include <thread>
#include <atomic>
#include <array>
#include <condition_variable>

namespace usbipdcpp {

class LibevdevMouseInterfaceHandler : public HidVirtualInterfaceHandler {
public:
    LibevdevMouseInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;
    data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;


    data_type report_descriptor{
            // HID report descriptor - 5-button mouse with scroll wheel
            0x05, 0x01, // Usage Page (Generic Desktop)
            0x09, 0x02, // Usage (Mouse)
            0xA1, 0x01, // Collection (Application)
            0x09, 0x01, //   Usage (Pointer)
            0xA1, 0x00, //   Collection (Physical)

            // Button area (5 buttons + 3-bit padding)
            0x05, 0x09, //   Usage Page (Button)
            0x19, 0x01, //   Usage Minimum (Button 1)
            0x29, 0x05, //   Usage Maximum (Button 5)
            0x15, 0x00, //   Logical Minimum (0)
            0x25, 0x01, //   Logical Maximum (1)
            0x95, 0x05, //   Report Count (5)  // 5 buttons
            0x75, 0x01, //   Report Size (1)   // 1 bit per button
            0x81, 0x02, //   Input (Data,Var,Abs)

            0x95, 0x01, //   Report Count (1)  // 3-bit padding
            0x75, 0x03, //   Report Size (3)
            0x81, 0x03, //   Input (Const,Var,Abs) // constant padding

            // Cursor movement area (X/Y axes)
            0x05, 0x01, //   Usage Page (Generic Desktop)
            0x09, 0x30, //   Usage (X)
            0x09, 0x31, //   Usage (Y)
            0x15, 0x81, //   Logical Minimum (-127)
            0x25, 0x7F, //   Logical Maximum (127)
            0x75, 0x08, //   Report Size (8)   // 8-bit resolution
            0x95, 0x02, //   Report Count (2)  // X and Y axes
            0x81, 0x06, //   Input (Data,Var,Rel) // relative coordinates

            // Scroll wheel area
            0x09, 0x38, //   Usage (Wheel)
            0x15, 0x81, //   Logical Minimum (-127)
            0x25, 0x7F, //   Logical Maximum (127)
            0x75, 0x08, //   Report Size (8)
            0x95, 0x01, //   Report Count (1)
            0x81, 0x06, //   Input (Data,Var,Rel) // relative scroll amount

            0xC0, //   End Collection (Physical)
            0xC0, // End Collection (Application)

    };
    /*
Report descriptor notes:
Button section:

5 independent buttons (left, right, middle, side1, side2)
Each button uses 1 bit (0=released, 1=pressed)
3 constant padding bits for byte alignment
Cursor movement:

X/Y axis relative movement
8-bit signed integer (-127 to +127)
Relative coordinate mode (REL)
Scroll wheel:

Vertical scroll amount
8-bit signed integer (-127 to +127)
Acts as a button when pressed, as a wheel when scrolled
Report format:
[Byte 0] | Button state (bit0-4) + padding (bit5-7)
[Byte 1] | X-axis movement (relative)
[Byte 2] | Y-axis movement (relative)
[Byte 3] | Scroll wheel movement (relative)
*/


    /**
     * @brief Not locked — caller is responsible for acquiring the lock.
     */
    void reset_relative_data();

    struct State {
        bool left_pressed = false;
        bool right_pressed = false;
        bool middle_pressed = false;
        bool side_pressed = false;
        bool extra_pressed = false;

        std::int8_t wheel_vertical = 0;

        std::int8_t move_horizontal = 0;
        std::int8_t move_vertical = 0;

        bool operator==(const State &) const = default;
    };

    std::atomic_bool should_immediately_stop = false;

    State current_state;
    State last_state;  // Stores the state at the last send
    std::mutex state_mutex;
    std::condition_variable state_cv;  // Wait for state change
    std::thread send_thread;  // Send thread

    std::atomic<std::int16_t> idle_speed = 1;
};

}
