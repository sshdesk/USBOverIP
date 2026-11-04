/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "usbip.h"
#include "ctrl_c_guard.h"
#include "log.h"
#include "custom_commands.h"
#include <libusbip/vhci.h>
#include <print>
#include <thread>
#include <chrono>
struct rtc_data_channel;
extern "C" rtc_data_channel *get_active_rtc_channel(void);


bool usbip::cmd_detach(const detach_args &args)
{
        auto dev = vhci::open();
        if (!dev) {
                log::error(get_last_error_msg());
                return false;
        }

        ctrl_c_guard guard(dev.get());

        // Snapshot the imported devices before the local VHCI ports are removed.
        // Their locations are needed for the server-side release request.
        auto imported = vhci::get_imported_devices(dev.get());
        if (imported && args.port != vhci::port_all_closeonly) {
                for (const auto &item : *imported) {
                        const bool selected = args.port <= 0 || item.port == args.port;
                        if (!selected || item.location.hostname.empty() || item.location.busid.empty()) {
                                continue;
                        }

                        // The API returns a count (0 is a valid success), and -1 means
                        // failure. Treating 0 as failure produced the misleading
                        // "The operation completed successfully" message.
                        const int cancelled = vhci::stop_attach_attempts(dev.get(), &item.location);
                        if (cancelled < 0) {
                                log::error("Unable to stop automatic attach attempts for BUSID {}: {}",
                                           item.location.busid, get_last_error_msg());
                        } else {
                                log::debug("{} automatic attach request(s) cancelled for BUSID {}",
                                           cancelled, item.location.busid);
                        }
                }
        }

        // IMPORTANT: remove the local VHCI port before asking the server to close
        // the remote import socket. If the server closes it first, VHCI can interpret
        // that as a transient network failure and immediately retry IMPORT while the
        // port is still present, creating the random reconnect loop seen in logs.
        auto ok = vhci::detach(dev.get(), args.port);
        if (!ok) {
                log::error(get_last_error_msg());
                return false;
        }

        // Allow a brief settling window for active TCP commands to complete and sockets to tear down Poll until the previous TCP client socket is
        // fully closed/released and ready for a new connection
       
        
         std::this_thread::sleep_for(std::chrono::milliseconds(20)); 
        if (true)
        {
          if (args.port == vhci::port_all_closeonly) {
            log::debug("all network connections are closed");
          } else if (args.port <= 0) {
            log::debug("all ports are detached");
          } else {
            log::debug("port {} is successfully detached", args.port);
          }

        }
        



        // Invoke custom detach via TCP once the port/connection is ready
        if (imported ) {
                for (const auto &item : *imported) {
                         rtc_data_channel *channel = get_active_rtc_channel();

                        log::debug("Custom detach invoked via TCP for BUSID {}",
                                   item.location.busid);

                           if (!custom::request_status(channel,
                                                    USBIP_CMD_CUSTOM_DETACH,
                                                    item.location.busid)) {
                          log::error(
                              "Remote DETACH cleanup failed for BUSID {}: {}",
                              item.location.busid, get_last_error_msg());
                        }

                        // Using TCP request_status overload/implementation
           /*             if (!custom::request_status(
                                item.location.hostname, item.location.service,
                                                    USBIP_CMD_CUSTOM_DETACH, item.location.busid)) {
                                log::error("Remote DETACH cleanup failed for {}:{} BUSID {}: {}",
                                           item.location.hostname, item.location.service,
                                           item.location.busid, get_last_error_msg());
                        } else {
                                log::debug("Remote DETACH cleanup succeeded for BUSID {}", item.location.busid);
                        }*/
                }
        }



        return true;
}
