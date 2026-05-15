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
alias CFTypeID = ulong;
alias IOHIDManagerRef = void*;
alias IOHIDDeviceRef = void*;
alias IOReturn = int;
alias InputReportCallback = fn void(void* context, IOReturn result, void* sender, int report_type, uint report_id, char* report, CFIndex report_len);

extern fn CFStringRef cf_string_create_with_cstring(CFAllocatorRef allocator, char* cstr, uint encoding) @cname("CFStringCreateWithCString");
extern fn CFNumberRef cf_number_create(CFAllocatorRef allocator, int number_type, void* value) @cname("CFNumberCreate");
extern fn CFMutableDictionaryRef cf_dictionary_create_mutable(CFAllocatorRef allocator, CFIndex capacity, void* key_callbacks, void* value_callbacks) @cname("CFDictionaryCreateMutable");
extern fn void cf_dictionary_set_value(CFMutableDictionaryRef dict, void* key, void* value) @cname("CFDictionarySetValue");
extern fn CFIndex cf_set_get_count(CFSetRef set) @cname("CFSetGetCount");
extern fn void cf_set_get_values(CFSetRef set, void** values) @cname("CFSetGetValues");
extern fn void* cf_retain(void* value) @cname("CFRetain");
extern fn void cf_release(void* value) @cname("CFRelease");
extern fn CFTypeID cf_get_type_id(void* value) @cname("CFGetTypeID");
extern fn CFTypeID cf_number_get_type_id() @cname("CFNumberGetTypeID");
extern fn char cf_number_get_value(CFNumberRef number, int number_type, void* value) @cname("CFNumberGetValue");
extern fn CFRunLoopRef cf_run_loop_get_current() @cname("CFRunLoopGetCurrent");
extern fn int cf_run_loop_run_in_mode(CFStringRef mode, double seconds, bool return_after_source_handled) @cname("CFRunLoopRunInMode");

extern fn IOHIDManagerRef io_hid_manager_create(CFAllocatorRef allocator, int options) @cname("IOHIDManagerCreate");
extern fn void io_hid_manager_set_device_matching(IOHIDManagerRef manager, CFMutableDictionaryRef matching) @cname("IOHIDManagerSetDeviceMatching");
extern fn IOReturn io_hid_manager_open(IOHIDManagerRef manager, int options) @cname("IOHIDManagerOpen");
extern fn void io_hid_manager_close(IOHIDManagerRef manager, int options) @cname("IOHIDManagerClose");
extern fn CFSetRef io_hid_manager_copy_devices(IOHIDManagerRef manager) @cname("IOHIDManagerCopyDevices");
extern fn IOReturn io_hid_device_open(IOHIDDeviceRef device, int options) @cname("IOHIDDeviceOpen");
extern fn void io_hid_device_close(IOHIDDeviceRef device, int options) @cname("IOHIDDeviceClose");
extern fn IOReturn io_hid_device_set_report(IOHIDDeviceRef device, int report_type, uint report_id, char* report, CFIndex report_len) @cname("IOHIDDeviceSetReport");
extern fn IOReturn io_hid_device_get_report(IOHIDDeviceRef device, int report_type, uint report_id, char* report, CFIndex* report_len) @cname("IOHIDDeviceGetReport");
extern fn void* io_hid_device_get_property(IOHIDDeviceRef device, CFStringRef key) @cname("IOHIDDeviceGetProperty");
extern fn void io_hid_device_register_input_report_callback(IOHIDDeviceRef device, char* report, CFIndex report_len, InputReportCallback callback, void* context) @cname("IOHIDDeviceRegisterInputReportCallback");
extern fn void io_hid_device_schedule_with_run_loop(IOHIDDeviceRef device, CFRunLoopRef run_loop, CFStringRef mode) @cname("IOHIDDeviceScheduleWithRunLoop");
extern fn void io_hid_device_unschedule_from_run_loop(IOHIDDeviceRef device, CFRunLoopRef run_loop, CFStringRef mode) @cname("IOHIDDeviceUnscheduleFromRunLoop");
extern char k_cf_type_dictionary_key_callbacks @cname("kCFTypeDictionaryKeyCallBacks");
extern char k_cf_type_dictionary_value_callbacks @cname("kCFTypeDictionaryValueCallBacks");
extern CFStringRef k_cf_run_loop_default_mode @cname("kCFRunLoopDefaultMode");

struct MacosIokitTransport {
    IOHIDManagerRef manager;
    IOHIDDeviceRef device;
    char[REPORT_SIZE] input_report;
    char[REPORT_SIZE] response;
    sz response_len;
    char expected_cmd;
    char report_id;
    bool got_response;
    bool closing;
    bool device_opened;
}

fn int transport_list_devices(Options* opt @unused, bool include_usage) {
    IOHIDManagerRef manager = create_manager_with_match(include_usage);
    if (manager == null) return 1;
    IOReturn open_rc = io_hid_manager_open(manager, K_IOHID_OPTIONS_TYPE_NONE);
    if (open_rc != K_IO_RETURN_SUCCESS) {
        io::printfn("io_hid_manager_open failed: 0x%08x", (uint)open_rc);
        cf_release(manager);
        return 1;
    }
    CFSetRef devices = io_hid_manager_copy_devices(manager);
    if (devices == null) {
        io::printfn("No matching HID devices found.");
        io_hid_manager_close(manager, K_IOHID_OPTIONS_TYPE_NONE);
        cf_release(manager);
        return 1;
    }
    CFIndex count = cf_set_get_count(devices);
    io::printfn("Found %d matching HID device(s).", count);
    cf_release(devices);
    io_hid_manager_close(manager, K_IOHID_OPTIONS_TYPE_NONE);
    cf_release(manager);
    return count > 0 ? 0 : 1;
}

fn int transport_open(ClockTransport* transport, Options* opt) {
    *transport = {};
    MacosIokitTransport* macos = mem::new(MacosIokitTransport);
    *macos = {};
    transport.kind = MACOS_IOKIT;
    transport.impl = macos;
    macos.report_id = DEFAULT_REPORT_ID;

    macos.manager = create_manager_with_match(true);
    if (macos.manager == null) {
        transport_close(transport);
        return -1;
    }

    IOReturn open_rc = io_hid_manager_open(macos.manager, K_IOHID_OPTIONS_TYPE_NONE);
    if (open_rc != K_IO_RETURN_SUCCESS) {
        io::printfn("io_hid_manager_open failed: 0x%08x", (uint)open_rc);
        transport_close(transport);
        return -1;
    }

    CFSetRef devices = io_hid_manager_copy_devices(macos.manager);
    if (devices == null || cf_set_get_count(devices) == 0) {
        io::printfn("No matching HID device found");
        if (devices != null) cf_release(devices);
        transport_close(transport);
        return -1;
    }

    CFIndex count = cf_set_get_count(devices);
    void** refs = mem::new_array(void*, (sz)count);
    cf_set_get_values(devices, refs);
    macos.device = (IOHIDDeviceRef)refs[0];
    for (sz i = 0; i < (sz)count; i++) {
        IOHIDDeviceRef candidate = (IOHIDDeviceRef)refs[i];
        int max_input = get_int_property(candidate, "MaxInputReportSize", -1);
        int max_output = get_int_property(candidate, "MaxOutputReportSize", -1);
        if (opt.debug) {
            io::printfn("HID candidate %d max_in=%d max_out=%d", i, max_input, max_output);
        }
        if (max_input >= REPORT_SIZE && max_output >= REPORT_SIZE) {
            macos.device = candidate;
            break;
        }
    }
    cf_retain(macos.device);
    mem::free(refs);
    cf_release(devices);

    IOReturn dev_rc = io_hid_device_open(macos.device, K_IOHID_OPTIONS_TYPE_NONE);
    if (dev_rc != K_IO_RETURN_SUCCESS) {
        io::printfn("io_hid_device_open failed: 0x%08x", (uint)dev_rc);
        transport_close(transport);
        return -1;
    }
    macos.device_opened = true;

    io_hid_device_register_input_report_callback(macos.device, &macos.input_report, REPORT_SIZE, &input_report_callback, macos);
    io_hid_device_schedule_with_run_loop(macos.device, cf_run_loop_get_current(), k_cf_run_loop_default_mode);
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
            macos.closing = true;
            io_hid_device_unschedule_from_run_loop(macos.device, cf_run_loop_get_current(), k_cf_run_loop_default_mode);
            io_hid_device_register_input_report_callback(macos.device, null, 0, null, null);
            cf_run_loop_run_in_mode(k_cf_run_loop_default_mode, 0.01, true);
            io_hid_device_close(macos.device, K_IOHID_OPTIONS_TYPE_NONE);
        }
        cf_release(macos.device);
    }
    if (macos.manager != null) {
        io_hid_manager_close(macos.manager, K_IOHID_OPTIONS_TYPE_NONE);
        cf_release(macos.manager);
    }
    mem::free(macos);
    *transport = {};
}

fn int transport_get_feature_report(ClockTransport* transport, char report_id, char* report, sz* report_len) {
    MacosIokitTransport* macos = transport_macos(transport);
    if (macos == null) return -1;
    CFIndex len = (CFIndex)*report_len;
    IOReturn rc = io_hid_device_get_report(macos.device, K_IOHID_REPORT_TYPE_FEATURE, (uint)report_id, report, &len);
    *report_len = (sz)len;
    if (rc != K_IO_RETURN_SUCCESS) {
        io::printfn("io_hid_device_get_report feature report 0x%02x failed: 0x%08x", (uint)report_id, (uint)rc);
        return -1;
    }
    return 0;
}

fn int transport_send_wait_checked(ClockTransport* transport, char* packet, char expected_cmd, int timeout_ms, char* response, sz* response_len, bool check_status) {
    MacosIokitTransport* macos = transport_macos(transport);
    if (macos == null) return -1;
    macos.expected_cmd = expected_cmd;
    macos.got_response = false;
    macos.response_len = 0;
    clear_bytes(macos.response[..]);
    clear_bytes(macos.input_report[..]);

    char* report_data = packet;
    CFIndex report_len = REPORT_SIZE;

    IOReturn rc = io_hid_device_set_report(macos.device, K_IOHID_REPORT_TYPE_OUTPUT, (uint)macos.report_id, report_data, report_len);
    if (rc != K_IO_RETURN_SUCCESS) {
        io::printfn("io_hid_device_set_report cmd=0x%02x failed: 0x%08x", (uint)expected_cmd, (uint)rc);
        return -1;
    }

    Clock deadline = clock::now() + time::ms((long)timeout_ms);
    while (!macos.got_response && clock::now() < deadline) {
        cf_run_loop_run_in_mode(k_cf_run_loop_default_mode, 0.01, true);
    }

    if (!macos.got_response) {
        io::printfn("Timed out waiting for response to cmd=0x%02x", (uint)expected_cmd);
        return -1;
    }

    if (response != null && response_len != null) {
        sz copy_len = macos.response_len;
        if (copy_len > REPORT_SIZE) copy_len = REPORT_SIZE;
        for (sz i = 0; i < copy_len; i++) response[i] = macos.response[i];
        *response_len = copy_len;
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

fn int transport_send_wait(ClockTransport* transport, char* packet, char expected_cmd, int timeout_ms, char* response, sz* response_len) {
    return transport_send_wait_checked(transport, packet, expected_cmd, timeout_ms, response, response_len, true);
}

fn int transport_send_wait_ignoring_response_error(ClockTransport* transport, char* packet, char expected_cmd, int timeout_ms, char* response, sz* response_len, bool debug, bool* timed_out) {
    MacosIokitTransport* macos = transport_macos(transport);
    if (macos == null) return -1;
    if (timed_out != null) *timed_out = false;
    macos.expected_cmd = expected_cmd;
    macos.got_response = false;
    macos.response_len = 0;
    clear_bytes(macos.response[..]);
    clear_bytes(macos.input_report[..]);

    IOReturn rc = io_hid_device_set_report(macos.device, K_IOHID_REPORT_TYPE_OUTPUT, (uint)macos.report_id, packet, REPORT_SIZE);
    if (rc != K_IO_RETURN_SUCCESS) {
        io::printfn("io_hid_device_set_report cmd=0x%02x failed: 0x%08x", (uint)expected_cmd, (uint)rc);
        return -1;
    }

    Clock deadline = clock::now() + time::ms((long)timeout_ms);
    while (!macos.got_response && clock::now() < deadline) {
        cf_run_loop_run_in_mode(k_cf_run_loop_default_mode, 0.01, true);
    }

    if (!macos.got_response) {
        if (timed_out != null) *timed_out = true;
        if (debug) io::printfn("Ignoring timeout waiting for response to cmd=0x%02x", (uint)expected_cmd);
        return 0;
    }

    if (response != null && response_len != null) {
        sz copy_len = macos.response_len;
        if (copy_len > REPORT_SIZE) copy_len = REPORT_SIZE;
        for (sz i = 0; i < copy_len; i++) response[i] = macos.response[i];
        *response_len = copy_len;
    }

    if (macos.response_len >= 8 && (macos.response[7] == 0xff || macos.response[7] == 0xfe)) {
        if (debug) {
            io::printfn("Ignoring status 0x%02x for cmd=0x%02x", (uint)macos.response[7], (uint)expected_cmd);
        }
        return 0;
    }
    return 0;
}

fn MacosIokitTransport* transport_macos(ClockTransport* transport) {
    if (transport == null || transport.kind != MACOS_IOKIT || transport.impl == null) {
        return null;
    }
    return (MacosIokitTransport*)transport.impl;
}

fn CFStringRef cfstr(char* text) {
    return cf_string_create_with_cstring(null, text, K_CF_STRING_ENCODING_UTF8);
}

fn void dict_set_int(CFMutableDictionaryRef dict, char* key_text, int value) {
    CFStringRef key = cfstr(key_text);
    CFNumberRef number = cf_number_create(null, K_CF_NUMBER_INT_TYPE, &value);
    if (key != null && number != null) {
        cf_dictionary_set_value(dict, key, number);
    }
    if (number != null) cf_release(number);
    if (key != null) cf_release(key);
}

fn int get_int_property(IOHIDDeviceRef device, char* key_text, int fallback) {
    CFStringRef key = cfstr(key_text);
    if (key == null) return fallback;
    void* value = io_hid_device_get_property(device, key);
    cf_release(key);
    if (value == null || cf_get_type_id(value) != cf_number_get_type_id()) return fallback;
    int out = fallback;
    if (cf_number_get_value((CFNumberRef)value, K_CF_NUMBER_INT_TYPE, &out) == 0) return fallback;
    return out;
}

fn CFMutableDictionaryRef create_matching_dict(bool include_usage) {
    CFMutableDictionaryRef dict = cf_dictionary_create_mutable(
        null,
        0,
        &k_cf_type_dictionary_key_callbacks,
        &k_cf_type_dictionary_value_callbacks);
    if (dict == null) return null;
    dict_set_int(dict, "VendorID", (int)DEFAULT_VID);
    dict_set_int(dict, "ProductID", (int)DEFAULT_PID);
    if (include_usage) {
        dict_set_int(dict, "PrimaryUsagePage", (int)DEFAULT_USAGE_PAGE);
        dict_set_int(dict, "PrimaryUsage", (int)DEFAULT_USAGE);
    }
    return dict;
}

fn IOHIDManagerRef create_manager_with_match(bool include_usage) {
    IOHIDManagerRef manager = io_hid_manager_create(null, K_IOHID_OPTIONS_TYPE_NONE);
    if (manager == null) {
        io::printfn("io_hid_manager_create failed");
        return null;
    }
    CFMutableDictionaryRef match = create_matching_dict(include_usage);
    if (match == null) {
        cf_release(manager);
        io::printfn("Could not create HID matching dictionary");
        return null;
    }
    io_hid_manager_set_device_matching(manager, match);
    cf_release(match);
    return manager;
}

fn void input_report_callback(void* context, IOReturn result, void* sender, int report_type, uint report_id, char* report, CFIndex report_len) {
    MacosIokitTransport* macos = (MacosIokitTransport*)context;
    if (macos == null || macos.closing || result != K_IO_RETURN_SUCCESS || report == null || report_len <= 0) return;

    char[REPORT_SIZE] normalized;
    clear_bytes(normalized[..]);
    sz normalized_len = 0;
    sz raw_len = report_len > (CFIndex)REPORT_SIZE ? (sz)REPORT_SIZE : (sz)report_len;
    if (raw_len == 0) return;

    if (report[0] == macos.report_id) {
        normalized_len = raw_len;
        for (sz i = 0; i < normalized_len; i++) normalized[i] = report[i];
    } else {
        normalized[0] = (char)report_id;
        sz copy_len = raw_len > REPORT_SIZE - 1 ? REPORT_SIZE - 1 : raw_len;
        for (sz i = 0; i < copy_len; i++) normalized[i + 1] = report[i];
        normalized_len = copy_len + 1;
    }

    if (normalized_len >= 4 &&
        (macos.expected_cmd == 0xff || normalized[3] == macos.expected_cmd)) {
        for (sz i = 0; i < normalized_len; i++) macos.response[i] = normalized[i];
        macos.response_len = normalized_len;
        macos.got_response = true;
    }
}
