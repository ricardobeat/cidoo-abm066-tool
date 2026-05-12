module cidoo_clock_c3;

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

enum Command : int {
    NONE,
    HELP,
    LIST,
    PROBE,
    GET_FEATURE,
    READ_RAW,
    DRY_RUN,
    READ_TEMPLATE,
    READ_TEMPLATE_SPLIT,
    UPDATE_TIME,
}

struct Options {
    Command command;
    ushort vid;
    ushort pid;
    ushort usage_page;
    ushort usage;
    char report_id;
    char raw_command;
    usz raw_length;
    ushort raw_offset;
    bool raw_offset_set;
    int slot;
    int timeout_ms;
    String template_hex;
    bool has_template_hex;
    String template_file;
    bool has_template_file;
    String time_arg;
    String out_file;
    bool has_out_file;
    String expected_template_sha256;
    bool has_expected_template_sha256;
    String expected_stable_sha256;
    bool has_expected_stable_sha256;
    String confirm_write;
    bool has_confirm_write;
    bool print_packets;
    bool allow_hid_query;
    bool allow_config_write;
    bool strip_report_id_for_iohid;
}

fn int main(String[] args) {
    Options opt;
    init_options(&opt);
    if (parse_options(args, &opt) != 0) return 2;

    switch (opt.command) {
        case HELP:
            usage();
            return 0;
        case LIST:
            return transport_list_devices(&opt, true);
        case PROBE:
            return transport_list_devices(&opt, false);
        case GET_FEATURE:
            return get_feature_report(&opt);
        case READ_RAW:
            return run_read_raw(&opt);
        case DRY_RUN:
            return run_dry_run(&opt);
        case READ_TEMPLATE:
            return run_read_template(&opt);
        case READ_TEMPLATE_SPLIT:
            printf("read-template-split is not ported in the C3 version; use read-template.\n");
            return 2;
        case UPDATE_TIME:
            return run_update_time(&opt);
        default:
            usage();
            return 2;
    }
}

fn void usage() {
    printf(
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
        "  --strip-report-id-for-iohid       diagnostic: pass bytes 1..63 to IOKit\n",
        (uint)DEFAULT_VID,
        (uint)DEFAULT_PID,
        (uint)DEFAULT_USAGE_PAGE,
        (uint)DEFAULT_USAGE,
        (uint)DEFAULT_REPORT_ID);
}

fn void init_options(Options* opt) {
    memset(opt, 0, Options.sizeof);
    opt.command = NONE;
    opt.vid = DEFAULT_VID;
    opt.pid = DEFAULT_PID;
    opt.usage_page = DEFAULT_USAGE_PAGE;
    opt.usage = DEFAULT_USAGE;
    opt.report_id = DEFAULT_REPORT_ID;
    opt.raw_length = RECORD_SIZE;
    opt.slot = 0;
    opt.timeout_ms = 1500;
    opt.time_arg = "now";
}

fn int parse_options(String[] args, Options* opt) {
    if (args.len < 2) {
        usage();
        return -1;
    }

    opt.command = parse_command(args[1]);
    if (opt.command == NONE) {
        printf("Unknown command: %.*s\n", (int)args[1].len, args[1].ptr);
        usage();
        return -1;
    }

    for (usz i = 2; i < args.len; i++) {
        String arg = args[i];
        if (str_eq(arg, "--help") || str_eq(arg, "-h")) {
            opt.command = HELP;
        } else if (str_eq(arg, "--vid")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.vid = (ushort)parse_uint_or_die(args[i], arg, 0xffff);
        } else if (str_eq(arg, "--pid")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.pid = (ushort)parse_uint_or_die(args[i], arg, 0xffff);
        } else if (str_eq(arg, "--usage-page")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.usage_page = (ushort)parse_uint_or_die(args[i], arg, 0xffff);
        } else if (str_eq(arg, "--usage")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.usage = (ushort)parse_uint_or_die(args[i], arg, 0xffff);
        } else if (str_eq(arg, "--report-id")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.report_id = (char)parse_uint_or_die(args[i], arg, 0xff);
        } else if (str_eq(arg, "--raw-command")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.raw_command = (char)parse_uint_or_die(args[i], arg, 0xff);
        } else if (str_eq(arg, "--raw-length")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.raw_length = (usz)parse_int_range_or_die(args[i], arg, 0, REPORT_SIZE - CONFIG_PAYLOAD_OFFSET);
        } else if (str_eq(arg, "--raw-offset")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.raw_offset = (ushort)parse_uint_or_die(args[i], arg, 0xffff);
            opt.raw_offset_set = true;
        } else if (str_eq(arg, "--slot")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.slot = parse_int_range_or_die(args[i], arg, 0, 1024);
        } else if (str_eq(arg, "--time")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.time_arg = args[i];
        } else if (str_eq(arg, "--template")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.template_hex = args[i];
            opt.has_template_hex = true;
        } else if (str_eq(arg, "--template-file")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.template_file = args[i];
            opt.has_template_file = true;
        } else if (str_eq(arg, "--out")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.out_file = args[i];
            opt.has_out_file = true;
        } else if (str_eq(arg, "--expected-template-sha256")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.expected_template_sha256 = args[i];
            opt.has_expected_template_sha256 = true;
        } else if (str_eq(arg, "--expected-stable-sha256")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.expected_stable_sha256 = args[i];
            opt.has_expected_stable_sha256 = true;
        } else if (str_eq(arg, "--confirm-write")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.confirm_write = args[i];
            opt.has_confirm_write = true;
        } else if (str_eq(arg, "--print-packets")) {
            opt.print_packets = true;
        } else if (str_eq(arg, "--timeout-ms")) {
            if (!need_value(args, &i, arg)) return -1;
            opt.timeout_ms = parse_int_range_or_die(args[i], arg, 100, 30000);
        } else if (str_eq(arg, "--allow-hid-query")) {
            opt.allow_hid_query = true;
        } else if (str_eq(arg, "--allow-config-write")) {
            opt.allow_config_write = true;
        } else if (str_eq(arg, "--strip-report-id-for-iohid")) {
            opt.strip_report_id_for_iohid = true;
        } else {
            printf("Unexpected argument: %.*s\n", (int)arg.len, arg.ptr);
            return -1;
        }
    }

    if ((uint)opt.slot * 0x31 > 0xffff - (RECORD_SIZE - 1)) {
        printf("--slot is too large for the 16-bit config offset\n");
        return -1;
    }
    return 0;
}

fn bool need_value(String[] args, usz* i, String arg) {
    if (*i + 1 >= args.len) {
        printf("Missing value for %.*s\n", (int)arg.len, arg.ptr);
        return false;
    }
    *i += 1;
    return true;
}

fn Command parse_command(String s) {
    if (str_eq(s, "help") || str_eq(s, "--help") || str_eq(s, "-h")) return HELP;
    if (str_eq(s, "list")) return LIST;
    if (str_eq(s, "probe")) return PROBE;
    if (str_eq(s, "get-feature")) return GET_FEATURE;
    if (str_eq(s, "read-raw")) return READ_RAW;
    if (str_eq(s, "dry-run")) return DRY_RUN;
    if (str_eq(s, "read-template")) return READ_TEMPLATE;
    if (str_eq(s, "read-template-split")) return READ_TEMPLATE_SPLIT;
    if (str_eq(s, "update-time")) return UPDATE_TIME;
    return NONE;
}

fn uint parse_uint_or_die(String s, String name, uint max) {
    uint value = 0;
    if (!parse_uint(s, max, &value)) {
        printf("Invalid %.*s: %.*s\n", (int)name.len, name.ptr, (int)s.len, s.ptr);
        exit(2);
        return 0;
    }
    return value;
}

fn int parse_int_range_or_die(String s, String name, int min, int max) {
    uint value = 0;
    if (!parse_uint(s, (uint)max, &value) || (int)value < min) {
        printf("Invalid %.*s: %.*s\n", (int)name.len, name.ptr, (int)s.len, s.ptr);
        exit(2);
        return 0;
    }
    return (int)value;
}

fn bool parse_uint(String s, uint max, uint* out) {
    if (s.len == 0) return false;
    usz index = 0;
    uint base = 10;
    if (s.len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        index = 2;
        base = 16;
    }
    if (index >= s.len) return false;
    uint value = 0;
    for (; index < s.len; index++) {
        int digit = -1;
        char c = s[index];
        if (c >= '0' && c <= '9') {
            digit = (int)(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = (int)(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = (int)(c - 'A' + 10);
        } else {
            return false;
        }
        if ((uint)digit >= base) return false;
        if (value > (max - (uint)digit) / base) return false;
        value = value * base + (uint)digit;
    }
    *out = value;
    return true;
}

fn bool str_eq(String a, String b) {
    if (a.len != b.len) return false;
    for (usz i = 0; i < a.len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
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

fn bool record_all_zero(char* record) {
    for (usz i = 0; i < RECORD_SIZE; i++) {
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

fn String time_validation_error(char* record) {
    if (!bcd_in_range(record[35], 0, 59)) return "seconds byte 35 is not valid BCD 00..59";
    if (!bcd_in_range(record[36], 0, 59)) return "minutes byte 36 is not valid BCD 00..59";
    if (!bcd_in_range(record[37], 0, 23)) return "hours byte 37 is not valid BCD 00..23";
    if (record[38] > 6) return "weekday byte 38 is not in range 0..6";
    if (!bcd_in_range(record[39], 1, 31)) return "day byte 39 is not valid BCD 01..31";
    if (!bcd_in_range(record[40], 1, 12)) return "month byte 40 is not valid BCD 01..12";
    if (!bcd_in_range(record[41], 0, 99)) return "year byte 41 is not valid BCD 00..99";
    return "ok";
}

fn void print_time_fields(String label, char* record) {
    int sec = unbcd(record[35]);
    int min = unbcd(record[36]);
    int hour = unbcd(record[37]);
    int day = unbcd(record[39]);
    int mon = unbcd(record[40]);
    int year = unbcd(record[41]);

    printf("%.*s: bytes hh=%02x mm=%02x ss=%02x weekday=%u day=%02x month=%02x year=%02x",
        (int)label.len,
        label.ptr,
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
        printf(" decoded=20%02d-%02d-%02d %02d:%02d:%02d weekday=%u",
            year, mon, day, hour, min, sec, (uint)record[38]);
    }
    printf("\n");
}

fn void print_hex(String label, char* buf, usz len) {
    if (label.len > 0) printf("%.*s", (int)label.len, label.ptr);
    for (usz i = 0; i < len; i++) {
        if (i > 0) printf(" ");
        printf("%02x", (uint)buf[i]);
    }
    printf("\n");
}

fn char hex_digit(int v) {
    return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

fn void sha256_hex(char* buf, usz len, char* out) {
    char[CC_SHA256_DIGEST_LENGTH] digest;
    CC_SHA256(buf, (uint)len, &digest);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        out[i * 2] = hex_digit((digest[i] >> 4) & 0x0f);
        out[i * 2 + 1] = hex_digit(digest[i] & 0x0f);
    }
    out[64] = 0;
}

fn void stable_sha256_hex(char* record, char* out) {
    char[RECORD_SIZE] masked;
    for (usz i = 0; i < RECORD_SIZE; i++) masked[i] = record[i];
    for (usz i = TIME_OFFSET; i < TIME_OFFSET + TIME_SIZE; i++) masked[i] = 0;
    sha256_hex(&masked, RECORD_SIZE, out);
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

fn int parse_hex_record_text(String text, char* out) {
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
                printf("Template has more than 48 bytes of hex\n");
                return -1;
            }
            digits[count] = c;
            count++;
            continue;
        }
        if (is_hex_separator(c)) continue;
        printf("Template contains non-hex character: %c\n", (int)c);
        return -1;
    }

    if (count != RECORD_SIZE * 2) {
        printf("Template must contain exactly 48 bytes, got %zu hex digits\n", count);
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

fn bool looks_like_hex_text(char* buf, usz len) {
    for (usz i = 0; i < len; i++) {
        char c = buf[i];
        if (is_hex_digit(c) || is_hex_separator(c) || c == 'x' || c == 'X') continue;
        return false;
    }
    return true;
}

fn int load_record_template(Options* opt, char* out) {
    if (opt.has_template_hex && opt.has_template_file) {
        printf("Use either --template or --template-file, not both\n");
        return -1;
    }
    if (opt.has_template_hex) return parse_hex_record_text(opt.template_hex, out);
    if (!opt.has_template_file) {
        printf("dry-run needs --template or --template-file\n");
        return -1;
    }

    FilePtr f = fopen(opt.template_file.ptr, "rb");
    if (f == null) {
        perror(opt.template_file.ptr);
        return -1;
    }
    char[4096] buf;
    usz len = fread(&buf, 1, 4096, f);
    if (ferror(f) != 0) {
        perror(opt.template_file.ptr);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (len == RECORD_SIZE && !looks_like_hex_text(&buf, len)) {
        memcpy(out, &buf, RECORD_SIZE);
        return 0;
    }

    String text = buf[0:len];
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

fn int fill_time_bytes(String arg, char* out) {
    Tm tmv;
    memset(&tmv, 0, Tm.sizeof);

    if (str_eq(arg, "now")) {
        TimeT now = time(null);
        if (localtime_r(&now, &tmv) == null) {
            perror("localtime_r");
            return -1;
        }
    } else {
        if (arg.len != 19 || arg[4] != '-' || arg[7] != '-' ||
            !(arg[10] == 'T' || arg[10] == ' ') ||
            arg[13] != ':' || arg[16] != ':') {
            printf("Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time\n");
            return -1;
        }
        int year = parse_fixed_4(arg, 0);
        int mon = parse_fixed_2(arg, 5);
        int day = parse_fixed_2(arg, 8);
        int hour = parse_fixed_2(arg, 11);
        int min = parse_fixed_2(arg, 14);
        int sec = parse_fixed_2(arg, 17);
        if (year < 2000 || year > 2099 || mon < 1 || mon > 12 ||
            day < 1 || day > 31 || hour < 0 || hour > 23 ||
            min < 0 || min > 59 || sec < 0 || sec > 59) {
            printf("Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time\n");
            return -1;
        }

        Tm requested;
        memset(&requested, 0, Tm.sizeof);
        requested.tm_year = year - 1900;
        requested.tm_mon = mon - 1;
        requested.tm_mday = day;
        requested.tm_hour = hour;
        requested.tm_min = min;
        requested.tm_sec = sec;
        requested.tm_isdst = -1;
        TimeT t = mktime(&requested);
        if (t == (TimeT)-1 || localtime_r(&t, &tmv) == null) {
            printf("Could not normalize --time value\n");
            return -1;
        }
        if (tmv.tm_year != year - 1900 || tmv.tm_mon != mon - 1 ||
            tmv.tm_mday != day || tmv.tm_hour != hour ||
            tmv.tm_min != min || tmv.tm_sec != sec) {
            printf("Invalid --time, date/time is not representable in local time\n");
            return -1;
        }
    }

    out[0] = bcd(tmv.tm_sec);
    out[1] = bcd(tmv.tm_min);
    out[2] = bcd(tmv.tm_hour);
    out[3] = (char)tmv.tm_wday;
    out[4] = bcd(tmv.tm_mday);
    out[5] = bcd(tmv.tm_mon + 1);
    out[6] = bcd((tmv.tm_year + 1900) % 100);
    return 0;
}

fn void patch_time(char* input, char* time_bytes, char* output) {
    for (usz i = 0; i < RECORD_SIZE; i++) output[i] = input[i];
    for (usz i = 0; i < TIME_SIZE; i++) output[TIME_OFFSET + i] = time_bytes[i];
}

fn int verify_only_time_changed(char* before, char* after) {
    for (usz i = 0; i < RECORD_SIZE; i++) {
        if (!is_time_offset(i) && before[i] != after[i]) {
            printf("Refusing: non-time byte changed at offset %zu\n", i);
            return -1;
        }
    }
    return 0;
}

fn void print_record_diff(char* before, char* after) {
    printf("Changed offsets:\n");
    for (usz i = 0; i < RECORD_SIZE; i++) {
        if (before[i] != after[i]) {
            printf("  %02zu: %02x -> %02x%s\n",
                i,
                (uint)before[i],
                (uint)after[i],
                is_time_offset(i) ? "" : "  NON-TIME");
        }
    }
}

fn void sleep_millis(long millis) {
    Timespec ts;
    ts.tv_sec = millis / 1000;
    ts.tv_nsec = (millis % 1000) * 1000000;
    nanosleep(&ts, null);
}

fn void checksum_packet(char* packet) {
    ushort sum = 0;
    for (usz i = 3; i < REPORT_SIZE; i++) {
        sum = (ushort)(sum + (ushort)packet[i]);
    }
    packet[1] = (char)(sum & 0xff);
    packet[2] = (char)((sum >> 8) & 0xff);
}

fn void build_simple_packet(char report_id, char cmd, char* packet) {
    memset(packet, 0, REPORT_SIZE);
    packet[0] = report_id;
    packet[3] = cmd;
    checksum_packet(packet);
}

fn void build_config_packet(char report_id, char cmd, char* payload, usz payload_len, ushort offset, char* packet) {
    memset(packet, 0, REPORT_SIZE);
    packet[0] = report_id;
    packet[3] = cmd;
    packet[4] = (char)payload_len;
    packet[5] = (char)(offset & 0xff);
    packet[6] = (char)((offset >> 8) & 0xff);
    packet[7] = 0;
    if (payload != null && payload_len > 0) {
        usz copy_len = payload_len;
        if (copy_len > REPORT_SIZE - CONFIG_PAYLOAD_OFFSET) copy_len = REPORT_SIZE - CONFIG_PAYLOAD_OFFSET;
        memcpy(packet + CONFIG_PAYLOAD_OFFSET, payload, copy_len);
    }
    checksum_packet(packet);
}

fn ushort slot_offset(int slot) {
    return (ushort)(slot * 0x31);
}

fn int read_template_from_device(Options* opt, ClockTransport* transport, char* record) {
    char[REPORT_SIZE] packet;
    char[REPORT_SIZE] response;
    ushort offset = slot_offset(opt.slot);
    memset(record, 0, RECORD_SIZE);

    for (usz pos = 0; pos < RECORD_SIZE; pos += 4) {
        usz chunk_len = RECORD_SIZE - pos;
        if (chunk_len > 4) chunk_len = 4;
        usz response_len = 0;
        build_config_packet(opt.report_id, 0x05, null, chunk_len, (ushort)(offset + pos), &packet);
        if (opt.print_packets) {
            printf("cmd 0x05 chunk offset=0x%04x packet: ", (uint)(offset + pos));
            print_hex("", &packet, REPORT_SIZE);
        }
        if (transport_send_wait_checked(transport, &packet, 0x05, opt.timeout_ms, &response, &response_len, false) != 0) {
            return -1;
        }
        if (opt.print_packets) {
            printf("cmd 0x05 chunk offset=0x%04x response: ", (uint)(offset + pos));
            print_hex("", &response, response_len);
        }
        if (response_len == REPORT_SIZE && memcmp(&response, &packet, REPORT_SIZE) == 0) {
            printf("Refusing echoed read response: this HID collection did not return a real template\n");
            return -1;
        }
        if (response_len < 4 + chunk_len) {
            printf("Read response too short at offset 0x%04x: got %zu bytes, need at least %zu\n",
                (uint)(offset + pos), response_len, 4 + chunk_len);
            return -1;
        }
        memcpy(record + pos, &response + 4, chunk_len);
    }

    if (record_all_zero(record)) {
        printf("Refusing all-zero template: this is not a valid screen/clock config record\n");
        return -1;
    }
    return 0;
}

fn int write_time_to_device(Options* opt, ClockTransport* transport, char* patched) {
    char[REPORT_SIZE] begin;
    char[REPORT_SIZE] write;
    char[REPORT_SIZE] commit;
    char[REPORT_SIZE] response;
    usz response_len = 0;

    build_simple_packet(opt.report_id, 0x01, &begin);
    build_config_packet(opt.report_id, 0x06, patched, RECORD_SIZE, slot_offset(opt.slot), &write);
    build_simple_packet(opt.report_id, 0x02, &commit);

    if (opt.print_packets) {
        print_hex("cmd 0x01 packet: ", &begin, REPORT_SIZE);
        print_hex("cmd 0x06 packet: ", &write, REPORT_SIZE);
        print_hex("cmd 0x02 packet: ", &commit, REPORT_SIZE);
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

    if (load_record_template(opt, &template_record) != 0) return 1;
    if (record_all_zero(&template_record)) {
        printf("Refusing all-zero template: this is not a valid screen/clock config record\n");
        return 1;
    }
    if (fill_time_bytes(opt.time_arg, &time_bytes) != 0) return 1;
    patch_time(&template_record, &time_bytes, &patched);
    if (verify_only_time_changed(&template_record, &patched) != 0) return 1;

    sha256_hex(&template_record, RECORD_SIZE, &hash_before);
    sha256_hex(&patched, RECORD_SIZE, &hash_after);
    stable_sha256_hex(&template_record, &stable_hash_before);
    stable_sha256_hex(&patched, &stable_hash_after);
    print_hex("template: ", &template_record, RECORD_SIZE);
    print_hex("patched:  ", &patched, RECORD_SIZE);
    printf("template sha256: %s\n", &hash_before);
    printf("patched  sha256: %s\n", &hash_after);
    printf("template stable sha256: %s\n", &stable_hash_before);
    printf("patched  stable sha256: %s\n", &stable_hash_after);
    print_time_fields("old time bytes", &template_record);
    print_time_fields("new time bytes", &patched);
    print_record_diff(&template_record, &patched);

    String why = time_validation_error(&template_record);
    if (!str_eq(why, "ok")) printf("template time validation warning: %.*s\n", (int)why.len, why.ptr);

    if (opt.print_packets) {
        build_config_packet(opt.report_id, 0x06, &patched, RECORD_SIZE, slot_offset(opt.slot), &packet);
        print_hex("cmd 0x06 packet: ", &packet, REPORT_SIZE);
    }
    printf("No HID device was opened. No report was sent.\n");
    return 0;
}

fn int run_read_template(Options* opt) {
    if (!opt.allow_hid_query) {
        printf("Refusing HID query without --allow-hid-query\n");
        return 2;
    }

    ClockTransport transport;
    char[RECORD_SIZE] record;
    char[65] hash;
    char[65] stable_hash;

    if (transport_open(&transport, opt) != 0) return 1;
    int rc = read_template_from_device(opt, &transport, &record);
    transport_close(&transport);
    if (rc != 0) return 1;

    sha256_hex(&record, RECORD_SIZE, &hash);
    stable_sha256_hex(&record, &stable_hash);
    print_hex("template: ", &record, RECORD_SIZE);
    printf("template sha256: %s\n", &hash);
    printf("template stable sha256: %s\n", &stable_hash);
    print_time_fields("template time bytes", &record);
    String why = time_validation_error(&record);
    if (!str_eq(why, "ok")) printf("template time validation warning: %.*s\n", (int)why.len, why.ptr);

    if (opt.has_out_file) {
        FilePtr f = fopen(opt.out_file.ptr, "wb");
        if (f == null) {
            perror(opt.out_file.ptr);
            return 1;
        }
        if (fwrite(&record, 1, RECORD_SIZE, f) != RECORD_SIZE) {
            perror(opt.out_file.ptr);
            fclose(f);
            return 1;
        }
        fclose(f);
        printf("wrote raw 48-byte template to %.*s\n", (int)opt.out_file.len, opt.out_file.ptr);
    }
    return 0;
}

fn int run_update_time(Options* opt) {
    if (!opt.allow_hid_query) {
        printf("Refusing update without --allow-hid-query\n");
        return 2;
    }
    if (!opt.allow_config_write) {
        printf("Refusing update without --allow-config-write\n");
        return 2;
    }
    if (!opt.has_confirm_write || !str_eq(opt.confirm_write, WRITE_CONFIRM_TEXT)) {
        printf("Refusing update without --confirm-write PATCH_ONLY_BYTES_35_41\n");
        return 2;
    }
    if (!opt.has_expected_stable_sha256) {
        printf("Refusing update without --expected-stable-sha256 from read-template\n");
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

    if (fill_time_bytes(opt.time_arg, &time_bytes) != 0) return 1;
    if (transport_open(&transport, opt) != 0) return 1;

    int rc = read_template_from_device(opt, &transport, &current);
    if (rc == 0) {
        sha256_hex(&current, RECORD_SIZE, &hash_current);
        stable_sha256_hex(&current, &stable_hash_current);
        if (opt.has_expected_template_sha256 && !hex_eq_ci(opt.expected_template_sha256, &hash_current)) {
            printf("Refusing update: current template sha256 %s does not match expected %.*s\n",
                &hash_current,
                (int)opt.expected_template_sha256.len,
                opt.expected_template_sha256.ptr);
            rc = -1;
        }
        if (!hex_eq_ci(opt.expected_stable_sha256, &stable_hash_current)) {
            printf("Refusing update: stable template sha256 %s does not match expected %.*s\n",
                &stable_hash_current,
                (int)opt.expected_stable_sha256.len,
                opt.expected_stable_sha256.ptr);
            rc = -1;
        }
    }
    if (rc == 0) {
        String why = time_validation_error(&current);
        if (!str_eq(why, "ok")) {
            printf("Refusing update: current record time fields look invalid: %.*s\n", (int)why.len, why.ptr);
            rc = -1;
        }
    }
    if (rc == 0) {
        patch_time(&current, &time_bytes, &patched);
        rc = verify_only_time_changed(&current, &patched);
    }
    if (rc == 0) {
        sha256_hex(&patched, RECORD_SIZE, &hash_patched);
        stable_sha256_hex(&patched, &stable_hash_patched);
        printf("current template sha256: %s\n", &hash_current);
        printf("patched  template sha256: %s\n", &hash_patched);
        printf("current stable sha256: %s\n", &stable_hash_current);
        printf("patched  stable sha256: %s\n", &stable_hash_patched);
        print_time_fields("old time bytes", &current);
        print_time_fields("new time bytes", &patched);
        print_record_diff(&current, &patched);
        rc = write_time_to_device(opt, &transport, &patched);
    }

    transport_close(&transport);
    if (rc != 0) return 1;
    printf("Clock update command sequence completed.\n");
    return 0;
}

fn int get_feature_report(Options* opt) {
    if (!opt.allow_hid_query) {
        printf("Refusing HID feature read without --allow-hid-query\n");
        return 2;
    }

    ClockTransport transport;
    char[REPORT_SIZE] report;
    usz report_len = REPORT_SIZE;
    if (transport_open(&transport, opt) != 0) return 1;

    memset(&report, 0, REPORT_SIZE);
    report[0] = opt.report_id;
    int rc = transport_get_feature_report(&transport, opt.report_id, &report, &report_len);
    transport_close(&transport);
    if (rc != 0) return 1;
    print_hex("feature response: ", &report, report_len);
    return 0;
}

fn int run_read_raw(Options* opt) {
    if (!opt.allow_hid_query) {
        printf("Refusing raw HID read without --allow-hid-query\n");
        return 2;
    }
    if (opt.raw_command == 0) {
        printf("read-raw needs --raw-command HEX\n");
        return 2;
    }

    ClockTransport transport;
    char[REPORT_SIZE] packet;
    char[REPORT_SIZE] response;
    usz response_len = 0;
    ushort offset = opt.raw_offset_set ? opt.raw_offset : slot_offset(opt.slot);

    if (transport_open(&transport, opt) != 0) return 1;
    build_config_packet(opt.report_id, opt.raw_command, null, opt.raw_length, offset, &packet);
    print_hex("raw read packet: ", &packet, REPORT_SIZE);
    int rc = transport_send_wait(&transport, &packet, opt.raw_command, opt.timeout_ms, &response, &response_len);
    transport_close(&transport);
    if (response_len > 0) print_hex("raw read response: ", &response, response_len);
    return rc == 0 ? 0 : 1;
}
