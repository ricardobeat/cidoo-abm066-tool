module cidoo_clock_c3;

import std::core::mem;
import std::io;
import std::time;
import std::time::clock;

const int K_IOHID_OPTIONS_TYPE_NONE = 0;
const int K_IOHID_REPORT_TYPE_OUTPUT = 1;
const int K_IOHID_REPORT_TYPE_FEATURE = 2;
const int K_CF_NUMBER_INT_TYPE = 9;
const uint K_CF_STRING_ENCODING_UTF8 = 0x08000100;
const int K_IO_RETURN_SUCCESS = 0;

alias CFIndex = long;
alias CFAllocatorRef = void*;
alias CFStringRef = void*;
alias CFNumberRef = void*;
alias CFMutableDictionaryRef = void*;
alias CFSetRef = void*;
alias CFRunLoopRef = void*;
alias IOHIDManagerRef = void*;
alias IOHIDDeviceRef = void*;
alias IOReturn = int;
alias InputReportCallback = fn void(void* context, IOReturn result, void* sender, int report_type, uint report_id, char* report, CFIndex report_len);

extern fn CFStringRef CFStringCreateWithCString(CFAllocatorRef allocator, char* cstr, uint encoding);
extern fn CFNumberRef CFNumberCreate(CFAllocatorRef allocator, int number_type, void* value);
extern fn CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef allocator, CFIndex capacity, void* key_callbacks, void* value_callbacks);
extern fn void CFDictionarySetValue(CFMutableDictionaryRef dict, void* key, void* value);
extern fn CFIndex CFSetGetCount(CFSetRef set);
extern fn void CFSetGetValues(CFSetRef set, void** values);
extern fn void* CFRetain(void* value);
extern fn void CFRelease(void* value);
extern fn CFRunLoopRef CFRunLoopGetCurrent();
extern fn int CFRunLoopRunInMode(CFStringRef mode, double seconds, bool return_after_source_handled);

extern fn IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef allocator, int options);
extern fn void IOHIDManagerSetDeviceMatching(IOHIDManagerRef manager, CFMutableDictionaryRef matching);
extern fn IOReturn IOHIDManagerOpen(IOHIDManagerRef manager, int options);
extern fn void IOHIDManagerClose(IOHIDManagerRef manager, int options);
extern fn CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef manager);
extern fn IOReturn IOHIDDeviceOpen(IOHIDDeviceRef device, int options);
extern fn void IOHIDDeviceClose(IOHIDDeviceRef device, int options);
extern fn IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef device, int report_type, uint report_id, char* report, CFIndex report_len);
extern fn IOReturn IOHIDDeviceGetReport(IOHIDDeviceRef device, int report_type, uint report_id, char* report, CFIndex* report_len);
extern fn void IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef device, char* report, CFIndex report_len, InputReportCallback callback, void* context);
extern fn void IOHIDDeviceScheduleWithRunLoop(IOHIDDeviceRef device, CFRunLoopRef run_loop, CFStringRef mode);
extern fn void IOHIDDeviceUnscheduleFromRunLoop(IOHIDDeviceRef device, CFRunLoopRef run_loop, CFStringRef mode);

struct MacosIokitTransport {
    IOHIDManagerRef manager;
    IOHIDDeviceRef device;
    char[REPORT_SIZE] input_report;
    char[REPORT_SIZE] response;
    usz response_len;
    char expected_cmd;
    char report_id;
    bool got_response;
    bool strip_report_id_for_iohid;
    bool device_opened;
}

fn int transport_list_devices(Options* opt, bool include_usage) {
    IOHIDManagerRef manager = create_manager_with_match(opt, include_usage);
    if (manager == null) return 1;
    IOReturn open_rc = IOHIDManagerOpen(manager, K_IOHID_OPTIONS_TYPE_NONE);
    if (open_rc != K_IO_RETURN_SUCCESS) {
        io::printfn("IOHIDManagerOpen failed: 0x%08x", (uint)open_rc);
        CFRelease(manager);
        return 1;
    }
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices == null) {
        io::printfn("No matching HID devices found.");
        IOHIDManagerClose(manager, K_IOHID_OPTIONS_TYPE_NONE);
        CFRelease(manager);
        return 1;
    }
    CFIndex count = CFSetGetCount(devices);
    io::printfn("Found %ld matching HID device(s).", count);
    CFRelease(devices);
    IOHIDManagerClose(manager, K_IOHID_OPTIONS_TYPE_NONE);
    CFRelease(manager);
    return count > 0 ? 0 : 1;
}

fn int transport_open(ClockTransport* transport, Options* opt) {
    *transport = {};
    MacosIokitTransport* macos = mem::new(MacosIokitTransport);
    transport.kind = MACOS_IOKIT;
    transport.impl = macos;
    macos.report_id = opt.report_id;
    macos.strip_report_id_for_iohid = opt.strip_report_id_for_iohid;

    macos.manager = create_manager_with_match(opt, true);
    if (macos.manager == null) {
        transport_close(transport);
        return -1;
    }

    IOReturn open_rc = IOHIDManagerOpen(macos.manager, K_IOHID_OPTIONS_TYPE_NONE);
    if (open_rc != K_IO_RETURN_SUCCESS) {
        io::printfn("IOHIDManagerOpen failed: 0x%08x", (uint)open_rc);
        transport_close(transport);
        return -1;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(macos.manager);
    if (devices == null || CFSetGetCount(devices) == 0) {
        io::printfn("No matching HID device found");
        if (devices != null) CFRelease(devices);
        transport_close(transport);
        return -1;
    }

    CFIndex count = CFSetGetCount(devices);
    void** refs = mem::new_array(void*, (sz)count);
    CFSetGetValues(devices, refs);
    macos.device = (IOHIDDeviceRef)refs[0];
    CFRetain(macos.device);
    mem::free(refs);
    CFRelease(devices);

    IOReturn dev_rc = IOHIDDeviceOpen(macos.device, K_IOHID_OPTIONS_TYPE_NONE);
    if (dev_rc != K_IO_RETURN_SUCCESS) {
        io::printfn("IOHIDDeviceOpen failed: 0x%08x", (uint)dev_rc);
        transport_close(transport);
        return -1;
    }
    macos.device_opened = true;

    IOHIDDeviceRegisterInputReportCallback(macos.device, &macos.input_report, REPORT_SIZE, &input_report_callback, macos);
    CFStringRef mode = cfstr("kCFRunLoopDefaultMode");
    IOHIDDeviceScheduleWithRunLoop(macos.device, CFRunLoopGetCurrent(), mode);
    CFRelease(mode);
    return 0;
}

fn void transport_close(ClockTransport* transport) {
    MacosIokitTransport* macos = transport_macos(transport);
    if (macos == null) {
        *transport = {};
        return;
    }
    if (macos.device != null) {
        if (macos.device_opened) {
            CFStringRef mode = cfstr("kCFRunLoopDefaultMode");
            IOHIDDeviceUnscheduleFromRunLoop(macos.device, CFRunLoopGetCurrent(), mode);
            CFRelease(mode);
            IOHIDDeviceClose(macos.device, K_IOHID_OPTIONS_TYPE_NONE);
        }
        CFRelease(macos.device);
    }
    if (macos.manager != null) {
        IOHIDManagerClose(macos.manager, K_IOHID_OPTIONS_TYPE_NONE);
        CFRelease(macos.manager);
    }
    mem::free(macos);
    *transport = {};
}

fn int transport_get_feature_report(ClockTransport* transport, char report_id, char* report, usz* report_len) {
    MacosIokitTransport* macos = transport_macos(transport);
    if (macos == null) return -1;
    CFIndex len = (CFIndex)*report_len;
    IOReturn rc = IOHIDDeviceGetReport(macos.device, K_IOHID_REPORT_TYPE_FEATURE, (uint)report_id, report, &len);
    *report_len = (usz)len;
    if (rc != K_IO_RETURN_SUCCESS) {
        io::printfn("IOHIDDeviceGetReport feature report 0x%02x failed: 0x%08x", (uint)report_id, (uint)rc);
        return -1;
    }
    return 0;
}

fn int transport_send_wait_checked(ClockTransport* transport, char* packet, char expected_cmd, int timeout_ms, char* response, usz* response_len, bool check_status) {
    MacosIokitTransport* macos = transport_macos(transport);
    if (macos == null) return -1;
    macos.expected_cmd = expected_cmd;
    macos.got_response = false;
    macos.response_len = 0;
    clear_bytes(macos.response[..]);
    clear_bytes(macos.input_report[..]);

    char* report_data = packet;
    CFIndex report_len = REPORT_SIZE;
    if (macos.strip_report_id_for_iohid) {
        report_data = packet + 1;
        report_len = REPORT_SIZE - 1;
    }

    IOReturn rc = IOHIDDeviceSetReport(macos.device, K_IOHID_REPORT_TYPE_OUTPUT, (uint)macos.report_id, report_data, report_len);
    if (rc != K_IO_RETURN_SUCCESS) {
        io::printfn("IOHIDDeviceSetReport cmd=0x%02x failed: 0x%08x", (uint)expected_cmd, (uint)rc);
        return -1;
    }

    CFStringRef mode = cfstr("kCFRunLoopDefaultMode");
    Clock deadline = clock::now() + time::ms((long)timeout_ms);
    while (!macos.got_response && clock::now() < deadline) {
        CFRunLoopRunInMode(mode, 0.01, true);
    }
    CFRelease(mode);

    if (!macos.got_response) {
        io::printfn("Timed out waiting for response to cmd=0x%02x", (uint)expected_cmd);
        return -1;
    }

    if (response != null && response_len != null) {
        for (usz i = 0; i < macos.response_len; i++) response[i] = macos.response[i];
        *response_len = macos.response_len;
    }

    if (check_status && macos.response_len >= 8) {
        if (macos.response[7] == 0xff) {
            io::printfn("Device returned status 0xff for cmd=0x%02x", (uint)expected_cmd);
            return -1;
        }
        if (macos.response[7] == 0xfe) {
            io::printfn("Device returned status 0xfe for cmd=0x%02x", (uint)expected_cmd);
            return -1;
        }
    }
    return 0;
}

fn int transport_send_wait(ClockTransport* transport, char* packet, char expected_cmd, int timeout_ms, char* response, usz* response_len) {
    return transport_send_wait_checked(transport, packet, expected_cmd, timeout_ms, response, response_len, true);
}

fn MacosIokitTransport* transport_macos(ClockTransport* transport) {
    if (transport == null || transport.kind != MACOS_IOKIT || transport.impl == null) {
        return null;
    }
    return (MacosIokitTransport*)transport.impl;
}

fn CFStringRef cfstr(char* text) {
    return CFStringCreateWithCString(null, text, K_CF_STRING_ENCODING_UTF8);
}

fn void dict_set_int(CFMutableDictionaryRef dict, char* key_text, int value) {
    CFStringRef key = cfstr(key_text);
    CFNumberRef number = CFNumberCreate(null, K_CF_NUMBER_INT_TYPE, &value);
    if (key != null && number != null) {
        CFDictionarySetValue(dict, key, number);
    }
    if (number != null) CFRelease(number);
    if (key != null) CFRelease(key);
}

fn CFMutableDictionaryRef create_matching_dict(Options* opt, bool include_usage) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(null, 0, null, null);
    if (dict == null) return null;
    dict_set_int(dict, "VendorID", (int)opt.vid);
    dict_set_int(dict, "ProductID", (int)opt.pid);
    if (include_usage) {
        dict_set_int(dict, "PrimaryUsagePage", (int)opt.usage_page);
        dict_set_int(dict, "PrimaryUsage", (int)opt.usage);
    }
    return dict;
}

fn IOHIDManagerRef create_manager_with_match(Options* opt, bool include_usage) {
    IOHIDManagerRef manager = IOHIDManagerCreate(null, K_IOHID_OPTIONS_TYPE_NONE);
    if (manager == null) {
        io::printfn("IOHIDManagerCreate failed");
        return null;
    }
    CFMutableDictionaryRef match = create_matching_dict(opt, include_usage);
    if (match == null) {
        CFRelease(manager);
        io::printfn("Could not create HID matching dictionary");
        return null;
    }
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    return manager;
}

fn void input_report_callback(void* context, IOReturn result, void* sender, int report_type, uint report_id, char* report, CFIndex report_len) {
    MacosIokitTransport* macos = (MacosIokitTransport*)context;
    if (result != K_IO_RETURN_SUCCESS || report_len <= 0) return;

    char[REPORT_SIZE] normalized;
    clear_bytes(normalized[..]);
    usz normalized_len = 0;

    if (report[0] == macos.report_id) {
        normalized_len = report_len > REPORT_SIZE ? REPORT_SIZE : (usz)report_len;
        for (usz i = 0; i < normalized_len; i++) normalized[i] = report[i];
    } else {
        normalized[0] = (char)report_id;
        usz copy_len = report_len > REPORT_SIZE - 1 ? REPORT_SIZE - 1 : (usz)report_len;
        for (usz i = 0; i < copy_len; i++) normalized[i + 1] = report[i];
        normalized_len = copy_len + 1;
    }

    if (normalized_len >= 4 &&
        (macos.expected_cmd == 0xff || normalized[3] == macos.expected_cmd)) {
        for (usz i = 0; i < normalized_len; i++) macos.response[i] = normalized[i];
        macos.response_len = normalized_len;
        macos.got_response = true;
    }
}
