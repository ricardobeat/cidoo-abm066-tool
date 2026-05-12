#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
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
#define FRAMEBUFFER_WIDTH 240
#define FRAMEBUFFER_HEIGHT 135
#define PORTRAIT_WIDTH 135
#define PORTRAIT_HEIGHT 240
#define IMAGE_PIXELS (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)
#define RGB565_SIZE (IMAGE_PIXELS * 2)
#define PADDED_IMAGE_SIZE 65536
#define IMAGE_CHUNK_SIZE 0x38
#define IMAGE_PAYLOAD_OFFSET 8
#define CONFIG_RECORD_SIZE 48
#define CONFIG_SLOT_STRIDE 0x31
#define CONFIG_PAYLOAD_OFFSET 8
#define TIME_OFFSET 35
#define TIME_SIZE 7
#define ACTIVE_BUCKET_OFFSET 33
#define BUCKET1_COUNT_OFFSET 34
#define BUCKET2_COUNT_OFFSET 46
#define DEFAULT_MAX_FRAMES_PER_BUCKET 64
#define MAX_HARD_FRAMES_PER_BUCKET 255
#define MAX_TOTAL_FRAMES 255
#define DEFAULT_VID 0x320f
#define DEFAULT_PID 0x5055
#define DEFAULT_USAGE_PAGE 0xff1c
#define DEFAULT_USAGE 0x92
#define DEFAULT_REPORT_ID 0x04
#define UPLOAD_CONFIRM_TEXT "UPLOAD_SCREEN_IMAGE"
#define IMAGE_UPLOAD_METADATA_IMPLEMENTED 0

typedef enum {
    CMD_NONE,
    CMD_HELP,
    CMD_DRY_RUN,
    CMD_UPLOAD,
    CMD_DRY_RUN_BUCKETS,
    CMD_UPLOAD_BUCKETS,
} command_t;

typedef enum {
    FIT_COVER,
    FIT_CONTAIN,
    FIT_STRETCH,
} fit_mode_t;

typedef enum {
    ORIENTATION_LANDSCAPE,
    ORIENTATION_PORTRAIT,
} orientation_t;

typedef struct {
    command_t command;
    uint16_t vid;
    uint16_t pid;
    uint16_t usage_page;
    uint16_t usage;
    uint8_t report_id;
    const char *image_file;
    const char *bucket1_file;
    const char *bucket2_file;
    const char *template_file;
    const char *expected_stable_sha256;
    const char *preview_file;
    const char *framebuffer_preview_file;
    const char *bucket1_preview_file;
    const char *bucket2_preview_file;
    const char *bucket1_framebuffer_preview_file;
    const char *bucket2_framebuffer_preview_file;
    const char *confirm_upload;
    int slot;
    int active_bucket;
    int max_frames_per_bucket;
    int timeout_ms;
    int rotate_degrees;
    fit_mode_t fit_mode;
    orientation_t orientation;
    bool allow_hid_query;
    bool allow_config_write;
    bool allow_image_write;
    bool preserve_clock;
    bool active_bucket_set;
    bool print_packets;
    bool rotate_set;
    bool strip_report_id_for_iohid;
} options_t;

typedef struct {
    uint8_t *payload;
    size_t frame_count;
} frame_bucket_t;

typedef struct {
    frame_bucket_t bucket1;
    frame_bucket_t bucket2;
    uint8_t *payload;
    size_t payload_len;
    uint32_t payload_offset;
    bool write_bucket1;
    bool write_bucket2;
    size_t existing_bucket1_frames;
    size_t existing_bucket2_frames;
    size_t final_bucket1_frames;
    size_t final_bucket2_frames;
    int final_active_bucket;
} bucket_upload_plan_t;

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

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  cidoo-image dry-run --image PATH [options]\n"
            "  cidoo-image upload --image PATH --allow-image-write \\\n"
            "      --confirm-upload " UPLOAD_CONFIRM_TEXT " [options]\n"
            "  cidoo-image dry-run-buckets --bucket1 PATH|--bucket2 PATH \\\n"
            "      --template-file PATH [options]\n"
            "  cidoo-image upload-buckets --bucket1 PATH --bucket2 PATH \\\n"
            "      --allow-hid-query --allow-config-write --allow-image-write \\\n"
            "      --expected-stable-sha256 HEX --confirm-upload " UPLOAD_CONFIRM_TEXT " [options]\n"
            "\n"
            "Options:\n"
            "  --image PATH                      PNG/JPEG/etc image to upload\n"
            "  --bucket1 PATH                    still image or GIF for Custom1 / GIF1\n"
            "  --bucket2 PATH                    still image or GIF for Custom2 / GIF2\n"
            "  --template-file PATH              48-byte raw template for dry-run-buckets\n"
            "  --expected-stable-sha256 HEX      upload guard; time bytes 35..41 masked\n"
            "  --slot N                          config slot, offset = N * 0x31, default 0\n"
            "  --active-bucket 1|2               bucket selected after upload, default auto\n"
            "  --max-frames-per-bucket N         default %d, hard max %d\n"
            "  --orientation landscape|portrait  physical screen orientation, default landscape\n"
            "  --rotate 0|90|180|270             rotate preview into framebuffer\n"
            "  --preview-out PATH                write fitted physical-screen PNG preview\n"
            "  --framebuffer-preview-out PATH    write encoded 240x135 framebuffer PNG preview\n"
            "  --bucket1-preview-out PATH        write GIF1 first-frame physical preview\n"
            "  --bucket2-preview-out PATH        write GIF2 first-frame physical preview\n"
            "  --bucket1-framebuffer-preview-out PATH\n"
            "                                    write GIF1 first-frame framebuffer preview\n"
            "  --bucket2-framebuffer-preview-out PATH\n"
            "                                    write GIF2 first-frame framebuffer preview\n"
            "  --fit cover|contain|stretch       image fit mode, default cover\n"
            "  --vid HEX                         USB VID, default 0x%04x\n"
            "  --pid HEX                         USB PID, default 0x%04x\n"
            "  --usage-page HEX                  HID usage page, default 0x%04x\n"
            "  --usage HEX                       HID usage, default 0x%02x\n"
            "  --report-id HEX                   HID report ID, default 0x%02x\n"
            "  --timeout-ms N                    HID response timeout, default 1500\n"
            "  --print-packets                   print first/last HID packets\n"
            "  --allow-hid-query                 permit command 0x05 template read\n"
            "  --allow-config-write              permit command 0x06 config write path\n"
            "  --allow-image-write               permit command 0x01/0x21/0x02 upload\n"
            "  --preserve-clock                  do not refresh bytes 35..41 during bucket upload\n"
            "  --confirm-upload TEXT             must be " UPLOAD_CONFIRM_TEXT "\n"
            "  --strip-report-id-for-iohid       diagnostic: pass bytes 1..63 to IOKit\n"
            "\n"
            "Safety model:\n"
            "  dry-run commands never open HID and never send a report. bucket uploads\n"
            "  read the current 48-byte template, patch only metadata for requested\n"
            "  buckets and, unless --preserve-clock is used, bytes 35..41, check the\n"
            "  stable hash, then send only after all explicit flags are present. Real\n"
            "  uploads require both buckets because command 0x23 appears to operate on\n"
            "  the combined custom-image store.\n",
            DEFAULT_MAX_FRAMES_PER_BUCKET,
            MAX_HARD_FRAMES_PER_BUCKET,
            DEFAULT_VID,
            DEFAULT_PID,
            DEFAULT_USAGE_PAGE,
            DEFAULT_USAGE,
            DEFAULT_REPORT_ID);
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

static bool is_time_offset(size_t offset) {
    return offset >= TIME_OFFSET && offset < TIME_OFFSET + TIME_SIZE;
}

static bool record_all_zero(const uint8_t record[CONFIG_RECORD_SIZE]) {
    for (size_t i = 0; i < CONFIG_RECORD_SIZE; i++) {
        if (record[i] != 0) {
            return false;
        }
    }
    return true;
}

static uint8_t bcd(int value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static int fill_now_time_bytes(uint8_t out[TIME_SIZE]) {
    struct tm tmv;
    time_t now = time(NULL);
    memset(&tmv, 0, sizeof(tmv));
    if (localtime_r(&now, &tmv) == NULL) {
        perror("localtime_r");
        return -1;
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

static void stable_config_sha256_hex(const uint8_t record[CONFIG_RECORD_SIZE], char out[65]) {
    uint8_t masked[CONFIG_RECORD_SIZE];
    memcpy(masked, record, CONFIG_RECORD_SIZE);
    memset(masked + TIME_OFFSET, 0, TIME_SIZE);
    sha256_hex(masked, CONFIG_RECORD_SIZE, out);
}

static uint16_t slot_offset(int slot) {
    return (uint16_t)(slot * CONFIG_SLOT_STRIDE);
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

static void build_image_packet(uint8_t report_id,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint32_t offset,
                               uint8_t packet[REPORT_SIZE]) {
    memset(packet, 0, REPORT_SIZE);
    packet[0] = report_id;
    packet[3] = 0x21;
    packet[4] = (uint8_t)payload_len;
    packet[5] = (uint8_t)(offset & 0xff);
    packet[6] = (uint8_t)((offset >> 8) & 0xff);
    packet[7] = (uint8_t)((offset >> 16) & 0xff);
    if (payload_len > REPORT_SIZE - IMAGE_PAYLOAD_OFFSET) {
        payload_len = REPORT_SIZE - IMAGE_PAYLOAD_OFFSET;
    }
    if (payload_len > 0) {
        memcpy(packet + IMAGE_PAYLOAD_OFFSET, payload, payload_len);
    }
    checksum_packet(packet);
}

static void sleep_millis(long millis) {
    struct timespec ts;
    ts.tv_sec = millis / 1000;
    ts.tv_nsec = (millis % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
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

static CFMutableDictionaryRef create_matching_dict(const options_t *opt) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                            0,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    if (dict == NULL) {
        return NULL;
    }
    dict_set_int(dict, CFSTR(kIOHIDVendorIDKey), opt->vid);
    dict_set_int(dict, CFSTR(kIOHIDProductIDKey), opt->pid);
    dict_set_int(dict, CFSTR(kIOHIDPrimaryUsagePageKey), opt->usage_page);
    dict_set_int(dict, CFSTR(kIOHIDPrimaryUsageKey), opt->usage);
    return dict;
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

    session->manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (session->manager == NULL) {
        fprintf(stderr, "IOHIDManagerCreate failed\n");
        return -1;
    }

    CFMutableDictionaryRef match = create_matching_dict(opt);
    if (match == NULL) {
        fprintf(stderr, "Could not create HID matching dictionary\n");
        CFRelease(session->manager);
        memset(session, 0, sizeof(*session));
        return -1;
    }
    IOHIDManagerSetDeviceMatching(session->manager, match);
    CFRelease(match);

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
        int max_output = get_int_property(refs[i], CFSTR(kIOHIDMaxOutputReportSizeKey), -1);
        if (max_output >= REPORT_SIZE) {
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

static int parse_hex_config_text(const char *text, uint8_t out[CONFIG_RECORD_SIZE]) {
    char digits[CONFIG_RECORD_SIZE * 2 + 1];
    size_t count = 0;

    for (const char *p = text; *p != '\0'; p++) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p++;
            continue;
        }
        if (isxdigit((unsigned char)*p)) {
            if (count >= CONFIG_RECORD_SIZE * 2) {
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

    if (count != CONFIG_RECORD_SIZE * 2) {
        fprintf(stderr, "Template must contain exactly 48 bytes, got %zu hex digits\n", count);
        return -1;
    }
    digits[count] = '\0';

    for (size_t i = 0; i < CONFIG_RECORD_SIZE; i++) {
        int hi = hex_nibble(digits[i * 2]);
        int lo = hex_nibble(digits[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int load_config_template_file(const char *path, uint8_t out[CONFIG_RECORD_SIZE]) {
    if (path == NULL) {
        fprintf(stderr, "dry-run-buckets needs --template-file PATH\n");
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }

    uint8_t buf[4096];
    size_t len = fread(buf, 1, sizeof(buf), f);
    if (ferror(f)) {
        perror(path);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (len == CONFIG_RECORD_SIZE && !looks_like_hex_text(buf, len)) {
        memcpy(out, buf, CONFIG_RECORD_SIZE);
        return 0;
    }

    char *text = calloc(len + 1, 1);
    if (text == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    memcpy(text, buf, len);
    int rc = parse_hex_config_text(text, out);
    free(text);
    return rc;
}

static int read_config_template_from_device(const options_t *opt,
                                            hid_session_t *session,
                                            uint8_t record[CONFIG_RECORD_SIZE]) {
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    uint16_t offset = slot_offset(opt->slot);
    memset(record, 0, CONFIG_RECORD_SIZE);

    for (size_t pos = 0; pos < CONFIG_RECORD_SIZE; pos += 4) {
        size_t chunk_len = CONFIG_RECORD_SIZE - pos;
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
        if (opt->print_packets) {
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
        if (opt->print_packets) {
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
                "Refusing all-zero template: this is not a valid screen config record\n");
        return -1;
    }
    return 0;
}

static int load_image_rgba(const char *path,
                           fit_mode_t fit_mode,
                           size_t canvas_w,
                           size_t canvas_h,
                           uint8_t **out_rgba) {
    int rc = -1;
    CFStringRef path_string = NULL;
    CFURLRef url = NULL;
    CGImageSourceRef source = NULL;
    CGImageRef image = NULL;
    CGColorSpaceRef color_space = NULL;
    CGContextRef context = NULL;
    uint8_t *rgba = NULL;

    path_string = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    if (path_string == NULL) {
        fprintf(stderr, "Could not create path string for %s\n", path);
        goto out;
    }
    url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                        path_string,
                                        kCFURLPOSIXPathStyle,
                                        false);
    if (url == NULL) {
        fprintf(stderr, "Could not create file URL for %s\n", path);
        goto out;
    }
    source = CGImageSourceCreateWithURL(url, NULL);
    if (source == NULL) {
        fprintf(stderr, "Could not open image: %s\n", path);
        goto out;
    }
    image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    if (image == NULL) {
        fprintf(stderr, "Could not decode image: %s\n", path);
        goto out;
    }

    size_t src_w = CGImageGetWidth(image);
    size_t src_h = CGImageGetHeight(image);
    if (src_w == 0 || src_h == 0) {
        fprintf(stderr, "Decoded image has invalid dimensions\n");
        goto out;
    }

    rgba = calloc(canvas_w * canvas_h, 4);
    if (rgba == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    color_space = CGColorSpaceCreateDeviceRGB();
    if (color_space == NULL) {
        fprintf(stderr, "Could not create RGB color space\n");
        goto out;
    }
    context = CGBitmapContextCreate(rgba,
                                    canvas_w,
                                    canvas_h,
                                    8,
                                    canvas_w * 4,
                                    color_space,
                                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (context == NULL) {
        fprintf(stderr, "Could not create bitmap context\n");
        goto out;
    }

    CGContextSetRGBFillColor(context, 0, 0, 0, 1);
    CGContextFillRect(context, CGRectMake(0, 0, canvas_w, canvas_h));
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);

    double src_aspect = (double)src_w / (double)src_h;
    double dst_aspect = (double)canvas_w / (double)canvas_h;
    double draw_w = canvas_w;
    double draw_h = canvas_h;
    double draw_x = 0;
    double draw_y = 0;

    if (fit_mode == FIT_CONTAIN) {
        if (src_aspect > dst_aspect) {
            draw_w = canvas_w;
            draw_h = (double)canvas_w / src_aspect;
        } else {
            draw_h = canvas_h;
            draw_w = (double)canvas_h * src_aspect;
        }
        draw_x = ((double)canvas_w - draw_w) / 2.0;
        draw_y = ((double)canvas_h - draw_h) / 2.0;
    } else if (fit_mode == FIT_COVER) {
        if (src_aspect > dst_aspect) {
            draw_h = canvas_h;
            draw_w = (double)canvas_h * src_aspect;
        } else {
            draw_w = canvas_w;
            draw_h = (double)canvas_w / src_aspect;
        }
        draw_x = ((double)canvas_w - draw_w) / 2.0;
        draw_y = ((double)canvas_h - draw_h) / 2.0;
    }

    CGContextDrawImage(context, CGRectMake(draw_x, draw_y, draw_w, draw_h), image);
    *out_rgba = rgba;
    rgba = NULL;
    rc = 0;

out:
    if (context != NULL) {
        CGContextRelease(context);
    }
    if (color_space != NULL) {
        CGColorSpaceRelease(color_space);
    }
    if (image != NULL) {
        CGImageRelease(image);
    }
    if (source != NULL) {
        CFRelease(source);
    }
    if (url != NULL) {
        CFRelease(url);
    }
    if (path_string != NULL) {
        CFRelease(path_string);
    }
    free(rgba);
    return rc;
}

static int render_cgimage_rgba(CGImageRef image,
                               fit_mode_t fit_mode,
                               size_t canvas_w,
                               size_t canvas_h,
                               uint8_t **out_rgba) {
    int rc = -1;
    CGColorSpaceRef color_space = NULL;
    CGContextRef context = NULL;
    uint8_t *rgba = NULL;

    size_t src_w = CGImageGetWidth(image);
    size_t src_h = CGImageGetHeight(image);
    if (src_w == 0 || src_h == 0) {
        fprintf(stderr, "Decoded image frame has invalid dimensions\n");
        return -1;
    }

    rgba = calloc(canvas_w * canvas_h, 4);
    if (rgba == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    color_space = CGColorSpaceCreateDeviceRGB();
    if (color_space == NULL) {
        fprintf(stderr, "Could not create RGB color space\n");
        goto out;
    }
    context = CGBitmapContextCreate(rgba,
                                    canvas_w,
                                    canvas_h,
                                    8,
                                    canvas_w * 4,
                                    color_space,
                                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (context == NULL) {
        fprintf(stderr, "Could not create bitmap context\n");
        goto out;
    }

    CGContextSetRGBFillColor(context, 0, 0, 0, 1);
    CGContextFillRect(context, CGRectMake(0, 0, canvas_w, canvas_h));
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);

    double src_aspect = (double)src_w / (double)src_h;
    double dst_aspect = (double)canvas_w / (double)canvas_h;
    double draw_w = canvas_w;
    double draw_h = canvas_h;
    double draw_x = 0;
    double draw_y = 0;

    if (fit_mode == FIT_CONTAIN) {
        if (src_aspect > dst_aspect) {
            draw_w = canvas_w;
            draw_h = (double)canvas_w / src_aspect;
        } else {
            draw_h = canvas_h;
            draw_w = (double)canvas_h * src_aspect;
        }
        draw_x = ((double)canvas_w - draw_w) / 2.0;
        draw_y = ((double)canvas_h - draw_h) / 2.0;
    } else if (fit_mode == FIT_COVER) {
        if (src_aspect > dst_aspect) {
            draw_h = canvas_h;
            draw_w = (double)canvas_h * src_aspect;
        } else {
            draw_w = canvas_w;
            draw_h = (double)canvas_w / src_aspect;
        }
        draw_x = ((double)canvas_w - draw_w) / 2.0;
        draw_y = ((double)canvas_h - draw_h) / 2.0;
    }

    CGContextDrawImage(context, CGRectMake(draw_x, draw_y, draw_w, draw_h), image);
    *out_rgba = rgba;
    rgba = NULL;
    rc = 0;

out:
    if (context != NULL) {
        CGContextRelease(context);
    }
    if (color_space != NULL) {
        CGColorSpaceRelease(color_space);
    }
    free(rgba);
    return rc;
}

static int load_image_frame_rgba(CGImageSourceRef source,
                                 size_t frame_index,
                                 const char *label,
                                 fit_mode_t fit_mode,
                                 size_t canvas_w,
                                 size_t canvas_h,
                                 uint8_t **out_rgba) {
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, frame_index, NULL);
    if (image == NULL) {
        fprintf(stderr, "Could not decode %s frame %zu\n", label, frame_index);
        return -1;
    }
    int rc = render_cgimage_rgba(image, fit_mode, canvas_w, canvas_h, out_rgba);
    CGImageRelease(image);
    return rc;
}

static int write_preview_png(const char *path,
                             const uint8_t *rgba,
                             size_t width,
                             size_t height) {
    int rc = -1;
    CFStringRef path_string = NULL;
    CFURLRef url = NULL;
    CGColorSpaceRef color_space = NULL;
    CGContextRef context = NULL;
    CGImageRef image = NULL;
    CGImageDestinationRef destination = NULL;

    path_string = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    if (path_string == NULL) {
        fprintf(stderr, "Could not create preview path string for %s\n", path);
        goto out;
    }
    url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                        path_string,
                                        kCFURLPOSIXPathStyle,
                                        false);
    if (url == NULL) {
        fprintf(stderr, "Could not create preview file URL for %s\n", path);
        goto out;
    }
    color_space = CGColorSpaceCreateDeviceRGB();
    if (color_space == NULL) {
        fprintf(stderr, "Could not create RGB color space for preview\n");
        goto out;
    }
    context = CGBitmapContextCreate((void *)rgba,
                                    width,
                                    height,
                                    8,
                                    width * 4,
                                    color_space,
                                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (context == NULL) {
        fprintf(stderr, "Could not create preview bitmap context\n");
        goto out;
    }
    image = CGBitmapContextCreateImage(context);
    if (image == NULL) {
        fprintf(stderr, "Could not create preview image\n");
        goto out;
    }
    destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (destination == NULL) {
        fprintf(stderr, "Could not create preview destination: %s\n", path);
        goto out;
    }
    CGImageDestinationAddImage(destination, image, NULL);
    if (!CGImageDestinationFinalize(destination)) {
        fprintf(stderr, "Could not write preview PNG: %s\n", path);
        goto out;
    }
    printf("wrote preview PNG to %s\n", path);
    rc = 0;

out:
    if (destination != NULL) {
        CFRelease(destination);
    }
    if (image != NULL) {
        CGImageRelease(image);
    }
    if (context != NULL) {
        CGContextRelease(context);
    }
    if (color_space != NULL) {
        CGColorSpaceRelease(color_space);
    }
    if (url != NULL) {
        CFRelease(url);
    }
    if (path_string != NULL) {
        CFRelease(path_string);
    }
    return rc;
}

static void logical_dimensions(const options_t *opt, size_t *width, size_t *height) {
    if (opt->orientation == ORIENTATION_PORTRAIT) {
        *width = PORTRAIT_WIDTH;
        *height = PORTRAIT_HEIGHT;
    } else {
        *width = FRAMEBUFFER_WIDTH;
        *height = FRAMEBUFFER_HEIGHT;
    }
}

static int rotate_logical_to_framebuffer(const uint8_t *logical,
                                         size_t logical_w,
                                         size_t logical_h,
                                         int rotate_degrees,
                                         uint8_t framebuffer[IMAGE_PIXELS * 4]) {
    size_t rotated_w = logical_w;
    size_t rotated_h = logical_h;
    if (rotate_degrees == 90 || rotate_degrees == 270) {
        rotated_w = logical_h;
        rotated_h = logical_w;
    }
    if (rotated_w != FRAMEBUFFER_WIDTH || rotated_h != FRAMEBUFFER_HEIGHT) {
        fprintf(stderr,
                "Rotation %d maps %zux%zu to %zux%zu, not required framebuffer %dx%d\n",
                rotate_degrees,
                logical_w,
                logical_h,
                rotated_w,
                rotated_h,
                FRAMEBUFFER_WIDTH,
                FRAMEBUFFER_HEIGHT);
        return -1;
    }

    for (size_t y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (size_t x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            size_t sx = x;
            size_t sy = y;
            if (rotate_degrees == 90) {
                sx = y;
                sy = logical_h - 1 - x;
            } else if (rotate_degrees == 180) {
                sx = logical_w - 1 - x;
                sy = logical_h - 1 - y;
            } else if (rotate_degrees == 270) {
                sx = logical_w - 1 - y;
                sy = x;
            }
            memcpy(framebuffer + ((y * FRAMEBUFFER_WIDTH + x) * 4),
                   logical + ((sy * logical_w + sx) * 4),
                   4);
        }
    }
    return 0;
}

static void encode_rgb565_payload(const uint8_t *rgba, uint8_t payload[PADDED_IMAGE_SIZE]) {
    memset(payload, 0, PADDED_IMAGE_SIZE);
    for (size_t i = 0; i < IMAGE_PIXELS; i++) {
        uint8_t red = rgba[i * 4 + 0];
        uint8_t green = rgba[i * 4 + 1];
        uint8_t blue = rgba[i * 4 + 2];
        uint16_t rgb565 = (uint16_t)(((red >> 3) << 11) |
                                     ((green >> 2) << 5) |
                                     (blue >> 3));
        payload[i * 2 + 0] = (uint8_t)(rgb565 >> 8);
        payload[i * 2 + 1] = (uint8_t)(rgb565 & 0xff);
    }
}

static size_t image_packet_count(void) {
    return (PADDED_IMAGE_SIZE + IMAGE_CHUNK_SIZE - 1) / IMAGE_CHUNK_SIZE;
}

static size_t payload_packet_count(size_t payload_len) {
    return (payload_len + IMAGE_CHUNK_SIZE - 1) / IMAGE_CHUNK_SIZE;
}

static void print_image_summary(const uint8_t payload[PADDED_IMAGE_SIZE],
                                const options_t *opt) {
    char raw_hash[65];
    char padded_hash[65];
    uint8_t begin[REPORT_SIZE];
    uint8_t first[REPORT_SIZE];
    uint8_t last[REPORT_SIZE];
    uint8_t commit[REPORT_SIZE];
    size_t packet_count = image_packet_count();
    size_t last_offset = (packet_count - 1) * IMAGE_CHUNK_SIZE;
    size_t last_len = PADDED_IMAGE_SIZE - last_offset;

    build_simple_packet(opt->report_id, 0x01, begin);
    build_image_packet(opt->report_id, payload, IMAGE_CHUNK_SIZE, 0, first);
    build_image_packet(opt->report_id, payload + last_offset, last_len, (uint32_t)last_offset, last);
    build_simple_packet(opt->report_id, 0x02, commit);

    sha256_hex(payload, RGB565_SIZE, raw_hash);
    sha256_hex(payload, PADDED_IMAGE_SIZE, padded_hash);

    printf("framebuffer: %dx%d\n", FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
    printf("rgb565 bytes: %d\n", RGB565_SIZE);
    printf("padded bytes: %d\n", PADDED_IMAGE_SIZE);
    printf("chunk bytes: %d\n", IMAGE_CHUNK_SIZE);
    printf("image packet count: %zu\n", packet_count);
    printf("rgb565 sha256: %s\n", raw_hash);
    printf("padded sha256: %s\n", padded_hash);
    print_hex("first 32 rgb565 bytes: ", payload, 32);

    if (opt->print_packets) {
        print_hex("cmd 0x01 packet: ", begin, REPORT_SIZE);
        print_hex("first cmd 0x21 packet: ", first, REPORT_SIZE);
        print_hex("last  cmd 0x21 packet: ", last, REPORT_SIZE);
        print_hex("cmd 0x02 packet: ", commit, REPORT_SIZE);
    }
}

static void free_frame_bucket(frame_bucket_t *bucket) {
    free(bucket->payload);
    bucket->payload = NULL;
    bucket->frame_count = 0;
}

static void free_bucket_upload_plan(bucket_upload_plan_t *plan) {
    free_frame_bucket(&plan->bucket1);
    free_frame_bucket(&plan->bucket2);
    free(plan->payload);
    memset(plan, 0, sizeof(*plan));
}

static CGImageSourceRef create_image_source_for_path(const char *path) {
    CFStringRef path_string = NULL;
    CFURLRef url = NULL;
    CGImageSourceRef source = NULL;

    path_string = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    if (path_string == NULL) {
        fprintf(stderr, "Could not create path string for %s\n", path);
        goto out;
    }
    url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                        path_string,
                                        kCFURLPOSIXPathStyle,
                                        false);
    if (url == NULL) {
        fprintf(stderr, "Could not create file URL for %s\n", path);
        goto out;
    }
    source = CGImageSourceCreateWithURL(url, NULL);
    if (source == NULL) {
        fprintf(stderr, "Could not open image/GIF: %s\n", path);
    }

out:
    if (url != NULL) {
        CFRelease(url);
    }
    if (path_string != NULL) {
        CFRelease(path_string);
    }
    return source;
}

static int build_bucket_payload(const options_t *opt,
                                const char *path,
                                const char *label,
                                const char *preview_file,
                                const char *framebuffer_preview_file,
                                frame_bucket_t *bucket) {
    int rc = -1;
    CGImageSourceRef source = NULL;
    uint8_t *logical_rgba = NULL;
    uint8_t *framebuffer_rgba = NULL;
    size_t logical_w = 0;
    size_t logical_h = 0;

    memset(bucket, 0, sizeof(*bucket));
    logical_dimensions(opt, &logical_w, &logical_h);

    source = create_image_source_for_path(path);
    if (source == NULL) {
        goto out;
    }

    size_t frame_count = CGImageSourceGetCount(source);
    if (frame_count == 0) {
        fprintf(stderr, "%s has no decodable frames: %s\n", label, path);
        goto out;
    }
    if (frame_count > (size_t)opt->max_frames_per_bucket) {
        fprintf(stderr,
                "%s has %zu frames, above --max-frames-per-bucket %d\n",
                label,
                frame_count,
                opt->max_frames_per_bucket);
        goto out;
    }

    bucket->payload = calloc(frame_count, PADDED_IMAGE_SIZE);
    if (bucket->payload == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    bucket->frame_count = frame_count;

    framebuffer_rgba = malloc(IMAGE_PIXELS * 4);
    if (framebuffer_rgba == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }

    for (size_t i = 0; i < frame_count; i++) {
        free(logical_rgba);
        logical_rgba = NULL;
        if (load_image_frame_rgba(source,
                                  i,
                                  label,
                                  opt->fit_mode,
                                  logical_w,
                                  logical_h,
                                  &logical_rgba) != 0) {
            goto out;
        }
        if (rotate_logical_to_framebuffer(logical_rgba,
                                          logical_w,
                                          logical_h,
                                          opt->rotate_degrees,
                                          framebuffer_rgba) != 0) {
            goto out;
        }
        encode_rgb565_payload(framebuffer_rgba, bucket->payload + i * PADDED_IMAGE_SIZE);

        if (i == 0 && preview_file != NULL &&
            write_preview_png(preview_file, logical_rgba, logical_w, logical_h) != 0) {
            goto out;
        }
        if (i == 0 && framebuffer_preview_file != NULL &&
            write_preview_png(framebuffer_preview_file,
                              framebuffer_rgba,
                              FRAMEBUFFER_WIDTH,
                              FRAMEBUFFER_HEIGHT) != 0) {
            goto out;
        }
    }

    printf("%s frames: %zu\n", label, bucket->frame_count);
    fflush(stdout);
    rc = 0;

out:
    if (rc != 0) {
        free_frame_bucket(bucket);
    }
    free(framebuffer_rgba);
    free(logical_rgba);
    if (source != NULL) {
        CFRelease(source);
    }
    return rc;
}

static int build_requested_bucket_payloads(const options_t *opt, bucket_upload_plan_t *plan) {
    int rc = -1;
    memset(plan, 0, sizeof(*plan));

    plan->write_bucket1 = opt->bucket1_file != NULL;
    plan->write_bucket2 = opt->bucket2_file != NULL;
    if (!plan->write_bucket1 && !plan->write_bucket2) {
        fprintf(stderr, "bucket upload needs --bucket1 PATH, --bucket2 PATH, or both\n");
        return -1;
    }

    if (plan->write_bucket1) {
        if (build_bucket_payload(opt,
                                 opt->bucket1_file,
                                 "GIF1",
                                 opt->bucket1_preview_file,
                                 opt->bucket1_framebuffer_preview_file,
                                 &plan->bucket1) != 0) {
            goto out;
        }
    }
    if (plan->write_bucket2) {
        if (build_bucket_payload(opt,
                                 opt->bucket2_file,
                                 "GIF2",
                                 opt->bucket2_preview_file,
                                 opt->bucket2_framebuffer_preview_file,
                                 &plan->bucket2) != 0) {
            goto out;
        }
    }

    rc = 0;

out:
    if (rc != 0) {
        free_bucket_upload_plan(plan);
    }
    return rc;
}

static int finalize_bucket_upload_plan(const options_t *opt,
                                       const uint8_t template_record[CONFIG_RECORD_SIZE],
                                       bucket_upload_plan_t *plan) {
    uint8_t *payload = NULL;
    size_t total_frames = 0;
    size_t payload_frames = 0;

    plan->existing_bucket1_frames = template_record[BUCKET1_COUNT_OFFSET];
    plan->existing_bucket2_frames = template_record[BUCKET2_COUNT_OFFSET];
    plan->final_bucket1_frames = plan->write_bucket1 ?
                                 plan->bucket1.frame_count :
                                 plan->existing_bucket1_frames;
    plan->final_bucket2_frames = plan->write_bucket2 ?
                                 plan->bucket2.frame_count :
                                 plan->existing_bucket2_frames;

    if (plan->write_bucket1 && !plan->write_bucket2 &&
        plan->existing_bucket2_frames > 0 &&
        plan->final_bucket1_frames != plan->existing_bucket1_frames) {
        fprintf(stderr,
                "Refusing GIF1-only write: current GIF2 has %zu frame(s), and changing "
                "GIF1 from %zu to %zu frame(s) would move GIF2's storage start. "
                "Write both buckets, or use a GIF1 with exactly %zu frame(s).\n",
                plan->existing_bucket2_frames,
                plan->existing_bucket1_frames,
                plan->final_bucket1_frames,
                plan->existing_bucket1_frames);
        return -1;
    }

    total_frames = plan->final_bucket1_frames + plan->final_bucket2_frames;
    if (total_frames == 0 || total_frames > MAX_TOTAL_FRAMES) {
        fprintf(stderr,
                "Refusing %zu final frame(s); safe hard limit is %d frames total\n",
                total_frames,
                MAX_TOTAL_FRAMES);
        return -1;
    }

    if (plan->write_bucket1 && plan->write_bucket2) {
        plan->payload_offset = 0;
        payload_frames = plan->bucket1.frame_count + plan->bucket2.frame_count;
    } else if (plan->write_bucket1) {
        plan->payload_offset = 0;
        payload_frames = plan->bucket1.frame_count;
    } else {
        size_t offset = plan->final_bucket1_frames * PADDED_IMAGE_SIZE;
        if (offset > 0x00ffffffu) {
            fprintf(stderr, "Refusing GIF2 offset 0x%zx: exceeds 24-bit packet offset\n", offset);
            return -1;
        }
        plan->payload_offset = (uint32_t)offset;
        payload_frames = plan->bucket2.frame_count;
    }

    plan->payload_len = payload_frames * PADDED_IMAGE_SIZE;
    payload = malloc(plan->payload_len);
    if (payload == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    if (plan->write_bucket1 && plan->write_bucket2) {
        memcpy(payload, plan->bucket1.payload, plan->bucket1.frame_count * PADDED_IMAGE_SIZE);
        memcpy(payload + plan->bucket1.frame_count * PADDED_IMAGE_SIZE,
               plan->bucket2.payload,
               plan->bucket2.frame_count * PADDED_IMAGE_SIZE);
    } else if (plan->write_bucket1) {
        memcpy(payload, plan->bucket1.payload, plan->bucket1.frame_count * PADDED_IMAGE_SIZE);
    } else {
        memcpy(payload, plan->bucket2.payload, plan->bucket2.frame_count * PADDED_IMAGE_SIZE);
    }

    if (opt->active_bucket_set) {
        plan->final_active_bucket = opt->active_bucket;
    } else if (plan->write_bucket2 && !plan->write_bucket1) {
        plan->final_active_bucket = 2;
    } else {
        plan->final_active_bucket = 1;
    }

    plan->payload = payload;
    return 0;
}

static const char *config_offset_label(size_t offset) {
    if (offset == ACTIVE_BUCKET_OFFSET) {
        return "active bucket";
    }
    if (offset == BUCKET1_COUNT_OFFSET) {
        return "GIF1 frame count";
    }
    if (offset == BUCKET2_COUNT_OFFSET) {
        return "GIF2 frame count";
    }
    if (is_time_offset(offset)) {
        return "clock";
    }
    return "unexpected";
}

static void print_config_diff(const uint8_t before[CONFIG_RECORD_SIZE],
                              const uint8_t after[CONFIG_RECORD_SIZE]) {
    bool any = false;
    printf("Changed config offsets:\n");
    for (size_t i = 0; i < CONFIG_RECORD_SIZE; i++) {
        if (before[i] != after[i]) {
            printf("  %02zu: %02x -> %02x  %s\n",
                   i,
                   before[i],
                   after[i],
                   config_offset_label(i));
            any = true;
        }
    }
    if (!any) {
        printf("  none\n");
    }
}

static int verify_only_bucket_metadata_changed(const uint8_t before[CONFIG_RECORD_SIZE],
                                               const uint8_t after[CONFIG_RECORD_SIZE],
                                               bool allow_bucket1_count,
                                               bool allow_bucket2_count,
                                               bool preserve_clock) {
    for (size_t i = 0; i < CONFIG_RECORD_SIZE; i++) {
        bool allowed = i == ACTIVE_BUCKET_OFFSET ||
                       (allow_bucket1_count && i == BUCKET1_COUNT_OFFSET) ||
                       (allow_bucket2_count && i == BUCKET2_COUNT_OFFSET) ||
                       (!preserve_clock && is_time_offset(i));
        if (!allowed && before[i] != after[i]) {
            fprintf(stderr,
                    "Refusing: unexpected config byte changed at offset %zu (%02x -> %02x)\n",
                    i,
                    before[i],
                    after[i]);
            return -1;
        }
    }
    return 0;
}

static int patch_bucket_config(const options_t *opt,
                               const uint8_t before[CONFIG_RECORD_SIZE],
                               const bucket_upload_plan_t *plan,
                               uint8_t after[CONFIG_RECORD_SIZE]) {
    uint8_t time_bytes[TIME_SIZE];

    if (plan->final_bucket1_frames > MAX_HARD_FRAMES_PER_BUCKET ||
        plan->final_bucket2_frames > MAX_HARD_FRAMES_PER_BUCKET) {
        fprintf(stderr, "Refusing invalid bucket frame counts: GIF1=%zu GIF2=%zu\n",
                plan->final_bucket1_frames,
                plan->final_bucket2_frames);
        return -1;
    }
    if (plan->final_bucket1_frames + plan->final_bucket2_frames == 0) {
        fprintf(stderr, "Refusing config with zero total custom frames\n");
        return -1;
    }

    memcpy(after, before, CONFIG_RECORD_SIZE);
    after[ACTIVE_BUCKET_OFFSET] = (uint8_t)plan->final_active_bucket;
    after[BUCKET1_COUNT_OFFSET] = (uint8_t)plan->final_bucket1_frames;
    after[BUCKET2_COUNT_OFFSET] = (uint8_t)plan->final_bucket2_frames;
    if (!opt->preserve_clock) {
        if (fill_now_time_bytes(time_bytes) != 0) {
            return -1;
        }
        memcpy(after + TIME_OFFSET, time_bytes, TIME_SIZE);
    }
    return verify_only_bucket_metadata_changed(before,
                                               after,
                                               plan->write_bucket1,
                                               plan->write_bucket2,
                                               opt->preserve_clock);
}

static void print_bucket_config_summary(const options_t *opt,
                                        const uint8_t before[CONFIG_RECORD_SIZE],
                                        const uint8_t after[CONFIG_RECORD_SIZE]) {
    char hash_before[65];
    char hash_after[65];
    char stable_before[65];
    char stable_after[65];
    uint8_t packet[REPORT_SIZE];

    sha256_hex(before, CONFIG_RECORD_SIZE, hash_before);
    sha256_hex(after, CONFIG_RECORD_SIZE, hash_after);
    stable_config_sha256_hex(before, stable_before);
    stable_config_sha256_hex(after, stable_after);

    print_hex("template: ", before, CONFIG_RECORD_SIZE);
    print_hex("patched:  ", after, CONFIG_RECORD_SIZE);
    printf("template sha256: %s\n", hash_before);
    printf("patched  sha256: %s\n", hash_after);
    printf("template stable sha256: %s\n", stable_before);
    printf("patched  stable sha256: %s\n", stable_after);
    print_config_diff(before, after);

    if (opt->print_packets) {
        build_config_packet(opt->report_id,
                            0x06,
                            after,
                            CONFIG_RECORD_SIZE,
                            slot_offset(opt->slot),
                            packet);
        print_hex("cmd 0x06 config packet: ", packet, REPORT_SIZE);
    }
}

static void print_bucket_image_summary(const options_t *opt,
                                       const bucket_upload_plan_t *plan) {
    char payload_hash[65];
    uint8_t config_begin[REPORT_SIZE];
    uint8_t config_commit[REPORT_SIZE];
    uint8_t image_prepare[REPORT_SIZE];
    uint8_t first[REPORT_SIZE];
    uint8_t last[REPORT_SIZE];
    uint8_t image_commit[REPORT_SIZE];
    size_t packet_count = payload_packet_count(plan->payload_len);
    size_t last_offset = (packet_count - 1) * IMAGE_CHUNK_SIZE;
    size_t last_len = plan->payload_len - last_offset;

    build_simple_packet(opt->report_id, 0x01, config_begin);
    build_simple_packet(opt->report_id, 0x02, config_commit);
    build_simple_packet(opt->report_id, 0x23, image_prepare);
    build_image_packet(opt->report_id,
                       plan->payload,
                       IMAGE_CHUNK_SIZE,
                       plan->payload_offset,
                       first);
    build_image_packet(opt->report_id,
                       plan->payload + last_offset,
                       last_len,
                       plan->payload_offset + (uint32_t)last_offset,
                       last);
    build_simple_packet(opt->report_id, 0x02, image_commit);
    sha256_hex(plan->payload, plan->payload_len, payload_hash);

    printf("framebuffer: %dx%d\n", FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
    printf("rgb565 bytes per frame: %d\n", RGB565_SIZE);
    printf("padded bytes per frame: %d\n", PADDED_IMAGE_SIZE);
    printf("chunk bytes: %d\n", IMAGE_CHUNK_SIZE);
    printf("write GIF1: %s\n", plan->write_bucket1 ? "yes" : "no");
    printf("write GIF2: %s\n", plan->write_bucket2 ? "yes" : "no");
    printf("existing GIF1 frame count: %zu\n", plan->existing_bucket1_frames);
    printf("existing GIF2 frame count: %zu\n", plan->existing_bucket2_frames);
    printf("final GIF1 frame count: %zu\n", plan->final_bucket1_frames);
    printf("final GIF2 frame count: %zu\n", plan->final_bucket2_frames);
    printf("final total frame count: %zu\n",
           plan->final_bucket1_frames + plan->final_bucket2_frames);
    printf("payload start frame: %zu\n", (size_t)plan->payload_offset / PADDED_IMAGE_SIZE);
    printf("payload start byte offset: %u\n", plan->payload_offset);
    printf("payload bytes sent: %zu\n", plan->payload_len);
    printf("image packet count: %zu\n", packet_count);
    printf("payload sha256: %s\n", payload_hash);
    print_hex("first 32 rgb565 bytes: ", plan->payload, 32);

    if (opt->print_packets) {
        print_hex("cmd 0x01 config begin packet: ", config_begin, REPORT_SIZE);
        print_hex("cmd 0x02 config commit packet: ", config_commit, REPORT_SIZE);
        print_hex("cmd 0x23 image prepare packet: ", image_prepare, REPORT_SIZE);
        print_hex("first cmd 0x21 image packet: ", first, REPORT_SIZE);
        print_hex("last  cmd 0x21 image packet: ", last, REPORT_SIZE);
        print_hex("cmd 0x02 image commit packet: ", image_commit, REPORT_SIZE);
    }
}

static int write_bucket_config_to_device(const options_t *opt,
                                         hid_session_t *session,
                                         const uint8_t patched[CONFIG_RECORD_SIZE]) {
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len = 0;

    build_simple_packet(opt->report_id, 0x01, packet);
    if (send_report_wait(session, packet, 0x01, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    build_config_packet(opt->report_id,
                        0x06,
                        patched,
                        CONFIG_RECORD_SIZE,
                        slot_offset(opt->slot),
                        packet);
    if (send_report_wait(session, packet, 0x06, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    sleep_millis(10);
    build_simple_packet(opt->report_id, 0x02, packet);
    if (send_report_wait(session, packet, 0x02, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    return 0;
}

static int write_bucket_images_to_device(const options_t *opt,
                                         hid_session_t *session,
                                         const bucket_upload_plan_t *plan) {
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len = 0;
    size_t packet_count = payload_packet_count(plan->payload_len);

    build_simple_packet(opt->report_id, 0x23, packet);
    if (send_report_wait(session, packet, 0x23, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }

    for (size_t i = 0; i < packet_count; i++) {
        size_t offset = i * IMAGE_CHUNK_SIZE;
        size_t chunk_len = plan->payload_len - offset;
        uint32_t absolute_offset = plan->payload_offset + (uint32_t)offset;
        if (chunk_len > IMAGE_CHUNK_SIZE) {
            chunk_len = IMAGE_CHUNK_SIZE;
        }
        build_image_packet(opt->report_id,
                           plan->payload + offset,
                           chunk_len,
                           absolute_offset,
                           packet);
        if (send_report_wait(session, packet, 0x21, opt->timeout_ms, response, &response_len) != 0) {
            return -1;
        }
        if (offset == 0) {
            sleep_millis(500);
        }
        if ((i + 1) % 100 == 0 || i + 1 == packet_count) {
            printf("uploaded image packets: %zu/%zu\n", i + 1, packet_count);
            fflush(stdout);
        }
    }

    sleep_millis(10);
    build_simple_packet(opt->report_id, 0x02, packet);
    if (send_report_wait(session, packet, 0x02, opt->timeout_ms, response, &response_len) != 0) {
        return -1;
    }
    return 0;
}

static int run_dry_run(const options_t *opt) {
    uint8_t *logical_rgba = NULL;
    uint8_t *framebuffer_rgba = NULL;
    uint8_t *payload = NULL;
    size_t logical_w = 0;
    size_t logical_h = 0;
    int rc = 1;

    if (opt->image_file == NULL) {
        fprintf(stderr, "dry-run needs --image PATH\n");
        return 2;
    }
    logical_dimensions(opt, &logical_w, &logical_h);
    if (load_image_rgba(opt->image_file, opt->fit_mode, logical_w, logical_h, &logical_rgba) != 0) {
        return 1;
    }
    framebuffer_rgba = malloc(IMAGE_PIXELS * 4);
    if (framebuffer_rgba == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    if (rotate_logical_to_framebuffer(logical_rgba,
                                      logical_w,
                                      logical_h,
                                      opt->rotate_degrees,
                                      framebuffer_rgba) != 0) {
        goto out;
    }
    payload = malloc(PADDED_IMAGE_SIZE);
    if (payload == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    encode_rgb565_payload(framebuffer_rgba, payload);
    if (opt->preview_file != NULL &&
        write_preview_png(opt->preview_file, logical_rgba, logical_w, logical_h) != 0) {
        goto out;
    }
    if (opt->framebuffer_preview_file != NULL &&
        write_preview_png(opt->framebuffer_preview_file,
                          framebuffer_rgba,
                          FRAMEBUFFER_WIDTH,
                          FRAMEBUFFER_HEIGHT) != 0) {
        goto out;
    }
    printf("physical preview: %zux%zu\n", logical_w, logical_h);
    printf("rotation into framebuffer: %d\n", opt->rotate_degrees);
    print_image_summary(payload, opt);
    printf("No HID device was opened. No report was sent.\n");
    rc = 0;

out:
    free(payload);
    free(framebuffer_rgba);
    free(logical_rgba);
    return rc;
}

static int run_dry_run_buckets(const options_t *opt) {
    bucket_upload_plan_t plan;
    uint8_t template_record[CONFIG_RECORD_SIZE];
    uint8_t patched[CONFIG_RECORD_SIZE];
    size_t logical_w = 0;
    size_t logical_h = 0;
    int rc = 1;

    if (opt->bucket1_file == NULL && opt->bucket2_file == NULL) {
        fprintf(stderr, "dry-run-buckets needs --bucket1 PATH, --bucket2 PATH, or both\n");
        return 2;
    }
    if (load_config_template_file(opt->template_file, template_record) != 0) {
        return 1;
    }
    if (record_all_zero(template_record)) {
        fprintf(stderr,
                "Refusing all-zero template: this is not a valid screen config record\n");
        return 1;
    }

    if (build_requested_bucket_payloads(opt, &plan) != 0) {
        return 1;
    }
    if (finalize_bucket_upload_plan(opt, template_record, &plan) != 0) {
        goto out;
    }
    if (patch_bucket_config(opt, template_record, &plan, patched) != 0) {
        goto out;
    }

    logical_dimensions(opt, &logical_w, &logical_h);
    printf("physical preview: %zux%zu\n", logical_w, logical_h);
    printf("rotation into framebuffer: %d\n", opt->rotate_degrees);
    printf("active bucket after upload: %d\n", plan.final_active_bucket);
    printf("clock bytes: %s\n", opt->preserve_clock ? "preserved" : "refreshed to local time");
    print_bucket_config_summary(opt, template_record, patched);
    print_bucket_image_summary(opt, &plan);
    printf("No HID device was opened. No report was sent.\n");
    rc = 0;

out:
    free_bucket_upload_plan(&plan);
    return rc;
}

static int run_upload(const options_t *opt) {
    uint8_t *logical_rgba = NULL;
    uint8_t *framebuffer_rgba = NULL;
    uint8_t *payload = NULL;
    hid_session_t session;
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len = 0;
    size_t logical_w = 0;
    size_t logical_h = 0;
    int rc = 1;

    if (opt->image_file == NULL) {
        fprintf(stderr, "upload needs --image PATH\n");
        return 2;
    }
    if (!opt->allow_image_write) {
        fprintf(stderr, "Refusing upload without --allow-image-write\n");
        return 2;
    }
    if (opt->confirm_upload == NULL ||
        strcmp(opt->confirm_upload, UPLOAD_CONFIRM_TEXT) != 0) {
        fprintf(stderr, "Refusing upload without --confirm-upload " UPLOAD_CONFIRM_TEXT "\n");
        return 2;
    }
#if !IMAGE_UPLOAD_METADATA_IMPLEMENTED
    fprintf(stderr,
            "Refusing upload: raw image packets are implemented, but the "
            "single-image command cannot safely update the two custom buckets.\n"
            "Use dry-run-buckets and upload-buckets for real hardware writes.\n");
    return 2;
#endif

    logical_dimensions(opt, &logical_w, &logical_h);
    if (load_image_rgba(opt->image_file, opt->fit_mode, logical_w, logical_h, &logical_rgba) != 0) {
        return 1;
    }
    framebuffer_rgba = malloc(IMAGE_PIXELS * 4);
    if (framebuffer_rgba == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out_no_hid;
    }
    if (rotate_logical_to_framebuffer(logical_rgba,
                                      logical_w,
                                      logical_h,
                                      opt->rotate_degrees,
                                      framebuffer_rgba) != 0) {
        goto out_no_hid;
    }
    payload = malloc(PADDED_IMAGE_SIZE);
    if (payload == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out_no_hid;
    }
    encode_rgb565_payload(framebuffer_rgba, payload);
    if (opt->preview_file != NULL &&
        write_preview_png(opt->preview_file, logical_rgba, logical_w, logical_h) != 0) {
        goto out_no_hid;
    }
    if (opt->framebuffer_preview_file != NULL &&
        write_preview_png(opt->framebuffer_preview_file,
                          framebuffer_rgba,
                          FRAMEBUFFER_WIDTH,
                          FRAMEBUFFER_HEIGHT) != 0) {
        goto out_no_hid;
    }
    printf("physical preview: %zux%zu\n", logical_w, logical_h);
    printf("rotation into framebuffer: %d\n", opt->rotate_degrees);
    print_image_summary(payload, opt);

    if (open_hid_session(opt, &session) != 0) {
        goto out_no_hid;
    }

    build_simple_packet(opt->report_id, 0x01, packet);
    if (send_report_wait(&session, packet, 0x01, opt->timeout_ms, response, &response_len) != 0) {
        goto out_hid;
    }

    size_t packet_count = image_packet_count();
    for (size_t i = 0; i < packet_count; i++) {
        size_t offset = i * IMAGE_CHUNK_SIZE;
        size_t chunk_len = PADDED_IMAGE_SIZE - offset;
        if (chunk_len > IMAGE_CHUNK_SIZE) {
            chunk_len = IMAGE_CHUNK_SIZE;
        }
        build_image_packet(opt->report_id,
                           payload + offset,
                           chunk_len,
                           (uint32_t)offset,
                           packet);
        if (send_report_wait(&session, packet, 0x21, opt->timeout_ms, response, &response_len) != 0) {
            goto out_hid;
        }
        if (offset == 0) {
            sleep_millis(500);
        }
        if ((i + 1) % 100 == 0 || i + 1 == packet_count) {
            printf("uploaded image packets: %zu/%zu\n", i + 1, packet_count);
            fflush(stdout);
        }
    }

    sleep_millis(10);
    build_simple_packet(opt->report_id, 0x02, packet);
    if (send_report_wait(&session, packet, 0x02, opt->timeout_ms, response, &response_len) != 0) {
        goto out_hid;
    }

    printf("Image upload command sequence completed.\n");
    rc = 0;

out_hid:
    close_hid_session(&session);
out_no_hid:
    free(payload);
    free(framebuffer_rgba);
    free(logical_rgba);
    return rc;
}

static int run_upload_buckets(const options_t *opt) {
    bucket_upload_plan_t plan;
    uint8_t current[CONFIG_RECORD_SIZE];
    uint8_t patched[CONFIG_RECORD_SIZE];
    char stable_hash_current[65];
    hid_session_t session;
    int rc = 1;
    bool hid_open = false;

    if (opt->bucket1_file == NULL && opt->bucket2_file == NULL) {
        fprintf(stderr, "upload-buckets needs --bucket1 PATH, --bucket2 PATH, or both\n");
        return 2;
    }
    if (opt->bucket1_file == NULL || opt->bucket2_file == NULL) {
        fprintf(stderr,
                "Refusing one-bucket upload: command 0x23 appears to operate on the "
                "combined custom-image store. Provide both --bucket1 and --bucket2.\n");
        return 2;
    }
    if (!opt->allow_hid_query) {
        fprintf(stderr, "Refusing upload without --allow-hid-query\n");
        return 2;
    }
    if (!opt->allow_config_write) {
        fprintf(stderr, "Refusing upload without --allow-config-write\n");
        return 2;
    }
    if (!opt->allow_image_write) {
        fprintf(stderr, "Refusing upload without --allow-image-write\n");
        return 2;
    }
    if (opt->confirm_upload == NULL ||
        strcmp(opt->confirm_upload, UPLOAD_CONFIRM_TEXT) != 0) {
        fprintf(stderr, "Refusing upload without --confirm-upload " UPLOAD_CONFIRM_TEXT "\n");
        return 2;
    }
    if (opt->expected_stable_sha256 == NULL) {
        fprintf(stderr, "Refusing upload without --expected-stable-sha256 from dry-run/read-template\n");
        return 2;
    }

    if (build_requested_bucket_payloads(opt, &plan) != 0) {
        return 1;
    }

    if (open_hid_session(opt, &session) != 0) {
        goto out_no_hid;
    }
    hid_open = true;

    if (read_config_template_from_device(opt, &session, current) != 0) {
        goto out;
    }
    stable_config_sha256_hex(current, stable_hash_current);
    if (!hex_equals_ci(stable_hash_current, opt->expected_stable_sha256)) {
        fprintf(stderr,
                "Refusing upload: stable template sha256 %s does not match expected %s\n",
                stable_hash_current,
                opt->expected_stable_sha256);
        goto out;
    }
    if (finalize_bucket_upload_plan(opt, current, &plan) != 0) {
        goto out;
    }
    if (patch_bucket_config(opt, current, &plan, patched) != 0) {
        goto out;
    }

    printf("active bucket after upload: %d\n", plan.final_active_bucket);
    printf("clock bytes: %s\n", opt->preserve_clock ? "preserved" : "refreshed to local time");
    print_bucket_config_summary(opt, current, patched);
    print_bucket_image_summary(opt, &plan);

    if (write_bucket_config_to_device(opt, &session, patched) != 0) {
        goto out;
    }
    if (write_bucket_images_to_device(opt, &session, &plan) != 0) {
        goto out;
    }

    printf("Two-bucket image upload command sequence completed.\n");
    rc = 0;

out:
    if (hid_open) {
        close_hid_session(&session);
    }
out_no_hid:
    free_bucket_upload_plan(&plan);
    return rc;
}

static command_t parse_command(const char *s) {
    if (strcmp(s, "help") == 0 || strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
        return CMD_HELP;
    }
    if (strcmp(s, "dry-run") == 0) {
        return CMD_DRY_RUN;
    }
    if (strcmp(s, "upload") == 0) {
        return CMD_UPLOAD;
    }
    if (strcmp(s, "dry-run-buckets") == 0) {
        return CMD_DRY_RUN_BUCKETS;
    }
    if (strcmp(s, "upload-buckets") == 0) {
        return CMD_UPLOAD_BUCKETS;
    }
    return CMD_NONE;
}

static fit_mode_t parse_fit_mode(const char *s) {
    if (strcmp(s, "cover") == 0) {
        return FIT_COVER;
    }
    if (strcmp(s, "contain") == 0) {
        return FIT_CONTAIN;
    }
    if (strcmp(s, "stretch") == 0) {
        return FIT_STRETCH;
    }
    fprintf(stderr, "Invalid --fit: %s\n", s);
    exit(2);
}

static orientation_t parse_orientation(const char *s) {
    if (strcmp(s, "landscape") == 0) {
        return ORIENTATION_LANDSCAPE;
    }
    if (strcmp(s, "portrait") == 0) {
        return ORIENTATION_PORTRAIT;
    }
    fprintf(stderr, "Invalid --orientation: %s\n", s);
    exit(2);
}

static int parse_rotate(const char *s) {
    int value = parse_int_range(s, "--rotate", 0, 270);
    if (value != 0 && value != 90 && value != 180 && value != 270) {
        fprintf(stderr, "Invalid --rotate: %s; expected 0, 90, 180, or 270\n", s);
        exit(2);
    }
    return value;
}

static void init_options(options_t *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->command = CMD_NONE;
    opt->vid = DEFAULT_VID;
    opt->pid = DEFAULT_PID;
    opt->usage_page = DEFAULT_USAGE_PAGE;
    opt->usage = DEFAULT_USAGE;
    opt->report_id = DEFAULT_REPORT_ID;
    opt->timeout_ms = 1500;
    opt->fit_mode = FIT_COVER;
    opt->orientation = ORIENTATION_LANDSCAPE;
    opt->rotate_degrees = 0;
    opt->slot = 0;
    opt->active_bucket = 1;
    opt->max_frames_per_bucket = DEFAULT_MAX_FRAMES_PER_BUCKET;
}

static int parse_options(int argc, char **argv, options_t *opt) {
    static const struct option long_opts[] = {
        {"image", required_argument, NULL, 1000},
        {"preview-out", required_argument, NULL, 1001},
        {"framebuffer-preview-out", required_argument, NULL, 1002},
        {"orientation", required_argument, NULL, 1003},
        {"rotate", required_argument, NULL, 1004},
        {"fit", required_argument, NULL, 1005},
        {"vid", required_argument, NULL, 1006},
        {"pid", required_argument, NULL, 1007},
        {"usage-page", required_argument, NULL, 1008},
        {"usage", required_argument, NULL, 1009},
        {"report-id", required_argument, NULL, 1010},
        {"timeout-ms", required_argument, NULL, 1011},
        {"print-packets", no_argument, NULL, 1012},
        {"allow-image-write", no_argument, NULL, 1013},
        {"confirm-upload", required_argument, NULL, 1014},
        {"strip-report-id-for-iohid", no_argument, NULL, 1015},
        {"bucket1", required_argument, NULL, 1016},
        {"bucket2", required_argument, NULL, 1017},
        {"template-file", required_argument, NULL, 1018},
        {"expected-stable-sha256", required_argument, NULL, 1019},
        {"slot", required_argument, NULL, 1020},
        {"active-bucket", required_argument, NULL, 1021},
        {"max-frames-per-bucket", required_argument, NULL, 1022},
        {"allow-hid-query", no_argument, NULL, 1023},
        {"allow-config-write", no_argument, NULL, 1024},
        {"preserve-clock", no_argument, NULL, 1025},
        {"bucket1-preview-out", required_argument, NULL, 1026},
        {"bucket2-preview-out", required_argument, NULL, 1027},
        {"bucket1-framebuffer-preview-out", required_argument, NULL, 1028},
        {"bucket2-framebuffer-preview-out", required_argument, NULL, 1029},
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
                opt->image_file = optarg;
                break;
            case 1001:
                opt->preview_file = optarg;
                break;
            case 1002:
                opt->framebuffer_preview_file = optarg;
                break;
            case 1003:
                opt->orientation = parse_orientation(optarg);
                break;
            case 1004:
                opt->rotate_degrees = parse_rotate(optarg);
                opt->rotate_set = true;
                break;
            case 1005:
                opt->fit_mode = parse_fit_mode(optarg);
                break;
            case 1006:
                opt->vid = parse_u16(optarg, "--vid");
                break;
            case 1007:
                opt->pid = parse_u16(optarg, "--pid");
                break;
            case 1008:
                opt->usage_page = parse_u16(optarg, "--usage-page");
                break;
            case 1009:
                opt->usage = parse_u16(optarg, "--usage");
                break;
            case 1010:
                opt->report_id = (uint8_t)parse_int_range(optarg, "--report-id", 0, 255);
                break;
            case 1011:
                opt->timeout_ms = parse_int_range(optarg, "--timeout-ms", 100, 30000);
                break;
            case 1012:
                opt->print_packets = true;
                break;
            case 1013:
                opt->allow_image_write = true;
                break;
            case 1014:
                opt->confirm_upload = optarg;
                break;
            case 1015:
                opt->strip_report_id_for_iohid = true;
                break;
            case 1016:
                opt->bucket1_file = optarg;
                break;
            case 1017:
                opt->bucket2_file = optarg;
                break;
            case 1018:
                opt->template_file = optarg;
                break;
            case 1019:
                opt->expected_stable_sha256 = optarg;
                break;
            case 1020:
                opt->slot = parse_int_range(optarg, "--slot", 0, 1024);
                break;
            case 1021:
                opt->active_bucket = parse_int_range(optarg, "--active-bucket", 1, 2);
                opt->active_bucket_set = true;
                break;
            case 1022:
                opt->max_frames_per_bucket = parse_int_range(optarg,
                                                              "--max-frames-per-bucket",
                                                              1,
                                                              MAX_HARD_FRAMES_PER_BUCKET);
                break;
            case 1023:
                opt->allow_hid_query = true;
                break;
            case 1024:
                opt->allow_config_write = true;
                break;
            case 1025:
                opt->preserve_clock = true;
                break;
            case 1026:
                opt->bucket1_preview_file = optarg;
                break;
            case 1027:
                opt->bucket2_preview_file = optarg;
                break;
            case 1028:
                opt->bucket1_framebuffer_preview_file = optarg;
                break;
            case 1029:
                opt->bucket2_framebuffer_preview_file = optarg;
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

    if (opt->orientation == ORIENTATION_PORTRAIT && !opt->rotate_set) {
        opt->rotate_degrees = 90;
    }
    if (opt->orientation == ORIENTATION_LANDSCAPE &&
        (opt->rotate_degrees == 90 || opt->rotate_degrees == 270)) {
        fprintf(stderr, "--orientation landscape supports --rotate 0 or 180\n");
        return -1;
    }
    if (opt->orientation == ORIENTATION_PORTRAIT &&
        (opt->rotate_degrees == 0 || opt->rotate_degrees == 180)) {
        fprintf(stderr, "--orientation portrait supports --rotate 90 or 270\n");
        return -1;
    }
    if ((unsigned)opt->slot * CONFIG_SLOT_STRIDE > 0xffffu - (CONFIG_RECORD_SIZE - 1u)) {
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
        case CMD_DRY_RUN:
            return run_dry_run(&opt);
        case CMD_UPLOAD:
            return run_upload(&opt);
        case CMD_DRY_RUN_BUCKETS:
            return run_dry_run_buckets(&opt);
        case CMD_UPLOAD_BUCKETS:
            return run_upload_buckets(&opt);
        case CMD_NONE:
        default:
            usage(stderr);
            return 2;
    }
}
