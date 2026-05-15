#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CommonCrypto/CommonDigest.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REPORT_SIZE 64
#define FRAMEBUFFER_WIDTH 240
#define FRAMEBUFFER_HEIGHT 135
#define PORTRAIT_WIDTH 135
#define PORTRAIT_HEIGHT 240
#define DEVICE_LCD_WIDTH 96
#define DEVICE_LCD_HEIGHT 160
#define DEVICE_LCD_MEMORY_WIDTH 160
#define DEVICE_LCD_MEMORY_HEIGHT 96
#define DEVICE_SQUARE_WIDTH 128
#define DEVICE_SQUARE_HEIGHT 128
#define DEVICE_HALF_WIDTH 135
#define DEVICE_HALF_HEIGHT 120
#define IMAGE_PIXELS (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)
#define RGB565_SIZE (IMAGE_PIXELS * 2)
#define PADDED_IMAGE_SIZE 65536
#define DEFAULT_DEVICE_FRAME_BYTES 32768
#define DEFAULT_DEVICE_ROW_BYTES (DEVICE_LCD_MEMORY_WIDTH * 2)
#define IMAGE_PAYLOAD_OFFSET 8
#define DEFAULT_IMAGE_CHUNK_SIZE 0x18
#define MAX_IMAGE_CHUNK_SIZE (REPORT_SIZE - IMAGE_PAYLOAD_OFFSET)
#define CONFIG_RECORD_SIZE 48
#define CONFIG_SLOT_STRIDE 0x31
#define CONFIG_PAYLOAD_OFFSET 8
#define TIME_OFFSET 35
#define TIME_SIZE 7
#define ACTIVE_BUCKET_OFFSET 33
#define BUCKET1_COUNT_OFFSET 34
#define BUCKET2_COUNT_OFFSET 46
#define DEFAULT_MAX_FRAMES_PER_BUCKET 200
#define MAX_HARD_FRAMES_PER_BUCKET 255
#define MAX_TOTAL_FRAMES 200
#define DEFAULT_VID 0x320f
#define DEFAULT_PID 0x5055
#define DEFAULT_USAGE_PAGE 0xff1c
#define DEFAULT_USAGE 0x92
#define DEFAULT_REPORT_ID 0x04
#define IMAGE_UPLOAD_METADATA_IMPLEMENTED 0
#define IMAGE_PREPARE_BASE_TIMEOUT_MS 500
#define IMAGE_PREPARE_PER_FRAME_TIMEOUT_MS 300
#define IMAGE_PREPARE_WAIT_MS 1500

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

typedef enum {
    DEVICE_LAYOUT_LCD160X96,
    DEVICE_LAYOUT_LCD96X160,
    DEVICE_LAYOUT_SQUARE128,
    DEVICE_LAYOUT_HALF_PORTRAIT,
    DEVICE_LAYOUT_FRAMEBUFFER,
} device_layout_t;

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
    int slot;
    int active_bucket;
    int max_frames_per_bucket;
    int timeout_ms;
    int image_chunk_size;
    int device_frame_bytes;
    int device_row_bytes;
    int rotate_degrees;
    fit_mode_t fit_mode;
    orientation_t orientation;
    device_layout_t device_layout;
    bool preserve_clock;
    bool active_bucket_set;
    bool device_row_bytes_set;
    bool debug;
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
    uint8_t final_active_selector;
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

static const char *device_layout_name(device_layout_t layout);
static size_t device_layout_width(device_layout_t layout);
static size_t device_layout_height(device_layout_t layout);

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  cidoo-image dry-run --image PATH [options]\n"
            "  cidoo-image upload --image PATH [options]\n"
            "  cidoo-image dry-run-buckets --bucket1 PATH|--bucket2 PATH \\\n"
            "      --template-file PATH [options]\n"
            "  cidoo-image upload-buckets --bucket1 PATH --bucket2 PATH [options]\n"
            "\n"
            "Options:\n"
            "  --image PATH                      PNG/JPEG/etc image to upload\n"
            "  --bucket1 PATH                    still image or GIF for Custom1 / GIF1\n"
            "  --bucket2 PATH                    still image or GIF for Custom2 / GIF2\n"
            "  --template-file PATH              48-byte raw template for dry-run-buckets\n"
            "  --expected-stable-sha256 HEX      optional guard; time bytes 35..41 masked\n"
            "  --slot N                          config slot, offset = N * 0x31, default 0\n"
            "  --active-bucket 1|2               diagnostic selector override; default preserve\n"
            "  --max-frames-per-bucket N         default %d, hard max %d; total max %d\n"
            "  --image-chunk-size 24|56          command 0x21 payload bytes, default 24\n"
            "  --device-frame-bytes 32768|65536  bucket storage stride, default 32768\n"
            "  --device-row-bytes N              device row stride, default 320\n"
            "  --device-layout lcd160x96|lcd96x160|square128|half-portrait|framebuffer\n"
            "                                    RGB565 layout, default lcd160x96\n"
            "  --orientation landscape|portrait  physical screen orientation, default portrait\n"
            "  --rotate 0|90|180|270             override preview-to-framebuffer rotation\n"
            "  --preview-out PATH                write fitted physical-screen PNG preview\n"
            "  --framebuffer-preview-out PATH    write encoded 240x135 framebuffer PNG preview\n"
            "  --bucket1-preview-out PATH        write GIF1 first-frame physical preview\n"
            "  --bucket2-preview-out PATH        write GIF2 first-frame physical preview\n"
            "  --bucket1-framebuffer-preview-out PATH\n"
            "                                    write GIF1 first-frame framebuffer preview\n"
            "  --bucket2-framebuffer-preview-out PATH\n"
            "                                    write GIF2 first-frame framebuffer preview\n"
            "  --fit cover|contain|stretch       image fit mode, default stretch\n"
            "  --vid HEX                         USB VID, default 0x%04x\n"
            "  --pid HEX                         USB PID, default 0x%04x\n"
            "  --usage-page HEX                  HID usage page, default 0x%04x\n"
            "  --usage HEX                       HID usage, default 0x%02x\n"
            "  --report-id HEX                   HID report ID, default 0x%02x\n"
            "  --timeout-ms N                    HID response timeout, default 1500\n"
            "  --debug                           print hashes, config diffs, and HID packets\n"
            "  --preserve-clock                  do not refresh bytes 35..41 during bucket upload\n"
            "  --strip-report-id-for-iohid       diagnostic: pass bytes 1..63 to IOKit\n"
            "\n"
            "Safety model:\n"
            "  dry-run commands never open HID and never send a report. bucket uploads\n"
            "  read the current 48-byte template, preserve the selector byte by default,\n"
            "  patch only frame counts for requested buckets and, unless --preserve-clock\n"
            "  is used, bytes 35..41. If --expected-stable-sha256 is provided, the\n"
            "  freshly-read template must match it. Real uploads require both buckets\n"
            "  because command 0x23 appears to operate on the combined custom-image store.\n",
            DEFAULT_MAX_FRAMES_PER_BUCKET,
            MAX_HARD_FRAMES_PER_BUCKET,
            MAX_TOTAL_FRAMES,
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

static int send_report_wait_ignoring_response_error(hid_session_t *session,
                                                    const uint8_t packet[REPORT_SIZE],
                                                    uint8_t expected_cmd,
                                                    int timeout_ms,
                                                    uint8_t response[REPORT_SIZE],
                                                    size_t *response_len,
                                                    bool debug,
                                                    bool *timed_out) {
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
    if (timed_out != NULL) {
        *timed_out = !session->got_response;
    }
    if (!session->got_response) {
        if (debug) {
            fprintf(stderr, "Timed out waiting for response to cmd=0x%02x\n", expected_cmd);
        }
        return 0;
    }

    if (response != NULL && response_len != NULL) {
        memcpy(response, session->response, session->response_len);
        *response_len = session->response_len;
    }
    if (session->response_len >= 8 &&
        (session->response[7] == 0xff || session->response[7] == 0xfe)) {
        if (debug) {
            fprintf(stderr,
                    "Device returned status 0x%02x for cmd=0x%02x\n",
                    session->response[7],
                    expected_cmd);
        }
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
    if (opt->device_layout == DEVICE_LAYOUT_LCD160X96 ||
        opt->device_layout == DEVICE_LAYOUT_LCD96X160 ||
        opt->device_layout == DEVICE_LAYOUT_SQUARE128 ||
        opt->device_layout == DEVICE_LAYOUT_HALF_PORTRAIT) {
        if (opt->device_layout == DEVICE_LAYOUT_LCD160X96) {
            *width = DEVICE_LCD_WIDTH;
            *height = DEVICE_LCD_HEIGHT;
        } else {
            *width = device_layout_width(opt->device_layout);
            *height = device_layout_height(opt->device_layout);
        }
        return;
    }
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

static void encode_rgb565_strided_payload(const uint8_t *rgba,
                                          size_t width,
                                          size_t height,
                                          size_t row_bytes,
                                          uint8_t payload[PADDED_IMAGE_SIZE]) {
    memset(payload, 0, PADDED_IMAGE_SIZE);
    for (size_t y = 0; y < height; y++) {
        uint8_t *row = payload + y * row_bytes;
        for (size_t x = 0; x < width; x++) {
            size_t i = y * width + x;
            uint8_t red = rgba[i * 4 + 0];
            uint8_t green = rgba[i * 4 + 1];
            uint8_t blue = rgba[i * 4 + 2];
            uint16_t rgb565 = (uint16_t)(((red >> 3) << 11) |
                                         ((green >> 2) << 5) |
                                         (blue >> 3));
            row[x * 2 + 0] = (uint8_t)(rgb565 >> 8);
            row[x * 2 + 1] = (uint8_t)(rgb565 & 0xff);
        }
    }
}

static size_t device_layout_width(device_layout_t layout) {
    if (layout == DEVICE_LAYOUT_LCD160X96) {
        return DEVICE_LCD_MEMORY_WIDTH;
    }
    if (layout == DEVICE_LAYOUT_LCD96X160) {
        return DEVICE_LCD_WIDTH;
    }
    if (layout == DEVICE_LAYOUT_SQUARE128) {
        return DEVICE_SQUARE_WIDTH;
    }
    if (layout == DEVICE_LAYOUT_HALF_PORTRAIT) {
        return DEVICE_HALF_WIDTH;
    }
    return FRAMEBUFFER_WIDTH;
}

static size_t device_layout_height(device_layout_t layout) {
    if (layout == DEVICE_LAYOUT_LCD160X96) {
        return DEVICE_LCD_MEMORY_HEIGHT;
    }
    if (layout == DEVICE_LAYOUT_LCD96X160) {
        return DEVICE_LCD_HEIGHT;
    }
    if (layout == DEVICE_LAYOUT_SQUARE128) {
        return DEVICE_SQUARE_HEIGHT;
    }
    if (layout == DEVICE_LAYOUT_HALF_PORTRAIT) {
        return DEVICE_HALF_HEIGHT;
    }
    return FRAMEBUFFER_HEIGHT;
}

static void logical_to_scaled_rgba(const uint8_t *logical,
                                   size_t logical_w,
                                   size_t logical_h,
                                   size_t device_w,
                                   size_t device_h,
                                   uint8_t *device_rgba) {
    for (size_t y = 0; y < device_h; y++) {
        size_t sy = (y * logical_h) / device_h;
        if (sy >= logical_h) {
            sy = logical_h - 1;
        }
        for (size_t x = 0; x < device_w; x++) {
            size_t sx = (x * logical_w) / device_w;
            if (sx >= logical_w) {
                sx = logical_w - 1;
            }
            memcpy(device_rgba + ((y * device_w + x) * 4),
                   logical + ((sy * logical_w + sx) * 4),
                   4);
        }
    }
}

static int logical_to_lcd160x96_rgba(const uint8_t *logical,
                                     size_t logical_w,
                                     size_t logical_h,
                                     uint8_t *device_rgba) {
    if (logical_w != DEVICE_LCD_WIDTH || logical_h != DEVICE_LCD_HEIGHT) {
        fprintf(stderr,
                "lcd160x96 layout requires a %dx%d physical source, got %zux%zu\n",
                DEVICE_LCD_WIDTH,
                DEVICE_LCD_HEIGHT,
                logical_w,
                logical_h);
        return -1;
    }

    for (size_t y = 0; y < DEVICE_LCD_MEMORY_HEIGHT; y++) {
        for (size_t x = 0; x < DEVICE_LCD_MEMORY_WIDTH; x++) {
            size_t sx = DEVICE_LCD_WIDTH - 1 - y;
            size_t sy = x;
            memcpy(device_rgba + ((y * DEVICE_LCD_MEMORY_WIDTH + x) * 4),
                   logical + ((sy * DEVICE_LCD_WIDTH + sx) * 4),
                   4);
        }
    }
    return 0;
}

static int encode_logical_frame_payload(const options_t *opt,
                                        const uint8_t *logical_rgba,
                                        size_t logical_w,
                                        size_t logical_h,
                                        const char *device_preview_file,
                                        uint8_t payload[PADDED_IMAGE_SIZE]) {
    if (opt->device_layout == DEVICE_LAYOUT_LCD160X96 ||
        opt->device_layout == DEVICE_LAYOUT_LCD96X160 ||
        opt->device_layout == DEVICE_LAYOUT_SQUARE128 ||
        opt->device_layout == DEVICE_LAYOUT_HALF_PORTRAIT) {
        size_t device_w = device_layout_width(opt->device_layout);
        size_t device_h = device_layout_height(opt->device_layout);
        uint8_t *device_rgba = malloc(device_w * device_h * 4);
        if (device_rgba == NULL) {
            fprintf(stderr, "out of memory\n");
            return -1;
        }
        if (opt->device_layout == DEVICE_LAYOUT_LCD160X96) {
            if (logical_to_lcd160x96_rgba(logical_rgba,
                                          logical_w,
                                          logical_h,
                                          device_rgba) != 0) {
                free(device_rgba);
                return -1;
            }
        } else {
            logical_to_scaled_rgba(logical_rgba,
                                   logical_w,
                                   logical_h,
                                   device_w,
                                   device_h,
                                   device_rgba);
        }
        encode_rgb565_strided_payload(device_rgba,
                                      device_w,
                                      device_h,
                                      (size_t)opt->device_row_bytes,
                                      payload);
        if (device_preview_file != NULL &&
            write_preview_png(device_preview_file,
                              device_rgba,
                              device_w,
                              device_h) != 0) {
            free(device_rgba);
            return -1;
        }
        free(device_rgba);
        return 0;
    }

    uint8_t *framebuffer_rgba = malloc(IMAGE_PIXELS * 4);
    if (framebuffer_rgba == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    if (rotate_logical_to_framebuffer(logical_rgba,
                                      logical_w,
                                      logical_h,
                                      opt->rotate_degrees,
                                      framebuffer_rgba) != 0) {
        free(framebuffer_rgba);
        return -1;
    }
    encode_rgb565_payload(framebuffer_rgba, payload);
    if (device_preview_file != NULL &&
        write_preview_png(device_preview_file,
                          framebuffer_rgba,
                          FRAMEBUFFER_WIDTH,
                          FRAMEBUFFER_HEIGHT) != 0) {
        free(framebuffer_rgba);
        return -1;
    }
    free(framebuffer_rgba);
    return 0;
}

static size_t image_packet_count(const options_t *opt) {
    size_t chunk_size = (size_t)opt->image_chunk_size;
    return (PADDED_IMAGE_SIZE + chunk_size - 1) / chunk_size;
}

static size_t payload_packet_count(const options_t *opt, size_t payload_len) {
    size_t chunk_size = (size_t)opt->image_chunk_size;
    return (payload_len + chunk_size - 1) / chunk_size;
}

static size_t device_frame_bytes(const options_t *opt) {
    return (size_t)opt->device_frame_bytes;
}

static size_t image_prepare_frame_count(const bucket_upload_plan_t *plan) {
    size_t existing_total = plan->existing_bucket1_frames + plan->existing_bucket2_frames;
    size_t final_total = plan->final_bucket1_frames + plan->final_bucket2_frames;
    return existing_total > final_total ? existing_total : final_total;
}

static int image_prepare_windows_timeout_ms(const bucket_upload_plan_t *plan) {
    size_t prepare_frames = image_prepare_frame_count(plan);
    size_t windows_timeout = prepare_frames * IMAGE_PREPARE_PER_FRAME_TIMEOUT_MS +
                             IMAGE_PREPARE_BASE_TIMEOUT_MS;
    return (int)windows_timeout;
}

static int image_prepare_wait_ms(const options_t *opt) {
    return opt->timeout_ms > IMAGE_PREPARE_WAIT_MS ? opt->timeout_ms : IMAGE_PREPARE_WAIT_MS;
}

static void print_image_summary(const uint8_t payload[PADDED_IMAGE_SIZE],
                                const options_t *opt) {
    char raw_hash[65];
    char padded_hash[65];
    uint8_t begin[REPORT_SIZE];
    uint8_t first[REPORT_SIZE];
    uint8_t last[REPORT_SIZE];
    uint8_t commit[REPORT_SIZE];
    size_t chunk_size = (size_t)opt->image_chunk_size;
    size_t packet_count = image_packet_count(opt);
    size_t last_offset = (packet_count - 1) * chunk_size;
    size_t last_len = PADDED_IMAGE_SIZE - last_offset;

    if (opt->debug) {
        build_simple_packet(opt->report_id, 0x01, begin);
        build_image_packet(opt->report_id, payload, chunk_size, 0, first);
        build_image_packet(opt->report_id, payload + last_offset, last_len, (uint32_t)last_offset, last);
        build_simple_packet(opt->report_id, 0x02, commit);
        sha256_hex(payload, RGB565_SIZE, raw_hash);
        sha256_hex(payload, PADDED_IMAGE_SIZE, padded_hash);
    }

    printf("chunk bytes: %zu\n", chunk_size);
    printf("image packet count: %zu\n", packet_count);
    if (opt->debug) {
        printf("framebuffer: %dx%d\n", FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
        printf("rgb565 bytes: %d\n", RGB565_SIZE);
        printf("padded bytes: %d\n", PADDED_IMAGE_SIZE);
        printf("rgb565 sha256: %s\n", raw_hash);
        printf("padded sha256: %s\n", padded_hash);
        print_hex("first 32 rgb565 bytes: ", payload, 32);
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

static bool has_extension_ci(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return false;
    }
    while (*dot != '\0' && *ext != '\0') {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*ext)) {
            return false;
        }
        dot++;
        ext++;
    }
    return *dot == '\0' && *ext == '\0';
}

static const char *magick_command(void) {
    if (access("/opt/homebrew/bin/magick", X_OK) == 0) {
        return "/opt/homebrew/bin/magick";
    }
    if (access("/usr/local/bin/magick", X_OK) == 0) {
        return "/usr/local/bin/magick";
    }
    return "magick";
}

static int run_magick_coalesce(const char *input_path, const char *output_pattern) {
    const char *magick = magick_command();
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execlp(magick, magick, input_path, "-coalesce", output_pattern, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            fprintf(stderr,
                    "Could not run ImageMagick 'magick'. Install it with: brew install imagemagick\n");
        } else {
            fprintf(stderr, "ImageMagick GIF coalesce failed for %s\n", input_path);
        }
        return -1;
    }
    return 0;
}

static int png_frame_filter(const struct dirent *entry) {
    return strncmp(entry->d_name, "frame-", 6) == 0 &&
           has_extension_ci(entry->d_name, ".png");
}

static int list_coalesced_frames(const char *dir,
                                 int max_frames,
                                 char ***out_paths,
                                 size_t *out_count) {
    struct dirent **entries = NULL;
    int n = scandir(dir, &entries, png_frame_filter, alphasort);
    if (n < 0) {
        perror(dir);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "ImageMagick did not produce any coalesced PNG frames\n");
        free(entries);
        return -1;
    }
    if (n > max_frames) {
        fprintf(stderr,
                "GIF has %d coalesced frames, above --max-frames-per-bucket %d\n",
                n,
                max_frames);
        for (int i = 0; i < n; i++) {
            free(entries[i]);
        }
        free(entries);
        return -1;
    }

    char **paths = calloc((size_t)n, sizeof(*paths));
    if (paths == NULL) {
        fprintf(stderr, "out of memory\n");
        for (int i = 0; i < n; i++) {
            free(entries[i]);
        }
        free(entries);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        size_t len = strlen(dir) + 1 + strlen(entries[i]->d_name) + 1;
        paths[i] = malloc(len);
        if (paths[i] == NULL) {
            fprintf(stderr, "out of memory\n");
            for (int j = 0; j < i; j++) {
                free(paths[j]);
            }
            free(paths);
            for (int j = 0; j < n; j++) {
                free(entries[j]);
            }
            free(entries);
            return -1;
        }
        snprintf(paths[i], len, "%s/%s", dir, entries[i]->d_name);
    }

    for (int i = 0; i < n; i++) {
        free(entries[i]);
    }
    free(entries);

    *out_paths = paths;
    *out_count = (size_t)n;
    return 0;
}

static void cleanup_coalesced_frames(const char *dir, char **paths, size_t count) {
    if (paths != NULL) {
        for (size_t i = 0; i < count; i++) {
            if (paths[i] != NULL) {
                unlink(paths[i]);
                free(paths[i]);
            }
        }
        free(paths);
    }
    if (dir != NULL) {
        rmdir(dir);
    }
}

static int build_bucket_payload_from_frame_files(const options_t *opt,
                                                 char **frame_paths,
                                                 size_t frame_count,
                                                 const char *label,
                                                 const char *preview_file,
                                                 const char *framebuffer_preview_file,
                                                 frame_bucket_t *bucket) {
    int rc = -1;
    uint8_t *logical_rgba = NULL;
    size_t logical_w = 0;
    size_t logical_h = 0;

    logical_dimensions(opt, &logical_w, &logical_h);
    bucket->payload = calloc(frame_count, PADDED_IMAGE_SIZE);
    if (bucket->payload == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    bucket->frame_count = frame_count;

    for (size_t i = 0; i < frame_count; i++) {
        free(logical_rgba);
        logical_rgba = NULL;
        if (load_image_rgba(frame_paths[i],
                            opt->fit_mode,
                            logical_w,
                            logical_h,
                            &logical_rgba) != 0) {
            goto out;
        }
        if (i == 0 && preview_file != NULL &&
            write_preview_png(preview_file, logical_rgba, logical_w, logical_h) != 0) {
            goto out;
        }
        if (encode_logical_frame_payload(opt,
                                         logical_rgba,
                                         logical_w,
                                         logical_h,
                                         i == 0 ? framebuffer_preview_file : NULL,
                                         bucket->payload + i * PADDED_IMAGE_SIZE) != 0) {
            goto out;
        }
    }

    if (opt->debug) {
        printf("%s frames: %zu\n", label, bucket->frame_count);
        fflush(stdout);
    }
    rc = 0;

out:
    if (rc != 0) {
        free_frame_bucket(bucket);
    }
    free(logical_rgba);
    return rc;
}

static int build_bucket_payload_from_gif(const options_t *opt,
                                         const char *path,
                                         const char *label,
                                         const char *preview_file,
                                         const char *framebuffer_preview_file,
                                         frame_bucket_t *bucket) {
    int rc = -1;
    char tmp_template[] = "/private/tmp/cidoo-image-coalesce.XXXXXX";
    char *tmp_dir = mkdtemp(tmp_template);
    char output_pattern[PATH_MAX];
    char **frame_paths = NULL;
    size_t frame_count = 0;

    if (tmp_dir == NULL) {
        perror("mkdtemp");
        return -1;
    }
    snprintf(output_pattern,
             sizeof(output_pattern),
             "PNG32:%s/frame-%%03d.png",
             tmp_dir);

    if (opt->debug) {
        printf("%s GIF: coalescing frames with ImageMagick\n", label);
        fflush(stdout);
    }
    if (run_magick_coalesce(path, output_pattern) != 0) {
        goto out;
    }
    if (list_coalesced_frames(tmp_dir,
                              opt->max_frames_per_bucket,
                              &frame_paths,
                              &frame_count) != 0) {
        goto out;
    }
    rc = build_bucket_payload_from_frame_files(opt,
                                               frame_paths,
                                               frame_count,
                                               label,
                                               preview_file,
                                               framebuffer_preview_file,
                                               bucket);

out:
    cleanup_coalesced_frames(tmp_dir, frame_paths, frame_count);
    return rc;
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
    size_t logical_w = 0;
    size_t logical_h = 0;

    memset(bucket, 0, sizeof(*bucket));
    logical_dimensions(opt, &logical_w, &logical_h);

    if (has_extension_ci(path, ".gif")) {
        return build_bucket_payload_from_gif(opt,
                                             path,
                                             label,
                                             preview_file,
                                             framebuffer_preview_file,
                                             bucket);
    }

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
        if (i == 0 && preview_file != NULL &&
            write_preview_png(preview_file, logical_rgba, logical_w, logical_h) != 0) {
            goto out;
        }
        if (encode_logical_frame_payload(opt,
                                         logical_rgba,
                                         logical_w,
                                         logical_h,
                                         i == 0 ? framebuffer_preview_file : NULL,
                                         bucket->payload + i * PADDED_IMAGE_SIZE) != 0) {
            goto out;
        }
    }

    if (opt->debug) {
        printf("%s frames: %zu\n", label, bucket->frame_count);
        fflush(stdout);
    }
    rc = 0;

out:
    if (rc != 0) {
        free_frame_bucket(bucket);
    }
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
    size_t out_frame_bytes = device_frame_bytes(opt);

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
        size_t offset = plan->final_bucket1_frames * out_frame_bytes;
        if (offset > 0x00ffffffu) {
            fprintf(stderr, "Refusing GIF2 offset 0x%zx: exceeds 24-bit packet offset\n", offset);
            return -1;
        }
        plan->payload_offset = (uint32_t)offset;
        payload_frames = plan->bucket2.frame_count;
    }

    plan->payload_len = payload_frames * out_frame_bytes;
    payload = malloc(plan->payload_len);
    if (payload == NULL) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    if (plan->write_bucket1 && plan->write_bucket2) {
        for (size_t i = 0; i < plan->bucket1.frame_count; i++) {
            memcpy(payload + i * out_frame_bytes,
                   plan->bucket1.payload + i * PADDED_IMAGE_SIZE,
                   out_frame_bytes);
        }
        size_t bucket2_out_offset = plan->bucket1.frame_count * out_frame_bytes;
        for (size_t i = 0; i < plan->bucket2.frame_count; i++) {
            memcpy(payload + bucket2_out_offset + i * out_frame_bytes,
                   plan->bucket2.payload + i * PADDED_IMAGE_SIZE,
                   out_frame_bytes);
        }
    } else if (plan->write_bucket1) {
        for (size_t i = 0; i < plan->bucket1.frame_count; i++) {
            memcpy(payload + i * out_frame_bytes,
                   plan->bucket1.payload + i * PADDED_IMAGE_SIZE,
                   out_frame_bytes);
        }
    } else {
        for (size_t i = 0; i < plan->bucket2.frame_count; i++) {
            memcpy(payload + i * out_frame_bytes,
                   plan->bucket2.payload + i * PADDED_IMAGE_SIZE,
                   out_frame_bytes);
        }
    }

    plan->final_active_selector = opt->active_bucket_set ?
                                  (uint8_t)opt->active_bucket :
                                  template_record[ACTIVE_BUCKET_OFFSET];

    plan->payload = payload;
    return 0;
}

static const char *config_offset_label(size_t offset) {
    if (offset == ACTIVE_BUCKET_OFFSET) {
        return "active selector";
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
    after[ACTIVE_BUCKET_OFFSET] = plan->final_active_selector;
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

    if (!opt->debug) {
        return;
    }

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

    build_config_packet(opt->report_id,
                        0x06,
                        after,
                        CONFIG_RECORD_SIZE,
                        slot_offset(opt->slot),
                        packet);
    print_hex("cmd 0x06 config packet: ", packet, REPORT_SIZE);
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
    size_t chunk_size = (size_t)opt->image_chunk_size;
    size_t packet_count = payload_packet_count(opt, plan->payload_len);
    size_t last_offset = (packet_count - 1) * chunk_size;
    size_t last_len = plan->payload_len - last_offset;

    if (opt->debug) {
        build_simple_packet(opt->report_id, 0x01, config_begin);
        build_simple_packet(opt->report_id, 0x02, config_commit);
        build_simple_packet(opt->report_id, 0x23, image_prepare);
        build_image_packet(opt->report_id,
                           plan->payload,
                           chunk_size,
                           plan->payload_offset,
                           first);
        build_image_packet(opt->report_id,
                           plan->payload + last_offset,
                           last_len,
                           plan->payload_offset + (uint32_t)last_offset,
                           last);
        build_simple_packet(opt->report_id, 0x02, image_commit);
        sha256_hex(plan->payload, plan->payload_len, payload_hash);
    }

    printf("device layout: %s\n", device_layout_name(opt->device_layout));
    if (opt->device_layout == DEVICE_LAYOUT_LCD160X96 ||
        opt->device_layout == DEVICE_LAYOUT_LCD96X160 ||
        opt->device_layout == DEVICE_LAYOUT_SQUARE128 ||
        opt->device_layout == DEVICE_LAYOUT_HALF_PORTRAIT) {
        size_t device_w = device_layout_width(opt->device_layout);
        size_t device_h = device_layout_height(opt->device_layout);
        printf("device logical frame: %zux%zu\n", device_w, device_h);
        if (opt->debug) {
            printf("device rgb565 bytes per frame: %zu\n", device_w * device_h * 2);
            printf("device row bytes: %d\n", opt->device_row_bytes);
            printf("device row-padded bytes per frame: %zu\n",
                   (size_t)opt->device_row_bytes * device_h);
        }
    }
    printf("device bytes per frame: %zu\n", device_frame_bytes(opt));
    printf("chunk bytes: %zu\n", chunk_size);
    if (opt->debug) {
        printf("framebuffer: %dx%d\n", FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
        printf("rgb565 bytes per frame: %d\n", RGB565_SIZE);
        printf("host padded bytes per frame: %d\n", PADDED_IMAGE_SIZE);
        printf("write GIF1: %s\n", plan->write_bucket1 ? "yes" : "no");
        printf("write GIF2: %s\n", plan->write_bucket2 ? "yes" : "no");
        printf("existing GIF1 frame count: %zu\n", plan->existing_bucket1_frames);
        printf("existing GIF2 frame count: %zu\n", plan->existing_bucket2_frames);
    }
    printf("final GIF1 frame count: %zu\n", plan->final_bucket1_frames);
    printf("final GIF2 frame count: %zu\n", plan->final_bucket2_frames);
    printf("final total frame count: %zu\n",
           plan->final_bucket1_frames + plan->final_bucket2_frames);
    printf("payload bytes sent: %zu\n", plan->payload_len);
    printf("image packet count: %zu\n", packet_count);

    if (opt->debug) {
        printf("cmd 0x23 image prepare frame basis: %zu\n", image_prepare_frame_count(plan));
        printf("cmd 0x23 Windows prepare timeout formula: %d ms\n",
               image_prepare_windows_timeout_ms(plan));
        printf("cmd 0x23 local prepare wait: %d ms\n", image_prepare_wait_ms(opt));
        printf("payload start frame: %zu\n",
               (size_t)plan->payload_offset / device_frame_bytes(opt));
        printf("payload start byte offset: %u\n", plan->payload_offset);
        printf("payload sha256: %s\n", payload_hash);
        print_hex("first 32 rgb565 bytes: ", plan->payload, 32);
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

static int verify_bucket_config_readback(const options_t *opt,
                                         hid_session_t *session,
                                         const uint8_t expected[CONFIG_RECORD_SIZE]) {
    uint8_t actual[CONFIG_RECORD_SIZE];
    char expected_stable[65];
    char actual_stable[65];

    if (read_config_template_from_device(opt, session, actual) != 0) {
        fprintf(stderr,
                "Refusing image stream: could not read back config after metadata write\n");
        return -1;
    }

    stable_config_sha256_hex(expected, expected_stable);
    stable_config_sha256_hex(actual, actual_stable);
    if (!hex_equals_ci(expected_stable, actual_stable)) {
        fprintf(stderr,
                "Refusing image stream: config readback stable sha256 %s does not match "
                "patched stable sha256 %s\n",
                actual_stable,
                expected_stable);
        print_config_diff(expected, actual);
        return -1;
    }

    if (opt->debug) {
        printf("verified config readback stable sha256: %s\n", actual_stable);
    } else {
        printf("verified config readback\n");
    }
    return 0;
}

static int write_bucket_images_to_device(const options_t *opt,
                                         hid_session_t *session,
                                         const bucket_upload_plan_t *plan) {
    uint8_t packet[REPORT_SIZE];
    uint8_t response[REPORT_SIZE];
    size_t response_len = 0;
    size_t chunk_size = (size_t)opt->image_chunk_size;
    size_t packet_count = payload_packet_count(opt, plan->payload_len);
    int prepare_timeout_ms = image_prepare_wait_ms(opt);
    int windows_prepare_timeout_ms = image_prepare_windows_timeout_ms(plan);
    bool prepare_timed_out = false;

    build_simple_packet(opt->report_id, 0x23, packet);
    if (opt->debug) {
        printf("waiting up to %d ms for cmd 0x23 image prepare\n", prepare_timeout_ms);
    }
    if (send_report_wait_ignoring_response_error(
            session,
            packet,
            0x23,
            prepare_timeout_ms,
            response,
            &response_len,
            opt->debug,
            &prepare_timed_out) != 0) {
        return -1;
    }
    if (prepare_timed_out && windows_prepare_timeout_ms > prepare_timeout_ms) {
        int settle_ms = windows_prepare_timeout_ms - prepare_timeout_ms;
        if (opt->debug) {
            printf("cmd 0x23 did not respond in %d ms; waiting %d ms for device prepare\n",
                   prepare_timeout_ms,
                   settle_ms);
        } else {
            printf("waiting %d ms for image store prepare\n", settle_ms);
        }
        fflush(stdout);
        sleep_millis(settle_ms);
    }

    for (size_t i = 0; i < packet_count; i++) {
        size_t offset = i * chunk_size;
        size_t chunk_len = plan->payload_len - offset;
        uint32_t absolute_offset = plan->payload_offset + (uint32_t)offset;
        if (chunk_len > chunk_size) {
            chunk_len = chunk_size;
        }
        build_image_packet(opt->report_id,
                           plan->payload + offset,
                           chunk_len,
                           absolute_offset,
                           packet);
        if (send_report_wait(session, packet, 0x21, opt->timeout_ms, response, &response_len) != 0) {
            return -1;
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
    if (opt->device_layout == DEVICE_LAYOUT_FRAMEBUFFER) {
        printf("rotation into framebuffer: %d\n", opt->rotate_degrees);
    } else {
        printf("rotation into framebuffer: not used\n");
    }
    printf("active selector byte after upload: %u\n", plan.final_active_selector);
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

    size_t chunk_size = (size_t)opt->image_chunk_size;
    size_t packet_count = image_packet_count(opt);
    for (size_t i = 0; i < packet_count; i++) {
        size_t offset = i * chunk_size;
        size_t chunk_len = PADDED_IMAGE_SIZE - offset;
        if (chunk_len > chunk_size) {
            chunk_len = chunk_size;
        }
        build_image_packet(opt->report_id,
                           payload + offset,
                           chunk_len,
                           (uint32_t)offset,
                           packet);
        if (send_report_wait(&session, packet, 0x21, opt->timeout_ms, response, &response_len) != 0) {
            goto out_hid;
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
    if (opt->debug) {
        printf("current template stable sha256: %s\n", stable_hash_current);
    }
    if (opt->expected_stable_sha256 != NULL &&
        !hex_equals_ci(stable_hash_current, opt->expected_stable_sha256)) {
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

    printf("active selector byte after upload: %u\n", plan.final_active_selector);
    printf("clock bytes: %s\n", opt->preserve_clock ? "preserved" : "refreshed to local time");
    print_bucket_config_summary(opt, current, patched);
    print_bucket_image_summary(opt, &plan);

    if (write_bucket_config_to_device(opt, &session, patched) != 0) {
        goto out;
    }
    if (verify_bucket_config_readback(opt, &session, patched) != 0) {
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

static device_layout_t parse_device_layout(const char *s) {
    if (strcmp(s, "lcd160x96") == 0 ||
        strcmp(s, "screen160x96") == 0 ||
        strcmp(s, "160x96") == 0) {
        return DEVICE_LAYOUT_LCD160X96;
    }
    if (strcmp(s, "lcd96x160") == 0 ||
        strcmp(s, "screen96x160") == 0 ||
        strcmp(s, "96x160") == 0) {
        return DEVICE_LAYOUT_LCD96X160;
    }
    if (strcmp(s, "square128") == 0) {
        return DEVICE_LAYOUT_SQUARE128;
    }
    if (strcmp(s, "half-portrait") == 0) {
        return DEVICE_LAYOUT_HALF_PORTRAIT;
    }
    if (strcmp(s, "framebuffer") == 0) {
        return DEVICE_LAYOUT_FRAMEBUFFER;
    }
    fprintf(stderr, "Invalid --device-layout: %s\n", s);
    exit(2);
}

static const char *device_layout_name(device_layout_t layout) {
    switch (layout) {
        case DEVICE_LAYOUT_LCD160X96:
            return "lcd160x96";
        case DEVICE_LAYOUT_LCD96X160:
            return "lcd96x160";
        case DEVICE_LAYOUT_SQUARE128:
            return "square128";
        case DEVICE_LAYOUT_HALF_PORTRAIT:
            return "half-portrait";
        case DEVICE_LAYOUT_FRAMEBUFFER:
            return "framebuffer";
    }
    return "unknown";
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
    opt->fit_mode = FIT_STRETCH;
    opt->orientation = ORIENTATION_PORTRAIT;
    opt->device_layout = DEVICE_LAYOUT_LCD160X96;
    opt->rotate_degrees = 0;
    opt->slot = 0;
    opt->active_bucket = 1;
    opt->image_chunk_size = DEFAULT_IMAGE_CHUNK_SIZE;
    opt->device_frame_bytes = DEFAULT_DEVICE_FRAME_BYTES;
    opt->device_row_bytes = DEFAULT_DEVICE_ROW_BYTES;
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
        {"debug", no_argument, NULL, 1012},
        {"strip-report-id-for-iohid", no_argument, NULL, 1015},
        {"bucket1", required_argument, NULL, 1016},
        {"bucket2", required_argument, NULL, 1017},
        {"template-file", required_argument, NULL, 1018},
        {"expected-stable-sha256", required_argument, NULL, 1019},
        {"slot", required_argument, NULL, 1020},
        {"active-bucket", required_argument, NULL, 1021},
        {"max-frames-per-bucket", required_argument, NULL, 1022},
        {"preserve-clock", no_argument, NULL, 1025},
        {"bucket1-preview-out", required_argument, NULL, 1026},
        {"bucket2-preview-out", required_argument, NULL, 1027},
        {"bucket1-framebuffer-preview-out", required_argument, NULL, 1028},
        {"bucket2-framebuffer-preview-out", required_argument, NULL, 1029},
        {"image-chunk-size", required_argument, NULL, 1030},
        {"device-frame-bytes", required_argument, NULL, 1031},
        {"device-layout", required_argument, NULL, 1032},
        {"device-row-bytes", required_argument, NULL, 1033},
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
                opt->debug = true;
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
            case 1030:
                opt->image_chunk_size = parse_int_range(optarg,
                                                         "--image-chunk-size",
                                                         1,
                                                         MAX_IMAGE_CHUNK_SIZE);
                if (opt->image_chunk_size != 0x18 && opt->image_chunk_size != 0x38) {
                    fprintf(stderr,
                            "Invalid --image-chunk-size: %s; expected 24 or 56\n",
                            optarg);
                    return -1;
                }
                break;
            case 1031:
                opt->device_frame_bytes = parse_int_range(optarg,
                                                          "--device-frame-bytes",
                                                          1,
                                                          PADDED_IMAGE_SIZE);
                if (opt->device_frame_bytes != 32768 &&
                    opt->device_frame_bytes != PADDED_IMAGE_SIZE) {
                    fprintf(stderr,
                            "Invalid --device-frame-bytes: %s; expected 32768 or 65536\n",
                            optarg);
                    return -1;
                }
                break;
            case 1032:
                opt->device_layout = parse_device_layout(optarg);
                break;
            case 1033:
                opt->device_row_bytes = parse_int_range(optarg,
                                                        "--device-row-bytes",
                                                        1,
                                                        PADDED_IMAGE_SIZE);
                opt->device_row_bytes_set = true;
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
        opt->rotate_degrees = 270;
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
    if (opt->device_layout == DEVICE_LAYOUT_LCD160X96 ||
        opt->device_layout == DEVICE_LAYOUT_LCD96X160 ||
        opt->device_layout == DEVICE_LAYOUT_SQUARE128 ||
        opt->device_layout == DEVICE_LAYOUT_HALF_PORTRAIT) {
        size_t device_w = device_layout_width(opt->device_layout);
        size_t device_h = device_layout_height(opt->device_layout);
        size_t row_bytes = (size_t)opt->device_row_bytes;
        if (!opt->device_row_bytes_set) {
            row_bytes = device_w * 2;
            opt->device_row_bytes = (int)row_bytes;
        }
        if ((row_bytes % 2) != 0) {
            fprintf(stderr, "--device-row-bytes must be even for RGB565 rows\n");
            return -1;
        }
        if (row_bytes < device_w * 2) {
            fprintf(stderr,
                    "--device-row-bytes %zu is too small for a %zux%zu RGB565 frame\n",
                    row_bytes,
                    device_w,
                    device_h);
            return -1;
        }
        if (row_bytes * device_h > (size_t)opt->device_frame_bytes) {
            fprintf(stderr,
                    "--device-row-bytes %zu does not fit %zux%zu into --device-frame-bytes %d\n",
                    row_bytes,
                    device_w,
                    device_h,
                    opt->device_frame_bytes);
            return -1;
        }
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
