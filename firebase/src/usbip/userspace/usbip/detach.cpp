/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include "ctrl_c_guard.h"
#include "log.h"
#include "custom_commands.h"
#include <libusbip/vhci.h>

#include <print>

bool usbip::cmd_detach(const detach_args &args)
{
        auto dev = vhci::open();
        if (!dev) {
                log::error(get_last_error_msg());
                return false;
        }

        ctrl_c_guard guard(dev.get());

        // Ask the remote server to release the physical capture explicitly.
        // Local VHCI detach is still performed even if the server is unreachable.
        auto imported = vhci::get_imported_devices(dev.get());
        if (imported) {
                for (const auto &item : *imported) {
                        const bool selected = args.port <= 0 || item.port == args.port;
                        if (!selected || item.location.hostname.empty() || item.location.busid.empty()) {
                                continue;
                        }
                        if (!custom::request_status(item.location.hostname, item.location.service,
                                                    USBIP_CMD_CUSTOM_DETACH, item.location.busid)) {
                                log::error("Remote DETACH failed for {}:{} BUSID {}: {}",
                                           item.location.hostname, item.location.service,
                                           item.location.busid, get_last_error_msg());
                        }
                }
        }

        auto ok = vhci::detach(dev.get(), args.port);

        if (!ok) {
                log::error(get_last_error_msg());
        } else if (args.port == vhci::port_all_closeonly) {
                std::println("all network connections are closed");
        } else if (args.port <= 0) {
                std::println("all ports are detached");
        } else {
                std::println("port {} is successfully detached", args.port);
        }

        return ok;
}
