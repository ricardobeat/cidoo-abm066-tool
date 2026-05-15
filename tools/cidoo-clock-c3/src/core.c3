module cdoo;

import std::core::mem;

const int REPORT_SIZE = 64;
const int RECORD_SIZE = 48;
const int CONFIG_PAYLOAD_OFFSET = 8;
const int TIME_OFFSET = 35;
const int TIME_SIZE = 7;
const int IMAGE_PAYLOAD_OFFSET = 8;
const int DEFAULT_IMAGE_CHUNK_SIZE = 0x18;
const int MAX_IMAGE_CHUNK_SIZE = REPORT_SIZE - IMAGE_PAYLOAD_OFFSET;
const int DEVICE_LCD_WIDTH = 96;
const int DEVICE_LCD_HEIGHT = 160;
const int DEVICE_LCD_MEMORY_WIDTH = 160;
const int DEVICE_LCD_MEMORY_HEIGHT = 96;
const int DEFAULT_DEVICE_FRAME_BYTES = 32768;
const int DEFAULT_DEVICE_ROW_BYTES = DEVICE_LCD_MEMORY_WIDTH * 2;
const int ACTIVE_BUCKET_OFFSET = 33;
const int BUCKET1_COUNT_OFFSET = 34;
const int BUCKET2_COUNT_OFFSET = 46;
const int MAX_TOTAL_FRAMES = 200;
const int IMAGE_PREPARE_BASE_TIMEOUT_MS = 500;
const int IMAGE_PREPARE_PER_FRAME_TIMEOUT_MS = 300;
const int IMAGE_PREPARE_WAIT_MS = 1500;
const ushort DEFAULT_VID = 0x320f;
const ushort DEFAULT_PID = 0x5055;
const ushort DEFAULT_USAGE_PAGE = 0xff1c;
const ushort DEFAULT_USAGE = 0x92;
const char DEFAULT_REPORT_ID = 0x04;

fn void clear_bytes(char[] bytes) {
    for (sz i = 0; i < bytes.len; i++) bytes[i] = 0;
}

fn void clear_pointer_bytes(char* bytes, sz len) {
    for (sz i = 0; i < len; i++) bytes[i] = 0;
}

fn void copy_bytes(char[] dst, char[] src) {
    sz len = dst.len < src.len ? dst.len : src.len;
    for (sz i = 0; i < len; i++) dst[i] = src[i];
}

fn bool bytes_equal(char[] a, char[] b) {
    if (a.len != b.len) return false;
    for (sz i = 0; i < a.len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

fn bool record_all_zero(char[] record) {
    for (sz i = 0; i < record.len; i++) {
        if (record[i] != 0) return false;
    }
    return true;
}

fn int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

fn void checksum_packet(char[] packet) {
    ushort sum = 0;
    for (sz i = 3; i < REPORT_SIZE; i++) {
        sum = (ushort)(sum + (ushort)packet[i]);
    }
    packet[1] = (char)(sum & 0xff);
    packet[2] = (char)((sum >> 8) & 0xff);
}

fn void build_simple_packet(char report_id, char cmd, char[] packet) {
    clear_bytes(packet);
    packet[0] = report_id;
    packet[3] = cmd;
    checksum_packet(packet);
}

fn void build_config_packet(char report_id, char cmd, char[] payload, sz payload_len, ushort offset, char[] packet) {
    clear_bytes(packet);
    packet[0] = report_id;
    packet[3] = cmd;
    packet[4] = (char)payload_len;
    packet[5] = (char)(offset & 0xff);
    packet[6] = (char)((offset >> 8) & 0xff);
    packet[7] = 0;
    if (payload.len > 0 && payload_len > 0) {
        sz copy_len = payload_len;
        if (copy_len > REPORT_SIZE - CONFIG_PAYLOAD_OFFSET) copy_len = REPORT_SIZE - CONFIG_PAYLOAD_OFFSET;
        if (copy_len > payload.len) copy_len = payload.len;
        copy_bytes(packet[CONFIG_PAYLOAD_OFFSET..], payload[:copy_len]);
    }
    checksum_packet(packet);
}

fn void build_image_packet(char report_id, char* payload, sz payload_len, uint offset, char[] packet) {
    clear_bytes(packet);
    packet[0] = report_id;
    packet[3] = 0x21;
    packet[4] = (char)payload_len;
    packet[5] = (char)(offset & 0xff);
    packet[6] = (char)((offset >> 8) & 0xff);
    packet[7] = (char)((offset >> 16) & 0xff);
    if (payload_len > MAX_IMAGE_CHUNK_SIZE) payload_len = MAX_IMAGE_CHUNK_SIZE;
    for (sz i = 0; i < payload_len; i++) packet[IMAGE_PAYLOAD_OFFSET + i] = payload[i];
    checksum_packet(packet);
}

fn ushort slot_offset(int slot) {
    return (ushort)(slot * 0x31);
}

fn bool is_time_offset(sz i) {
    return i >= TIME_OFFSET && i < TIME_OFFSET + TIME_SIZE;
}

fn char bcd(int value) {
    return (char)(((value / 10) << 4) | (value % 10));
}

fn int unbcd(char value) {
    int hi = (int)((value >> 4) & 0x0f);
    int lo = (int)(value & 0x0f);
    if (hi > 9 || lo > 9) return -1;
    return hi * 10 + lo;
}

fn bool bcd_in_range(char value, int min, int max) {
    int decoded = unbcd(value);
    return decoded >= min && decoded <= max;
}

fn String time_validation_error(char[] record) {
    if (!bcd_in_range(record[35], 0, 59)) return "seconds byte 35 is not valid BCD 00..59";
    if (!bcd_in_range(record[36], 0, 59)) return "minutes byte 36 is not valid BCD 00..59";
    if (!bcd_in_range(record[37], 0, 23)) return "hours byte 37 is not valid BCD 00..23";
    if (record[38] > 6) return "weekday byte 38 is not in range 0..6";
    if (!bcd_in_range(record[39], 1, 31)) return "day byte 39 is not valid BCD 01..31";
    if (!bcd_in_range(record[40], 1, 12)) return "month byte 40 is not valid BCD 01..12";
    if (!bcd_in_range(record[41], 0, 99)) return "year byte 41 is not valid BCD 00..99";
    return "ok";
}

fn void write_time_bytes(int sec, int min, int hour, int weekday, int day, int mon, int year, char[] out) {
    out[0] = bcd(sec);
    out[1] = bcd(min);
    out[2] = bcd(hour);
    out[3] = (char)weekday;
    out[4] = bcd(day);
    out[5] = bcd(mon);
    out[6] = bcd(year % 100);
}

fn void patch_time(char[] input, char[] time_bytes, char[] output) {
    for (sz i = 0; i < RECORD_SIZE; i++) output[i] = input[i];
    for (sz i = 0; i < TIME_SIZE; i++) output[TIME_OFFSET + i] = time_bytes[i];
}

fn int time_change_violation_offset(char[] before, char[] after) {
    if (before.len < RECORD_SIZE || after.len < RECORD_SIZE) return -2;
    for (sz i = 0; i < RECORD_SIZE; i++) {
        if (!is_time_offset(i) && before[i] != after[i]) return (int)i;
    }
    return -1;
}

fn void encode_lcd160x96_frame(char* logical, char* frame) {
    clear_pointer_bytes(frame, DEFAULT_DEVICE_FRAME_BYTES);
    for (sz y = 0; y < DEVICE_LCD_MEMORY_HEIGHT; y++) {
        for (sz x = 0; x < DEVICE_LCD_MEMORY_WIDTH; x++) {
            sz sx = DEVICE_LCD_WIDTH - 1 - y;
            sz sy = x;
            sz src = ((sy * DEVICE_LCD_WIDTH + sx) * 4);
            uint red = ((uint)logical[src]) & 0xff;
            uint green = ((uint)logical[src + 1]) & 0xff;
            uint blue = ((uint)logical[src + 2]) & 0xff;
            ushort rgb565 = (ushort)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
            sz dst = y * DEFAULT_DEVICE_ROW_BYTES + x * 2;
            frame[dst] = (char)((rgb565 >> 8) & 0xff);
            frame[dst + 1] = (char)(rgb565 & 0xff);
        }
    }
}

fn int patch_image_config_record(char[] before, uint active_selector, sz bucket1_frames, sz bucket2_frames, char[] after) {
    if (before.len < RECORD_SIZE || after.len < RECORD_SIZE) return -1;
    if (bucket1_frames > 255 || bucket2_frames > 255 || active_selector > 255) return -1;
    for (sz i = 0; i < RECORD_SIZE; i++) after[i] = before[i];
    after[ACTIVE_BUCKET_OFFSET] = (char)active_selector;
    after[BUCKET1_COUNT_OFFSET] = (char)bucket1_frames;
    after[BUCKET2_COUNT_OFFSET] = (char)bucket2_frames;
    return image_config_change_violation_offset(before, after) < 0 ? 0 : -1;
}

fn int image_config_change_violation_offset(char[] before, char[] after) {
    if (before.len < RECORD_SIZE || after.len < RECORD_SIZE) return -2;
    for (sz i = 0; i < RECORD_SIZE; i++) {
        bool allowed = i == ACTIVE_BUCKET_OFFSET || i == BUCKET1_COUNT_OFFSET || i == BUCKET2_COUNT_OFFSET;
        if (!allowed && before[i] != after[i]) return (int)i;
    }
    return -1;
}

fn sz payload_packet_count(sz payload_len) {
    return (payload_len + DEFAULT_IMAGE_CHUNK_SIZE - 1) / DEFAULT_IMAGE_CHUNK_SIZE;
}

fn sz image_prepare_frame_count_values(sz existing_bucket1_frames, sz existing_bucket2_frames, sz final_bucket1_frames, sz final_bucket2_frames) {
    sz existing_total = existing_bucket1_frames + existing_bucket2_frames;
    sz final_total = final_bucket1_frames + final_bucket2_frames;
    return existing_total > final_total ? existing_total : final_total;
}

fn int image_prepare_windows_timeout_ms_values(sz existing_bucket1_frames, sz existing_bucket2_frames, sz final_bucket1_frames, sz final_bucket2_frames) {
    return (int)(image_prepare_frame_count_values(
        existing_bucket1_frames,
        existing_bucket2_frames,
        final_bucket1_frames,
        final_bucket2_frames) * IMAGE_PREPARE_PER_FRAME_TIMEOUT_MS + IMAGE_PREPARE_BASE_TIMEOUT_MS);
}

fn uptr cdoo_alloc(int size) @export @cname("cdoo_alloc") {
    if (size <= 0) return 0;
    void* ptr = mem::malloc((sz)size);
    return (uptr)ptr;
}

fn void cdoo_free(uptr ptr) @export @cname("cdoo_free") {
    if (ptr != 0) mem::free((void*)ptr);
}

fn int cdoo_report_size() @export @cname("cdoo_report_size") => REPORT_SIZE;
fn int cdoo_record_size() @export @cname("cdoo_record_size") => RECORD_SIZE;
fn int cdoo_time_offset() @export @cname("cdoo_time_offset") => TIME_OFFSET;
fn int cdoo_time_size() @export @cname("cdoo_time_size") => TIME_SIZE;
fn int cdoo_default_report_id() @export @cname("cdoo_default_report_id") => (int)DEFAULT_REPORT_ID;
fn int cdoo_default_vid() @export @cname("cdoo_default_vid") => (int)DEFAULT_VID;
fn int cdoo_default_pid() @export @cname("cdoo_default_pid") => (int)DEFAULT_PID;
fn int cdoo_default_usage_page() @export @cname("cdoo_default_usage_page") => (int)DEFAULT_USAGE_PAGE;
fn int cdoo_default_usage() @export @cname("cdoo_default_usage") => (int)DEFAULT_USAGE;
fn int cdoo_default_image_chunk_size() @export @cname("cdoo_default_image_chunk_size") => DEFAULT_IMAGE_CHUNK_SIZE;
fn int cdoo_default_device_frame_bytes() @export @cname("cdoo_default_device_frame_bytes") => DEFAULT_DEVICE_FRAME_BYTES;
fn int cdoo_max_total_frames() @export @cname("cdoo_max_total_frames") => MAX_TOTAL_FRAMES;

fn int cdoo_checksum_packet(uptr packet_ptr) @export @cname("cdoo_checksum_packet") {
    checksum_packet(((char*)packet_ptr)[0:REPORT_SIZE]);
    return 0;
}

fn int cdoo_build_simple_packet(int report_id, int command, uptr out_packet_ptr) @export @cname("cdoo_build_simple_packet") {
    build_simple_packet((char)report_id, (char)command, ((char*)out_packet_ptr)[0:REPORT_SIZE]);
    return 0;
}

fn int cdoo_build_config_packet(int report_id, int command, uptr payload_ptr, int payload_len, int offset, uptr out_packet_ptr) @export @cname("cdoo_build_config_packet") {
    if (payload_len < 0 || payload_len > RECORD_SIZE) return -1;
    build_config_packet(
        (char)report_id,
        (char)command,
        ((char*)payload_ptr)[0:(sz)payload_len],
        (sz)payload_len,
        (ushort)offset,
        ((char*)out_packet_ptr)[0:REPORT_SIZE]);
    return 0;
}

fn int cdoo_build_image_packet(int report_id, uptr payload_ptr, int payload_len, int offset, uptr out_packet_ptr) @export @cname("cdoo_build_image_packet") {
    if (payload_len < 0) return -1;
    build_image_packet(
        (char)report_id,
        (char*)payload_ptr,
        (sz)payload_len,
        (uint)offset,
        ((char*)out_packet_ptr)[0:REPORT_SIZE]);
    return 0;
}

fn int cdoo_write_time_bytes(int sec, int min, int hour, int weekday, int day, int mon, int year, uptr out_time_ptr) @export @cname("cdoo_write_time_bytes") {
    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23 ||
        weekday < 0 || weekday > 6 || day < 1 || day > 31 || mon < 1 || mon > 12 ||
        year < 2000 || year > 2099) {
        return -1;
    }
    write_time_bytes(sec, min, hour, weekday, day, mon, year, ((char*)out_time_ptr)[0:TIME_SIZE]);
    return 0;
}

fn int cdoo_patch_time(uptr record_ptr, uptr time_ptr, uptr out_record_ptr) @export @cname("cdoo_patch_time") {
    patch_time(
        ((char*)record_ptr)[0:RECORD_SIZE],
        ((char*)time_ptr)[0:TIME_SIZE],
        ((char*)out_record_ptr)[0:RECORD_SIZE]);
    return 0;
}

fn int cdoo_time_change_violation_offset(uptr before_ptr, uptr after_ptr) @export @cname("cdoo_time_change_violation_offset") {
    return time_change_violation_offset(
        ((char*)before_ptr)[0:RECORD_SIZE],
        ((char*)after_ptr)[0:RECORD_SIZE]);
}

fn int cdoo_decode_bcd(int value) @export @cname("cdoo_decode_bcd") {
    return unbcd((char)value);
}

fn int cdoo_encode_lcd160x96_frame(uptr logical_rgba_ptr, uptr out_frame_ptr) @export @cname("cdoo_encode_lcd160x96_frame") {
    encode_lcd160x96_frame((char*)logical_rgba_ptr, (char*)out_frame_ptr);
    return 0;
}

fn int cdoo_patch_image_config(uptr before_ptr, int active_selector, int bucket1_frames, int bucket2_frames, uptr after_ptr) @export @cname("cdoo_patch_image_config") {
    if (bucket1_frames < 0 || bucket2_frames < 0 || active_selector < 0) return -1;
    return patch_image_config_record(
        ((char*)before_ptr)[0:RECORD_SIZE],
        (uint)active_selector,
        (sz)bucket1_frames,
        (sz)bucket2_frames,
        ((char*)after_ptr)[0:RECORD_SIZE]);
}

fn int cdoo_image_config_change_violation_offset(uptr before_ptr, uptr after_ptr) @export @cname("cdoo_image_config_change_violation_offset") {
    return image_config_change_violation_offset(
        ((char*)before_ptr)[0:RECORD_SIZE],
        ((char*)after_ptr)[0:RECORD_SIZE]);
}

fn int cdoo_payload_packet_count(int payload_len) @export @cname("cdoo_payload_packet_count") {
    if (payload_len < 0) return -1;
    return (int)payload_packet_count((sz)payload_len);
}

fn int cdoo_image_prepare_windows_timeout_ms(int existing_bucket1_frames, int existing_bucket2_frames, int final_bucket1_frames, int final_bucket2_frames) @export @cname("cdoo_image_prepare_windows_timeout_ms") {
    if (existing_bucket1_frames < 0 || existing_bucket2_frames < 0 || final_bucket1_frames < 0 || final_bucket2_frames < 0) return -1;
    return image_prepare_windows_timeout_ms_values(
        (sz)existing_bucket1_frames,
        (sz)existing_bucket2_frames,
        (sz)final_bucket1_frames,
        (sz)final_bucket2_frames);
}
