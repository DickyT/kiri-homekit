// Kiri Bridge — canonical .kiri parser.
//
// .kiri files are ZIP archives in STORE mode (no compression). This parser
// only handles store mode and rejects compressed entries; that lets us avoid
// pulling in JSZip / pako in any client (firmware admin, installer setup,
// public flash.html). All callers — site/flash.html, webui Admin,
// webui installer — import this same file.
//
// Format reminders (PKZIP APPNOTE.TXT, simplified):
//   end-of-central-directory record (EOCD): signature 0x06054b50, fixed 22B
//     plus optional comment.
//   central directory header (CDH): signature 0x02014b50.
//   local file header (LFH): signature 0x04034b50.

const SIG_EOCD = 0x06054b50;
const SIG_CDH  = 0x02014b50;
const SIG_LFH  = 0x04034b50;
const EOCD_MIN_SIZE = 22;
const EOCD_MAX_COMMENT = 0xffff;

function findEocd(view) {
  const total = view.byteLength;
  if (total < EOCD_MIN_SIZE) throw new Error('kiri: file too small to be a zip');
  const minStart = Math.max(0, total - EOCD_MIN_SIZE - EOCD_MAX_COMMENT);
  for (let i = total - EOCD_MIN_SIZE; i >= minStart; i--) {
    if (view.getUint32(i, true) === SIG_EOCD) return i;
  }
  throw new Error('kiri: end-of-central-directory not found');
}

function readEntries(view) {
  const eocdOffset = findEocd(view);
  const totalEntries = view.getUint16(eocdOffset + 10, true);
  const cdSize = view.getUint32(eocdOffset + 12, true);
  const cdOffset = view.getUint32(eocdOffset + 16, true);
  const cdEnd = cdOffset + cdSize;

  const entries = [];
  let p = cdOffset;
  for (let i = 0; i < totalEntries; i++) {
    if (p + 46 > cdEnd) throw new Error('kiri: central directory truncated');
    if (view.getUint32(p, true) !== SIG_CDH) throw new Error('kiri: bad central directory signature');
    const method = view.getUint16(p + 10, true);
    const compressedSize = view.getUint32(p + 20, true);
    const uncompressedSize = view.getUint32(p + 24, true);
    const nameLen = view.getUint16(p + 28, true);
    const extraLen = view.getUint16(p + 30, true);
    const commentLen = view.getUint16(p + 32, true);
    const localHeaderOffset = view.getUint32(p + 42, true);
    const nameBytes = new Uint8Array(view.buffer, view.byteOffset + p + 46, nameLen);
    const name = new TextDecoder().decode(nameBytes);
    if (method !== 0) {
      throw new Error(`kiri: entry ${name} uses compression method ${method}; only STORE (0) is supported`);
    }
    if (compressedSize !== uncompressedSize) {
      throw new Error(`kiri: entry ${name} has mismatched compressed/uncompressed sizes`);
    }
    entries.push({ name, size: uncompressedSize, localHeaderOffset });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

function readEntryBytes(view, entry) {
  const p = entry.localHeaderOffset;
  if (p + 30 > view.byteLength) throw new Error(`kiri: local header for ${entry.name} truncated`);
  if (view.getUint32(p, true) !== SIG_LFH) throw new Error(`kiri: bad local file header for ${entry.name}`);
  const method = view.getUint16(p + 8, true);
  if (method !== 0) throw new Error(`kiri: ${entry.name} local header reports method ${method}`);
  const nameLen = view.getUint16(p + 26, true);
  const extraLen = view.getUint16(p + 28, true);
  const dataStart = p + 30 + nameLen + extraLen;
  const dataEnd = dataStart + entry.size;
  if (dataEnd > view.byteLength) throw new Error(`kiri: ${entry.name} body truncated`);
  return new Uint8Array(view.buffer, view.byteOffset + dataStart, entry.size);
}

/**
 * Parse a .kiri (ZIP STORE) blob and return manifest plus byte slices of
 * every named part. Throws on any malformed input.
 *
 * @param {ArrayBuffer | Uint8Array | Blob} input
 * @returns {Promise<{ manifest: object, parts: Map<string, Uint8Array> }>}
 */
export async function parseKiri(input) {
  let buffer;
  if (input instanceof Blob) buffer = await input.arrayBuffer();
  else if (input instanceof Uint8Array) buffer = input.buffer.slice(input.byteOffset, input.byteOffset + input.byteLength);
  else buffer = input;

  const view = new DataView(buffer);
  const entries = readEntries(view);

  const parts = new Map();
  for (const entry of entries) {
    parts.set(entry.name, readEntryBytes(view, entry));
  }

  const manifestBytes = parts.get('manifest.json');
  if (!manifestBytes) throw new Error('kiri: package is missing manifest.json');
  const manifest = JSON.parse(new TextDecoder().decode(manifestBytes));
  return { manifest, parts };
}

/**
 * Compute SHA-256 of a Uint8Array using the browser's native subtle crypto.
 * Throws if subtle crypto is unavailable (insecure context); callers running
 * inside the firmware over plain http should rely on server-side hashing
 * instead of calling this.
 *
 * @param {Uint8Array} bytes
 * @returns {Promise<string>} lowercase hex digest
 */
export async function sha256Hex(bytes) {
  if (!(globalThis.crypto && globalThis.crypto.subtle)) {
    throw new Error('kiri: crypto.subtle unavailable; cannot compute SHA-256 in this context');
  }
  const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, '0')).join('');
}
