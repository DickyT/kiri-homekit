// Kiri Bridge — shared OTA upload helper.
//
// Used by both the production admin page and the installer setup page.
// They differ only in which manifest variants they're willing to flash:
//   admin: ["app"]                    — only production app firmware
//   installer: ["app", "installer"]   — either production app or the installer itself
//
// Server side, both endpoints are registered by the same components/ota_handler
// component, so this client is the only piece that needs to know which
// variants are accepted in each context (for early UX feedback).

import { parseKiri } from "../../../site/lib/kiri.js";

const KIRI_PACKAGE_FORMAT = "kiri-firmware-package-v1";
const KIRI_PROJECT_ID = "kiri-bridge";

export type ValidateUploadOptions = {
  acceptVariants: readonly string[];
  onProgress?: (pct: number) => void;
};

export type OtaUploadResult = {
  ok: boolean;
  bytes?: number;
  partition?: string;
  variant?: string;
  target?: string;
  board?: string;
  board_name?: string;
  current_version?: string;
  uploaded_version?: string;
  rollback?: boolean;
  same_or_older?: boolean;
  warning?: string;
  message?: string;
};

function partitionMatches(pkg: any, dev: any): boolean {
  if (!pkg || !dev) return false;
  return Number(pkg.offset) === Number(dev.address) && Number(pkg.size) === Number(dev.size);
}

export async function validateAndUpload(
  file: File,
  opts: ValidateUploadOptions,
): Promise<OtaUploadResult> {
  if (!file.name.toLowerCase().endsWith(".kiri")) {
    throw new Error("Only .kiri firmware packages are allowed.");
  }

  const { manifest, parts } = await parseKiri(file);
  if (manifest.format !== KIRI_PACKAGE_FORMAT) throw new Error("Not a supported Kiri firmware package.");
  if (manifest.project !== KIRI_PROJECT_ID) throw new Error(`Wrong project package: ${manifest.project ?? "unknown"}.`);
  if (!opts.acceptVariants.includes(manifest.variant)) {
    throw new Error(
      `This page only accepts ${opts.acceptVariants.join(" or ")} packages; uploaded package is ${manifest.variant}.`
    );
  }
  if (!manifest.app || manifest.app.path !== "app.bin") throw new Error("Manifest does not describe app.bin.");

  const appBytes: Uint8Array | undefined = parts.get(manifest.app.path);
  if (!appBytes) throw new Error("Package is missing app.bin.");
  if (Number(manifest.app.size) !== appBytes.byteLength) throw new Error("App size does not match manifest.");
  // Don't compute SHA in the browser. The firmware verifies the upload against
  // the X-Kiri-Sha256 header (mbedtls), which works in any context including
  // the insecure http://device-ip:8080 admin URL where crypto.subtle isn't
  // available.
  const expectedSha = String(manifest.app.sha256 ?? "").toLowerCase();
  if (expectedSha.length !== 64) throw new Error("Manifest app.sha256 is missing or malformed.");

  // Match partition layout against the device. /api/ota/info returns ota_0
  // and ota_1 from the running partition table; reject any package whose
  // manifest.partitions doesn't line up — that catches "you flashed the
  // wrong partition layout build" before we touch the OTA partition.
  const otaInfo = await (await fetch("/api/ota/info")).json();
  if (!otaInfo.ok) throw new Error(otaInfo.error ?? "OTA info request failed");
  if (manifest.target && otaInfo.target && manifest.target !== otaInfo.target) {
    throw new Error(`Package target ${manifest.target} does not match this device target ${otaInfo.target}.`);
  }
  if (!otaInfo.next_partition || appBytes.byteLength > Number(otaInfo.next_partition.size ?? 0)) {
    throw new Error("App is larger than the next OTA partition.");
  }
  const m0 = manifest.partitions?.ota_0;
  const m1 = manifest.partitions?.ota_1;
  if (!partitionMatches(m0, otaInfo.partitions?.ota_0) || !partitionMatches(m1, otaInfo.partitions?.ota_1)) {
    throw new Error("Package partition table does not match this device.");
  }

  return new Promise<OtaUploadResult>((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota/upload");
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.setRequestHeader("X-Kiri-Sha256", expectedSha);
    if (manifest.target) xhr.setRequestHeader("X-Kiri-Target", manifest.target);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && opts.onProgress) opts.onProgress(Math.round((e.loaded / e.total) * 100));
    };
    xhr.onload = () => {
      let result: any = null;
      try { result = JSON.parse(xhr.responseText || "{}"); } catch {}
      if (xhr.status >= 200 && xhr.status < 300 && result?.ok) {
        opts.onProgress?.(100);
        resolve({
          ...result,
          target: manifest.target,
          board: manifest.board,
          board_name: manifest.boardName,
        } as OtaUploadResult);
      } else {
        reject(new Error(result?.error ?? xhr.responseText ?? `HTTP ${xhr.status}`));
      }
    };
    xhr.onerror = () => reject(new Error("network error"));
    const uploadBytes = new Uint8Array(appBytes.byteLength);
    uploadBytes.set(appBytes);
    xhr.send(new Blob([uploadBytes.buffer], { type: "application/octet-stream" }));
  });
}

export async function applyOta(): Promise<void> {
  await fetch("/api/ota/apply", { method: "POST" }).catch(() => undefined);
}
