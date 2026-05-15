module cdoo;

import std::hash::sha256;
import std::io;

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
