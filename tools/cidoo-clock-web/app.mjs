import {
  DEFAULTS,
  REPORT_SIZE,
  RECORD_SIZE,
  CONFIG_PAYLOAD_OFFSET,
  buildConfigPacket,
  buildSimplePacket,
  bytesEqual,
  bytesToHex,
  changedOffsets,
  fillTimeBytes,
  formatDecodedTime,
  formatRecordHex,
  normalizeInputReport,
  outputReportData,
  parseLocalDateTime,
  patchTime,
  recordAllZero,
  sha256Hex,
  slotOffset,
  stableSha256Hex,
  validateTimeFields,
  verifyOnlyTimeChanged,
} from "./cidoo-clock-core.mjs";

const state = {
  device: null,
  record: null,
  templateSha256: null,
  stableSha256: null,
};

const els = {
  connect: document.querySelector("#connect"),
  controls: document.querySelector("#controls"),
  disconnect: document.querySelector("#disconnect"),
  read: document.querySelector("#read-template"),
  update: document.querySelector("#update-clock"),
  clearLog: document.querySelector("#clear-log"),
  status: document.querySelector("#status"),
  deviceName: document.querySelector("#device-name"),
  recordHex: document.querySelector("#record-hex"),
  diff: document.querySelector("#diff"),
  log: document.querySelector("#log"),
  templateHash: document.querySelector("#template-hash"),
  stableHash: document.querySelector("#stable-hash"),
  decodedTime: document.querySelector("#decoded-time"),
  pendingTime: document.querySelector("#pending-time"),
  support: document.querySelector("#support"),
  writeConfirm: document.querySelector("#write-confirm"),
  nowMode: document.querySelector("#time-now"),
  customMode: document.querySelector("#time-custom"),
  customTime: document.querySelector("#custom-time"),
  vid: document.querySelector("#vid"),
  pid: document.querySelector("#pid"),
  usagePage: document.querySelector("#usage-page"),
  usage: document.querySelector("#usage"),
  reportId: document.querySelector("#report-id"),
  slot: document.querySelector("#slot"),
  timeoutMs: document.querySelector("#timeout-ms"),
};

init();

function init() {
  els.vid.value = prefixedHex(DEFAULTS.vendorId, 4);
  els.pid.value = prefixedHex(DEFAULTS.productId, 4);
  els.usagePage.value = prefixedHex(DEFAULTS.usagePage, 4);
  els.usage.value = prefixedHex(DEFAULTS.usage, 2);
  els.reportId.value = prefixedHex(DEFAULTS.reportId, 2);
  els.slot.value = String(DEFAULTS.slot);
  els.timeoutMs.value = String(DEFAULTS.timeoutMs);
  els.customTime.value = toDateTimeLocalValue(new Date());

  els.connect.addEventListener("click", connect);
  els.disconnect.addEventListener("click", disconnect);
  els.read.addEventListener("click", readTemplate);
  els.update.addEventListener("click", updateClock);
  els.clearLog.addEventListener("click", () => {
    els.log.textContent = "";
  });
  els.writeConfirm.addEventListener("change", updateButtonState);
  els.nowMode.addEventListener("change", renderPendingTime);
  els.customMode.addEventListener("change", renderPendingTime);
  els.customTime.addEventListener("input", renderPendingTime);
  els.controls.addEventListener("submit", (event) => {
    event.preventDefault();
  });

  if (!("hid" in navigator)) {
    els.support.textContent = "WebHID unavailable. Use Chrome or Edge over localhost/HTTPS.";
    setStatus("WebHID unavailable");
    setConnected(false);
  } else {
    els.support.textContent = "WebHID ready";
    navigator.hid.addEventListener("disconnect", (event) => {
      if (state.device === event.device) {
        state.device = null;
        state.record = null;
        renderRecord(null);
        setConnected(false);
        setStatus("Device disconnected");
      }
    });
    setConnected(false);
    setStatus("Disconnected");
  }

  renderPendingTime();
  updateButtonState();
}

async function connect() {
  try {
    requireWebHid();
    const options = readOptions();
    const devices = await navigator.hid.requestDevice({
      filters: [
        {
          vendorId: options.vendorId,
          productId: options.productId,
          usagePage: options.usagePage,
          usage: options.usage,
        },
      ],
    });

    if (devices.length === 0) {
      setStatus("No device selected");
      return;
    }

    if (state.device?.opened) {
      await state.device.close();
    }

    state.device = devices[0];
    await state.device.open();
    state.record = null;
    renderRecord(null);
    setConnected(true);
    setStatus("Connected");
    logLine(`connected: ${deviceLabel(state.device)}`);
  } catch (error) {
    reportError(error);
  }
}

async function disconnect() {
  try {
    if (state.device?.opened) {
      await state.device.close();
    }
    state.device = null;
    state.record = null;
    renderRecord(null);
    setConnected(false);
    setStatus("Disconnected");
  } catch (error) {
    reportError(error);
  }
}

async function readTemplate() {
  try {
    const options = readOptions();
    requireOpenDevice();
    setStatus("Reading record");
    state.record = await readTemplateFromDevice(options);
    state.templateSha256 = await sha256Hex(state.record);
    state.stableSha256 = await stableSha256Hex(state.record);
    renderRecord(state.record);
    setStatus("Record read");
    logLine(`template sha256: ${state.templateSha256}`);
    logLine(`template stable sha256: ${state.stableSha256}`);
  } catch (error) {
    reportError(error);
  }
}

async function updateClock() {
  try {
    const options = readOptions();
    requireOpenDevice();

    if (state.stableSha256 == null) {
      throw new Error("Read the current record before updating.");
    }
    if (!els.writeConfirm.checked) {
      throw new Error("Enable the write confirmation before updating.");
    }

    setStatus("Re-reading record");
    const current = await readTemplateFromDevice(options);
    const currentStableHash = await stableSha256Hex(current);
    if (currentStableHash.toLowerCase() !== state.stableSha256.toLowerCase()) {
      throw new Error(`Stable hash changed: current ${currentStableHash}, expected ${state.stableSha256}.`);
    }

    validateTimeFields(current);
    const timeBytes = fillTimeBytes(selectedDate());
    const patched = patchTime(current, timeBytes);
    verifyOnlyTimeChanged(current, patched);

    const patchedStableHash = await stableSha256Hex(patched);
    if (patchedStableHash.toLowerCase() !== currentStableHash.toLowerCase()) {
      throw new Error("Patched stable hash changed.");
    }

    renderDiff(current, patched);
    setStatus("Writing clock");
    await writeTimeToDevice(options, patched);

    state.record = patched;
    state.templateSha256 = await sha256Hex(patched);
    state.stableSha256 = patchedStableHash;
    renderRecord(patched);
    els.writeConfirm.checked = false;
    updateButtonState();
    setStatus("Clock update complete");
    logLine("clock update command sequence completed");
  } catch (error) {
    reportError(error);
  }
}

async function readTemplateFromDevice(options) {
  const offset = slotOffset(options.slot);
  const record = new Uint8Array(RECORD_SIZE);

  for (let pos = 0; pos < RECORD_SIZE; pos += 4) {
    const chunkLength = Math.min(4, RECORD_SIZE - pos);
    const packet = buildConfigPacket(
      options.reportId,
      0x05,
      null,
      chunkLength,
      offset + pos,
    );
    const response = await sendPacketWait(options, packet, 0x05, { checkStatus: false });

    if (response.length === REPORT_SIZE && bytesEqual(response, packet)) {
      throw new Error("Refusing echoed read response from the wrong HID collection.");
    }
    if (response.length < 4 + chunkLength) {
      throw new Error(
        `Read response too short at offset 0x${(offset + pos).toString(16)}: got ${response.length}, need ${4 + chunkLength}.`,
      );
    }

    record.set(response.subarray(4, 4 + chunkLength), pos);
  }

  if (recordAllZero(record)) {
    throw new Error("Refusing all-zero template.");
  }
  return record;
}

async function writeTimeToDevice(options, patched) {
  const begin = buildSimplePacket(options.reportId, 0x01);
  const write = buildConfigPacket(options.reportId, 0x06, patched, RECORD_SIZE, slotOffset(options.slot));
  const commit = buildSimplePacket(options.reportId, 0x02);

  logLine(`cmd 0x01 packet: ${bytesToHex(begin)}`);
  await sendPacketWait(options, begin, 0x01);

  logLine(`cmd 0x06 packet: ${bytesToHex(write)}`);
  await sendPacketWait(options, write, 0x06);

  await sleep(10);
  logLine(`cmd 0x02 packet: ${bytesToHex(commit)}`);
  await sendPacketWait(options, commit, 0x02);
}

async function sendPacketWait(options, packet, expectedCommand, { checkStatus = true } = {}) {
  const device = requireOpenDevice();

  return new Promise((resolve, reject) => {
    let settled = false;
    const timeout = window.setTimeout(() => {
      settle(() => reject(new Error(`Timed out waiting for response to cmd=0x${hex(expectedCommand, 2)}.`)));
    }, options.timeoutMs);

    const onInputReport = (event) => {
      if (event.device !== device || event.reportId !== options.reportId) {
        return;
      }

      const response = normalizeInputReport(event.reportId, event.data);
      if (response.length < 4 || response[3] !== expectedCommand) {
        return;
      }

      if (checkStatus && response.length >= CONFIG_PAYLOAD_OFFSET) {
        if (response[7] === 0xff) {
          settle(() => reject(new Error(`Device returned status 0xff for cmd=0x${hex(expectedCommand, 2)}.`)));
          return;
        }
        if (response[7] === 0xfe) {
          settle(() => reject(new Error(`Device returned status 0xfe for cmd=0x${hex(expectedCommand, 2)}.`)));
          return;
        }
      }

      settle(() => resolve(response));
    };

    const settle = (finish) => {
      if (settled) {
        return;
      }
      settled = true;
      window.clearTimeout(timeout);
      device.removeEventListener("inputreport", onInputReport);
      finish();
    };

    device.addEventListener("inputreport", onInputReport);
    try {
      device.sendReport(options.reportId, outputReportData(packet)).catch((error) => {
        settle(() => reject(error));
      });
    } catch (error) {
      settle(() => reject(error));
    }
  });
}

function selectedDate() {
  if (els.nowMode.checked) {
    return new Date();
  }
  return parseLocalDateTime(els.customTime.value);
}

function readOptions() {
  const options = {
    vendorId: parseHexInput(els.vid.value, "VID", 0xffff),
    productId: parseHexInput(els.pid.value, "PID", 0xffff),
    usagePage: parseHexInput(els.usagePage.value, "usage page", 0xffff),
    usage: parseHexInput(els.usage.value, "usage", 0xffff),
    reportId: parseHexInput(els.reportId.value, "report ID", 0xff),
    slot: parseIntInput(els.slot.value, "slot", 0, 1024),
    timeoutMs: parseIntInput(els.timeoutMs.value, "timeout", 100, 30000),
  };
  slotOffset(options.slot);
  return options;
}

function parseHexInput(value, label, max) {
  const trimmed = value.trim();
  let digits = trimmed;
  let radix = 10;
  if (/^0x/i.test(trimmed)) {
    digits = trimmed.slice(2);
    radix = 16;
  } else if (/[a-f]/i.test(trimmed)) {
    radix = 16;
  }

  const pattern = radix === 16 ? /^[0-9a-f]+$/i : /^\d+$/;
  if (!pattern.test(digits)) {
    throw new Error(`${label} must be a decimal number or hexadecimal text.`);
  }

  const parsed = Number.parseInt(digits, radix);
  if (!Number.isInteger(parsed) || parsed < 0 || parsed > max) {
    throw new Error(`${label} must be between 0x0 and 0x${max.toString(16)}.`);
  }
  return parsed;
}

function parseIntInput(value, label, min, max) {
  const parsed = Number.parseInt(value.trim(), 10);
  if (!Number.isInteger(parsed) || parsed < min || parsed > max) {
    throw new Error(`${label} must be between ${min} and ${max}.`);
  }
  return parsed;
}

function renderRecord(record) {
  if (record == null) {
    els.recordHex.textContent = "";
    els.diff.textContent = "";
    els.templateHash.textContent = "";
    els.stableHash.textContent = "";
    els.decodedTime.textContent = "";
    state.templateSha256 = null;
    state.stableSha256 = null;
    updateButtonState();
    return;
  }

  els.recordHex.textContent = formatRecordHex(record);
  els.templateHash.textContent = state.templateSha256 ?? "";
  els.stableHash.textContent = state.stableSha256 ?? "";
  try {
    validateTimeFields(record);
    els.decodedTime.textContent = formatDecodedTime(record);
  } catch (error) {
    els.decodedTime.textContent = `invalid: ${error.message}`;
  }
  renderPendingTime();
  updateButtonState();
}

function renderDiff(before, after) {
  const changes = changedOffsets(before, after).map((change) => {
    const marker = change.isTime ? "" : " NON-TIME";
    return `${String(change.offset).padStart(2, "0")}: ${hex(change.before, 2)} -> ${hex(change.after, 2)}${marker}`;
  });
  els.diff.textContent = changes.join("\n");
}

function renderPendingTime() {
  try {
    const timeBytes = fillTimeBytes(selectedDate());
    els.pendingTime.textContent = bytesToHex(timeBytes);
  } catch (error) {
    els.pendingTime.textContent = error.message;
  }
}

function setConnected(isConnected) {
  els.connect.disabled = isConnected || !("hid" in navigator);
  els.disconnect.disabled = !isConnected;
  els.read.disabled = !isConnected;
  els.deviceName.textContent = isConnected ? deviceLabel(state.device) : "";
  updateButtonState();
}

function updateButtonState() {
  els.update.disabled = !state.device?.opened || state.stableSha256 == null || !els.writeConfirm.checked;
}

function setStatus(text) {
  els.status.textContent = text;
}

function logLine(text) {
  const timestamp = new Date().toLocaleTimeString();
  els.log.textContent += `[${timestamp}] ${text}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}

function reportError(error) {
  console.error(error);
  setStatus("Error");
  logLine(`error: ${error.message ?? error}`);
}

function requireWebHid() {
  if (!("hid" in navigator)) {
    throw new Error("WebHID is unavailable in this browser/context.");
  }
}

function requireOpenDevice() {
  if (state.device == null || !state.device.opened) {
    throw new Error("No open HID device.");
  }
  return state.device;
}

function deviceLabel(device) {
  if (device == null) {
    return "";
  }
  const product = device.productName || "HID device";
  return `${product} vid=0x${hex(device.vendorId, 4)} pid=0x${hex(device.productId, 4)}`;
}

function toDateTimeLocalValue(date) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  const hour = String(date.getHours()).padStart(2, "0");
  const minute = String(date.getMinutes()).padStart(2, "0");
  const second = String(date.getSeconds()).padStart(2, "0");
  return `${year}-${month}-${day}T${hour}:${minute}:${second}`;
}

function sleep(ms) {
  return new Promise((resolve) => {
    window.setTimeout(resolve, ms);
  });
}

function hex(value, width) {
  return value.toString(16).padStart(width, "0");
}

function prefixedHex(value, width) {
  return `0x${hex(value, width)}`;
}
