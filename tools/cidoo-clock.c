#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CommonCrypto/CommonDigest.h>

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPORT_SIZE 64
#define RECORD_SIZE 48
#define CONFIG_PAYLOAD_OFFSET 8
#define TIME_OFFSET 35
#define TIME_SIZE 7
#define DEFAULT_VID 0x320f
#define DEFAULT_PID 0x5055
#define DEFAULT_USAGE_PAGE 0xff1c
#define DEFAULT_USAGE 0x92
#define DEFAULT_REPORT_ID 0x04

typedef enum {
    CMD_NONE,
    CMD_HELP,
    CMD_LIST,
    CMD_PROBE,
    CMD_GET_FEATURE,
    CMD_READ_RAW,
    CMD_DRY_RUN,
    CMD_READ_TEMPLATE,
    CMD_READ_TEMPLATE_SPLIT,
    CMD_UPDATE_TIME,
    CMD_RESTORE_TEMPLATE,
} command_t;

typedef struct {
    command_t command;
    uint16_t vid;
    uint16_t pid;
    uint16_t usage_page;
    uint16_t usage;
    uint8_t report_id;
    uint8_t raw_command;
    size_t raw_length;
    uint16_t raw_offset;
    bool raw_offset_set;
    int slot;
    int timeout_ms;
    const char *template_hex;
    const char *template_file;
    const char *time_arg;
    const char *out_file;
    const char *expected_template_sha256;
    const char *expected_stable_sha256;
    bool debug;
    bool strip_report_id_for_iohid;
} options_t;

typedef struct {
    IOHIDManagerRef manager;
    IOHIDDeviceRef device;
    uint8_t input_report[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len;
    uint8_t expected_cmd;
    uint8_t report_id;
    bool got_response;
    bool strip_report_id_for_iohid;
} hid_session_t;

static int open_hid_session(const options_t *opt, hid_session_t *session);
static void close_hid_session(hid_session_t *session);
static int send_report_wait(hid_session_t *session,
                            const uint8_t packet[REPORT_SIZE],
                            uint8_t expected_cmd,
                            int timeout_ms,
                            uint8_t response[REPORT_SIZE],
                            size_t *response_len);
static int send_report_wait_checked(hid_session_t *session,
                                    const uint8_t packet[REPORT_SIZE],
                                    uint8_t expected_cmd,
                                    int timeout_ms,
                                    uint8_t response[REPORT_SIZE],
                                    size_t *response_len,
                                    bool check_status);

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage:\n"
        "  cidoo-clock list [options]\n"
        "  cidoo-clock probe [options]\n"
        "  cidoo-clock get-feature [options]\n"
        "  cidoo-clock read-raw --raw-command HEX [options]\n"
        "  cidoo-clock dry-run --template HEX|--template-file PATH [options]\n"
        "  cidoo-clock read-template [options]\n"
        "  cidoo-clock read-template-split [options]\n"
        "  cidoo-clock update-time [options]\n"
        "  cidoo-clock restore-template --template-file PATH [options]\n"
        "\n"
        "Options:\n"
        "  --vid HEX                         USB VID, default 0x%04x\n"
        "  --pid HEX                         USB PID, default 0x%04x\n"
        "  --usage-page HEX                  HID usage page, default 0x%04x\n"
        "  --usage HEX                       HID usage, default 0x%02x\n"
        "  --report-id HEX                   HID report ID, default 0x%02x\n"
        "  --raw-command HEX                 diagnostic read command for read-raw\n"
        "  --raw-length N                    diagnostic payload length, default 48\n"
        "  --raw-offset N                    diagnostic offset, default slot * 0x31\n"
        "  --slot N                          config slot, offset = N * 0x31, default 0\n"
        "  --time now|YYYY-MM-DDTHH:MM:SS    local time to encode, default now\n"
        "  --template HEX                    48-byte template as hex text\n"
        "  --template-file PATH              48-byte raw file or hex text file\n"
        "  --out PATH                        write read-template record to raw file\n"
        "  --expected-template-sha256 HEX    optional full-record update guard\n"
        "  --expected-stable-sha256 HEX      optional guard; time bytes 35..41 masked\n"
        "  --debug                           print hashes, diffs, and HID packets\n"
        "  --timeout-ms N                    HID response timeout, default 1500\n"
        "  --strip-report-id-for-iohid       diagnostic: pass bytes 1..63 to IOKit\n"
        "\n"
        "Safety model:\n"
        "  dry-run never opens HID. read-template sends only command 0x05. update-time\n"
        "  reads the current 48-byte record, patches only bytes 35..41, verifies no\n"
        "  other byte changed, then writes. restore-template reads the current record,\n"
        "  patches the restore template with the current local time, and writes the\n"
        "  resulting 48-byte record. Optional expected-hash flags remain available as\n"
        "  extra guards.\n",
        DEFAULT_VID, DEFAULT_PID, DEFAULT_USAGE_PAGE, DEFAULT_USAGE, DEFAULT_REPORT_ID);
}

static uint16_t parse_u16(const char *s, const char *name) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || value > 0xffffUL) {
        fprintf(stderr, "Invalid %s: %s\n", name, s);
        exit(2);
    }
    return (uint16_t)value;
}

static int parse_int_range(const char *s, const char *name, int min, int max) {
    char *end = NULL;
    errno = 0;
    long value = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || value < min || value > max) {
        fprintf(stderr, "Invalid %s: %s\n", name, s);
        exit(2);
    }
    return (int)value;
}

static bool is_time_offset(size_t i) {
    return i >= TIME_OFFSET && i < TIME_OFFSET + TIME_SIZE;
}

static bool record_all_zero(const uint8_t record[RECORD_SIZE]) {
    for (size_t i = 0; i < RECORD_SIZE; i++) {
        if (record[i] != 0) {
            return false;
        }
    }
    return true;
}

static uint8_t bcd(int value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static int unbcd(uint8_t value) {
    int hi = (value >> 4) & 0x0f;
    int lo = value & 0x0f;
    if (hi > 9 || lo > 9) {
        return -1;
    }
    return hi * 10 + lo;
}

static bool bcd_in_range(uint8_t value, int min, int max) {
    int decoded = unbcd(value);
    return decoded >= min && decoded <= max;
}

static int validate_time_fields(const uint8_t record[RECORD_SIZE],
                                char *why,
                                size_t why_len) {
    if (!bcd_in_range(record[35], 0, 59)) {
        snprintf(why, why_len, "seconds byte 35 is not valid BCD 00..59");
        return -1;
    }
    if (!bcd_in_range(record[36], 0, 59)) {
        snprintf(why, why_len, "minutes byte 36 is not valid BCD 00..59");
        return -1;
    }
    if (!bcd_in_range(record[37], 0, 23)) {
        snprintf(why, why_len, "hours byte 37 is not valid BCD 00..23");
        return -1;
    }
    if (record[38] > 6) {
        snprintf(why, why_len, "weekday byte 38 is not in range 0..6");
        return -1;
    }
    if (!bcd_in_range(record[39], 1, 31)) {
        snprintf(why, why_len, "day byte 39 is not valid BCD 01..31");
        return -1;
    }
    if (!bcd_in_range(record[40], 1, 12)) {
        snprintf(why, why_len, "month byte 40 is not valid BCD 01..12");
        return -1;
    }
    if (!bcd_in_range(record[41], 0, 99)) {
        snprintf(why, why_len, "year byte 41 is not valid BCD 00..99");
        return -1;
    }
    snprintf(why, why_len, "ok");
    return 0;
}

static void print_time_fields(const char *label, const uint8_t record[RECORD_SIZE]) {
    int sec = unbcd(record[35]);
    int min = unbcd(record[36]);
    int hour = unbcd(record[37]);
    int day = unbcd(record[39]);
    int mon = unbcd(record[40]);
    int year = unbcd(record[41]);

    printf("%s: bytes hh=%02x mm=%02x ss=%02x weekday=%u day=%02x month=%02x year=%02x",
           label,
           record[37],
           record[36],
           record[35],
           record[38],
           record[39],
           record[40],
           record[41]);
    if (sec >= 0 && min >= 0 && hour >= 0 && day >= 0 && mon >= 0 && year >= 0 &&
        hour <= 23 && min <= 59 && sec <= 59 && record[38] <= 6 &&
        day >= 1 && day <= 31 && mon >= 1 && mon <= 12) {
        printf(" decoded=20%02d-%02d-%02d %02d:%02d:%02d weekday=%u",
               year,
               mon,
               day,
               hour,
               min,
               sec,
               record[38]);
    }
    putchar('\n');
}

static void print_hex(const char *label, const uint8_t *buf, size_t len) {
    if (label != NULL) {
        printf("%s", label);
    }
    for (size_t i = 0; i < len; i++) {
        if (i > 0) {
            putchar(' ');
        }
        printf("%02x", buf[i]);
    }
    putchar('\n');
}

static void sha256_hex(const uint8_t *buf, size_t len, char out[65]) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(buf, (CC_LONG)len, digest);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    out[64] = '\0';
}

static void stable_sha256_hex(const uint8_t record[RECORD_SIZE], char out[65]) {
    uint8_t masked[RECORD_SIZE];
    memcpy(masked, record, RECORD_SIZE);
    memset(masked + TIME_OFFSET, 0, TIME_SIZE);
    sha256_hex(masked, RECORD_SIZE, out);
}

static bool hex_equals_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex_record_text(const char *text, uint8_t out[RECORD_SIZE]) {
    char digits[RECORD_SIZE * 2 + 1];
    size_t count = 0;

    for (const char *p = text; *p != '\0'; p++) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p++;
            continue;
        }
        if (isxdigit((unsigned char)*p)) {
            if (count >= RECORD_SIZE * 2) {
                fprintf(stderr, "Template has more than 48 bytes of hex\n");
                return -1;
            }
            digits[count++] = *p;
            continue;
        }
        if (isspace((unsigned char)*p) || *p == ':' || *p == ',' || *p == '-') {
            continue;
        }
        fprintf(stderr, "Template contains non-hex character: %c\n", *p);
        return -1;
    }

    if (count != RECORD_SIZE * 2) {
        fprintf(stderr, "Template must contain exactly 48 bytes, got %zu hex digits\n", count);
        return -1;
    }
    digits[count] = '\0';

    for (size_t i = 0; i < RECORD_SIZE; i++) {
        int hi = hex_nibble(digits[i * 2]);
        int lo = hex_nibble(digits[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static bool looks_like_hex_text(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (isxdigit(c) || isspace(c) || c == ':' || c == ',' || c == '-' || c == 'x' || c == 'X') {
            continue;
        }
        return false;
    }
    return true;
}

static int load_record_template(const options_t *opt, uint8_t out[RECORD_SIZE]) {
    if (opt->template_hex != NULL && opt->template_file != NULL) {
        fprintf(stderr, "Use either --template or --template-file, not both\n");
        return -1;
    }
    if (opt->template_hex != NULL) {
        return parse_hex_record_text(opt->template_hex, out);
    }
    if (opt->template_file == NULL) {
        fprintf(stderr, "dry-run needs --template or --template-file\n");
        return -1;
    }

    FILE *f = fopen(opt->template_file, "rb");
    if (f == NULL) {
        perror(opt->template_file);
        return -1;
    }

    uint8_t buf[4096];
    size_t len = fread(buf, 1, sizeof(buf), f);
    if (ferror(f)) {
        perror(opt->template_file);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (len == RECORD_SIZE && !looks_like_hex_text(buf, len)) {
        memcpy(out, buf, RECORD_SIZE);
        return 0;
    }

    char *text = calloc(len + 1, 1);
    if (text == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    memcpy(text, buf, len);
    int rc = parse_hex_record_text(text, out);
    free(text);
    return rc;
}

static int fill_time_bytes(const char *arg, uint8_t out[TIME_SIZE]) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));

    if (arg == NULL || strcmp(arg, "now") == 0) {
        time_t now = time(NULL);
        if (localtime_r(&now, &tmv) == NULL) {
            perror("localtime_r");
            return -1;
        }
    } else {
        int year = 0;
        int mon = 0;
        int day = 0;
        int hour = 0;
        int min = 0;
        int sec = 0;
        int matched = sscanf(arg, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
        if (matched != 6) {
            matched = sscanf(arg, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
        }
        if (matched != 6 || year < 2000 || year > 2099 || mon < 1 || mon > 12 ||
            day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
            sec < 0 || sec > 59) {
            fprintf(stderr, "Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time\n");
            return -1;
        }
        struct tm requested;
        memset(&requested, 0, sizeof(requested));
        requested.tm_year = year - 1900;
        requested.tm_mon = mon - 1;
        requested.tm_mday = day;
        requested.tm_hour = hour;
        requested.tm_min = min;
        requested.tm_sec = sec;
        requested.tm_isdst = -1;
        time_t t = mktime(&requested);
        if (t == (time_t)-1 || localtime_r(&t, &tmv) == NULL) {
            fprintf(stderr, "Could not normalize --time value\n");
            return -1;
        }
        if (tmv.tm_year != year - 1900 || tmv.tm_mon != mon - 1 ||
            tmv.tm_mday != day || tmv.tm_hour != hour ||
            tmv.tm_min != min || tmv.tm_sec != sec) {
            fprintf(stderr, "Invalid --time, date/time is not representable in local time\n");
            return -1;
        }
    }

    out[0] = bcd(tmv.tm_sec);
    out[1] = bcd(tmv.tm_min);
    out[2] = bcd(tmv.tm_hour);
    out[3] = (uint8_t)tmv.tm_wday;
    out[4] = bcd(tmv.tm_mday);
    out[5] = bcd(tmv.tm_mon + 1);
    out[6] = bcd((tmv.tm_year + 1900) % 100);
    return 0;
}

static void patch_time(const uint8_t input[RECORD_SIZE],
                       const uint8_t time_bytes[TIME_SIZE],
                       uint8_t output[RECORD_SIZE]) {
    memcpy(output, input, RECORD_SIZE);
    memcpy(output + TIME_OFFSET, time_bytes, TIME_SIZE);
}

static int verify_only_time_changed(const uint8_t before[RECORD_SIZE],
                                    const uint8_t after[RECORD_SIZE]) {
    for (size_t i = 0; i < RECORD_SIZE; i++) {
        if (!is_time_offset(i) && before[i] != after[i]) {
            fprintf(stderr, "Refusing: non-time byte changed at offset %zu\n", i);
            return -1;
        }
    }
    return 0;
}

static void print_record_diff(const uint8_t before[RECORD_SIZE],
                              const uint8_t after[RECORD_SIZE]) {
    printf("Changed offsets:\n");
    for (size_t i = 0; i < RECORD_SIZE; i++) {
        if (before[i] != after[i]) {
            printf("  %02zu: %02x -> %02x%s\n",
                   i,
                   before[i],
                   after[i],
                   is_time_offset(i) ? "" : "  NON-TIME");
        }
    }
}

static void sleep_millis(long millis) {
    struct timespec ts;
    ts.tv_sec = millis / 1000;
    ts.tv_nsec = (millis % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void checksum_packet(uint8_t packet[REPORT_SIZE]) {
    uint16_t sum = 0;
    for (size_t i = 3; i < REPORT_SIZE; i++) {
        sum = (uint16_t)(sum + packet[i]);
    }
    packet[1] = (uint8_t)(sum & 0xff);
    packet[2] = (uint8_t)((sum >> 8) & 0xff);
}

static void build_simple_packet(uint8_t report_id, uint8_t cmd, uint8_t packet[REPORT_SIZE]) {
    memset(packet, 0, REPORT_SIZE);
    packet[0] = report_id;
    packet[3] = cmd;
    checksum_packet(packet);
}

static void build_config_packet(uint8_t report_id,
                                uint8_t cmd,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint16_t offset,
                                uint8_t packet[REPORT_SIZE]) {
    memset(packet, 0, REPORT_SIZE);
    packet[0] = report_id;
    packet[3] = cmd;
    packet[4] = (uint8_t)payload_len;
    packet[5] = (uint8_t)(offset & 0xff);
    packet[6] = (uint8_t)((offset >> 8) & 0xff);
    packet[7] = 0;
    if (payload != NULL && payload_len > 0) {
        if (payload_len > REPORT_SIZE - CONFIG_PAYLOAD_OFFSET) {
            payload_len = REPORT_SIZE - CONFIG_PAYLOAD_OFFSET;
        }
        memcpy(packet + CONFIG_PAYLOAD_OFFSET, payload, payload_len);
    }
    checksum_packet(packet);
}

static uint16_t slot_offset(int slot) {
    return (uint16_t)(slot * 0x31);
}

static void dict_set_int(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    if (number != NULL) {
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
}

static int get_int_property(IOHIDDeviceRef device, CFStringRef key, int fallback) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    int out = fallback;
    if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &out);
    }
    return out;
}

static void get_string_property(IOHIDDeviceRef device,
                                CFStringRef key,
                                char *out,
                                size_t out_len) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
        if (CFStringGetCString((CFStringRef)value, out, out_len, kCFStringEncodingUTF8)) {
            return;
        }
    }
    snprintf(out, out_len, "(unknown)");
}

static void print_data_hex(CFDataRef data) {
    CFIndex len = CFDataGetLength(data);
    const UInt8 *bytes = CFDataGetBytePtr(data);
    for (CFIndex i = 0; i < len; i++) {
        printf("%02x", bytes[i]);
    }
}

static void print_device_summary(IOHIDDeviceRef device, CFIndex index) {
    char product[256];
    char manufacturer[256];
    get_string_property(device, CFSTR(kIOHIDProductKey), product, sizeof(product));
    get_string_property(device, CFSTR(kIOHIDManufacturerKey), manufacturer, sizeof(manufacturer));
    int vid = get_int_property(device, CFSTR(kIOHIDVendorIDKey), -1);
    int pid = get_int_property(device, CFSTR(kIOHIDProductIDKey), -1);
    int usage_page = get_int_property(device, CFSTR(kIOHIDPrimaryUsagePageKey), -1);
    int usage = get_int_property(device, CFSTR(kIOHIDPrimaryUsageKey), -1);
    int location = get_int_property(device, CFSTR(kIOHIDLocationIDKey), 0);
    int max_input = get_int_property(device, CFSTR(kIOHIDMaxInputReportSizeKey), -1);
    int max_output = get_int_property(device, CFSTR(kIOHIDMaxOutputReportSizeKey), -1);
    int max_feature = get_int_property(device, CFSTR(kIOHIDMaxFeatureReportSizeKey), -1);

    printf("[%ld] %s / %s  vid=0x%04x pid=0x%04x usage_page=0x%04x usage=0x%02x location=0x%x",
           (long)index,
           manufacturer,
           product,
           vid,
           pid,
           usage_page,
           usage,
           location);
    if (max_input >= 0 || max_output >= 0 || max_feature >= 0) {
        printf(" max_in=%d max_out=%d max_feature=%d", max_input, max_output, max_feature);
    }
    putchar('\n');
}

static CFMutableDictionaryRef create_matching_dict_with_usage(const options_t *opt,
                                                              bool include_usage) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                            0,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    if (dict == NULL) {
        return NULL;
    }
    dict_set_int(dict, CFSTR(kIOHIDVendorIDKey), opt->vid);
    dict_set_int(dict, CFSTR(kIOHIDProductIDKey), opt->pid);
    if (include_usage) {
        dict_set_int(dict, CFSTR(kIOHIDPrimaryUsagePageKey), opt->usage_page);
        dict_set_int(dict, CFSTR(kIOHIDPrimaryUsageKey), opt->usage);
    }
    return dict;
}

static IOHIDManagerRef create_manager_with_match(const options_t *opt, bool include_usage) {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager == NULL) {
        fprintf(stderr, "IOHIDManagerCreate failed\n");
        return NULL;
    }
    CFMutableDictionaryRef match = create_matching_dict_with_usage(opt, include_usage);
    if (match == NULL) {
        CFRelease(manager);
        fprintf(stderr, "Could not create HID matching dictionary\n");
        return NULL;
    }
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    return manager;
}

static int list_devices(const options_t *opt) {
    IOHIDManagerRef manager = create_manager_with_match(opt, true);
    if (manager == NULL) {
        return 1;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices == NULL) {
        printf("No matching HID devices found.\n");
        CFRelease(manager);
        return 1;
    }

    CFIndex count = CFSetGetCount(devices);
    IOHIDDeviceRef *refs = calloc((size_t)count, sizeof(*refs));
    if (refs == NULL) {
        fprintf(stderr, "out of memory\n");
        CFRelease(devices);
        CFRelease(manager);
        return 1;
    }
    CFSetGetValues(devices, (const void **)refs);

    printf("Found %ld matching HID device(s).\n", (long)count);
    for (CFIndex i = 0; i < count; i++) {
        print_device_summary(refs[i], i);
    }

    free(refs);
    CFRelease(devices);
    CFRelease(manager);
    return count > 0 ? 0 : 1;
}

static int probe_devices(const options_t *opt) {
    IOHIDManagerRef manager = create_manager_with_match(opt, false);
    if (manager == NULL) {
        return 1;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices == NULL) {
        printf("No matching HID devices found for vid=0x%04x pid=0x%04x.\n",
               opt->vid,
               opt->pid);
        CFRelease(manager);
        return 1;
    }

    CFIndex count = CFSetGetCount(devices);
    IOHIDDeviceRef *refs = calloc((size_t)count, sizeof(*refs));
    if (refs == NULL) {
        fprintf(stderr, "out of memory\n");
        CFRelease(devices);
        CFRelease(manager);
        return 1;
    }
    CFSetGetValues(devices, (const void **)refs);

    printf("Found %ld HID collection(s) for vid=0x%04x pid=0x%04x.\n",
           (long)count,
           opt->vid,
           opt->pid);
    for (CFIndex i = 0; i < count; i++) {
        print_device_summary(refs[i], i);
        CFTypeRef descriptor = IOHIDDeviceGetProperty(refs[i], CFSTR(kIOHIDReportDescriptorKey));
        if (descriptor != NULL && CFGetTypeID(descriptor) == CFDataGetTypeID()) {
            printf("    descriptor=");
            print_data_hex((CFDataRef)descriptor);
            putchar('\n');
        }
    }

    free(refs);
    CFRelease(devices);
    CFRelease(manager);
    return count > 0 ? 0 : 1;
}

static int get_feature_report(const options_t *opt) {
    hid_session_t session;
    uint8_t report[REPORT_SIZE];
    CFIndex report_len = REPORT_SIZE;

    if (open_hid_session(opt, &session) != 0) {
        return 1;
    }

    memset(report, 0, sizeof(report));
    report[0] = opt->report_id;
    IOReturn rc = IOHIDDeviceGetReport(session.device,
                                       kIOHIDReportTypeFeature,
                                       opt->report_id,
                                       report,
                                       &report_len);
    close_hid_session(&session);
    if (rc != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDDeviceGetReport feature report 0x%02x failed: 0x%08x\n",
                opt->report_id,
                rc);
        return 1;
    }

    print_hex("feature response: ", report, (size_t)report_len);
    return 0;
}

static int run_read_raw(const options_t *opt) {
    if (opt->raw_command == 0) {
        fprintf(stderr, "read-raw needs --raw-command HEX\n");
        return 2;
    }

    hid_session_t session;
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len = 0;
    uint16_t offset = opt->raw_offset_set ? opt->raw_offset : slot_offset(opt->slot);

    if (open_hid_session(opt, &session) != 0) {
        return 1;
    }

    build_config_packet(opt->report_id, opt->raw_command, NULL, opt->raw_length, offset, packet);
    if (opt->debug) {
        print_hex("raw read packet: ", packet, REPORT_SIZE);
    }
    int rc = send_report_wait(&session,
                              packet,
                              opt->raw_command,
                              opt->timeout_ms,
                              response,
                              &response_len);
    close_hid_session(&session);
    if (response_len > 0) {
        print_hex("raw read response: ", response, response_len);
    }
    return rc == 0 ? 0 : 1;
}

static void input_report_callback(void *context,
                                  IOReturn result,
                                  void *sender,
                                  IOHIDReportType type,
                                  uint32_t report_id,
                                  uint8_t *report,
                                  CFIndex report_len) {
    (void)sender;
    (void)type;

    hid_session_t *session = (hid_session_t *)context;
    if (result != kIOReturnSuccess || report_len <= 0) {
        return;
    }

    uint8_t normalized[REPORT_SIZE];
    size_t normalized_len = 0;
    memset(normalized, 0, sizeof(normalized));

    if (report[0] == session->report_id) {
        normalized_len = report_len > REPORT_SIZE ? REPORT_SIZE : (size_t)report_len;
        memcpy(normalized, report, normalized_len);
    } else {
        normalized[0] = (uint8_t)report_id;
        size_t copy_len = report_len > REPORT_SIZE - 1 ? REPORT_SIZE - 1 : (size_t)report_len;
        memcpy(normalized + 1, report, copy_len);
        normalized_len = copy_len + 1;
    }

    if (normalized_len >= 4 &&
        (session->expected_cmd == 0xff || normalized[3] == session->expected_cmd)) {
        memcpy(session->response, normalized, normalized_len);
        session->response_len = normalized_len;
        session->got_response = true;
    }
}

static int open_hid_session(const options_t *opt, hid_session_t *session) {
    memset(session, 0, sizeof(*session));
    session->report_id = opt->report_id;
    session->strip_report_id_for_iohid = opt->strip_report_id_for_iohid;

    session->manager = create_manager_with_match(opt, true);
    if (session->manager == NULL) {
        return -1;
    }

    IOReturn open_rc = IOHIDManagerOpen(session->manager, kIOHIDOptionsTypeNone);
    if (open_rc != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDManagerOpen failed: 0x%08x\n", open_rc);
        CFRelease(session->manager);
        memset(session, 0, sizeof(*session));
        return -1;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(session->manager);
    if (devices == NULL || CFSetGetCount(devices) == 0) {
        fprintf(stderr, "No matching HID device found\n");
        if (devices != NULL) {
            CFRelease(devices);
        }
        IOHIDManagerClose(session->manager, kIOHIDOptionsTypeNone);
        CFRelease(session->manager);
        memset(session, 0, sizeof(*session));
        return -1;
    }

    CFIndex count = CFSetGetCount(devices);
    IOHIDDeviceRef *refs = calloc((size_t)count, sizeof(*refs));
    if (refs == NULL) {
        fprintf(stderr, "out of memory\n");
        CFRelease(devices);
        IOHIDManagerClose(session->manager, kIOHIDOptionsTypeNone);
        CFRelease(session->manager);
        memset(session, 0, sizeof(*session));
        return -1;
    }
    CFSetGetValues(devices, (const void **)refs);

    session->device = refs[0];
    for (CFIndex i = 0; i < count; i++) {
        int max_input = get_int_property(refs[i], CFSTR(kIOHIDMaxInputReportSizeKey), -1);
        int max_output = get_int_property(refs[i], CFSTR(kIOHIDMaxOutputReportSizeKey), -1);
        if (max_input >= REPORT_SIZE && max_output >= REPORT_SIZE) {
            session->device = refs[i];
            break;
        }
    }
    CFRetain(session->device);
    free(refs);
    CFRelease(devices);

    IOReturn dev_rc = IOHIDDeviceOpen(session->device, kIOHIDOptionsTypeNone);
    if (dev_rc != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDDeviceOpen failed: 0x%08x\n", dev_rc);
        CFRelease(session->device);
        IOHIDManagerClose(session->manager, kIOHIDOptionsTypeNone);
        CFRelease(session->manager);
        memset(session, 0, sizeof(*session));
        return -1;
    }

    IOHIDDeviceRegisterInputReportCallback(session->device,
                                           session->input_report,
                                           sizeof(session->input_report),
                                           input_report_callback,
                                           session);
    IOHIDDeviceScheduleWithRunLoop(session->device,
                                   CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);
    return 0;
}

static void close_hid_session(hid_session_t *session) {
    if (session->device != NULL) {
        IOHIDDeviceUnscheduleFromRunLoop(session->device,
                                         CFRunLoopGetCurrent(),
                                         kCFRunLoopDefaultMode);
        IOHIDDeviceClose(session->device, kIOHIDOptionsTypeNone);
        CFRelease(session->device);
    }
    if (session->manager != NULL) {
        IOHIDManagerClose(session->manager, kIOHIDOptionsTypeNone);
        CFRelease(session->manager);
    }
    memset(session, 0, sizeof(*session));
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int send_report_wait_checked(hid_session_t *session,
                                    const uint8_t packet[REPORT_SIZE],
                                    uint8_t expected_cmd,
                                    int timeout_ms,
                                    uint8_t response[REPORT_SIZE],
                                    size_t *response_len,
                                    bool check_status) {
    session->expected_cmd = expected_cmd;
    session->got_response = false;
    session->response_len = 0;
    memset(session->response, 0, sizeof(session->response));
    memset(session->input_report, 0, sizeof(session->input_report));

    const uint8_t *report_data = packet;
    CFIndex report_len = REPORT_SIZE;
    if (session->strip_report_id_for_iohid) {
        report_data = packet + 1;
        report_len = REPORT_SIZE - 1;
    }

    IOReturn rc = IOHIDDeviceSetReport(session->device,
                                       kIOHIDReportTypeOutput,
                                       session->report_id,
                                       report_data,
                                       report_len);
    if (rc != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDDeviceSetReport cmd=0x%02x failed: 0x%08x\n",
                expected_cmd,
                rc);
        return -1;
    }

    double deadline = monotonic_seconds() + (double)timeout_ms / 1000.0;
    while (!session->got_response && monotonic_seconds() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }

    if (!session->got_response) {
        fprintf(stderr, "Timed out waiting for response to cmd=0x%02x\n", expected_cmd);
        return -1;
    }

    if (response != NULL && response_len != NULL) {
        memcpy(response, session->response, session->response_len);
        *response_len = session->response_len;
    }

    if (check_status && session->response_len >= 8) {
        if (session->response[7] == 0xff) {
            fprintf(stderr, "Device returned status 0xff for cmd=0x%02x\n", expected_cmd);
            return -1;
        }
        if (session->response[7] == 0xfe) {
            fprintf(stderr, "Device returned status 0xfe for cmd=0x%02x\n", expected_cmd);
            return -1;
        }
    }
    return 0;
}

static int send_report_wait(hid_session_t *session,
                            const uint8_t packet[REPORT_SIZE],
                            uint8_t expected_cmd,
                            int timeout_ms,
                            uint8_t response[REPORT_SIZE],
                            size_t *response_len) {
    return send_report_wait_checked(session,
                                    packet,
                                    expected_cmd,
                                    timeout_ms,
                                    response,
                                    response_len,
                                    true);
}

static int read_template_from_device(const options_t *opt,
                                     hid_session_t *session,
                                     uint8_t record[RECORD_SIZE]) {
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    uint16_t offset = slot_offset(opt->slot);
    memset(record, 0, RECORD_SIZE);

    for (size_t pos = 0; pos < RECORD_SIZE; pos += 4) {
        size_t chunk_len = RECORD_SIZE - pos;
        size_t response_len = 0;
        char label[96];
        if (chunk_len > 4) {
            chunk_len = 4;
        }

        build_config_packet(opt->report_id,
                            0x05,
                            NULL,
                            chunk_len,
                            (uint16_t)(offset + pos),
                            packet);
        if (opt->debug) {
            snprintf(label,
                     sizeof(label),
                     "cmd 0x05 chunk offset=0x%04x packet: ",
                     (unsigned)(offset + pos));
            print_hex(label, packet, REPORT_SIZE);
        }

        if (send_report_wait_checked(session,
                                     packet,
                                     0x05,
                                     opt->timeout_ms,
                                     response,
                                     &response_len,
                                     false) != 0) {
            return -1;
        }
        if (opt->debug) {
            snprintf(label,
                     sizeof(label),
                     "cmd 0x05 chunk offset=0x%04x response: ",
                     (unsigned)(offset + pos));
            print_hex(label, response, response_len);
        }
        if (response_len == REPORT_SIZE && memcmp(response, packet, REPORT_SIZE) == 0) {
            fprintf(stderr,
                    "Refusing echoed read response: this HID collection did not return a real template\n");
            return -1;
        }
        if (response_len < 4 + chunk_len) {
            fprintf(stderr,
                    "Read response too short at offset 0x%04x: got %zu bytes, need at least %zu\n",
                    (unsigned)(offset + pos),
                    response_len,
                    4 + chunk_len);
            return -1;
        }

        memcpy(record + pos, response + 4, chunk_len);
    }

    if (record_all_zero(record)) {
        fprintf(stderr,
                "Refusing all-zero template: this is not a valid screen/clock config record\n");
        return -1;
    }
    return 0;
}

static int write_time_to_device(const options_t *opt,
                                hid_session_t *session,
                                const uint8_t patched[RECORD_SIZE]) {
    uint8_t begin[REPORT_SIZE];
    uint8_t write[REPORT_SIZE];
    uint8_t commit[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len = 0;

    build_simple_packet(opt->report_id, 0x01, begin);
    build_config_packet(opt->report_id, 0x06, patched, RECORD_SIZE, slot_offset(opt->slot), write);
    build_simple_packet(opt->report_id, 0x02, commit);

    if (opt->debug) {
        print_hex("cmd 0x01 packet: ", begin, REPORT_SIZE);
        print_hex("cmd 0x06 packet: ", write, REPORT_SIZE);
        print_hex("cmd 0x02 packet: ", commit, REPORT_SIZE);
    }

    if (send_report_wait(session, begin, 0x01, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    if (send_report_wait(session, write, 0x06, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    sleep_millis(10);
    if (send_report_wait(session, commit, 0x02, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    return 0;
}

static int run_dry_run(const options_t *opt) {
    uint8_t template_record[RECORD_SIZE];
    uint8_t time_bytes[TIME_SIZE];
    uint8_t patched[RECORD_SIZE];
    uint8_t packet[REPORT_SIZE];
    char hash_before[65];
    char hash_after[65];
    char stable_hash_before[65];
    char stable_hash_after[65];
    char why[128];

    if (load_record_template(opt, template_record) != 0) {
        return 1;
    }
    if (record_all_zero(template_record)) {
        fprintf(stderr,
                "Refusing all-zero template: this is not a valid screen/clock config record\n");
        return 1;
    }
    if (fill_time_bytes(opt->time_arg, time_bytes) != 0) {
        return 1;
    }
    patch_time(template_record, time_bytes, patched);
    if (verify_only_time_changed(template_record, patched) != 0) {
        return 1;
    }

    sha256_hex(template_record, RECORD_SIZE, hash_before);
    sha256_hex(patched, RECORD_SIZE, hash_after);
    stable_sha256_hex(template_record, stable_hash_before);
    stable_sha256_hex(patched, stable_hash_after);
    print_time_fields("old time bytes", template_record);
    print_time_fields("new time bytes", patched);
    if (opt->debug) {
        print_hex("template: ", template_record, RECORD_SIZE);
        print_hex("patched:  ", patched, RECORD_SIZE);
        printf("template sha256: %s\n", hash_before);
        printf("patched  sha256: %s\n", hash_after);
        printf("template stable sha256: %s\n", stable_hash_before);
        printf("patched  stable sha256: %s\n", stable_hash_after);
        print_record_diff(template_record, patched);
    }

    if (validate_time_fields(template_record, why, sizeof(why)) != 0) {
        printf("template time validation warning: %s\n", why);
    }

    if (opt->debug) {
        build_config_packet(opt->report_id, 0x06, patched, RECORD_SIZE, slot_offset(opt->slot), packet);
        print_hex("cmd 0x06 packet: ", packet, REPORT_SIZE);
    }

    printf("No HID device was opened. No report was sent.\n");
    return 0;
}

static int run_read_template(const options_t *opt) {
    hid_session_t session;
    uint8_t record[RECORD_SIZE];
    char hash[65];
    char stable_hash[65];
    char why[128];

    if (open_hid_session(opt, &session) != 0) {
        return 1;
    }
    int rc = read_template_from_device(opt, &session, record);
    close_hid_session(&session);
    if (rc != 0) {
        return 1;
    }

    sha256_hex(record, RECORD_SIZE, hash);
    stable_sha256_hex(record, stable_hash);
    print_hex("template: ", record, RECORD_SIZE);
    printf("template sha256: %s\n", hash);
    printf("template stable sha256: %s\n", stable_hash);
    print_time_fields("template time bytes", record);
    if (validate_time_fields(record, why, sizeof(why)) != 0) {
        printf("template time validation warning: %s\n", why);
    }

    if (opt->out_file != NULL) {
        FILE *f = fopen(opt->out_file, "wb");
        if (f == NULL) {
            perror(opt->out_file);
            return 1;
        }
        if (fwrite(record, 1, RECORD_SIZE, f) != RECORD_SIZE) {
            perror(opt->out_file);
            fclose(f);
            return 1;
        }
        fclose(f);
        printf("wrote raw 48-byte template to %s\n", opt->out_file);
    }
    return 0;
}

static int run_read_template_split(const options_t *opt) {
    options_t tx_opt = *opt;
    options_t rx_opt = *opt;
    tx_opt.usage_page = 0xff1c;
    tx_opt.usage = 0x92;
    tx_opt.report_id = 0x04;
    rx_opt.usage_page = 0x0001;
    rx_opt.usage = 0x06;
    rx_opt.report_id = 0x05;

    hid_session_t tx_session;
    hid_session_t rx_session;
    uint8_t packet[REPORT_SIZE];
    uint8_t tx_response[REPORT_SIZE];
    uint8_t rx_response[REPORT_SIZE];
    uint8_t record[RECORD_SIZE];
    size_t tx_response_len = 0;
    size_t rx_response_len = 0;
    char hash[65];
    char stable_hash[65];
    char why[128];
    int rc = 1;

    if (open_hid_session(&rx_opt, &rx_session) != 0) {
        return 1;
    }
    if (open_hid_session(&tx_opt, &tx_session) != 0) {
        close_hid_session(&rx_session);
        return 1;
    }

    rx_session.expected_cmd = 0xff;
    rx_session.got_response = false;
    rx_session.response_len = 0;
    memset(rx_session.response, 0, sizeof(rx_session.response));
    memset(rx_session.input_report, 0, sizeof(rx_session.input_report));

    build_config_packet(tx_opt.report_id, 0x05, NULL, RECORD_SIZE, slot_offset(opt->slot), packet);
    if (opt->debug) {
        print_hex("split tx cmd 0x05 packet: ", packet, REPORT_SIZE);
    }

    if (send_report_wait(&tx_session,
                         packet,
                         0x05,
                         opt->timeout_ms,
                         tx_response,
                         &tx_response_len) != 0) {
        goto out;
    }
    if (opt->debug) {
        print_hex("split tx cmd 0x05 response: ", tx_response, tx_response_len);
    }

    double deadline = monotonic_seconds() + (double)opt->timeout_ms / 1000.0;
    while (!rx_session.got_response && monotonic_seconds() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    if (!rx_session.got_response) {
        fprintf(stderr, "Timed out waiting for split 64-byte response on report ID 0x05\n");
        goto out;
    }

    memcpy(rx_response, rx_session.response, rx_session.response_len);
    rx_response_len = rx_session.response_len;
    if (opt->debug) {
        print_hex("split rx response: ", rx_response, rx_response_len);
    }
    if (rx_response_len < CONFIG_PAYLOAD_OFFSET + RECORD_SIZE) {
        fprintf(stderr,
                "Split read response too short: got %zu bytes, need at least %d\n",
                rx_response_len,
                CONFIG_PAYLOAD_OFFSET + RECORD_SIZE);
        goto out;
    }

    memcpy(record, rx_response + CONFIG_PAYLOAD_OFFSET, RECORD_SIZE);
    if (record_all_zero(record)) {
        fprintf(stderr,
                "Refusing all-zero template: this is not a valid screen/clock config record\n");
        goto out;
    }
    sha256_hex(record, RECORD_SIZE, hash);
    stable_sha256_hex(record, stable_hash);
    print_hex("template: ", record, RECORD_SIZE);
    printf("template sha256: %s\n", hash);
    printf("template stable sha256: %s\n", stable_hash);
    print_time_fields("template time bytes", record);
    if (validate_time_fields(record, why, sizeof(why)) != 0) {
        printf("template time validation warning: %s\n", why);
    }

    if (opt->out_file != NULL) {
        FILE *f = fopen(opt->out_file, "wb");
        if (f == NULL) {
            perror(opt->out_file);
            goto out;
        }
        if (fwrite(record, 1, RECORD_SIZE, f) != RECORD_SIZE) {
            perror(opt->out_file);
            fclose(f);
            goto out;
        }
        fclose(f);
        printf("wrote raw 48-byte template to %s\n", opt->out_file);
    }

    rc = 0;

out:
    close_hid_session(&tx_session);
    close_hid_session(&rx_session);
    return rc;
}

static int run_update_time(const options_t *opt) {
    hid_session_t session;
    uint8_t current[RECORD_SIZE];
    uint8_t patched[RECORD_SIZE];
    uint8_t time_bytes[TIME_SIZE];
    char hash_current[65];
    char hash_patched[65];
    char stable_hash_current[65];
    char stable_hash_patched[65];
    char why[128];

    if (fill_time_bytes(opt->time_arg, time_bytes) != 0) {
        return 1;
    }

    if (open_hid_session(opt, &session) != 0) {
        return 1;
    }

    int rc = read_template_from_device(opt, &session, current);
    if (rc == 0) {
        sha256_hex(current, RECORD_SIZE, hash_current);
        stable_sha256_hex(current, stable_hash_current);
        if (opt->expected_template_sha256 != NULL &&
            !hex_equals_ci(hash_current, opt->expected_template_sha256)) {
            fprintf(stderr,
                    "Refusing update: current template sha256 %s does not match expected %s\n",
                    hash_current,
                    opt->expected_template_sha256);
            rc = -1;
        }
        if (opt->expected_stable_sha256 != NULL &&
            !hex_equals_ci(stable_hash_current, opt->expected_stable_sha256)) {
            fprintf(stderr,
                    "Refusing update: stable template sha256 %s does not match expected %s\n",
                    stable_hash_current,
                    opt->expected_stable_sha256);
            rc = -1;
        }
    }
    if (rc == 0 && validate_time_fields(current, why, sizeof(why)) != 0) {
        fprintf(stderr, "Refusing update: current record time fields look invalid: %s\n", why);
        rc = -1;
    }
    if (rc == 0) {
        patch_time(current, time_bytes, patched);
        rc = verify_only_time_changed(current, patched);
    }
    if (rc == 0) {
        sha256_hex(patched, RECORD_SIZE, hash_patched);
        stable_sha256_hex(patched, stable_hash_patched);
        print_time_fields("new time bytes", patched);
        if (opt->debug) {
            printf("current template sha256: %s\n", hash_current);
            printf("patched  template sha256: %s\n", hash_patched);
            printf("current stable sha256: %s\n", stable_hash_current);
            printf("patched  stable sha256: %s\n", stable_hash_patched);
            print_time_fields("old time bytes", current);
            print_record_diff(current, patched);
        }
        rc = write_time_to_device(opt, &session, patched);
    }

    close_hid_session(&session);
    if (rc != 0) {
        return 1;
    }
    printf("Clock update command sequence completed.\n");
    return 0;
}

static int run_restore_template(const options_t *opt) {
    if (opt->template_hex == NULL && opt->template_file == NULL) {
        fprintf(stderr, "restore-template needs --template or --template-file PATH\n");
        return 2;
    }

    uint8_t restore_template[RECORD_SIZE];
    uint8_t current[RECORD_SIZE];
    uint8_t patched[RECORD_SIZE];
    uint8_t time_bytes[TIME_SIZE];
    char current_hash[65];
    char current_stable_hash[65];
    char restore_hash[65];
    char restore_stable_hash[65];
    char patched_hash[65];
    char patched_stable_hash[65];
    hid_session_t session;
    int rc = 1;

    if (load_record_template(opt, restore_template) != 0) {
        return 1;
    }
    if (record_all_zero(restore_template)) {
        fprintf(stderr,
                "Refusing all-zero restore template: this is not a valid config record\n");
        return 1;
    }
    sha256_hex(restore_template, RECORD_SIZE, restore_hash);
    stable_sha256_hex(restore_template, restore_stable_hash);
    if (opt->expected_template_sha256 != NULL &&
        !hex_equals_ci(restore_hash, opt->expected_template_sha256)) {
        fprintf(stderr,
                "Refusing restore: restore template sha256 %s does not match expected %s\n",
                restore_hash,
                opt->expected_template_sha256);
        return 1;
    }
    if (fill_time_bytes(opt->time_arg, time_bytes) != 0) {
        return 1;
    }
    patch_time(restore_template, time_bytes, patched);

    if (open_hid_session(opt, &session) != 0) {
        return 1;
    }
    if (read_template_from_device(opt, &session, current) != 0) {
        goto out;
    }

    sha256_hex(current, RECORD_SIZE, current_hash);
    stable_sha256_hex(current, current_stable_hash);
    if (opt->expected_stable_sha256 != NULL &&
        !hex_equals_ci(current_stable_hash, opt->expected_stable_sha256)) {
        fprintf(stderr,
                "Refusing restore: current stable template sha256 %s does not match expected %s\n",
                current_stable_hash,
                opt->expected_stable_sha256);
        goto out;
    }

    sha256_hex(patched, RECORD_SIZE, patched_hash);
    stable_sha256_hex(patched, patched_stable_hash);
    print_time_fields("patched time bytes", patched);
    if (opt->debug) {
        printf("current template sha256: %s\n", current_hash);
        printf("current stable sha256: %s\n", current_stable_hash);
        printf("restore template sha256: %s\n", restore_hash);
        printf("restore stable sha256: %s\n", restore_stable_hash);
        printf("patched  template sha256: %s\n", patched_hash);
        printf("patched  stable sha256: %s\n", patched_stable_hash);
        print_hex("current:  ", current, RECORD_SIZE);
        print_hex("restore:  ", restore_template, RECORD_SIZE);
        print_hex("patched:  ", patched, RECORD_SIZE);
        print_time_fields("current time bytes", current);
        print_record_diff(current, patched);
    }

    if (write_time_to_device(opt, &session, patched) != 0) {
        goto out;
    }
    printf("Template restore command sequence completed.\n");
    rc = 0;

out:
    close_hid_session(&session);
    return rc;
}

static command_t parse_command(const char *s) {
    if (strcmp(s, "help") == 0 || strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
        return CMD_HELP;
    }
    if (strcmp(s, "list") == 0) {
        return CMD_LIST;
    }
    if (strcmp(s, "probe") == 0) {
        return CMD_PROBE;
    }
    if (strcmp(s, "get-feature") == 0) {
        return CMD_GET_FEATURE;
    }
    if (strcmp(s, "read-raw") == 0) {
        return CMD_READ_RAW;
    }
    if (strcmp(s, "dry-run") == 0) {
        return CMD_DRY_RUN;
    }
    if (strcmp(s, "read-template") == 0) {
        return CMD_READ_TEMPLATE;
    }
    if (strcmp(s, "read-template-split") == 0) {
        return CMD_READ_TEMPLATE_SPLIT;
    }
    if (strcmp(s, "update-time") == 0) {
        return CMD_UPDATE_TIME;
    }
    if (strcmp(s, "restore-template") == 0) {
        return CMD_RESTORE_TEMPLATE;
    }
    return CMD_NONE;
}

static void init_options(options_t *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->command = CMD_NONE;
    opt->vid = DEFAULT_VID;
    opt->pid = DEFAULT_PID;
    opt->usage_page = DEFAULT_USAGE_PAGE;
    opt->usage = DEFAULT_USAGE;
    opt->report_id = DEFAULT_REPORT_ID;
    opt->raw_length = RECORD_SIZE;
    opt->slot = 0;
    opt->timeout_ms = 1500;
    opt->time_arg = "now";
}

static int parse_options(int argc, char **argv, options_t *opt) {
    static const struct option long_opts[] = {
        {"vid", required_argument, NULL, 1000},
        {"pid", required_argument, NULL, 1001},
        {"usage-page", required_argument, NULL, 1002},
        {"usage", required_argument, NULL, 1003},
        {"report-id", required_argument, NULL, 1004},
        {"raw-command", required_argument, NULL, 1005},
        {"raw-length", required_argument, NULL, 1006},
        {"raw-offset", required_argument, NULL, 1007},
        {"slot", required_argument, NULL, 1008},
        {"time", required_argument, NULL, 1009},
        {"template", required_argument, NULL, 1010},
        {"template-file", required_argument, NULL, 1011},
        {"out", required_argument, NULL, 1012},
        {"expected-template-sha256", required_argument, NULL, 1013},
        {"debug", no_argument, NULL, 1014},
        {"timeout-ms", required_argument, NULL, 1015},
        {"expected-stable-sha256", required_argument, NULL, 1019},
        {"strip-report-id-for-iohid", no_argument, NULL, 1020},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    if (argc < 2) {
        usage(stderr);
        return -1;
    }

    opt->command = parse_command(argv[1]);
    if (opt->command == CMD_NONE) {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage(stderr);
        return -1;
    }

    optind = 2;
    while (true) {
        int idx = 0;
        int c = getopt_long(argc, argv, "h", long_opts, &idx);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'h':
                opt->command = CMD_HELP;
                break;
            case 1000:
                opt->vid = parse_u16(optarg, "--vid");
                break;
            case 1001:
                opt->pid = parse_u16(optarg, "--pid");
                break;
            case 1002:
                opt->usage_page = parse_u16(optarg, "--usage-page");
                break;
            case 1003:
                opt->usage = parse_u16(optarg, "--usage");
                break;
            case 1004:
                opt->report_id = (uint8_t)parse_int_range(optarg, "--report-id", 0, 255);
                break;
            case 1005:
                opt->raw_command = (uint8_t)parse_int_range(optarg, "--raw-command", 0, 255);
                break;
            case 1006:
                opt->raw_length = (size_t)parse_int_range(optarg, "--raw-length", 0, REPORT_SIZE - CONFIG_PAYLOAD_OFFSET);
                break;
            case 1007:
                opt->raw_offset = parse_u16(optarg, "--raw-offset");
                opt->raw_offset_set = true;
                break;
            case 1008:
                opt->slot = parse_int_range(optarg, "--slot", 0, 1024);
                break;
            case 1009:
                opt->time_arg = optarg;
                break;
            case 1010:
                opt->template_hex = optarg;
                break;
            case 1011:
                opt->template_file = optarg;
                break;
            case 1012:
                opt->out_file = optarg;
                break;
            case 1013:
                opt->expected_template_sha256 = optarg;
                break;
            case 1014:
                opt->debug = true;
                break;
            case 1015:
                opt->timeout_ms = parse_int_range(optarg, "--timeout-ms", 100, 30000);
                break;
            case 1019:
                opt->expected_stable_sha256 = optarg;
                break;
            case 1020:
                opt->strip_report_id_for_iohid = true;
                break;
            default:
                usage(stderr);
                return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return -1;
    }

    if ((unsigned)opt->slot * 0x31u > 0xffffu - (RECORD_SIZE - 1u)) {
        fprintf(stderr, "--slot is too large for the 16-bit config offset\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    options_t opt;
    init_options(&opt);
    if (parse_options(argc, argv, &opt) != 0) {
        return 2;
    }

    switch (opt.command) {
        case CMD_HELP:
            usage(stdout);
            return 0;
        case CMD_LIST:
            return list_devices(&opt);
        case CMD_PROBE:
            return probe_devices(&opt);
        case CMD_GET_FEATURE:
            return get_feature_report(&opt);
        case CMD_READ_RAW:
            return run_read_raw(&opt);
        case CMD_DRY_RUN:
            return run_dry_run(&opt);
        case CMD_READ_TEMPLATE:
            return run_read_template(&opt);
        case CMD_READ_TEMPLATE_SPLIT:
            return run_read_template_split(&opt);
        case CMD_UPDATE_TIME:
            return run_update_time(&opt);
        case CMD_RESTORE_TEMPLATE:
            return run_restore_template(&opt);
        case CMD_NONE:
        default:
            usage(stderr);
            return 2;
    }
}
