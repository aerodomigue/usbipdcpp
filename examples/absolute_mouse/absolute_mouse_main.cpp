/**
 * @file absolute_mouse_main.cpp
 * @brief Absolute coordinate mouse virtual device example
 */

#include <atomic>
#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "Server.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/devices/AbsoluteMouseHandler.h"

using namespace usbipdcpp;

void print_usage() {
    std::cout << "\nCommand list (screen coordinates):\n"
              << "  p              - Print current button state\n"
              << "  pos <x y>      - Set screen coordinate position\n"
              << "  1              - Move to screen center\n"
              << "  2              - Move to top-left corner\n"
              << "  3              - Move to bottom-right corner\n"
              << "  6              - Left click\n"
              << "  7              - Right click\n"
              << "  8              - Double click\n"
              << "  9              - Smooth move (top-left corner -> center, 1 second)\n"
              << "  H              - Humanized move (top-left corner -> center)\n"
              << "  D              - Drag (center -> bottom-right corner)\n"
              << "  hd x1 y1 x2 y2 - Humanized drag (from -> to)\n"
              << "  raw <x y>      - Set HID raw coordinates (0-32767)\n"
              << "  screen W H     - Set screen size\n"
              << "  bounds x1 y1 x2 y2 - Set screen bounds\n"
              << "  q              - Quit\n"
              << "  h              - Show help\n"
              << std::endl;
}

int main(int argc, char **argv) {
    auto opts = make_example_options("absolute_mouse", "USB/IP absolute mouse device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {UsbInterface{
            .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
            .interface_subclass = 0x01,
            .interface_protocol = 0x02, // Mouse
            .endpoints = {{UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 1}}}}};

    interfaces[0].with_handler<AbsoluteMouseHandler>(string_pool, 1920, 1080);

    auto mouse_device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/absolute_mouse",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5680,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });

    auto device_handler = mouse_device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    auto mouse = std::dynamic_pointer_cast<AbsoluteMouseHandler>(mouse_device->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mouse_device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    server.start(endpoint);

    SPDLOG_INFO("Absolute mouse started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);

    // Initial position: screen center
    int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
    int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;
    mouse->set_position(cx, cy);

    print_usage();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;

        // Split command by spaces
        std::vector<std::string> parts;
        std::istringstream iss(line);
        std::string part;
        while (iss >> part) {
            parts.push_back(part);
        }

        if (parts.empty())
            continue;

        const std::string &cmd = parts[0];
        int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
        int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;

        if (cmd == "p") {
            auto state = mouse->get_button_state();
            std::cout << "Button state: left=" << (state.left_button ? "pressed" : "released")
                      << " right=" << (state.right_button ? "pressed" : "released")
                      << " middle=" << (state.middle_button ? "pressed" : "released")
                      << " wheel=" << static_cast<int>(state.wheel) << "\n"
                      << "Screen range: (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ") - ("
                      << mouse->get_screen_x2() << ", " << mouse->get_screen_y2() << ")\n"
                      << "Screen size: " << mouse->get_screen_width() << "x" << mouse->get_screen_height() << std::endl;
        }
        else if (cmd == "pos" && parts.size() >= 3) {
            int x = std::stoi(parts[1]);
            int y = std::stoi(parts[2]);
            mouse->set_position(x, y);
            std::cout << "Move to screen coordinates: (" << x << ", " << y << ")" << std::endl;
        }
        else if (cmd == "1") {
            std::cout << "Move to screen center (" << cx << ", " << cy << ")" << std::endl;
            mouse->set_position(cx, cy);
        }
        else if (cmd == "2") {
            std::cout << "Move to top-left corner (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ")"
                      << std::endl;
            mouse->set_position(mouse->get_screen_x1() + 1, mouse->get_screen_y1() + 1);
        }
        else if (cmd == "3") {
            std::cout << "Move to bottom-right corner (" << mouse->get_screen_x2() - 1 << ", " << mouse->get_screen_y2() - 1 << ")"
                      << std::endl;
            mouse->set_position(mouse->get_screen_x2() - 1, mouse->get_screen_y2() - 1);
        }
        else if (cmd == "6") {
            std::cout << "Left click (" << cx << ", " << cy << ")" << std::endl;
            mouse->left_click(cx, cy);
        }
        else if (cmd == "7") {
            std::cout << "Right click (" << cx << ", " << cy << ")" << std::endl;
            mouse->right_click(cx, cy);
        }
        else if (cmd == "8") {
            std::cout << "Double click (" << cx << ", " << cy << ")" << std::endl;
            mouse->double_click(cx, cy);
        }
        else if (cmd == "9") {
            int x1 = mouse->get_screen_x1() + 1;
            int y1 = mouse->get_screen_y1() + 1;
            std::cout << "Smooth move (" << x1 << ", " << y1 << ") -> (" << cx << ", " << cy << ") ..." << std::endl;
            mouse->move(x1, y1, cx, cy, 1000);
        }
        else if (cmd == "H") {
            int x1 = mouse->get_screen_x1() + 1;
            int y1 = mouse->get_screen_y1() + 1;
            std::cout << "Humanized move (" << x1 << ", " << y1 << ") -> (" << cx << ", " << cy << ") ..." << std::endl;
            mouse->humanized_move(x1, y1, cx, cy, 1500);
        }
        else if (cmd == "D") {
            int x2 = mouse->get_screen_x2() - 1;
            int y2 = mouse->get_screen_y2() - 1;
            std::cout << "Drag (" << cx << ", " << cy << ") -> (" << x2 << ", " << y2 << ") ..." << std::endl;
            mouse->drag(cx, cy, x2, y2, 1000);
        }
        else if (cmd == "hd" && parts.size() >= 5) {
            int x1 = std::stoi(parts[1]);
            int y1 = std::stoi(parts[2]);
            int x2 = std::stoi(parts[3]);
            int y2 = std::stoi(parts[4]);
            std::cout << "Humanized drag (" << x1 << ", " << y1 << ") -> (" << x2 << ", " << y2 << ") ..." << std::endl;
            mouse->humanized_drag(x1, y1, x2, y2, 1500);
        }
        else if (cmd == "raw" && parts.size() >= 3) {
            int x = std::stoi(parts[1]);
            int y = std::stoi(parts[2]);
            mouse->set_position_raw(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
            std::cout << "HID coordinates: (" << x << ", " << y << ")" << std::endl;
        }
        else if (cmd == "screen" && parts.size() >= 3) {
            int w = std::stoi(parts[1]);
            int h = std::stoi(parts[2]);
            mouse->set_screen_size(w, h);
            std::cout << "Screen size set to: " << w << "x" << h << std::endl;
        }
        else if (cmd == "bounds" && parts.size() >= 5) {
            int x1 = std::stoi(parts[1]);
            int y1 = std::stoi(parts[2]);
            int x2 = std::stoi(parts[3]);
            int y2 = std::stoi(parts[4]);
            mouse->set_screen_bounds(x1, y1, x2, y2);
            std::cout << "Screen bounds set to: (" << x1 << ", " << y1 << ") - (" << x2 << ", " << y2 << ")" << std::endl;
        }
        else if (cmd == "q") {
            std::cout << "Quitting..." << std::endl;
            server.stop();
            return 0;
        }
        else if (cmd == "h") {
            print_usage();
        }
        else {
            std::cout << "Unknown command: " << cmd << std::endl;
        }
    }

    server.stop();
    return 0;
}
