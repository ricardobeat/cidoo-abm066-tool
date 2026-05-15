import { loadCdooCore } from "./cdoo-core-wasm.mjs";

const els = {
  menuView: document.querySelector("#menuView"),
  clockView: document.querySelector("#clockView"),
  imagesView: document.querySelector("#imagesView"),
  status: document.querySelector("#status"),
  clockTime: document.querySelector("#clockTime"),
  updateClock: document.querySelector("#updateClock"),
  clockSuccess: document.querySelector("#clockSuccess"),
  image1: document.querySelector("#image1"),
  image2: document.querySelector("#image2"),
  image1Box: document.querySelector("#image1Box"),
  image2Box: document.querySelector("#image2Box"),
  image1Preview: document.querySelector("#image1Preview"),
  image2Preview: document.querySelector("#image2Preview"),
  image1Meta: document.querySelector("#image1Meta"),
  image2Meta: document.querySelector("#image2Meta"),
  uploadImages: document.querySelector("#uploadImages"),
  imagesSuccess: document.querySelector("#imagesSuccess"),
  imageStatus: document.querySelector("#imageStatus"),
  connectDevice: document.querySelector("#connectDevice"),
  entries: [...document.querySelectorAll(".entry")],
  entryList: document.querySelector(".entry-list"),
};

let core;
let defaults;
let device;
let selectedEntryIndex = location.hash === "#images" ? 1 : 0;
let lastWheelAt = 0;
let openTimer = 0;
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
  setStatus("");
  showView(location.hash === "#images" ? "images" : location.hash === "#clock" ? "clock" : "menu", false, false);
} catch (error) {
  setStatus(error.message);
  throw error;
}

els.updateClock.addEventListener("click", () => {
  runAction(els.updateClock, els.clockSuccess, updateClock, setStatus);
});

els.uploadImages.addEventListener("click", () => {
  runAction(els.uploadImages, els.imagesSuccess, uploadImages, setStatus);
});

els.clockSuccess.addEventListener("click", () => {
  resetSuccess(els.updateClock, els.clockSuccess);
});

els.imagesSuccess.addEventListener("click", () => {
  resetSuccess(els.uploadImages, els.imagesSuccess);
});

els.connectDevice.addEventListener("click", async () => {
  els.connectDevice.disabled = true;
  try {
    await openDevice();
    setStatus("ready");
    updateConnectionUi();
  } catch (error) {
    setStatus(error.message);
    els.connectDevice.hidden = true;
  }
});

for (const button of document.querySelectorAll("[data-view]")) {
  button.addEventListener("click", () => {
    const entryIndex = els.entries.indexOf(button);
    if (entryIndex < 0) {
      showView(button.dataset.view);
      return;
    }
    if (entryIndex >= 0 && entryIndex !== selectedEntryIndex) {
      setSelectedEntry(entryIndex);
    }
    openSelectedEntryDelayed();
  });
}

window.addEventListener("hashchange", () => {
  showView(location.hash === "#images" ? "images" : location.hash === "#clock" ? "clock" : "menu", false);
});

for (const slot of imageSlots) {
  slot.input.addEventListener("change", () => {
    handleImageSelection(slot);
  });
}

els.entryList.addEventListener("wheel", (event) => {
  if (els.menuView.hidden) return;
  event.preventDefault();
  const now = performance.now();
  if (now - lastWheelAt < 180 || Math.abs(event.deltaY) < 4) return;
  lastWheelAt = now;
  selectRelative(event.deltaY > 0 ? 1 : -1);
}, { passive: false });

window.addEventListener("keydown", (event) => {
  if (event.key === "Escape" || event.key === "Backspace") {
    if (els.menuView.hidden && !isTypingTarget(event.target)) {
      event.preventDefault();
      showView("menu");
    }
    return;
  }
  if (els.menuView.hidden && event.key === "ArrowDown") {
    event.preventDefault();
    if (!els.clockView.hidden) {
      els.updateClock.focus();
    } else if (!els.imagesView.hidden) {
      els.uploadImages.focus();
    }
    return;
  }
  if (els.menuView.hidden) return;
  if (event.key === "ArrowDown" || event.key === "ArrowRight") {
    event.preventDefault();
    selectRelative(-1);
  } else if (event.key === "ArrowUp" || event.key === "ArrowLeft") {
    event.preventDefault();
    selectRelative(1);
  } else if (event.key === "Enter") {
    event.preventDefault();
    showView(els.entries[selectedEntryIndex].dataset.view);
  }
});

function isTypingTarget(target) {
  return target instanceof HTMLInputElement ||
    target instanceof HTMLTextAreaElement ||
    target instanceof HTMLSelectElement ||
    target?.isContentEditable;
}

setSelectedEntry(selectedEntryIndex);
updateClockTime();
window.setInterval(updateClockTime, 1000);

function selectRelative(delta) {
  setSelectedEntry((selectedEntryIndex + delta + els.entries.length) % els.entries.length);
}

function setSelectedEntry(index) {
  window.clearTimeout(openTimer);
  selectedEntryIndex = index;
  els.entryList.style.setProperty("--selected", String(index));
  for (let i = 0; i < els.entries.length; i++) {
    const selected = i === index;
    els.entries[i].classList.toggle("is-selected", selected);
    els.entries[i].setAttribute("aria-selected", selected ? "true" : "false");
    els.entries[i].tabIndex = selected ? 0 : -1;
  }
}

function openSelectedEntryDelayed() {
  window.clearTimeout(openTimer);
  const view = els.entries[selectedEntryIndex].dataset.view;
  openTimer = window.setTimeout(() => {
    showView(view);
  }, 1500);
}

function showView(name, updateHash = true, animate = true) {
  window.clearTimeout(openTimer);
  const selected = name === "clock" || name === "images" ? name : "menu";
  const apply = () => {
    els.menuView.hidden = selected !== "menu";
    els.clockView.hidden = selected !== "clock";
    els.imagesView.hidden = selected !== "images";

    if (updateHash) {
      const nextHash = selected === "menu" ? "" : `#${selected}`;
      if (location.hash !== nextHash) {
        history.pushState(null, "", `${location.pathname}${location.search}${nextHash}`);
      }
    }
  };

  if (animate && document.startViewTransition) {
    document.startViewTransition(apply);
  } else {
    apply();
  }
}

async function updateClock() {
  setStatus("clock: connecting");
  await progressDelay();
  await openDevice();

  setStatus("clock: reading config");
  await progressDelay();
  const current = await readTemplate();
  setStatus("clock: encoding local time");
  await progressDelay();
  const timeBytes = core.writeTimeBytes(new Date());
  const patched = core.patchTime(current, timeBytes);
  setStatus("clock: verifying patch");
  await progressDelay();
  verifyOnlyTimeChanged(current, patched);

  setStatus("clock: writing config");
  await progressDelay();
  await writeConfig(patched);
  setStatus("clock: done");
}

async function uploadImages() {
  const file1 = els.image1.files?.[0];
  const file2 = els.image2.files?.[0];
  if (!file1 || !file2) {
    throw new Error("Choose two images first.");
  }
  validateGifFile(file1);
  validateGifFile(file2);

  setStatus("images: decoding GIFs");
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

  setStatus("images: connecting");
  await openDevice();

  setStatus("images: reading config");
  const current = await readTemplate();
  const existingBucket1Frames = current[34];
  const existingBucket2Frames = current[46];
  const activeSelector = current[33];
  setStatus("images: patching metadata");
  const patched = core.patchImageConfig(current, activeSelector, gif1.frames.length, gif2.frames.length);
  verifyOnlyImageMetadataChanged(current, patched);

  setStatus("images: writing config");
  await writeConfig(patched);

  setStatus("images: verifying config");
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
  setStatus("images: done");
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
      setStatus(`${slot.label}: decoding ${i + 1}/${frameCount}`);
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
  updateConnectionUi();
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

  setStatus("images: preparing display");
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
      setStatus(`images: uploading ${i + 1}/${packetCount}`);
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
    setStatus(error.message);
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

async function runAction(button, successMessage, action, report = setStatus) {
  const originalLabel = button.textContent;
  setBusy(true);
  button.setAttribute("aria-busy", "true");
  if (button === els.uploadImages) button.textContent = "uploading...";
  try {
    await action();
    showSuccess(button, successMessage);
  } catch (error) {
    report(error.message);
    button.textContent = originalLabel;
  } finally {
    button.removeAttribute("aria-busy");
    setBusy(false, button);
  }
}

function showSuccess(button, successMessage) {
  button.hidden = true;
  button.disabled = true;
  successMessage.hidden = false;
  successMessage.focus();
}

function resetSuccess(button, successMessage) {
  successMessage.hidden = true;
  button.hidden = false;
  button.disabled = false;
  if (button === els.uploadImages) button.textContent = "Upload Images";
  button.focus();
}

function setBusy(value, except = null) {
  if (els.updateClock !== except) els.updateClock.disabled = value;
  if (els.uploadImages !== except) els.uploadImages.disabled = value;
  els.image1.disabled = value;
  els.image2.disabled = value;
}

function setStatus(message) {
  els.status.textContent = message;
  els.status.hidden = !message;
  els.connectDevice.hidden = !!message || !!device?.opened;
}

function updateConnectionUi() {
  const connected = !!device?.opened;
  els.connectDevice.hidden = connected;
  els.status.hidden = !connected || !els.status.textContent;
}

function updateClockTime() {
  const value = formatToolDateTime(new Date());
  els.clockTime.textContent = value;
  els.clockTime.dateTime = value;
}

function formatToolDateTime(date) {
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}T${pad2(date.getHours())}:${pad2(date.getMinutes())}:${pad2(date.getSeconds())}`;
}

function pad2(value) {
  return String(value).padStart(2, "0");
}

function sleep(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function progressDelay() {
  return sleep(350);
}
