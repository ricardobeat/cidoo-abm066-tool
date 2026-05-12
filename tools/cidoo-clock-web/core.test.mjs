import assert from "node:assert/strict";
import test from "node:test";
import {
  RECORD_SIZE,
  bcd,
  buildConfigPacket,
  buildSimplePacket,
  bytesToHex,
  changedOffsets,
  fillTimeBytes,
  formatDecodedTime,
  hexToBytes,
  patchTime,
  slotOffset,
  validateTimeFields,
  verifyOnlyTimeChanged,
} from "./cidoo-clock-core.mjs";

const template = hexToBytes(`
  00 0c 05 01 00 00 c8 00 ff 23 00 00 00 00 00 00
  00 00 00 00 ff 00 00 00 00 00 00 ff 03 00 00 00
  0f 00 14 00 49 18 04 01 08 24 00 64 00 00 00 0f
`);

test("builds protocol packets with the same checksum layout as the C tool", () => {
  const begin = buildSimplePacket(0x04, 0x01);
  assert.equal(bytesToHex(begin.subarray(0, 8)), "04 01 00 01 00 00 00 00");

  const read = buildConfigPacket(0x04, 0x05, null, 0x30, 0);
  assert.equal(bytesToHex(read.subarray(0, 8)), "04 35 00 05 30 00 00 00");
});

test("encodes local clock bytes as packed BCD", () => {
  const bytes = fillTimeBytes(new Date(2026, 4, 11, 15, 4, 9));
  assert.equal(bytesToHex(bytes), "09 04 15 01 11 05 26");
  assert.equal(bcd(59), 0x59);
});

test("patches only time offsets", () => {
  assert.equal(template.length, RECORD_SIZE);
  validateTimeFields(template);

  const bytes = fillTimeBytes(new Date(2026, 4, 11, 15, 4, 9));
  const patched = patchTime(template, bytes);
  verifyOnlyTimeChanged(template, patched);

  assert.equal(formatDecodedTime(patched), "2026-05-11 15:04:09 weekday=1");
  assert.deepEqual(
    changedOffsets(template, patched).map((change) => change.offset),
    [35, 36, 37, 38, 39, 40, 41],
  );
});

test("rejects slot offsets that cannot contain a full record", () => {
  assert.equal(slotOffset(0), 0);
  assert.equal(slotOffset(1), 0x31);
  assert.throws(() => slotOffset(2000), /Slot is too large/);
});
