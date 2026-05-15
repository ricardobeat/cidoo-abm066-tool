module cdoo;

import std::core::mem;
import std::hash::sha256;
import std::io;

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

struct Options {
    int slot;
    int timeout_ms;
    String time_arg;
    String image1_path;
    String image2_path;
    bool help;
    bool dry_run;
    bool debug;
}

enum TransportKind : int {
    NONE,
    MACOS_IOKIT,
}

struct ClockTransport {
    TransportKind kind;
    void* impl;
}

fn int main(String[] args) => @pool()
{
    if (args.len < 2) {
        usage();
        return 2;
    }

    String command = args[1];
    if (str_eq(command, "help") || str_eq(command, "--help") || str_eq(command, "-h")) {
        usage();
        return 0;
    }

    Options options = {
        .timeout_ms = 1500,
        .time_arg = "now",
    };

    if (parse_options(args, &options) != 0) return 2;

    if (options.help) {
        usage();
        return 0;
    }
    if (str_eq(command, "list")) return transport_list_devices(&options, true);
    if (str_eq(command, "probe")) return transport_list_devices(&options, false);
    if (str_eq(command, "get-feature")) return get_feature_report(&options);
    if (str_eq(command, "read-raw")) return run_read_raw(&options);
    if (str_eq(command, "read-template")) return run_read_template(&options);
    if (str_eq(command, "update-time")) return run_update_time(&options);
    if (str_eq(command, "upload-image")) return run_upload_image(&options);

    io::printfn("Unknown command: %s", command);
    usage();
    return 2;
}

fn void usage() {
    io::printf(
        "Usage:\n"
        "  cdoo list [options]\n"
        "  cdoo probe [options]\n"
        "  cdoo get-feature [options]\n"
        "  cdoo read-raw [options]\n"
        "  cdoo read-template [options]\n"
        "  cdoo update-time [--dry-run] [options]\n"
        "  cdoo upload-image [--dry-run] IMAGE1 IMAGE2 [options]\n\n"
        "Options:\n"
        "  --slot N                          config slot, offset = N * 0x31, default 0\n"
        "  --time now|YYYY-MM-DDTHH:MM:SS    local time to encode, default now\n"
        "  --dry-run                         read and patch without writing\n"
        "  --debug                           print HID packets and changed offsets\n"
        "  --timeout-ms N                    HID response timeout, default 1500\n"
        "  -h, --help                       show this help\n");
}

fn bool str_eq(String a, String b) {
    if (a.len != b.len) return false;
    for (sz i = 0; i < a.len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

fn void clear_bytes(char[] bytes) {
    for (sz i = 0; i < bytes.len; i++) bytes[i] = 0;
}

fn void copy_bytes(char[] dst, char[] src) {
    sz len = dst.len < src.len ? dst.len : src.len;
    if (len > 0) mem::copy(dst.ptr, src.ptr, (sz)len);
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

fn void print_hex(String label, char[] buf) {
    if (label.len > 0) io::printf("%s", label);
    for (sz i = 0; i < buf.len; i++) {
        if (i > 0) io::print(" ");
        io::printf("%02x", (uint)buf[i]);
    }
    io::printn();
}

fn char hex_digit(int v) {
    return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

fn void sha256_hex(char[] buf, char* out) {
    char[sha256::HASH_SIZE] digest = sha256::hash(buf);
    for (int i = 0; i < sha256::HASH_SIZE; i++) {
        int byte = (int)digest[i];
        out[i * 2] = hex_digit((byte >> 4) & 0x0f);
        out[i * 2 + 1] = hex_digit(byte & 0x0f);
    }
    out[64] = 0;
}

fn void stable_sha256_hex(char[] record, char* out) {
    char[RECORD_SIZE] masked;
    for (sz i = 0; i < RECORD_SIZE; i++) masked[i] = record[i];
    for (sz i = TIME_OFFSET; i < TIME_OFFSET + TIME_SIZE; i++) masked[i] = 0;
    sha256_hex(masked[..], out);
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
