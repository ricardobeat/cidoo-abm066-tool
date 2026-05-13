module cidoo_clock_c3;

import std::core::mem;
import std::hash::sha256;
import std::io;
import std::io::file;
import std::thread;
import std::time;
import std::time::datetime;

import opt;

const int REPORT_SIZE = 64;
const int RECORD_SIZE = 48;
const int CONFIG_PAYLOAD_OFFSET = 8;
const int TIME_OFFSET = 35;
const int TIME_SIZE = 7;
const ushort DEFAULT_VID = 0x320f;
const ushort DEFAULT_PID = 0x5055;
const ushort DEFAULT_USAGE_PAGE = 0xff1c;
const ushort DEFAULT_USAGE = 0x92;
const char DEFAULT_REPORT_ID = 0x04;
const String WRITE_CONFIRM_TEXT = "PATCH_ONLY_BYTES_35_41";
const int RAW_OFFSET_UNSET = -1;

struct Options {
    ushort vid;
    ushort pid;
    ushort usage_page;
    ushort usage;
    char report_id;
    char raw_command;
    uint raw_length;
    int raw_offset;
    int slot;
    int timeout_ms;
    String template_hex;
    String template_file;
    String time_arg;
    String out_file;
    String expected_template_sha256;
    String expected_stable_sha256;
    String confirm_write;
    bool help;
    bool print_packets;
    bool allow_hid_query;
    bool allow_config_write;
    bool strip_report_id_for_iohid;
}

fn int main(String[] args) {
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
        .vid = DEFAULT_VID,
        .pid = DEFAULT_PID,
        .usage_page = DEFAULT_USAGE_PAGE,
        .usage = DEFAULT_USAGE,
        .report_id = DEFAULT_REPORT_ID,
        .raw_length = (uint)RECORD_SIZE,
        .raw_offset = RAW_OFFSET_UNSET,
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
    if (str_eq(command, "dry-run")) return run_dry_run(&options);
    if (str_eq(command, "read-template")) return run_read_template(&options);
    if (str_eq(command, "read-template-split")) {
        io::printfn("read-template-split is not ported in the C3 version; use read-template.");
        return 2;
    }
    if (str_eq(command, "update-time")) return run_update_time(&options);

    io::printfn("Unknown command: %s", command);
    usage();
    return 2;
}

fn void usage() {
    io::printf(
        "Usage:\n"
        "  cidoo-clock-c3 list [options]\n"
        "  cidoo-clock-c3 probe [options]\n"
        "  cidoo-clock-c3 get-feature --allow-hid-query [options]\n"
        "  cidoo-clock-c3 read-raw --allow-hid-query --raw-command HEX [options]\n"
        "  cidoo-clock-c3 dry-run --template HEX|--template-file PATH [options]\n"
        "  cidoo-clock-c3 read-template --allow-hid-query [options]\n"
        "  cidoo-clock-c3 update-time --allow-hid-query --allow-config-write \\\n"
        "      --expected-stable-sha256 HEX \\\n"
        "      --confirm-write PATCH_ONLY_BYTES_35_41 [options]\n\n"
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
        "  --expected-stable-sha256 HEX      required update guard; time bytes masked\n"
        "  --print-packets                   print 64-byte HID packets\n"
        "  --timeout-ms N                    HID response timeout, default 1500\n"
        "  --allow-hid-query                 permit command 0x05 query report\n"
        "  --allow-config-write              permit command 0x06 config write path\n"
        "  --confirm-write TEXT              must be PATCH_ONLY_BYTES_35_41\n"
        "  -h, --help                       show this help\n"
        "  --strip-report-id-for-iohid       diagnostic: pass bytes 1..63 to IOKit\n",
        (uint)DEFAULT_VID,
        (uint)DEFAULT_PID,
        (uint)DEFAULT_USAGE_PAGE,
        (uint)DEFAULT_USAGE,
        (uint)DEFAULT_REPORT_ID);
}

fn int parse_options(String[] args, Options* options) {
    String[] option_args = args[1..];
    opt::@disable_errors();
    defer opt::@enable_errors();
    int? parsed_options = opt::@parse(
        option_args,
        "h", "help", &options.help,
        null, "vid", &options.vid,
        null, "pid", &options.pid,
        null, "usage-page", &options.usage_page,
        null, "usage", &options.usage,
        null, "report-id", &options.report_id,
        null, "raw-command", &options.raw_command,
        null, "raw-length", &options.raw_length,
        null, "raw-offset", &options.raw_offset,
        null, "slot", &options.slot,
        null, "time", &options.time_arg,
        null, "template", &options.template_hex,
        null, "template-file", &options.template_file,
        null, "out", &options.out_file,
        null, "expected-template-sha256", &options.expected_template_sha256,
        null, "expected-stable-sha256", &options.expected_stable_sha256,
        null, "confirm-write", &options.confirm_write,
        null, "print-packets", &options.print_packets,
        null, "timeout-ms", &options.timeout_ms,
        null, "allow-hid-query", &options.allow_hid_query,
        null, "allow-config-write", &options.allow_config_write,
        null, "strip-report-id-for-iohid", &options.strip_report_id_for_iohid
    );
    if (catch err = parsed_options) {
        switch (err) {
            case opt::MISSING_ARGUMENT:
                io::printfn("Missing option argument");
            case opt::ILLEGAL_OPTION:
                io::printfn("Unknown option");
            default:
                io::printfn("Invalid option: %s", err);
        }
        usage();
        return -1;
    }

    int end_index = parsed_options;
    if (options.help) return 0;

    if (end_index < option_args.len) {
        io::printfn("Unexpected argument: %s", option_args[end_index]);
        return -1;
    }

    return validate_options(options);
}

fn int validate_options(Options* options) {
    if (options.raw_length > REPORT_SIZE - CONFIG_PAYLOAD_OFFSET) {
        io::printfn("Invalid --raw-length: %u", options.raw_length);
        return -1;
    }
    if (options.raw_offset < RAW_OFFSET_UNSET || options.raw_offset > 0xffff) {
        io::printfn("Invalid --raw-offset: %d", options.raw_offset);
        return -1;
    }
    if (options.slot < 0 || options.slot > 1024) {
        io::printfn("Invalid --slot: %d", options.slot);
        return -1;
    }
    if (options.timeout_ms < 100 || options.timeout_ms > 30000) {
        io::printfn("Invalid --timeout-ms: %d", options.timeout_ms);
        return -1;
    }
    if ((uint)options.slot * 0x31 > 0xffff - (RECORD_SIZE - 1)) {
        io::printfn("--slot is too large for the 16-bit config offset");
        return -1;
    }
    return 0;
}

fn bool str_eq(String a, String b) {
    if (a.len != b.len) return false;
    for (usz i = 0; i < a.len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

fn void clear_bytes(char[] bytes) {
    for (usz i = 0; i < bytes.len; i++) bytes[i] = 0;
}

fn void copy_bytes(char[] dst, char[] src) {
    usz len = dst.len < src.len ? dst.len : src.len;
    if (len > 0) mem::copy(dst.ptr, src.ptr, (sz)len);
}

fn bool bytes_equal(char[] a, char[] b) {
    if (a.len != b.len) return false;
    for (usz i = 0; i < a.len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

fn bool string_was_set(String value) {
    return value.ptr != null;
}

fn bool hex_eq_ci(String a, char* b) {
    for (usz i = 0; i < a.len; i++) {
        char ac = lowercase(a[i]);
        char bc = lowercase(b[i]);
        if (bc == 0 || ac != bc) return false;
    }
    return b[a.len] == 0;
}

fn char lowercase(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

fn bool is_time_offset(usz i) {
    return i >= TIME_OFFSET && i < TIME_OFFSET + TIME_SIZE;
}

fn bool record_all_zero(char[] record) {
    for (usz i = 0; i < record.len; i++) {
        if (record[i] != 0) return false;
    }
    return true;
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

fn void print_time_fields(String label, char[] record) {
    int sec = unbcd(record[35]);
    int min = unbcd(record[36]);
    int hour = unbcd(record[37]);
    int day = unbcd(record[39]);
    int mon = unbcd(record[40]);
    int year = unbcd(record[41]);

    io::printf("%s: bytes hh=%02x mm=%02x ss=%02x weekday=%u day=%02x month=%02x year=%02x",
        label,
        (uint)record[37],
        (uint)record[36],
        (uint)record[35],
        (uint)record[38],
        (uint)record[39],
        (uint)record[40],
        (uint)record[41]);
    if (sec >= 0 && min >= 0 && hour >= 0 && day >= 0 && mon >= 0 && year >= 0 &&
        hour <= 23 && min <= 59 && sec <= 59 && record[38] <= 6 &&
        day >= 1 && day <= 31 && mon >= 1 && mon <= 12) {
        io::printf(" decoded=20%02d-%02d-%02d %02d:%02d:%02d weekday=%u",
            year, mon, day, hour, min, sec, (uint)record[38]);
    }
    io::printn();
}

fn void print_hex(String label, char[] buf) {
    if (label.len > 0) io::printf("%s", label);
    for (usz i = 0; i < buf.len; i++) {
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
    for (usz i = 0; i < RECORD_SIZE; i++) masked[i] = record[i];
    for (usz i = TIME_OFFSET; i < TIME_OFFSET + TIME_SIZE; i++) masked[i] = 0;
    sha256_hex(masked[..], out);
}

fn int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

fn bool is_hex_digit(char c) {
    return hex_nibble(c) >= 0;
}

fn bool is_hex_separator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':' || c == ',' || c == '-';
}

fn int parse_hex_record_text(String text, char[] out) {
    char[RECORD_SIZE * 2] digits;
    usz count = 0;

    for (usz i = 0; i < text.len; i++) {
        char c = text[i];
        if (c == '0' && i + 1 < text.len && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            i++;
            continue;
        }
        if (is_hex_digit(c)) {
            if (count >= RECORD_SIZE * 2) {
                io::printfn("Template has more than 48 bytes of hex");
                return -1;
            }
            digits[count] = c;
            count++;
            continue;
        }
        if (is_hex_separator(c)) continue;
        io::printfn("Template contains non-hex character: %c", (int)c);
        return -1;
    }

    if (count != RECORD_SIZE * 2) {
        io::printfn("Template must contain exactly 48 bytes, got %zu hex digits", count);
        return -1;
    }
    for (usz i = 0; i < RECORD_SIZE; i++) {
        int hi = hex_nibble(digits[i * 2]);
        int lo = hex_nibble(digits[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (char)((hi << 4) | lo);
    }
    return 0;
}

fn bool looks_like_hex_text(char[] buf) {
    for (usz i = 0; i < buf.len; i++) {
        char c = buf[i];
        if (is_hex_digit(c) || is_hex_separator(c) || c == 'x' || c == 'X') continue;
        return false;
    }
    return true;
}

fn int load_record_template(Options* opt, char[] out) {
    bool has_template_hex = string_was_set(opt.template_hex);
    bool has_template_file = string_was_set(opt.template_file);
    if (has_template_hex && has_template_file) {
        io::printfn("Use either --template or --template-file, not both");
        return -1;
    }
    if (has_template_hex) return parse_hex_record_text(opt.template_hex, out);
    if (!has_template_file) {
        io::printfn("dry-run needs --template or --template-file");
        return -1;
    }

    char[4096] buf;
    char[]? loaded = file::load_buffer(opt.template_file, buf[..]);
    if (catch err = loaded) {
        io::printfn("Could not read template file %s: %s", opt.template_file, err);
        return -1;
    }

    if (loaded.len == RECORD_SIZE && !looks_like_hex_text(loaded)) {
        copy_bytes(out, loaded);
        return 0;
    }

    String text = (String)loaded;
    return parse_hex_record_text(text, out);
}

fn int parse_fixed_2(String s, usz offset) {
    if (offset + 2 > s.len) return -1;
    int a = hex_nibble(s[offset]);
    int b = hex_nibble(s[offset + 1]);
    if (a < 0 || a > 9 || b < 0 || b > 9) return -1;
    return a * 10 + b;
}

fn int parse_fixed_4(String s, usz offset) {
    int a = parse_fixed_2(s, offset);
    int b = parse_fixed_2(s, offset + 2);
    if (a < 0 || b < 0) return -1;
    return a * 100 + b;
}

fn int fill_time_bytes(String arg, char[] out) {
    if (str_eq(arg, "now")) {
        TzDateTime local = datetime::now().to_local();
        write_time_bytes(
            (int)local.sec,
            (int)local.min,
            (int)local.hour,
            weekday_clock_value(local.weekday),
            (int)local.day,
            (int)local.month.ordinal + 1,
            local.year,
            out);
        return 0;
    } else {
        if (arg.len != 19 || arg[4] != '-' || arg[7] != '-' ||
            !(arg[10] == 'T' || arg[10] == ' ') ||
            arg[13] != ':' || arg[16] != ':') {
            io::printfn("Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time");
            return -1;
        }
        int year = parse_fixed_4(arg, 0);
        int mon = parse_fixed_2(arg, 5);
        int day = parse_fixed_2(arg, 8);
        int hour = parse_fixed_2(arg, 11);
        int min = parse_fixed_2(arg, 14);
        int sec = parse_fixed_2(arg, 17);
        if (year < 2000 || year > 2099 || mon < 1 || mon > 12 ||
            day < 1 || day > days_in_month(year, mon) || hour < 0 || hour > 23 ||
            min < 0 || min > 59 || sec < 0 || sec > 59) {
            io::printfn("Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time");
            return -1;
        }
        write_time_bytes(sec, min, hour, weekday_sunday_zero(year, mon, day), day, mon, year, out);
        return 0;
    }
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

fn int weekday_clock_value(Weekday weekday) {
    if (weekday == Weekday.SUNDAY) return 0;
    return (int)weekday.ordinal + 1;
}

fn int weekday_sunday_zero(int year, int mon, int day) {
    int[12] offsets = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (mon < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + offsets[mon - 1] + day) % 7;
}

fn int days_in_month(int year, int mon) {
    switch (mon) {
        case 1: return 31;
        case 2: return is_leap_year(year) ? 29 : 28;
        case 3: return 31;
        case 4: return 30;
        case 5: return 31;
        case 6: return 30;
        case 7: return 31;
        case 8: return 31;
        case 9: return 30;
        case 10: return 31;
        case 11: return 30;
        case 12: return 31;
        default: return 0;
    }
}

fn bool is_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

fn void patch_time(char[] input, char[] time_bytes, char[] output) {
    for (usz i = 0; i < RECORD_SIZE; i++) output[i] = input[i];
    for (usz i = 0; i < TIME_SIZE; i++) output[TIME_OFFSET + i] = time_bytes[i];
}

fn int verify_only_time_changed(char[] before, char[] after) {
    for (usz i = 0; i < RECORD_SIZE; i++) {
        if (!is_time_offset(i) && before[i] != after[i]) {
            io::printfn("Refusing: non-time byte changed at offset %zu", i);
            return -1;
        }
    }
    return 0;
}

fn void print_record_diff(char[] before, char[] after) {
    io::printfn("Changed offsets:");
    for (usz i = 0; i < RECORD_SIZE; i++) {
        if (before[i] != after[i]) {
            io::printfn("  %02zu: %02x -> %02x%s",
                i,
                (uint)before[i],
                (uint)after[i],
                is_time_offset(i) ? "" : "  NON-TIME");
        }
    }
}

fn void sleep_millis(long millis) {
    thread::sleep_ms(millis);
}

fn void checksum_packet(char[] packet) {
    ushort sum = 0;
    for (usz i = 3; i < REPORT_SIZE; i++) {
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

fn void build_config_packet(char report_id, char cmd, char[] payload, usz payload_len, ushort offset, char[] packet) {
    clear_bytes(packet);
    packet[0] = report_id;
    packet[3] = cmd;
    packet[4] = (char)payload_len;
    packet[5] = (char)(offset & 0xff);
    packet[6] = (char)((offset >> 8) & 0xff);
    packet[7] = 0;
    if (payload.len > 0 && payload_len > 0) {
        usz copy_len = payload_len;
        if (copy_len > REPORT_SIZE - CONFIG_PAYLOAD_OFFSET) copy_len = REPORT_SIZE - CONFIG_PAYLOAD_OFFSET;
        if (copy_len > payload.len) copy_len = payload.len;
        copy_bytes(packet[CONFIG_PAYLOAD_OFFSET..], payload[:copy_len]);
    }
    checksum_packet(packet);
}

fn ushort slot_offset(int slot) {
    return (ushort)(slot * 0x31);
}

fn int read_template_from_device(Options* opt, ClockTransport* transport, char[] record) {
    char[REPORT_SIZE] packet;
    char[REPORT_SIZE] response;
    ushort offset = slot_offset(opt.slot);
    clear_bytes(record);

    for (usz pos = 0; pos < RECORD_SIZE; pos += 4) {
        usz chunk_len = RECORD_SIZE - pos;
        if (chunk_len > 4) chunk_len = 4;
        usz response_len = 0;
        build_config_packet(opt.report_id, 0x05, "", chunk_len, (ushort)(offset + pos), packet[..]);
        if (opt.print_packets) {
            io::printf("cmd 0x05 chunk offset=0x%04x packet: ", (uint)(offset + pos));
            print_hex("", packet[..]);
        }
        if (transport_send_wait_checked(transport, &packet, 0x05, opt.timeout_ms, &response, &response_len, false) != 0) {
            return -1;
        }
        if (opt.print_packets) {
            io::printf("cmd 0x05 chunk offset=0x%04x response: ", (uint)(offset + pos));
            print_hex("", response[0:response_len]);
        }
        if (response_len == REPORT_SIZE && bytes_equal(response[..], packet[..])) {
            io::printfn("Refusing echoed read response: this HID collection did not return a real template");
            return -1;
        }
        if (response_len < 4 + chunk_len) {
            io::printfn("Read response too short at offset 0x%04x: got %zu bytes, need at least %zu",
                (uint)(offset + pos), response_len, 4 + chunk_len);
            return -1;
        }
        copy_bytes(record[pos..], response[4:chunk_len]);
    }

    if (record_all_zero(record)) {
        io::printfn("Refusing all-zero template: this is not a valid screen/clock config record");
        return -1;
    }
    return 0;
}

fn int write_time_to_device(Options* opt, ClockTransport* transport, char[] patched) {
    char[REPORT_SIZE] begin;
    char[REPORT_SIZE] write;
    char[REPORT_SIZE] commit;
    char[REPORT_SIZE] response;
    usz response_len = 0;

    build_simple_packet(opt.report_id, 0x01, begin[..]);
    build_config_packet(opt.report_id, 0x06, patched, RECORD_SIZE, slot_offset(opt.slot), write[..]);
    build_simple_packet(opt.report_id, 0x02, commit[..]);

    if (opt.print_packets) {
        print_hex("cmd 0x01 packet: ", begin[..]);
        print_hex("cmd 0x06 packet: ", write[..]);
        print_hex("cmd 0x02 packet: ", commit[..]);
    }

    if (transport_send_wait(transport, &begin, 0x01, opt.timeout_ms, &response, &response_len) != 0) return -1;
    if (transport_send_wait(transport, &write, 0x06, opt.timeout_ms, &response, &response_len) != 0) return -1;
    sleep_millis(10);
    if (transport_send_wait(transport, &commit, 0x02, opt.timeout_ms, &response, &response_len) != 0) return -1;
    return 0;
}

fn int run_dry_run(Options* opt) {
    char[RECORD_SIZE] template_record;
    char[TIME_SIZE] time_bytes;
    char[RECORD_SIZE] patched;
    char[REPORT_SIZE] packet;
    char[65] hash_before;
    char[65] hash_after;
    char[65] stable_hash_before;
    char[65] stable_hash_after;

    if (load_record_template(opt, template_record[..]) != 0) return 1;
    if (record_all_zero(template_record[..])) {
        io::printfn("Refusing all-zero template: this is not a valid screen/clock config record");
        return 1;
    }
    if (fill_time_bytes(opt.time_arg, time_bytes[..]) != 0) return 1;
    patch_time(template_record[..], time_bytes[..], patched[..]);
    if (verify_only_time_changed(template_record[..], patched[..]) != 0) return 1;

    sha256_hex(template_record[..], &hash_before);
    sha256_hex(patched[..], &hash_after);
    stable_sha256_hex(template_record[..], &stable_hash_before);
    stable_sha256_hex(patched[..], &stable_hash_after);
    print_hex("template: ", template_record[..]);
    print_hex("patched:  ", patched[..]);
    io::printfn("template sha256: %s", &hash_before);
    io::printfn("patched  sha256: %s", &hash_after);
    io::printfn("template stable sha256: %s", &stable_hash_before);
    io::printfn("patched  stable sha256: %s", &stable_hash_after);
    print_time_fields("old time bytes", template_record[..]);
    print_time_fields("new time bytes", patched[..]);
    print_record_diff(template_record[..], patched[..]);

    String why = time_validation_error(template_record[..]);
    if (!str_eq(why, "ok")) io::printfn("template time validation warning: %s", why);

    if (opt.print_packets) {
        build_config_packet(opt.report_id, 0x06, patched[..], RECORD_SIZE, slot_offset(opt.slot), packet[..]);
        print_hex("cmd 0x06 packet: ", packet[..]);
    }
    io::printfn("No HID device was opened. No report was sent.");
    return 0;
}

fn int run_read_template(Options* opt) {
    if (!opt.allow_hid_query) {
        io::printfn("Refusing HID query without --allow-hid-query");
        return 2;
    }

    ClockTransport transport;
    char[RECORD_SIZE] record;
    char[65] hash;
    char[65] stable_hash;

    if (transport_open(&transport, opt) != 0) return 1;
    int rc = read_template_from_device(opt, &transport, record[..]);
    transport_close(&transport);
    if (rc != 0) return 1;

    sha256_hex(record[..], &hash);
    stable_sha256_hex(record[..], &stable_hash);
    print_hex("template: ", record[..]);
    io::printfn("template sha256: %s", &hash);
    io::printfn("template stable sha256: %s", &stable_hash);
    print_time_fields("template time bytes", record[..]);
    String why = time_validation_error(record[..]);
    if (!str_eq(why, "ok")) io::printfn("template time validation warning: %s", why);

    if (string_was_set(opt.out_file)) {
        void? saved = file::save(opt.out_file, record[..]);
        if (catch err = saved) {
            io::printfn("Could not write %s: %s", opt.out_file, err);
            return 1;
        }
        io::printfn("wrote raw 48-byte template to %s", opt.out_file);
    }
    return 0;
}

fn int run_update_time(Options* opt) {
    if (!opt.allow_hid_query) {
        io::printfn("Refusing update without --allow-hid-query");
        return 2;
    }
    if (!opt.allow_config_write) {
        io::printfn("Refusing update without --allow-config-write");
        return 2;
    }
    if (!string_was_set(opt.confirm_write) || !str_eq(opt.confirm_write, WRITE_CONFIRM_TEXT)) {
        io::printfn("Refusing update without --confirm-write PATCH_ONLY_BYTES_35_41");
        return 2;
    }
    if (!string_was_set(opt.expected_stable_sha256)) {
        io::printfn("Refusing update without --expected-stable-sha256 from read-template");
        return 2;
    }

    ClockTransport transport;
    char[RECORD_SIZE] current;
    char[RECORD_SIZE] patched;
    char[TIME_SIZE] time_bytes;
    char[65] hash_current;
    char[65] hash_patched;
    char[65] stable_hash_current;
    char[65] stable_hash_patched;

    if (fill_time_bytes(opt.time_arg, time_bytes[..]) != 0) return 1;
    if (transport_open(&transport, opt) != 0) return 1;

    int rc = read_template_from_device(opt, &transport, current[..]);
    if (rc == 0) {
        sha256_hex(current[..], &hash_current);
        stable_sha256_hex(current[..], &stable_hash_current);
        if (string_was_set(opt.expected_template_sha256) && !hex_eq_ci(opt.expected_template_sha256, &hash_current)) {
            io::printfn("Refusing update: current template sha256 %s does not match expected %s",
                &hash_current,
                opt.expected_template_sha256);
            rc = -1;
        }
        if (!hex_eq_ci(opt.expected_stable_sha256, &stable_hash_current)) {
            io::printfn("Refusing update: stable template sha256 %s does not match expected %s",
                &stable_hash_current,
                opt.expected_stable_sha256);
            rc = -1;
        }
    }
    if (rc == 0) {
        String why = time_validation_error(current[..]);
        if (!str_eq(why, "ok")) {
            io::printfn("Refusing update: current record time fields look invalid: %s", why);
            rc = -1;
        }
    }
    if (rc == 0) {
        patch_time(current[..], time_bytes[..], patched[..]);
        rc = verify_only_time_changed(current[..], patched[..]);
    }
    if (rc == 0) {
        sha256_hex(patched[..], &hash_patched);
        stable_sha256_hex(patched[..], &stable_hash_patched);
        io::printfn("current template sha256: %s", &hash_current);
        io::printfn("patched  template sha256: %s", &hash_patched);
        io::printfn("current stable sha256: %s", &stable_hash_current);
        io::printfn("patched  stable sha256: %s", &stable_hash_patched);
        print_time_fields("old time bytes", current[..]);
        print_time_fields("new time bytes", patched[..]);
        print_record_diff(current[..], patched[..]);
        rc = write_time_to_device(opt, &transport, patched[..]);
    }

    transport_close(&transport);
    if (rc != 0) return 1;
    io::printfn("Clock update command sequence completed.");
    return 0;
}

fn int get_feature_report(Options* opt) {
    if (!opt.allow_hid_query) {
        io::printfn("Refusing HID feature read without --allow-hid-query");
        return 2;
    }

    ClockTransport transport;
    char[REPORT_SIZE] report;
    usz report_len = REPORT_SIZE;
    if (transport_open(&transport, opt) != 0) return 1;

    clear_bytes(report[..]);
    report[0] = opt.report_id;
    int rc = transport_get_feature_report(&transport, opt.report_id, &report, &report_len);
    transport_close(&transport);
    if (rc != 0) return 1;
    print_hex("feature response: ", report[0:report_len]);
    return 0;
}

fn int run_read_raw(Options* opt) {
    if (!opt.allow_hid_query) {
        io::printfn("Refusing raw HID read without --allow-hid-query");
        return 2;
    }
    if (opt.raw_command == 0) {
        io::printfn("read-raw needs --raw-command HEX");
        return 2;
    }

    ClockTransport transport;
    char[REPORT_SIZE] packet;
    char[REPORT_SIZE] response;
    usz response_len = 0;
    ushort offset = opt.raw_offset >= 0 ? (ushort)opt.raw_offset : slot_offset(opt.slot);

    if (transport_open(&transport, opt) != 0) return 1;
    build_config_packet(opt.report_id, opt.raw_command, "", (usz)opt.raw_length, offset, packet[..]);
    print_hex("raw read packet: ", packet[..]);
    int rc = transport_send_wait(&transport, &packet, opt.raw_command, opt.timeout_ms, &response, &response_len);
    transport_close(&transport);
    if (response_len > 0) print_hex("raw read response: ", response[0:response_len]);
    return rc == 0 ? 0 : 1;
}
