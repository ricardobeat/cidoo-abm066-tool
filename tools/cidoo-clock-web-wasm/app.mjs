import { loadCdooCore } from "./cdoo-core-wasm.mjs";

const els = {
  status: document.querySelector("#status"),
  updateClock: document.querySelector("#updateClock"),
  image1: document.querySelector("#image1"),
  image2: document.querySelector("#image2"),
  image1Box: document.querySelector("#image1Box"),
  image2Box: document.querySelector("#image2Box"),
  image1Preview: document.querySelector("#image1Preview"),
  image2Preview: document.querySelector("#image2Preview"),
  image1Meta: document.querySelector("#image1Meta"),
  image2Meta: document.querySelector("#image2Meta"),
  uploadImages: document.querySelector("#uploadImages"),
  imageStatus: document.querySelector("#imageStatus"),
};

let core;
let defaults;
let device;
const imageSlots = [
  { input: els.image1, box: els.image1Box, preview: els.image1Preview, meta: els.image1Meta, label: "Image 1", previewUrl: null },
  { input: els.image2, box: els.image2Box, preview: els.image2Preview, meta: els.image2Meta, label: "Image 2", previewUrl: null },
];

try {
  core = await loadCdooCore();
  defaults = Object.freeze({
    vendorId: core.defaultVid(),
    productId: core.defaultPid(),
    usagePage: core.defaultUsagePage(),
    usage: core.defaultUsage(),
    reportId: core.defaultReportId(),
    slot: 0,
    timeoutMs: 1500,
  });
  setStatus("Ready");
} catch (error) {
  setStatus(error.message);
  throw error;
}

els.updateClock.addEventListener("click", () => {
  runAction(els.updateClock, updateClock);
});

els.uploadImages.addEventListener("click", () => {
  runAction(els.uploadImages, uploadImages, setImageStatus);
});

for (const slot of imageSlots) {
  slot.input.addEventListener("change", () => {
    handleImageSelection(slot);
  });
}

async function updateClock() {
  setStatus("Connecting");
  await openDevice();

  setStatus("Reading clock config");
  const current = await readTemplate();
  const timeBytes = core.writeTimeBytes(new Date());
  const patched = core.patchTime(current, timeBytes);
  verifyOnlyTimeChanged(current, patched);

  setStatus("Writing clock");
  await writeConfig(patched);
  setStatus("Clock updated");
}

async function uploadImages() {
  const file1 = els.image1.files?.[0];
  const file2 = els.image2.files?.[0];
  if (!file1 || !file2) {
    throw new Error("Choose two images first.");
  }
  validateGifFile(file1);
  validateGifFile(file2);

  setImageStatus("Decoding GIFs");
  const gif1 = await decodeGifFrames(file1, imageSlots[0]);
  const gif2 = await decodeGifFrames(file2, imageSlots[1]);
  const totalFrames = gif1.frames.length + gif2.frames.length;
  if (totalFrames === 0) {
    throw new Error("No GIF frames decoded.");
  }
  if (totalFrames > core.maxTotalFrames()) {
    throw new Error(`Too many GIF frames: ${totalFrames}/${core.maxTotalFrames()}.`);
  }
  const payload = concatFrames(gif1.frames, gif2.frames);

  setImageStatus("Connecting");
  await openDevice();

  setImageStatus("Reading clock config");
  const current = await readTemplate();
  const existingBucket1Frames = current[34];
  const existingBucket2Frames = current[46];
  const activeSelector = current[33];
  const patched = core.patchImageConfig(current, activeSelector, gif1.frames.length, gif2.frames.length);
  verifyOnlyImageMetadataChanged(current, patched);

  setImageStatus("Writing image config");
  await writeConfig(patched);

  setImageStatus("Verifying image config");
  const readback = await readTemplate();
  if (!recordsEqualIgnoringTime(readback, patched)) {
    throw new Error("Image config readback did not match.");
  }

  await writeImagePayload(
    payload,
    existingBucket1Frames,
    existingBucket2Frames,
    gif1.frames.length,
    gif2.frames.length,
  );
  setImageStatus("Images uploaded");
}

async function decodeGifFrames(file, slot) {
  if (!("ImageDecoder" in window)) {
    throw new Error("This browser cannot decode GIF frames with ImageDecoder.");
  }

  const decoder = new ImageDecoder({
    data: await file.arrayBuffer(),
    type: "image/gif",
  });

  try {
    await decoder.tracks.ready;
    let frameCount = decoder.tracks.selectedTrack?.frameCount ?? 0;
    if ((!Number.isInteger(frameCount) || frameCount < 1) && decoder.completed) {
      await decoder.completed.catch(() => {});
      frameCount = decoder.tracks.selectedTrack?.frameCount ?? 0;
    }
    if (!Number.isInteger(frameCount) || frameCount < 1) {
      throw new Error(`${slot.label} has no decodable GIF frames.`);
    }

    const frames = [];
    for (let i = 0; i < frameCount; i++) {
      setImageStatus(`${slot.label}: decoding ${i + 1}/${frameCount}`);
      const { image } = await decoder.decode({ frameIndex: i, completeFramesOnly: true });
      try {
        frames.push(encodeDecodedFrame(image));
      } finally {
        image.close();
      }
    }
    slot.meta.textContent = `${file.name} - ${formatFrameCount(frames.length)}`;
    return { frames };
  } finally {
    decoder.close();
  }
}

function encodeDecodedFrame(image) {
  const canvas = document.createElement("canvas");
  canvas.width = 96;
  canvas.height = 160;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.drawImage(image, 0, 0, canvas.width, canvas.height);
  const rgba = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
  return core.encodeLcd160x96Frame(rgba);
}

function concatFrames(frames1, frames2) {
  const frameBytes = core.defaultDeviceFrameBytes();
  const payload = new Uint8Array((frames1.length + frames2.length) * frameBytes);
  let offset = 0;
  for (const frame of [...frames1, ...frames2]) {
    payload.set(frame, offset);
    offset += frameBytes;
  }
  return payload;
}

async function openDevice() {
  if (!("hid" in navigator)) {
    throw new Error("WebHID is unavailable in this browser.");
  }
  if (device?.opened) return device;

  const knownDevices = await navigator.hid.getDevices();
  device = knownDevices.find(matchesDevice);
  if (!device) {
    const selected = await navigator.hid.requestDevice({
      filters: [
        {
          vendorId: defaults.vendorId,
          productId: defaults.productId,
          usagePage: defaults.usagePage,
          usage: defaults.usage,
        },
      ],
    });
    device = selected.find(matchesDevice);
  }
  if (!device) {
    throw new Error("No matching CIDOO device selected.");
  }
  if (!device.opened) {
    await device.open();
  }
  return device;
}

function matchesDevice(candidate) {
  if (candidate.vendorId !== defaults.vendorId || candidate.productId !== defaults.productId) {
    return false;
  }
  return candidate.collections?.some(
    (collection) => collection.usagePage === defaults.usagePage && collection.usage === defaults.usage,
  );
}

async function readTemplate() {
  const record = new Uint8Array(core.recordSize());
  const baseOffset = slotOffset(defaults.slot);

  for (let pos = 0; pos < core.recordSize(); pos += 4) {
    const chunkLength = Math.min(4, core.recordSize() - pos);
    const packet = core.buildConfigPacket(defaults.reportId, 0x05, null, chunkLength, baseOffset + pos);
    const response = await sendPacketWait(packet, 0x05, { checkStatus: false });
    if (response.length === core.reportSize() && bytesEqual(response, packet)) {
      throw new Error("The HID collection echoed the read packet.");
    }
    if (response.length < 4 + chunkLength) {
      throw new Error("The clock returned a short config read.");
    }
    record.set(response.subarray(4, 4 + chunkLength), pos);
  }

  if (record.every((value) => value === 0)) {
    throw new Error("The clock returned an all-zero config.");
  }
  return record;
}

async function writeConfig(record) {
  await sendPacketWait(core.buildSimplePacket(defaults.reportId, 0x01), 0x01);
  await sendPacketWait(core.buildConfigPacket(defaults.reportId, 0x06, record, core.recordSize(), slotOffset(defaults.slot)), 0x06);
  await sleep(10);
  await sendPacketWait(core.buildSimplePacket(defaults.reportId, 0x02), 0x02);
}

async function writeImagePayload(payload, existingBucket1Frames, existingBucket2Frames, finalBucket1Frames, finalBucket2Frames) {
  const prepareTimeoutMs = Math.max(
    defaults.timeoutMs,
    core.imagePrepareWindowsTimeoutMs(
      existingBucket1Frames,
      existingBucket2Frames,
      finalBucket1Frames,
      finalBucket2Frames,
    ),
  );
  const packetCount = core.payloadPacketCount(payload.length);

  setImageStatus("Preparing display");
  await sendPacketWait(core.buildSimplePacket(defaults.reportId, 0x23), 0x23, {
    allowTimeout: true,
    checkStatus: false,
    timeoutMs: prepareTimeoutMs,
  });

  for (let i = 0; i < packetCount; i++) {
    const offset = i * core.defaultImageChunkSize();
    const chunkLength = Math.min(core.defaultImageChunkSize(), payload.length - offset);
    const packet = core.buildImagePacket(
      defaults.reportId,
      payload.subarray(offset, offset + chunkLength),
      chunkLength,
      offset,
    );
    await sendPacketWait(packet, 0x21);
    if ((i + 1) % 100 === 0 || i + 1 === packetCount) {
      setImageStatus(`Uploading images ${i + 1}/${packetCount}`);
    }
  }

  await sleep(10);
  await sendPacketWait(core.buildSimplePacket(defaults.reportId, 0x02), 0x02);
}

async function sendPacketWait(packet, expectedCommand, options = {}) {
  const {
    allowTimeout = false,
    checkStatus = true,
    timeoutMs = defaults.timeoutMs,
  } = options;
  const open = await openDevice();

  return new Promise((resolve, reject) => {
    let settled = false;
    const timeout = window.setTimeout(() => {
      settle(() => {
        if (allowTimeout) {
          resolve(null);
        } else {
          reject(new Error("The clock did not respond."));
        }
      });
    }, timeoutMs);

    const onInputReport = (event) => {
      if (event.device !== open || event.reportId !== defaults.reportId) return;

      const response = normalizeInputReport(event.reportId, event.data);
      if (response.length < 4 || response[3] !== expectedCommand) return;
      if (checkStatus && response.length >= 8 && (response[7] === 0xff || response[7] === 0xfe)) {
        settle(() => reject(new Error("The clock rejected the command.")));
        return;
      }
      settle(() => resolve(response));
    };

    const settle = (finish) => {
      if (settled) return;
      settled = true;
      window.clearTimeout(timeout);
      open.removeEventListener("inputreport", onInputReport);
      finish();
    };

    open.addEventListener("inputreport", onInputReport);
    open.sendReport(defaults.reportId, packet.subarray(1)).catch((error) => {
      settle(() => reject(error));
    });
  });
}

function normalizeInputReport(reportId, dataView) {
  const data = new Uint8Array(dataView.buffer, dataView.byteOffset, dataView.byteLength);
  const normalized = new Uint8Array(Math.min(core.reportSize(), data.length + 1));
  normalized[0] = reportId & 0xff;
  normalized.set(data.subarray(0, core.reportSize() - 1), 1);
  return normalized;
}

function verifyOnlyTimeChanged(before, after) {
  const offset = core.timeChangeViolationOffset(before, after);
  if (offset >= 0) {
    throw new Error("Refusing to change non-time clock config.");
  }
}

function verifyOnlyImageMetadataChanged(before, after) {
  const offset = core.imageConfigChangeViolationOffset(before, after);
  if (offset >= 0) {
    throw new Error("Refusing to change unrelated clock config.");
  }
}

function recordsEqualIgnoringTime(a, b) {
  if (a.length !== b.length) return false;
  const timeStart = core.timeOffset();
  const timeEnd = timeStart + core.timeSize();
  for (let i = 0; i < a.length; i++) {
    if (i >= timeStart && i < timeEnd) continue;
    if (a[i] !== b[i]) return false;
  }
  return true;
}

function slotOffset(slot) {
  return slot * 0x31;
}

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

function handleImageSelection(slot) {
  const file = slot.input.files?.[0];
  clearPreview(slot);
  if (!file) {
    slot.meta.textContent = "";
    return;
  }

  try {
    validateGifFile(file);
    slot.previewUrl = URL.createObjectURL(file);
    slot.preview.src = slot.previewUrl;
    slot.preview.hidden = false;
    slot.box.classList.add("has-preview");
    slot.meta.textContent = file.name;
  } catch (error) {
    slot.input.value = "";
    slot.meta.textContent = error.message;
    setImageStatus(error.message);
  }
}

function validateGifFile(file) {
  const hasGifExtension = file.name.toLowerCase().endsWith(".gif");
  const hasGifMime = file.type === "" || file.type === "image/gif";
  if (!hasGifExtension || !hasGifMime) {
    throw new Error("Only .gif files are allowed.");
  }
}

function clearPreview(slot) {
  if (slot.previewUrl) {
    URL.revokeObjectURL(slot.previewUrl);
    slot.previewUrl = null;
  }
  slot.preview.removeAttribute("src");
  slot.preview.hidden = true;
  slot.box.classList.remove("has-preview");
}

function formatFrameCount(count) {
  return count === 1 ? "1 frame" : `${count} frames`;
}

async function runAction(button, action, report = setStatus) {
  setBusy(true);
  button.setAttribute("aria-busy", "true");
  try {
    await action();
  } catch (error) {
    report(error.message);
  } finally {
    button.removeAttribute("aria-busy");
    setBusy(false);
  }
}

function setBusy(value) {
  els.updateClock.disabled = value;
  els.uploadImages.disabled = value;
  els.image1.disabled = value;
  els.image2.disabled = value;
}

function setStatus(message) {
  els.status.textContent = message;
}

function setImageStatus(message) {
  els.imageStatus.textContent = message;
}

function sleep(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}
