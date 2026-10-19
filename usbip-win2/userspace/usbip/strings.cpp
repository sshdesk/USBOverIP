/*
 * Copyright (c) 2021-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "strings.h"

#include <libusbip/src/usb_ids.h>
#include <format>
#include <utility>
#include <algorithm>
#include <cctype>

namespace
{

constexpr auto &str_zero_copy = "zero-copy";
constexpr auto &str_low_latency = "low-latency";

} // namespace

const char* usbip::to_string(receive_mode mode) noexcept
{
        return mode == receive_mode::low_latency ? str_low_latency : str_zero_copy;
}

const char* usbip::get_speed_str(USB_DEVICE_SPEED speed) noexcept
{
        static constexpr const char *names[] {
                "Low Speed(1.5Mbps)",
                "Full Speed(12Mbps)",
                "High Speed(480Mbps)",
                "Super Speed(5000Mbps)",
        };

        static_assert(UsbLowSpeed == 0);
        static_assert(UsbFullSpeed == 1);
        static_assert(UsbHighSpeed == 2);
        static_assert(UsbSuperSpeed == 3);

        auto idx = std::to_underlying(speed);
        return idx >= 0 && idx < std::ssize(names) ? names[idx] : "Unknown Speed";
}

std::string usbip::get_product(const UsbIds &ids, uint16_t vendor, uint16_t product)
{
        auto [vend, prod] = ids.find_product(vendor, product);
        return std::format("{} : {} ({:04x}:{:04x})",
                vend.empty() ? "unknown vendor" : vend,
                prod.empty() ? "unknown product" : prod,
                vendor, product);
}

std::string usbip::get_class(const UsbIds &ids, uint8_t class_, uint8_t subclass, uint8_t protocol)
{
        if (!(class_ || subclass || protocol)) {
                return "(Defined at Interface level) (00/00/00)";
        }

        auto [c, s, p] = ids.find_class_subclass_proto(class_, subclass, protocol);
        return std::format("{}/{}/{} ({:02x}/{:02x}/{:02x})",
                c.empty() ? "?" : c,
                s.empty() ? "?" : s,
                p.empty() ? "?" : p,
                class_, subclass, protocol);
}


std::string usbip::get_device_type(const UsbIds &ids, uint16_t vendor, uint16_t product,
                                   uint8_t class_, uint8_t subclass, uint8_t protocol)
{
        // Prefer the USB class descriptor when the device-level class is present.
        if (class_ == 0x03) {
                if (protocol == 0x02) return "Mouse (HID)";
                if (protocol == 0x01) return "Keyboard (HID)";
                return "Human interface device (HID)";
        }
        if (class_ == 0x08) return "Mass storage";
        if (class_ == 0x09) return "USB hub";
        if (class_ == 0x01) return "Audio";
        if (class_ == 0x02 || class_ == 0x0a) return "Communications";
        if (class_ == 0x06) return "Imaging";
        if (class_ == 0x07) return "Printer";
        if (class_ == 0x0e) return "Video / camera";

        // Many composite USB devices report class 00 at device level; use the
        // product database and a few well-known IDs as a useful fallback.
        if (vendor == 0x0951 && product == 0x16a1) return "Mass storage (Kingston USB drive)";
        if (vendor == 0x413c && product == 0x2113) return "Keyboard (HID)";
        if (vendor == 0x17ef && product == 0x600e) return "Mouse (HID)";
        if (vendor == 0x0a12 && product == 0x0001) return "Bluetooth adapter";

        auto [vendor_name, product_name] = ids.find_product(vendor, product);
        std::string name(product_name);
        std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
        });
        if (name.find("mouse") != std::string::npos) return "Mouse (HID)";
        if (name.find("keyboard") != std::string::npos) return "Keyboard (HID)";
        if (name.find("mass storage") != std::string::npos ||
            name.find("flash drive") != std::string::npos ||
            name.find("data traveler") != std::string::npos ||
            name.find("datatraveler") != std::string::npos ||
            name.find("storage") != std::string::npos) return "Mass storage";
        if (name.find("bluetooth") != std::string::npos) return "Bluetooth adapter";
        if (name.find("camera") != std::string::npos || name.find("webcam") != std::string::npos)
                return "Video / camera";
        if (name.find("hub") != std::string::npos) return "USB hub";

        if (class_ || subclass || protocol)
                return get_class(ids, class_, subclass, protocol);
        return "Composite / interface-defined (interface class unavailable)";
}
