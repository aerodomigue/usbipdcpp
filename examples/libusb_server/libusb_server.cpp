#include <asio.hpp>
#include "../example_utils.h"
#include <iostream>
#include <libusb-1.0/libusb.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "LibusbHandler/LibusbServer.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("libusb_server", "USB/IP libusb server");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();

    spdlog::set_level(spdlog::level::info);

    int err;
    // Enable libusb debug logging
    // libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    err = libusb_init(nullptr);
    if (err) {
        SPDLOG_ERROR("libusb_init failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return 1;
    }

    LibusbServer libusb_server;

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
    libusb_server.start(endpoint);

    // SPDLOG_DEBUG("Directly bind 3-5-1");
    // server.bind_host_device(server.find_by_busid("3-5-1"));
    // auto target_busid = "1-1";
    // SPDLOG_DEBUG("Directly bind 1-1");
    // auto found = LibusbServer::find_by_busid("1-1");
    // if (found) {
    //     server.bind_host_device(found);
    // }
    // else {
    //     SPDLOG_ERROR("Device {} does not exist", target_busid);
    // }


    if (!isatty(STDIN_FILENO)) {
        // Daemon mode (no TTY): block until SIGTERM/SIGINT
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        int sig;
        sigwait(&mask, &sig);
        goto loop_end;
    }

    char cmd;
    while (std::cin >> cmd) {
        switch (cmd) {
            case 's': {
                spdlog::info("There are {} sessions in this server", libusb_server.get_server().get_session_count());
                break;
            }
            case 'l': {
                spdlog::info("List all usb devices in the host");
                libusb_server.list_host_devices();
                break;
            }
            case 'd': {
                libusb_server.get_server().print_bound_devices();
                break;
            }
            case 'b': {
                spdlog::info("Binding device");
                std::string target_busid;
                std::cin >> target_busid;
                auto device = LibusbServer::find_by_busid(target_busid);
                if (device) {
                    libusb_server.bind_host_device(device);
                }
                else {
                    spdlog::warn("Can't find a device with busid {}", target_busid);
                }
                break;
            }
            case 'u': {
                spdlog::info("Unbinding device");
                std::string target_busid;
                std::cin >> target_busid;
                auto device = LibusbServer::find_by_busid(target_busid);
                if (device) {
                    libusb_server.unbind_host_device(device);
                }
                // Cannot find this device on the host; if it is still in a bound state but not found, remove it
                else if (libusb_server.get_server().has_bound_device(target_busid)) {
                    spdlog::warn("Can't find target busid {} in server, but it has been bound."
                                 "Has it been removed?", target_busid);
                    spdlog::warn("Try remove dead device:{}", target_busid);
                    libusb_server.try_remove_dead_device(target_busid);
                }
                else {
                    spdlog::warn("Can't find a device with busid {}", target_busid);
                }
                break;
            }
            case 'q': {
                goto loop_end;
                break;
            }

            default: {
                spdlog::warn("Unknown command {}", cmd);
            }
            case 'h': {
                std::cout << R"(
h : Print this help information.
s : show how many sessions the server has.
l : List all usb devices in the host.
d : Show all bound devices.
b busid : Try to bind a device.
u busid : Try to unbind a device.
q : Close the server.)" << std::endl;
                break;
            }
        }
    }
loop_end:
    spdlog::info("Trying to close server");
    libusb_server.stop();
    spdlog::info("Closed server successfully");

    libusb_exit(nullptr);
    return 0;
}
