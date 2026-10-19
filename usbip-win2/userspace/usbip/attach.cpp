/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include "ctrl_c_guard.h"
#include "strings.h"
#include "log.h"
#include "custom_commands.h"

#include <libusbip/vhci.h>
#include <libusbip/persistent.h>

#include <print>
#include <iostream>
#include <vector>

namespace
{

using namespace usbip;

bool attach_persistent_devices(_In_ HANDLE dev)
{
        auto v = vhci::get_persistent(dev);
        if (!v) {
                log::error(get_last_error_msg());
                return false;
        }

        auto ok = true;

        for (const auto &args: *v) {
                std::println("{}", args);

                if (!vhci::attach(dev, args)) {
                        log::error(get_last_error_msg());
                        ok = false;
                }
        }

        return ok;
}

bool stop_attach_attempts(_In_ HANDLE dev, _In_opt_ const device_location *loc)
{
        auto cnt = vhci::stop_attach_attempts(dev, loc);
        auto ok = cnt >= 0;

        if (ok) {
                log::debug("{} request(s) stopped", cnt);
        } else {
                log::error(get_last_error_msg());
        }

        return ok;
}

// The custom usbipd server uses standard USB/IP DEVLIST/IMPORT framing.
// Query the device list on one connection, let the user choose a BUSID,
// then the normal vhci::attach path opens a fresh connection for IMPORT.
bool select_remote_busid(const attach_args &args, std::string &selected_busid)
{
        ctrl_c_guard guard;
        auto sock = connect(args.remote.c_str(), global_args.tcp_port.c_str(), guard.token());
        if (!sock) {
                log::error("Unable to connect to USB/IP server {}:{}: {}",
                           args.remote, global_args.tcp_port, get_last_error_msg());
                return false;
        }

        std::vector<usb_device> devices;
        const auto on_device = [&devices](int, const usb_device &d) { devices.push_back(d); };
        const auto on_interface = [](int, const usb_device &, int, const usb_interface &) {};
        if (!enum_exportable_devices(sock.get(), on_device, on_interface)) {
                log::error("Failed to retrieve the remote USB device list: {}", get_last_error_msg());
                return false;
        }

        if (devices.empty()) {
                log::error("The server reported no exportable USB devices.");
                return false;
        }

        std::println("Exportable USB devices on {}:{}", args.remote, global_args.tcp_port);
        std::println("==============================================");
        for (size_t i = 0; i < devices.size(); ++i) {
                const auto &d = devices[i];
                auto &ids = get_ids();
                auto product = get_product(ids, d.idVendor, d.idProduct);
                auto type = get_device_type(ids, d.idVendor, d.idProduct,
                                            d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol);
                auto csp = get_class(ids, d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol);
                std::println("[{}] {:<8}: {}", i + 1, d.busid, product);
                std::println("           : Type: {}", type);
                std::println("           : {}", d.path);
                std::println("           : {}", csp);
                if (!d.bNumInterfaces) {
                        std::println("           : Interfaces: not provided by server");
                }
                std::println();
        }
        std::print("Select device number (1-{}), or 0 to cancel: ", devices.size());

        size_t choice = 0;
        if (!(std::cin >> choice)) {
                std::cin.clear();
                std::string discard;
                std::getline(std::cin, discard);
                log::error("Invalid selection.");
                return false;
        }
        if (choice == 0) {
                std::println("Cancelled.");
                return false;
        }
        if (choice > devices.size()) {
                log::error("Selection {} is out of range.", choice);
                return false;
        }

        selected_busid = devices[choice - 1].busid;
        std::println("Selected BUSID {} (VID:PID {:04X}:{:04X}).",
                     selected_busid, devices[choice - 1].idVendor, devices[choice - 1].idProduct);
        return true;
}

} // namespace


bool usbip::cmd_attach(const attach_args &args)
{
        std::string busid = args.busid;
        if (args.select_device) {
                if (!busid.empty()) {
                        log::error("Use either --select or --bus-id, not both.");
                        return false;
                }
                if (!select_remote_busid(args, busid)) {
                        return false;
                }
        } else if (busid.empty() && !args.persistent && !args.stop_all) {
                log::error("Specify --bus-id BUSID or use --select.");
                return false;
        }

        auto dev = vhci::open();
        if (!dev) {
                log::error(get_last_error_msg());
                return false;
        }

        ctrl_c_guard guard(dev.get());

        if (args.persistent) {
                return attach_persistent_devices(dev.get());
        }

        if (args.stop_all) {
                return stop_attach_attempts(dev.get(), nullptr);
        }

        device_location loc {
                .hostname = args.remote,
                .service = global_args.tcp_port,
                .busid = busid,
        };

        if (args.stop) {
                return stop_attach_attempts(dev.get(), &loc);
        }

        // Explicit custom BIND reserves/captures the selected device first.
        // The VHCI driver then opens its normal long-lived USB/IP data session;
        // the server reuses this reservation instead of capturing twice.
        if (!custom::request_status(args.remote, global_args.tcp_port,
                                    USBIP_CMD_CUSTOM_BIND, busid)) {
                log::error("Custom BIND failed for {}:{} BUSID {}: {}",
                           args.remote, global_args.tcp_port, busid, get_last_error_msg());
                return false;
        }

        vhci::attach_args cmd_args {
                .location = std::move(loc),
                .serial = args.serial,
                .recv_mode = args.recv_mode,
                .once = args.once,
        };

        auto port = vhci::attach(dev.get(), cmd_args);
        if (!port) {
                log::error(get_last_error_msg());
                return false;
        }

        if (args.terse) {
                std::println("{}", port);
        } else {
                std::println("successfully attached to port {}", port);
        }

        return true;
}
