module cidoo_clock_c3;

import std::io;

fn int parse_options(String[] args, Options* options) {
    bool upload_image = str_eq(args[1], "upload-image");
    int image_path_count = 0;

    for (sz i = 2; i < args.len; i++) {
        String arg = args[i];
        if (str_eq(arg, "-h") || str_eq(arg, "--help")) {
            options.help = true;
            continue;
        }
        if (arg.len < 3 || arg[0] != '-' || arg[1] != '-') {
            if (upload_image) {
                if (image_path_count == 0) {
                    options.image1_path = arg;
                } else if (image_path_count == 1) {
                    options.image2_path = arg;
                } else {
                    io::printfn("upload-image takes exactly two image paths");
                    return -1;
                }
                image_path_count++;
                continue;
            }
            io::printfn("Unexpected argument: %s", arg);
            return -1;
        }

        String name = arg[2..];
        String value = {};
        bool has_inline_value = false;
        int equals = find_char(name, '=');
        if (equals >= 0) {
            sz split = (sz)equals;
            value = split + 1 < name.len ? name[split + 1..] : "";
            name = split > 0 ? name[0:split] : "";
            has_inline_value = true;
        }
        if (name.len == 0) {
            io::printfn("Invalid option: %s", arg);
            return -1;
        }

        if (str_eq(name, "slot")) {
            uint parsed;
            if (read_option_value(name, value, has_inline_value, &i, args, &value) != 0 ||
                parse_uint_arg(value, "--slot", 1024, &parsed) != 0) return -1;
            options.slot = (int)parsed;
            continue;
        }
        if (str_eq(name, "timeout-ms")) {
            uint parsed;
            if (read_option_value(name, value, has_inline_value, &i, args, &value) != 0 ||
                parse_uint_arg(value, "--timeout-ms", 30000, &parsed) != 0) return -1;
            options.timeout_ms = (int)parsed;
            continue;
        }
        if (str_eq(name, "time")) {
            if (read_option_value(name, value, has_inline_value, &i, args, &options.time_arg) != 0) return -1;
            continue;
        }
        if (str_eq(name, "dry-run")) {
            if (reject_flag_value(name, has_inline_value) != 0) return -1;
            options.dry_run = true;
            continue;
        }
        if (str_eq(name, "debug")) {
            if (reject_flag_value(name, has_inline_value) != 0) return -1;
            options.debug = true;
            continue;
        }

        io::printfn("Unknown option: --%s", name);
        return -1;
    }

    if (options.help) return 0;
    if (options.dry_run && !str_eq(args[1], "update-time") && !upload_image) {
        io::printfn("--dry-run only applies to update-time and upload-image");
        return -1;
    }
    if (upload_image && !options.help && image_path_count != 2) {
        io::printfn("upload-image takes exactly two image paths");
        return -1;
    }
    return validate_options(options);
}

fn int find_char(String s, char needle) {
    for (sz i = 0; i < s.len; i++) {
        if (s[i] == needle) return (int)i;
    }
    return -1;
}

fn int read_option_value(String name, String inline_value, bool has_inline_value, sz* index, String[] args, String* out) {
    if (has_inline_value) {
        *out = inline_value;
        return 0;
    }
    *index = *index + 1;
    if (*index >= args.len) {
        io::printfn("Missing option argument for --%s", name);
        return -1;
    }
    *out = args[*index];
    return 0;
}

fn int reject_flag_value(String name, bool has_inline_value) {
    if (has_inline_value) {
        io::printfn("Option --%s does not take an argument", name);
        return -1;
    }
    return 0;
}

fn int parse_uint_arg(String s, String name, uint max, uint* out) {
    if (s.len == 0) {
        io::printfn("Invalid %s: %s", name, s);
        return -1;
    }

    sz pos = 0;
    uint radix = 10;
    if (s.len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        pos = 2;
        radix = 16;
    }
    if (pos >= s.len) {
        io::printfn("Invalid %s: %s", name, s);
        return -1;
    }

    ulong value = 0;
    for (; pos < s.len; pos++) {
        int digit = hex_nibble(s[pos]);
        if (digit < 0 || digit >= (int)radix) {
            io::printfn("Invalid %s: %s", name, s);
            return -1;
        }
        if (value > ((ulong)max - (ulong)digit) / (ulong)radix) {
            io::printfn("Invalid %s: %s", name, s);
            return -1;
        }
        value = value * (ulong)radix + (ulong)digit;
    }

    *out = (uint)value;
    return 0;
}

fn int validate_options(Options* options) {
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
