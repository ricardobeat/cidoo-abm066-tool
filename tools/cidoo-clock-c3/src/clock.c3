module cdoo;

import std::io;
import std::thread;
import std::time::datetime;

fn void print_time_fields(String label, char[] record) {
    int sec = unbcd(record[35]);
    int min = unbcd(record[36]);
    int hour = unbcd(record[37]);
    int day = unbcd(record[39]);
    int mon = unbcd(record[40]);
    int year = unbcd(record[41]);

    io::printf("%s: bytes hh=%02x mm=%02x ss=%02x weekday=%d day=%02x month=%02x year=%02x",
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
        io::printf(" decoded=20%02d-%02d-%02d %02d:%02d:%02d weekday=%d",
            year, mon, day, hour, min, sec, (uint)record[38]);
    }
    io::printn();
}

fn int parse_fixed_2(String s, sz offset) {
    if (offset + 2 > s.len) return -1;
    int a = hex_nibble(s[offset]);
    int b = hex_nibble(s[offset + 1]);
    if (a < 0 || a > 9 || b < 0 || b > 9) return -1;
    return a * 10 + b;
}

fn int parse_fixed_4(String s, sz offset) {
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
            day < 1 || day > 31 || hour < 0 || hour > 23 ||
            min < 0 || min > 59 || sec < 0 || sec > 59) {
            io::printfn("Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time");
            return -1;
        }

        Month month = Month::from_ordinal(mon - 1);
        DateTime parsed = datetime::at(year, month, day, hour, min, sec);
        if (parsed.year != year || parsed.month != month || (int)parsed.day != day ||
            (int)parsed.hour != hour || (int)parsed.min != min || (int)parsed.sec != sec) {
            io::printfn("Invalid --time, expected now or YYYY-MM-DDTHH:MM:SS in local time");
            return -1;
        }

        write_time_bytes(sec, min, hour, weekday_clock_value(parsed.weekday), day, mon, year, out);
        return 0;
    }
}

fn int weekday_clock_value(Weekday weekday) {
    if (weekday == Weekday.SUNDAY) return 0;
    return (int)weekday.ordinal + 1;
}

fn int verify_only_time_changed(char[] before, char[] after, bool debug) {
    int offset = time_change_violation_offset(before, after);
    if (offset >= 0) {
        if (debug) {
            io::printfn("Refusing: non-time byte changed at offset %d", offset);
        } else {
            io::printfn("Refusing: non-time byte changed");
        }
        return -1;
    }
    return 0;
}

fn void print_record_diff(char[] before, char[] after) {
    io::printfn("Changed offsets:");
    for (sz i = 0; i < RECORD_SIZE; i++) {
        if (before[i] != after[i]) {
            io::printfn("  %02d: %02x -> %02x%s",
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

fn int read_template_from_device(Options* opt, ClockTransport* transport, char[] record) {
    char[REPORT_SIZE] packet;
    char[REPORT_SIZE] response;
    ushort offset = slot_offset(opt.slot);
    clear_bytes(record);

    for (sz pos = 0; pos < RECORD_SIZE; pos += 4) {
        sz chunk_len = RECORD_SIZE - pos;
        if (chunk_len > 4) chunk_len = 4;
        sz response_len = 0;
        build_config_packet(DEFAULT_REPORT_ID, 0x05, "", chunk_len, (ushort)(offset + pos), packet[..]);
        if (opt.debug) {
            io::printf("cmd 0x05 chunk offset=0x%04x packet: ", (uint)(offset + pos));
            print_hex("", packet[..]);
        }
        if (transport_send_wait_checked(transport, &packet, 0x05, opt.timeout_ms, &response, &response_len, false) != 0) {
            return -1;
        }
        if (opt.debug) {
            io::printf("cmd 0x05 chunk offset=0x%04x response: ", (uint)(offset + pos));
            print_hex("", response[0:response_len]);
        }
        if (response_len == REPORT_SIZE && bytes_equal(response[..], packet[..])) {
            io::printfn("Refusing echoed read response: this HID collection did not return a real template");
            return -1;
        }
        if (response_len < 4 + chunk_len) {
            io::printfn("Read response too short at offset 0x%04x: got %d bytes, need at least %d",
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

fn void build_update_packets(Options* opt, char[] patched, char[] begin, char[] write, char[] commit) {
    build_simple_packet(DEFAULT_REPORT_ID, 0x01, begin);
    build_config_packet(DEFAULT_REPORT_ID, 0x06, patched, RECORD_SIZE, slot_offset(opt.slot), write);
    build_simple_packet(DEFAULT_REPORT_ID, 0x02, commit);
}

fn void print_update_packets(Options* opt, char[] patched) {
    char[REPORT_SIZE] begin;
    char[REPORT_SIZE] write;
    char[REPORT_SIZE] commit;
    build_update_packets(opt, patched, begin[..], write[..], commit[..]);
    print_hex("cmd 0x01 packet: ", begin[..]);
    print_hex("cmd 0x06 packet: ", write[..]);
    print_hex("cmd 0x02 packet: ", commit[..]);
}

fn int write_config_to_device(Options* opt, ClockTransport* transport, char[] patched) {
    char[REPORT_SIZE] begin;
    char[REPORT_SIZE] write;
    char[REPORT_SIZE] commit;
    char[REPORT_SIZE] response;
    sz response_len = 0;

    build_update_packets(opt, patched, begin[..], write[..], commit[..]);

    if (transport_send_wait(transport, &begin, 0x01, opt.timeout_ms, &response, &response_len) != 0) return -1;
    if (transport_send_wait(transport, &write, 0x06, opt.timeout_ms, &response, &response_len) != 0) return -1;
    sleep_millis(10);
    if (transport_send_wait(transport, &commit, 0x02, opt.timeout_ms, &response, &response_len) != 0) return -1;
    return 0;
}

fn int run_read_template(Options* opt) {
    ClockTransport transport;
    char[RECORD_SIZE] record;
    char[65] hash;
    char[65] stable_hash;

    if (transport_open(&transport, opt) != 0) return 1;
    int rc = read_template_from_device(opt, &transport, record[..]);
    transport_close(&transport);
    if (rc != 0) return 1;

    sha256_hex(record[..], &hash[0]);
    stable_sha256_hex(record[..], &stable_hash[0]);
    print_hex("template: ", record[..]);
    io::printfn("template sha256: %s", (String)hash[0:64]);
    io::printfn("template stable sha256: %s", (String)stable_hash[0:64]);
    print_time_fields("template time bytes", record[..]);
    String why = time_validation_error(record[..]);
    if (!str_eq(why, "ok")) io::printfn("template time validation warning: %s", why);
    return 0;
}

fn int run_update_time(Options* opt) {
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
        sha256_hex(current[..], &hash_current[0]);
        stable_sha256_hex(current[..], &stable_hash_current[0]);
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
        rc = verify_only_time_changed(current[..], patched[..], opt.debug);
    }
    if (rc == 0) {
        sha256_hex(patched[..], &hash_patched[0]);
        stable_sha256_hex(patched[..], &stable_hash_patched[0]);
        io::printfn("current template sha256: %s", (String)hash_current[0:64]);
        io::printfn("patched  template sha256: %s", (String)hash_patched[0:64]);
        io::printfn("current stable sha256: %s", (String)stable_hash_current[0:64]);
        io::printfn("patched  stable sha256: %s", (String)stable_hash_patched[0:64]);
        print_time_fields("old time bytes", current[..]);
        print_time_fields("new time bytes", patched[..]);
        if (opt.debug) {
            print_record_diff(current[..], patched[..]);
            print_update_packets(opt, patched[..]);
        }
        if (opt.dry_run) {
            io::printfn("Dry run: no config write sent.");
        } else if (confirm_write_prompt()) {
            rc = write_config_to_device(opt, &transport, patched[..]);
        } else {
            io::printfn("Aborted. No config write sent.");
            rc = 2;
        }
    }

    transport_close(&transport);
    if (rc != 0) return rc == 2 ? 2 : 1;
    io::printfn("Clock update command sequence completed.");
    return 0;
}

fn int get_feature_report(Options* opt) {
    ClockTransport transport;
    char[REPORT_SIZE] report;
    sz report_len = REPORT_SIZE;
    if (transport_open(&transport, opt) != 0) return 1;

    clear_bytes(report[..]);
    report[0] = DEFAULT_REPORT_ID;
    int rc = transport_get_feature_report(&transport, DEFAULT_REPORT_ID, &report, &report_len);
    transport_close(&transport);
    if (rc != 0) return 1;
    print_hex("feature response: ", report[0:report_len]);
    return 0;
}

fn int run_read_raw(Options* opt) {
    ClockTransport transport;
    char[RECORD_SIZE] record;

    if (transport_open(&transport, opt) != 0) return 1;
    int rc = read_template_from_device(opt, &transport, record[..]);
    transport_close(&transport);
    if (rc == 0) print_hex("raw template: ", record[..]);
    return rc == 0 ? 0 : 1;
}

fn bool confirm_write_prompt() {
    io::printf("Write updated clock config to the device? Type yes to continue: ");
    (void)io::stdout().flush();
    String? answer = io::treadline(io::stdin(), 16);
    if (catch err = answer) {
        io::printfn("Could not read confirmation: %s", err);
        return false;
    }
    return str_eq(answer, "yes");
}
