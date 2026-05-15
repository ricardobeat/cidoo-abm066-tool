export async function loadCdooCore(url = new URL("./cdoo-core.wasm", import.meta.url)) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Could not load C3 WASM core: HTTP ${response.status}.`);
  }
  const bytes = await response.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(bytes, {});
  instance.exports._initialize?.();
  return new CdooCore(instance.exports);
}

class CdooCore {
  constructor(exports) {
    this.exports = exports;
  }

  reportSize() {
    return this.exports.cdoo_report_size();
  }

  recordSize() {
    return this.exports.cdoo_record_size();
  }

  timeOffset() {
    return this.exports.cdoo_time_offset();
  }

  timeSize() {
    return this.exports.cdoo_time_size();
  }

  defaultReportId() {
    return this.exports.cdoo_default_report_id();
  }

  defaultVid() {
    return this.exports.cdoo_default_vid();
  }

  defaultPid() {
    return this.exports.cdoo_default_pid();
  }

  defaultUsagePage() {
    return this.exports.cdoo_default_usage_page();
  }

  defaultUsage() {
    return this.exports.cdoo_default_usage();
  }

  defaultImageChunkSize() {
    return this.exports.cdoo_default_image_chunk_size();
  }

  defaultDeviceFrameBytes() {
    return this.exports.cdoo_default_device_frame_bytes();
  }

  maxTotalFrames() {
    return this.exports.cdoo_max_total_frames();
  }

  buildSimplePacket(reportId, command) {
    const out = this.alloc(this.reportSize());
    try {
      this.check(this.exports.cdoo_build_simple_packet(reportId, command, out), "build packet");
      return this.read(out, this.reportSize());
    } finally {
      this.free(out);
    }
  }

  buildConfigPacket(reportId, command, payload, payloadLength, offset) {
    const payloadPtr = this.copyIn(payload ?? new Uint8Array(0), payloadLength);
    const out = this.alloc(this.reportSize());
    try {
      this.check(
        this.exports.cdoo_build_config_packet(reportId, command, payloadPtr, payloadLength, offset, out),
        "build config packet",
      );
      return this.read(out, this.reportSize());
    } finally {
      this.free(payloadPtr);
      this.free(out);
    }
  }

  buildImagePacket(reportId, payload, payloadLength, offset) {
    const payloadPtr = this.copyIn(payload ?? new Uint8Array(0), payloadLength);
    const out = this.alloc(this.reportSize());
    try {
      this.check(
        this.exports.cdoo_build_image_packet(reportId, payloadPtr, payloadLength, offset, out),
        "build image packet",
      );
      return this.read(out, this.reportSize());
    } finally {
      this.free(payloadPtr);
      this.free(out);
    }
  }

  writeTimeBytes(date) {
    const out = this.alloc(this.timeSize());
    try {
      this.check(
        this.exports.cdoo_write_time_bytes(
          date.getSeconds(),
          date.getMinutes(),
          date.getHours(),
          date.getDay(),
          date.getDate(),
          date.getMonth() + 1,
          date.getFullYear(),
          out,
        ),
        "write time bytes",
      );
      return this.read(out, this.timeSize());
    } finally {
      this.free(out);
    }
  }

  patchTime(record, timeBytes) {
    const recordPtr = this.copyIn(record, this.recordSize());
    const timePtr = this.copyIn(timeBytes, this.timeSize());
    const out = this.alloc(this.recordSize());
    try {
      this.check(this.exports.cdoo_patch_time(recordPtr, timePtr, out), "patch time");
      return this.read(out, this.recordSize());
    } finally {
      this.free(recordPtr);
      this.free(timePtr);
      this.free(out);
    }
  }

  timeChangeViolationOffset(before, after) {
    const beforePtr = this.copyIn(before, this.recordSize());
    const afterPtr = this.copyIn(after, this.recordSize());
    try {
      return this.exports.cdoo_time_change_violation_offset(beforePtr, afterPtr);
    } finally {
      this.free(beforePtr);
      this.free(afterPtr);
    }
  }

  encodeLcd160x96Frame(rgba) {
    const rgbaPtr = this.copyIn(rgba, 96 * 160 * 4);
    const out = this.alloc(this.defaultDeviceFrameBytes());
    try {
      this.check(this.exports.cdoo_encode_lcd160x96_frame(rgbaPtr, out), "encode LCD frame");
      return this.read(out, this.defaultDeviceFrameBytes());
    } finally {
      this.free(rgbaPtr);
      this.free(out);
    }
  }

  patchImageConfig(record, activeSelector, bucket1Frames, bucket2Frames) {
    const recordPtr = this.copyIn(record, this.recordSize());
    const out = this.alloc(this.recordSize());
    try {
      this.check(
        this.exports.cdoo_patch_image_config(recordPtr, activeSelector, bucket1Frames, bucket2Frames, out),
        "patch image config",
      );
      return this.read(out, this.recordSize());
    } finally {
      this.free(recordPtr);
      this.free(out);
    }
  }

  imageConfigChangeViolationOffset(before, after) {
    const beforePtr = this.copyIn(before, this.recordSize());
    const afterPtr = this.copyIn(after, this.recordSize());
    try {
      return this.exports.cdoo_image_config_change_violation_offset(beforePtr, afterPtr);
    } finally {
      this.free(beforePtr);
      this.free(afterPtr);
    }
  }

  payloadPacketCount(payloadLength) {
    return this.exports.cdoo_payload_packet_count(payloadLength);
  }

  imagePrepareWindowsTimeoutMs(existingBucket1Frames, existingBucket2Frames, finalBucket1Frames, finalBucket2Frames) {
    return this.exports.cdoo_image_prepare_windows_timeout_ms(
      existingBucket1Frames,
      existingBucket2Frames,
      finalBucket1Frames,
      finalBucket2Frames,
    );
  }

  alloc(size) {
    const ptr = Number(this.exports.cdoo_alloc(size));
    if (ptr === 0) throw new Error(`C3 WASM allocation failed for ${size} bytes.`);
    return ptr;
  }

  free(ptr) {
    if (ptr !== 0) this.exports.cdoo_free(ptr);
  }

  copyIn(bytes, size = bytes.length) {
    if (size === 0) return 0;
    const ptr = this.alloc(size);
    const memory = this.memory();
    memory.fill(0, ptr, ptr + size);
    memory.set(bytes.subarray(0, Math.min(bytes.length, size)), ptr);
    return ptr;
  }

  read(ptr, size) {
    return new Uint8Array(this.memory().slice(ptr, ptr + size));
  }

  memory() {
    return new Uint8Array(this.exports.memory.buffer);
  }

  check(code, action) {
    if (code !== 0) throw new Error(`C3 core failed to ${action} (${code}).`);
  }
}
