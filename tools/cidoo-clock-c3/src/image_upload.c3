module cidoo_clock_c3;

import std::core::mem;
import std::io;

const int K_CF_URL_POSIX_PATH_STYLE = 0;
const uint K_CG_IMAGE_ALPHA_PREMULTIPLIED_LAST = 1;
const uint K_CG_BITMAP_BYTE_ORDER_32_BIG = 0x4000;
const int K_CG_INTERPOLATION_HIGH = 3;

alias CFURLRef = void*;
alias CGImageSourceRef = void*;
alias CGImageRef = void*;
alias CGColorSpaceRef = void*;
alias CGContextRef = void*;

struct CGPoint {
    double x;
    double y;
}

struct CGSize {
    double width;
    double height;
}

struct CGRect {
    CGPoint origin;
    CGSize size;
}

struct ImageUploadPlan {
    char* payload;
    sz payload_len;
    uint payload_offset;
    sz bucket1_frames;
    sz bucket2_frames;
    sz existing_bucket1_frames;
    sz existing_bucket2_frames;
    sz final_bucket1_frames;
    sz final_bucket2_frames;
    uint final_active_selector;
}

extern fn CFURLRef cf_url_create_with_file_system_path(CFAllocatorRef allocator, CFStringRef file_path, int path_style, bool is_directory) @cname("CFURLCreateWithFileSystemPath");
extern fn CGImageSourceRef cg_image_source_create_with_url(CFURLRef url, void* options) @cname("CGImageSourceCreateWithURL");
extern fn sz cg_image_source_get_count(CGImageSourceRef source) @cname("CGImageSourceGetCount");
extern fn CGImageRef cg_image_source_create_image_at_index(CGImageSourceRef source, sz index, void* options) @cname("CGImageSourceCreateImageAtIndex");
extern fn sz cg_image_get_width(CGImageRef image) @cname("CGImageGetWidth");
extern fn sz cg_image_get_height(CGImageRef image) @cname("CGImageGetHeight");
extern fn CGColorSpaceRef cg_color_space_create_device_rgb() @cname("CGColorSpaceCreateDeviceRGB");
extern fn void cg_color_space_release(CGColorSpaceRef color_space) @cname("CGColorSpaceRelease");
extern fn CGContextRef cg_bitmap_context_create(void* data, sz width, sz height, sz bits_per_component, sz bytes_per_row, CGColorSpaceRef color_space, uint bitmap_info) @cname("CGBitmapContextCreate");
extern fn void cg_context_release(CGContextRef context) @cname("CGContextRelease");
extern fn void cg_context_set_rgb_fill_color(CGContextRef context, double red, double green, double blue, double alpha) @cname("CGContextSetRGBFillColor");
extern fn void cg_context_fill_rect(CGContextRef context, CGRect rect) @cname("CGContextFillRect");
extern fn void cg_context_set_interpolation_quality(CGContextRef context, int quality) @cname("CGContextSetInterpolationQuality");
extern fn void cg_context_draw_image(CGContextRef context, CGRect rect, CGImageRef image) @cname("CGContextDrawImage");
extern fn void cg_image_release(CGImageRef image) @cname("CGImageRelease");

fn int run_upload_image(Options* opt) {
    ImageUploadPlan plan;
    ClockTransport transport;
    char[RECORD_SIZE] current;
    char[RECORD_SIZE] patched;
    char[65] stable_hash_current;
    int rc = 1;
    bool hid_open = false;

    if (build_image_upload_plan(opt, &plan) != 0) return 1;

    if (transport_open(&transport, opt) == 0) {
        hid_open = true;
        if (read_template_from_device(opt, &transport, current[..]) == 0) {
            stable_sha256_hex(current[..], &stable_hash_current[0]);
            if (opt.debug) io::printfn("current template stable sha256: %s", (String)stable_hash_current[0:64]);
            if (finalize_image_upload_plan(current[..], &plan) == 0 &&
                patch_image_config(current[..], &plan, patched[..]) == 0) {
                print_image_upload_summary(opt, current[..], patched[..], &plan);
                if (opt.dry_run) {
                    io::printfn("Dry run: no config write or image stream sent.");
                    rc = 0;
                } else if (!confirm_image_upload_prompt()) {
                    io::printfn("Aborted. No config write or image stream sent.");
                    rc = 2;
                } else if (write_config_to_device(opt, &transport, patched[..]) == 0 &&
                           verify_image_config_readback(opt, &transport, patched[..]) == 0 &&
                           write_image_payload_to_device(opt, &transport, &plan) == 0) {
                    io::printfn("Two-image upload command sequence completed.");
                    rc = 0;
                }
            }
        }
    }
    if (hid_open) transport_close(&transport);
    free_image_upload_plan(&plan);
    return rc == 2 ? 2 : (rc == 0 ? 0 : 1);
}

fn int build_image_upload_plan(Options* opt, ImageUploadPlan* plan) {
    *plan = {};
    CGImageSourceRef source1 = create_image_source_for_path(opt.image1_path);
    if (source1 == null) return -1;
    CGImageSourceRef source2 = create_image_source_for_path(opt.image2_path);
    if (source2 == null) {
        cf_release(source1);
        return -1;
    }

    sz count1 = cg_image_source_get_count(source1);
    sz count2 = cg_image_source_get_count(source2);
    sz total = count1 + count2;
    if (count1 == 0) {
        io::printfn("GIF1 has no decodable frames: %s", opt.image1_path);
        cf_release(source2);
        cf_release(source1);
        return -1;
    }
    if (count2 == 0) {
        io::printfn("GIF2 has no decodable frames: %s", opt.image2_path);
        cf_release(source2);
        cf_release(source1);
        return -1;
    }
    if (total > MAX_TOTAL_FRAMES) {
        io::printfn("Refusing %d final frame(s); safe hard limit is %d frames total", total, MAX_TOTAL_FRAMES);
        cf_release(source2);
        cf_release(source1);
        return -1;
    }

    plan.bucket1_frames = count1;
    plan.bucket2_frames = count2;
    plan.payload_len = total * DEFAULT_DEVICE_FRAME_BYTES;
    plan.payload = mem::new_array(char, plan.payload_len);
    clear_pointer_bytes(plan.payload, plan.payload_len);

    int rc = 0;
    if (encode_image_source_frames(source1, "GIF1", count1, plan.payload, 0) != 0) rc = -1;
    if (rc == 0 && encode_image_source_frames(source2, "GIF2", count2, plan.payload, count1) != 0) rc = -1;

    cf_release(source2);
    cf_release(source1);

    if (rc != 0) free_image_upload_plan(plan);
    return rc;
}

fn void free_image_upload_plan(ImageUploadPlan* plan) {
    if (plan.payload != null) mem::free(plan.payload);
    *plan = {};
}

fn CGImageSourceRef create_image_source_for_path(String path) {
    CFStringRef path_string = cf_string_create_with_cstring(null, path.ptr, K_CF_STRING_ENCODING_UTF8);
    if (path_string == null) {
        io::printfn("Could not create path string for %s", path);
        return null;
    }
    CFURLRef url = cf_url_create_with_file_system_path(null, path_string, K_CF_URL_POSIX_PATH_STYLE, false);
    cf_release(path_string);
    if (url == null) {
        io::printfn("Could not create file URL for %s", path);
        return null;
    }
    CGImageSourceRef source = cg_image_source_create_with_url(url, null);
    cf_release(url);
    if (source == null) io::printfn("Could not open image/GIF: %s", path);
    return source;
}

fn int encode_image_source_frames(CGImageSourceRef source, String label, sz frame_count, char* payload, sz start_frame) {
    for (sz i = 0; i < frame_count; i++) {
        char* logical_rgba = null;
        if (load_image_frame_rgba(source, i, label, &logical_rgba) != 0) return -1;
        char* frame = payload + (start_frame + i) * DEFAULT_DEVICE_FRAME_BYTES;
        encode_lcd160x96_frame(logical_rgba, frame);
        mem::free(logical_rgba);
    }
    io::printfn("%s frames: %d", label, frame_count);
    return 0;
}

fn int load_image_frame_rgba(CGImageSourceRef source, sz frame_index, String label, char** out_rgba) {
    CGImageRef image = cg_image_source_create_image_at_index(source, frame_index, null);
    if (image == null) {
        io::printfn("Could not decode %s frame %d", label, frame_index);
        return -1;
    }
    int rc = render_cgimage_rgba(image, DEVICE_LCD_WIDTH, DEVICE_LCD_HEIGHT, out_rgba);
    cg_image_release(image);
    return rc;
}

fn int render_cgimage_rgba(CGImageRef image, int canvas_w, int canvas_h, char** out_rgba) {
    sz src_w = cg_image_get_width(image);
    sz src_h = cg_image_get_height(image);
    if (src_w == 0 || src_h == 0) {
        io::printfn("Decoded image frame has invalid dimensions");
        return -1;
    }

    sz byte_count = (sz)canvas_w * (sz)canvas_h * 4;
    char* rgba = mem::new_array(char, byte_count);
    clear_pointer_bytes(rgba, byte_count);

    CGColorSpaceRef color_space = cg_color_space_create_device_rgb();
    if (color_space == null) {
        io::printfn("Could not create RGB color space");
        mem::free(rgba);
        return -1;
    }

    CGContextRef context = cg_bitmap_context_create(
        rgba,
        (sz)canvas_w,
        (sz)canvas_h,
        8,
        (sz)canvas_w * 4,
        color_space,
        K_CG_IMAGE_ALPHA_PREMULTIPLIED_LAST | K_CG_BITMAP_BYTE_ORDER_32_BIG);
    if (context == null) {
        io::printfn("Could not create bitmap context");
        cg_color_space_release(color_space);
        mem::free(rgba);
        return -1;
    }

    cg_context_set_rgb_fill_color(context, 0.0, 0.0, 0.0, 1.0);
    cg_context_fill_rect(context, make_rect(0.0, 0.0, (double)canvas_w, (double)canvas_h));
    cg_context_set_interpolation_quality(context, K_CG_INTERPOLATION_HIGH);
    cg_context_draw_image(context, make_rect(0.0, 0.0, (double)canvas_w, (double)canvas_h), image);

    cg_context_release(context);
    cg_color_space_release(color_space);
    *out_rgba = rgba;
    return 0;
}

fn CGRect make_rect(double x, double y, double width, double height) {
    CGRect rect = {};
    rect.origin.x = x;
    rect.origin.y = y;
    rect.size.width = width;
    rect.size.height = height;
    return rect;
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

fn int finalize_image_upload_plan(char[] current, ImageUploadPlan* plan) {
    plan.existing_bucket1_frames = (sz)(((uint)current[BUCKET1_COUNT_OFFSET]) & 0xff);
    plan.existing_bucket2_frames = (sz)(((uint)current[BUCKET2_COUNT_OFFSET]) & 0xff);
    plan.final_bucket1_frames = plan.bucket1_frames;
    plan.final_bucket2_frames = plan.bucket2_frames;
    plan.final_active_selector = ((uint)current[ACTIVE_BUCKET_OFFSET]) & 0xff;
    plan.payload_offset = 0;

    sz total = plan.final_bucket1_frames + plan.final_bucket2_frames;
    if (total == 0 || total > MAX_TOTAL_FRAMES) {
        io::printfn("Refusing %d final frame(s); safe hard limit is %d frames total", total, MAX_TOTAL_FRAMES);
        return -1;
    }
    return 0;
}

fn int patch_image_config(char[] before, ImageUploadPlan* plan, char[] after) {
    for (sz i = 0; i < RECORD_SIZE; i++) after[i] = before[i];
    after[ACTIVE_BUCKET_OFFSET] = (char)plan.final_active_selector;
    after[BUCKET1_COUNT_OFFSET] = (char)plan.final_bucket1_frames;
    after[BUCKET2_COUNT_OFFSET] = (char)plan.final_bucket2_frames;
    return verify_only_image_metadata_changed(before, after);
}

fn int verify_only_image_metadata_changed(char[] before, char[] after) {
    for (sz i = 0; i < RECORD_SIZE; i++) {
        bool allowed = i == ACTIVE_BUCKET_OFFSET || i == BUCKET1_COUNT_OFFSET || i == BUCKET2_COUNT_OFFSET;
        if (!allowed && before[i] != after[i]) {
            io::printfn("Refusing: unexpected config byte changed at offset %d", i);
            return -1;
        }
    }
    return 0;
}

fn void print_image_upload_summary(Options* opt, char[] before, char[] after, ImageUploadPlan* plan) {
    sz packet_count = payload_packet_count(plan.payload_len);
    io::printfn("active selector byte after upload: %d", plan.final_active_selector);
    io::printfn("clock bytes: preserved");
    io::printfn("device layout: lcd160x96");
    io::printfn("device logical frame: %dx%d", DEVICE_LCD_WIDTH, DEVICE_LCD_HEIGHT);
    io::printfn("device memory frame: %dx%d", DEVICE_LCD_MEMORY_WIDTH, DEVICE_LCD_MEMORY_HEIGHT);
    io::printfn("device rgb565 bytes per frame: %d", DEVICE_LCD_MEMORY_WIDTH * DEVICE_LCD_MEMORY_HEIGHT * 2);
    io::printfn("device row bytes: %d", DEFAULT_DEVICE_ROW_BYTES);
    io::printfn("device bytes per frame: %d", DEFAULT_DEVICE_FRAME_BYTES);
    io::printfn("chunk bytes: %d", DEFAULT_IMAGE_CHUNK_SIZE);
    io::printfn("final GIF1 frame count: %d", plan.final_bucket1_frames);
    io::printfn("final GIF2 frame count: %d", plan.final_bucket2_frames);
    io::printfn("final total frame count: %d", plan.final_bucket1_frames + plan.final_bucket2_frames);
    io::printfn("cmd 0x23 image prepare frame basis: %d", image_prepare_frame_count(plan));
    io::printfn("cmd 0x23 Windows prepare timeout formula: %d ms", image_prepare_windows_timeout_ms(plan));
    io::printfn("cmd 0x23 local prepare wait: %d ms", image_prepare_wait_ms(opt));
    io::printfn("payload start byte offset: %d", plan.payload_offset);
    io::printfn("payload bytes sent: %d", plan.payload_len);
    io::printfn("image packet count: %d", packet_count);

    if (opt.debug) {
        char[65] payload_hash;
        sha256_hex(plan.payload[0:plan.payload_len], &payload_hash[0]);
        io::printfn("payload sha256: %s", (String)payload_hash[0:64]);
        print_image_config_diff(before, after);
        print_update_packets(opt, after);
        print_image_packet_samples(plan);
    }
}

fn void print_image_config_diff(char[] before, char[] after) {
    bool found = false;
    io::printfn("Changed config offsets:");
    for (sz i = 0; i < RECORD_SIZE; i++) {
        if (before[i] != after[i]) {
            found = true;
            io::printfn("  %02d: %02x -> %02x  %s",
                i,
                (uint)before[i],
                (uint)after[i],
                config_offset_label(i));
        }
    }
    if (!found) io::printfn("  none");
}

fn String config_offset_label(sz offset) {
    if (offset == ACTIVE_BUCKET_OFFSET) return "active selector";
    if (offset == BUCKET1_COUNT_OFFSET) return "GIF1 frame count";
    if (offset == BUCKET2_COUNT_OFFSET) return "GIF2 frame count";
    return "unexpected";
}

fn void print_image_packet_samples(ImageUploadPlan* plan) {
    char[REPORT_SIZE] prepare;
    char[REPORT_SIZE] first;
    char[REPORT_SIZE] last;
    char[REPORT_SIZE] commit;
    sz packet_count = payload_packet_count(plan.payload_len);
    sz last_offset = (packet_count - 1) * DEFAULT_IMAGE_CHUNK_SIZE;
    sz last_len = plan.payload_len - last_offset;
    if (last_len > DEFAULT_IMAGE_CHUNK_SIZE) last_len = DEFAULT_IMAGE_CHUNK_SIZE;

    build_simple_packet(DEFAULT_REPORT_ID, 0x23, prepare[..]);
    build_image_packet(DEFAULT_REPORT_ID, plan.payload, DEFAULT_IMAGE_CHUNK_SIZE, plan.payload_offset, first[..]);
    build_image_packet(DEFAULT_REPORT_ID, plan.payload + last_offset, last_len, plan.payload_offset + (uint)last_offset, last[..]);
    build_simple_packet(DEFAULT_REPORT_ID, 0x02, commit[..]);

    print_hex("first 32 rgb565 bytes: ", plan.payload[0:32]);
    print_hex("cmd 0x23 image prepare packet: ", prepare[..]);
    print_hex("first cmd 0x21 image packet: ", first[..]);
    print_hex("last  cmd 0x21 image packet: ", last[..]);
    print_hex("cmd 0x02 image commit packet: ", commit[..]);
}

fn int verify_image_config_readback(Options* opt, ClockTransport* transport, char[] expected) {
    char[RECORD_SIZE] actual;
    char[65] expected_stable;
    char[65] actual_stable;
    if (read_template_from_device(opt, transport, actual[..]) != 0) {
        io::printfn("Refusing image stream: could not read back config after metadata write");
        return -1;
    }
    stable_sha256_hex(expected, &expected_stable[0]);
    stable_sha256_hex(actual[..], &actual_stable[0]);
    if (!bytes_equal(expected_stable[0:64], actual_stable[0:64])) {
        io::printfn("Refusing image stream: config readback stable sha256 %s does not match patched stable sha256 %s",
            (String)actual_stable[0:64],
            (String)expected_stable[0:64]);
        print_image_config_diff(expected, actual[..]);
        return -1;
    }
    if (opt.debug) {
        io::printfn("verified config readback stable sha256: %s", (String)actual_stable[0:64]);
    } else {
        io::printfn("verified config readback");
    }
    return 0;
}

fn int write_image_payload_to_device(Options* opt, ClockTransport* transport, ImageUploadPlan* plan) {
    char[REPORT_SIZE] packet;
    char[REPORT_SIZE] response;
    sz response_len = 0;
    sz packet_count = payload_packet_count(plan.payload_len);
    int prepare_timeout_ms = image_prepare_wait_ms(opt);
    int windows_prepare_timeout_ms = image_prepare_windows_timeout_ms(plan);
    bool prepare_timed_out = false;

    build_simple_packet(DEFAULT_REPORT_ID, 0x23, packet[..]);
    io::printfn("waiting up to %d ms for cmd 0x23 image prepare", prepare_timeout_ms);
    if (transport_send_wait_ignoring_response_error(
            transport,
            &packet,
            0x23,
            prepare_timeout_ms,
            &response,
            &response_len,
            opt.debug,
            &prepare_timed_out) != 0) {
        return -1;
    }
    if (prepare_timed_out && windows_prepare_timeout_ms > prepare_timeout_ms) {
        int settle_ms = windows_prepare_timeout_ms - prepare_timeout_ms;
        io::printfn("cmd 0x23 did not respond in %d ms; waiting %d ms for device prepare", prepare_timeout_ms, settle_ms);
        sleep_millis((long)settle_ms);
    }

    for (sz i = 0; i < packet_count; i++) {
        sz offset = i * DEFAULT_IMAGE_CHUNK_SIZE;
        sz chunk_len = plan.payload_len - offset;
        if (chunk_len > DEFAULT_IMAGE_CHUNK_SIZE) chunk_len = DEFAULT_IMAGE_CHUNK_SIZE;
        uint absolute_offset = plan.payload_offset + (uint)offset;
        build_image_packet(DEFAULT_REPORT_ID, plan.payload + offset, chunk_len, absolute_offset, packet[..]);
        if (transport_send_wait(transport, &packet, 0x21, opt.timeout_ms, &response, &response_len) != 0) return -1;
        if ((i + 1) % 100 == 0 || i + 1 == packet_count) {
            io::printfn("uploaded image packets: %d/%d", i + 1, packet_count);
            (void)io::stdout().flush();
        }
    }

    sleep_millis(10);
    build_simple_packet(DEFAULT_REPORT_ID, 0x02, packet[..]);
    if (transport_send_wait(transport, &packet, 0x02, opt.timeout_ms, &response, &response_len) != 0) return -1;
    return 0;
}

fn sz payload_packet_count(sz payload_len) {
    return (payload_len + DEFAULT_IMAGE_CHUNK_SIZE - 1) / DEFAULT_IMAGE_CHUNK_SIZE;
}

fn sz image_prepare_frame_count(ImageUploadPlan* plan) {
    sz existing_total = plan.existing_bucket1_frames + plan.existing_bucket2_frames;
    sz final_total = plan.final_bucket1_frames + plan.final_bucket2_frames;
    return existing_total > final_total ? existing_total : final_total;
}

fn int image_prepare_windows_timeout_ms(ImageUploadPlan* plan) {
    return (int)(image_prepare_frame_count(plan) * IMAGE_PREPARE_PER_FRAME_TIMEOUT_MS + IMAGE_PREPARE_BASE_TIMEOUT_MS);
}

fn int image_prepare_wait_ms(Options* opt) {
    return opt.timeout_ms > IMAGE_PREPARE_WAIT_MS ? opt.timeout_ms : IMAGE_PREPARE_WAIT_MS;
}

fn void clear_pointer_bytes(char* bytes, sz len) {
    for (sz i = 0; i < len; i++) bytes[i] = 0;
}

fn bool confirm_image_upload_prompt() {
    io::printf("Upload images to the device? Type yes to continue: ");
    (void)io::stdout().flush();
    String? answer = io::treadline(io::stdin(), 16);
    if (catch err = answer) {
        io::printfn("Could not read confirmation: %s", err);
        return false;
    }
    return str_eq(answer, "yes");
}
