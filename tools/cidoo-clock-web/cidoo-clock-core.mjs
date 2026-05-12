export const REPORT_SIZE = 64;
export const RECORD_SIZE = 48;
export const CONFIG_PAYLOAD_OFFSET = 8;
export const TIME_OFFSET = 35;
export const TIME_SIZE = 7;
export const DEFAULTS = Object.freeze({
  vendorId: 0x320f,
  productId: 0x5055,
  usagePage: 0xff1c,
  usage: 0x92,
  reportId: 0x04,
  slot: 0,
  timeoutMs: 1500,
});

export class ProtocolError extends Error {
  constructor(message) {
    super(message);
    this.name = "ProtocolError";
  }
}

export function checksumPacket(packet) {
  assertLength(packet, REPORT_SIZE, "packet");

  let sum = 0;
  for (let i = 3; i < REPORT_SIZE; i++) {
    sum = (sum + packet[i]) & 0xffff;
  }
  packet[1] = sum & 0xff;
  packet[2] = (sum >> 8) & 0xff;
  return packet;
}

export function buildSimplePacket(reportId, command) {
  const packet = new Uint8Array(REPORT_SIZE);
  packet[0] = reportId & 0xff;
  packet[3] = command & 0xff;
  return checksumPacket(packet);
}

export function buildConfigPacket(reportId, command, payload, payloadLength, offset) {
  const packet = new Uint8Array(REPORT_SIZE);
  packet[0] = reportId & 0xff;
  packet[3] = command & 0xff;
  packet[4] = payloadLength & 0xff;
  packet[5] = offset & 0xff;
  packet[6] = (offset >> 8) & 0xff;
  packet[7] = 0;

  if (payload != null && payloadLength > 0) {
    const copyLength = Math.min(payloadLength, REPORT_SIZE - CONFIG_PAYLOAD_OFFSET);
    packet.set(payload.subarray(0, copyLength), CONFIG_PAYLOAD_OFFSET);
  }

  return checksumPacket(packet);
}

export function slotOffset(slot) {
  const normalized = Number(slot);
  if (!Number.isInteger(normalized) || normalized < 0) {
    throw new ProtocolError("Slot must be a non-negative integer.");
  }
  const offset = normalized * 0x31;
  if (offset > 0xffff - (RECORD_SIZE - 1)) {
    throw new ProtocolError("Slot is too large for the 16-bit config offset.");
  }
  return offset;
}

export function bcd(value) {
  if (!Number.isInteger(value) || value < 0 || value > 99) {
    throw new ProtocolError(`Cannot encode ${value} as BCD.`);
  }
  return ((Math.trunc(value / 10) << 4) | (value % 10)) & 0xff;
}

export function unbcd(value) {
  const hi = (value >> 4) & 0x0f;
  const lo = value & 0x0f;
  if (hi > 9 || lo > 9) {
    return -1;
  }
  return hi * 10 + lo;
}

export function fillTimeBytes(date) {
  if (!(date instanceof Date) || Number.isNaN(date.getTime())) {
    throw new ProtocolError("Invalid date.");
  }

  const year = date.getFullYear();
  if (year < 2000 || year > 2099) {
    throw new ProtocolError("Year must be between 2000 and 2099.");
  }

  return Uint8Array.from([
    bcd(date.getSeconds()),
    bcd(date.getMinutes()),
    bcd(date.getHours()),
    date.getDay() & 0xff,
    bcd(date.getDate()),
    bcd(date.getMonth() + 1),
    bcd(year % 100),
  ]);
}

export function patchTime(record, timeBytes) {
  assertLength(record, RECORD_SIZE, "record");
  assertLength(timeBytes, TIME_SIZE, "time bytes");

  const patched = new Uint8Array(record);
  patched.set(timeBytes, TIME_OFFSET);
  return patched;
}

export function verifyOnlyTimeChanged(before, after) {
  assertLength(before, RECORD_SIZE, "before record");
  assertLength(after, RECORD_SIZE, "after record");

  for (let i = 0; i < RECORD_SIZE; i++) {
    if ((i < TIME_OFFSET || i >= TIME_OFFSET + TIME_SIZE) && before[i] !== after[i]) {
      throw new ProtocolError(`Non-time byte changed at offset ${i}.`);
    }
  }
}

export function validateTimeFields(record) {
  assertLength(record, RECORD_SIZE, "record");

  const checks = [
    [35, 0, 59, "seconds byte 35 is not valid BCD 00..59"],
    [36, 0, 59, "minutes byte 36 is not valid BCD 00..59"],
    [37, 0, 23, "hours byte 37 is not valid BCD 00..23"],
    [39, 1, 31, "day byte 39 is not valid BCD 01..31"],
    [40, 1, 12, "month byte 40 is not valid BCD 01..12"],
    [41, 0, 99, "year byte 41 is not valid BCD 00..99"],
  ];

  for (const [offset, min, max, message] of checks) {
    const decoded = unbcd(record[offset]);
    if (decoded < min || decoded > max) {
      throw new ProtocolError(message);
    }
  }

  if (record[38] > 6) {
    throw new ProtocolError("weekday byte 38 is not in range 0..6");
  }
}

export function decodeTimeFields(record) {
  assertLength(record, RECORD_SIZE, "record");

  return {
    second: unbcd(record[35]),
    minute: unbcd(record[36]),
    hour: unbcd(record[37]),
    weekday: record[38],
    day: unbcd(record[39]),
    month: unbcd(record[40]),
    year: unbcd(record[41]),
  };
}

export function formatDecodedTime(record) {
  const value = decodeTimeFields(record);
  if (
    value.second < 0 ||
    value.minute < 0 ||
    value.hour < 0 ||
    value.day < 1 ||
    value.month < 1 ||
    value.year < 0 ||
    value.weekday > 6
  ) {
    return "invalid";
  }

  return `20${pad2(value.year)}-${pad2(value.month)}-${pad2(value.day)} ${pad2(value.hour)}:${pad2(value.minute)}:${pad2(value.second)} weekday=${value.weekday}`;
}

export function changedOffsets(before, after) {
  assertLength(before, RECORD_SIZE, "before record");
  assertLength(after, RECORD_SIZE, "after record");

  const changes = [];
  for (let i = 0; i < RECORD_SIZE; i++) {
    if (before[i] !== after[i]) {
      changes.push({
        offset: i,
        before: before[i],
        after: after[i],
        isTime: i >= TIME_OFFSET && i < TIME_OFFSET + TIME_SIZE,
      });
    }
  }
  return changes;
}

export function recordAllZero(record) {
  assertLength(record, RECORD_SIZE, "record");
  return record.every((value) => value === 0);
}

export function bytesEqual(a, b) {
  if (a.length !== b.length) {
    return false;
  }
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      return false;
    }
  }
  return true;
}

export function hexToBytes(text) {
  const digits = [];
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === "0" && (text[i + 1] === "x" || text[i + 1] === "X")) {
      i++;
      continue;
    }
    if (/[0-9a-fA-F]/.test(c)) {
      digits.push(c);
      continue;
    }
    if (/\s|:|,|-/.test(c)) {
      continue;
    }
    throw new ProtocolError(`Hex text contains non-hex character: ${c}`);
  }
  if (digits.length % 2 !== 0) {
    throw new ProtocolError("Hex text contains an odd number of digits.");
  }
  const bytes = new Uint8Array(digits.length / 2);
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = Number.parseInt(`${digits[i * 2]}${digits[i * 2 + 1]}`, 16);
  }
  return bytes;
}

export function bytesToHex(bytes, group = 1) {
  const parts = [];
  for (let i = 0; i < bytes.length; i++) {
    if (group > 0 && i > 0 && i % group === 0) {
      parts.push(" ");
    }
    parts.push(bytes[i].toString(16).padStart(2, "0"));
  }
  return parts.join("");
}

export function formatRecordHex(record) {
  assertLength(record, RECORD_SIZE, "record");

  const lines = [];
  for (let i = 0; i < RECORD_SIZE; i += 16) {
    const chunk = record.subarray(i, i + 16);
    lines.push(`${i.toString(16).padStart(4, "0")}: ${bytesToHex(chunk)}`);
  }
  return lines.join("\n");
}

export function parseLocalDateTime(value) {
  const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(value);
  if (match == null) {
    throw new ProtocolError("Custom time must be YYYY-MM-DDTHH:MM or YYYY-MM-DDTHH:MM:SS.");
  }

  const [, yearText, monthText, dayText, hourText, minuteText, secondText = "0"] = match;
  const year = Number(yearText);
  const month = Number(monthText);
  const day = Number(dayText);
  const hour = Number(hourText);
  const minute = Number(minuteText);
  const second = Number(secondText);
  const date = new Date(year, month - 1, day, hour, minute, second, 0);

  if (
    date.getFullYear() !== year ||
    date.getMonth() !== month - 1 ||
    date.getDate() !== day ||
    date.getHours() !== hour ||
    date.getMinutes() !== minute ||
    date.getSeconds() !== second
  ) {
    throw new ProtocolError("Custom time is not a valid local date/time.");
  }
  return date;
}

export async function sha256Hex(bytes) {
  if (!globalThis.crypto?.subtle) {
    throw new ProtocolError("Web Crypto SHA-256 is unavailable in this context.");
  }

  const digest = await globalThis.crypto.subtle.digest("SHA-256", bytes);
  return bytesToHex(new Uint8Array(digest), 0);
}

export async function stableSha256Hex(record) {
  assertLength(record, RECORD_SIZE, "record");

  const masked = new Uint8Array(record);
  masked.fill(0, TIME_OFFSET, TIME_OFFSET + TIME_SIZE);
  return sha256Hex(masked);
}

export function normalizeInputReport(reportId, dataView) {
  const data = new Uint8Array(dataView.buffer, dataView.byteOffset, dataView.byteLength);
  const normalized = new Uint8Array(Math.min(REPORT_SIZE, data.length + 1));
  normalized[0] = reportId & 0xff;
  normalized.set(data.subarray(0, REPORT_SIZE - 1), 1);
  return normalized;
}

export function outputReportData(packet) {
  assertLength(packet, REPORT_SIZE, "packet");
  return packet.subarray(1);
}

function assertLength(bytes, expected, label) {
  if (!(bytes instanceof Uint8Array) || bytes.length !== expected) {
    throw new ProtocolError(`${label} must be ${expected} bytes.`);
  }
}

function pad2(value) {
  return String(value).padStart(2, "0");
}
