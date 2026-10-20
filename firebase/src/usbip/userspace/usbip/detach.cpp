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

        // The local detach normally causes the server's import loop to exit and
        // release its capture. CUSTOM_DETACH is sent afterwards as an idempotent
        // cleanup barrier; it must not be used to trigger the disconnect before the
        // local VHCI port has been removed.
        if (imported && args.port != vhci::port_all_closeonly) {
                for (const auto &item : *imported) {
                        const bool selected = args.port <= 0 || item.port == args.port;
                        if (!selected || item.location.hostname.empty() || item.location.busid.empty()) {
                                continue;
                        }

                        if (!custom::request_status(item.location.hostname, item.location.service,
                                                    USBIP_CMD_CUSTOM_DETACH, item.location.busid)) {
                                log::error("Remote DETACH cleanup failed for {}:{} BUSID {}: {}",
                                           item.location.hostname, item.location.service,
                                           item.location.busid, get_last_error_msg());
                        }
                }
        }

        if (args.port == vhci::port_all_closeonly) {
                std::println("all network connections are closed");
        } else if (args.port <= 0) {
                std::println("all ports are detached");
        } else {
                std::println("port {} is successfully detached", args.port);
        }

        return true;
}



#if 0
#include "ctrl_c_guard.h"
#include "log.h"
#include "usbip.h"
#include <libusbip/vhci.h>

#include <print>

bool usbip::cmd_detach(const detach_args &args) {
  auto dev = vhci::open();
  if (!dev) {
    log::error(get_last_error_msg());
    return false;
  }

  ctrl_c_guard guard(dev.get());

  // Perform local VHCI detach directly without cross-module container
  // allocation/destruction
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









/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "ctrl_c_guard.h"
#include "custom_commands.h"
#include "log.h"
#include "usbip.h"
#include <libusbip/vhci.h>

#include <print>
#include <string>

bool usbip::cmd_detach(const detach_args &args) {
  auto dev = vhci::open();
  if (!dev) {
    log::error(get_last_error_msg());
    return false;
  }

  ctrl_c_guard guard(dev.get());

  // Safely query and send custom detach to remote usbipd server
  try {
    auto imported = vhci::get_imported_devices(dev.get());
    if (imported.has_value()) {
      for (const auto &item : *imported) {
        const bool selected = (args.port <= 0) || (item.port == args.port);
        if (!selected) {
          continue;
        }

        // Copy strings locally to isolate memory ownership and prevent heap
        // corruption
        std::string host = item.location.hostname;
        std::string service = item.location.service;
        std::string busid = item.location.busid;

        if (host.empty() || busid.empty()) {
          continue;
        }

        if (service.empty()) {
          service = "3240";
        }

        bool req_ok = false;
        try {
          req_ok = custom::request_status(host, service,
                                          USBIP_CMD_CUSTOM_DETACH, busid);
        } catch (...) {
          req_ok = false;
        }

        if (!req_ok) {
          log::error("Remote DETACH failed for {}:{} BUSID {}: {}", host,
                     service, busid, get_last_error_msg());
        }
      }
    }
  } catch (const std::exception &e) {
    log::error("Exception during remote detach notification: {}", e.what());
  } catch (...) {
    log::error("Unknown exception during remote detach notification.");
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

#endif
