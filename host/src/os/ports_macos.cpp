#include "os/ports.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace malgrind::host::os {

namespace {

struct CfRef {
    CFTypeRef ref = nullptr;
    explicit CfRef(CFTypeRef r = nullptr) noexcept : ref(r) {}
    ~CfRef() {
        if (ref != nullptr) CFRelease(ref);
    }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
};

struct IoObj {
    io_object_t obj = IO_OBJECT_NULL;
    explicit IoObj(io_object_t o = IO_OBJECT_NULL) noexcept : obj(o) {}
    ~IoObj() {
        if (obj != IO_OBJECT_NULL) IOObjectRelease(obj);
    }
    IoObj(const IoObj&) = delete;
    IoObj& operator=(const IoObj&) = delete;
};

std::optional<std::string> cf_string(CFTypeRef ref) {
    if (ref == nullptr || CFGetTypeID(ref) != CFStringGetTypeID()) return std::nullopt;
    const auto str = static_cast<CFStringRef>(ref);
    if (const char* fast = CFStringGetCStringPtr(str, kCFStringEncodingUTF8)) {
        return std::string(fast);
    }
    const CFIndex len = CFStringGetLength(str);
    const CFIndex max_bytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    if (max_bytes <= 0) return std::string{};
    std::string out(static_cast<std::size_t>(max_bytes), '\0');
    if (!CFStringGetCString(str, out.data(), max_bytes, kCFStringEncodingUTF8)) return std::nullopt;
    out.resize(std::strlen(out.data()));
    return out;
}

std::optional<std::uint32_t> cf_uint32(CFTypeRef ref) {
    if (ref == nullptr || CFGetTypeID(ref) != CFNumberGetTypeID()) return std::nullopt;
    std::int64_t val = 0;
    // Read as 64-bit signed so unsigned 32-bit locationID values with bit 31 set never overflow.
    if (!CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt64Type, &val)) return std::nullopt;
    return static_cast<std::uint32_t>(val);
}

std::optional<std::string> search_usb_string(io_service_t service, CFStringRef key) {
    const CfRef prop(IORegistryEntrySearchCFProperty(
        service, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents));
    return cf_string(prop.ref);
}

std::optional<std::uint32_t> search_usb_uint32(io_service_t service, CFStringRef key) {
    const CfRef prop(IORegistryEntrySearchCFProperty(
        service, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents));
    return cf_uint32(prop.ref);
}

} // namespace

std::vector<PortInfo> list_serial_ports() {
    CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
    if (matching == nullptr) return {};

    io_iterator_t raw_iter = IO_OBJECT_NULL;
    // IOServiceGetMatchingServices consumes one reference to `matching` even on error.
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &raw_iter) != KERN_SUCCESS) return {};
    const IoObj iter(raw_iter);

    std::vector<PortInfo> ports;
    for (;;) {
        const IoObj service(IOIteratorNext(iter.obj));
        if (service.obj == IO_OBJECT_NULL) break;

        const CfRef callout_ref(
            IORegistryEntryCreateCFProperty(service.obj, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0));
        const auto path = cf_string(callout_ref.ref);
        if (!path || !path->starts_with("/dev/cu.")) continue;

        const auto vid = search_usb_uint32(service.obj, CFSTR("idVendor"));
        if (!vid) continue; // skips Bluetooth and other non-USB serial services

        const auto pid = search_usb_uint32(service.obj, CFSTR("idProduct"));
        const auto serial = search_usb_string(service.obj, CFSTR("USB Serial Number"));
        const auto product = search_usb_string(service.obj, CFSTR("USB Product Name"));
        const auto manufacturer = search_usb_string(service.obj, CFSTR("USB Vendor Name"));
        const auto location_id = search_usb_uint32(service.obj, CFSTR("locationID"));

        UsbInfo usb;
        usb.vid = std::format("{:04x}", *vid & 0xffffu);
        usb.pid = pid ? std::format("{:04x}", *pid & 0xffffu) : std::string{};
        usb.product = product.value_or("");
        usb.manufacturer = manufacturer.value_or("");
        usb.serial = serial.value_or("");
        usb.location = location_id ? std::format("0x{:08x}", *location_id) : std::string{};

        const std::string name = fs::path(*path).filename().string();
        std::string description = usb.product;
        ports.push_back(PortInfo{
            *path,
            name,
            std::move(description),
            std::move(usb),
        });
    }

    std::ranges::sort(ports, {}, &PortInfo::name);
    return ports;
}

} // namespace malgrind::host::os
