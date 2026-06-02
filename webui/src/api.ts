// Kiri Bridge — fetch wrappers. Single place to swap headers/error handling.

import type { Status, LogList } from "./types";

async function jsonFetch<T>(url: string, init?: RequestInit): Promise<T> {
  const r = await fetch(url, init);
  if (!r.ok) {
    let body = "";
    try { body = await r.text(); } catch {}
    throw new Error(`HTTP ${r.status} ${url}${body ? ` — ${body.slice(0, 120)}` : ""}`);
  }
  return r.json() as Promise<T>;
}

export const api = {
  status: () => jsonFetch<Status>("/api/status"),
  refreshCn105: () => jsonFetch<Status & { error?: string }>("/api/cn105/refresh", { method: "POST" }),
  buildSet: (params: URLSearchParams) =>
    jsonFetch<{ ok: boolean; mock_state?: import("./types").Cn105MockState; error?: string }>(
      "/api/cn105/mock/build-set?" + params.toString(),
      { method: "POST" }
    ),
  reboot: () => jsonFetch<{ ok: boolean }>("/api/reboot", { method: "POST" }),
  logs: () => jsonFetch<LogList>("/api/logs"),
  logFile: (name: string) =>
    fetch("/api/log/file?file=" + encodeURIComponent(name)).then((r) => r.text()),
  liveLog: (offset: number) =>
    jsonFetch<{ ok: boolean; reset?: boolean; text?: string; nextOffset: number; error?: string }>(
      "/api/log/live?offset=" + offset
    ),
  deleteFile: (path: string) =>
    jsonFetch<{ ok: boolean; message?: string; error?: string }>(
      "/api/files/delete?path=" + encodeURIComponent(path),
      { method: "POST" }
    ),
  clearLogs: () =>
    jsonFetch<{ ok: boolean; message?: string; error?: string; rebooting?: boolean }>(
      "/api/maintenance/clear-logs",
      { method: "POST" }
    ),
  resetHomeKit: () =>
    jsonFetch<{ ok: boolean; message?: string; error?: string; rebooting?: boolean }>(
      "/api/maintenance/reset-homekit",
      { method: "POST" }
    ),
  clearSpiffs: () =>
    jsonFetch<{ ok: boolean; message?: string; error?: string; rebooting?: boolean }>(
      "/api/maintenance/clear-spiffs",
      { method: "POST" }
    ),
  saveConfig: (params: URLSearchParams) =>
    jsonFetch<{ ok: boolean; error?: string; message?: string }>(
      "/api/config/save",
      {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: params.toString(),
      }
    ),
  deviceCfgRead: () => fetch("/api/config/device-cfg-json").then((r) => r.text()),
  deviceCfgWrite: (body: string) =>
    jsonFetch<{ ok: boolean; error?: string; message?: string }>(
      "/api/config/device-cfg-json",
      {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body,
      }
    ),
  otaInfo: () =>
    jsonFetch<{ ok: boolean; project_name?: string; target?: string; board?: { id?: string; name?: string }; next_partition?: { size?: number }; partitions?: Record<string, { address: number; size: number }>; error?: string }>(
      "/api/ota/info"
    ),
  otaApply: () => fetch("/api/ota/apply", { method: "POST" }).catch(() => undefined),
  transportStatus: async () => {
    const s = await api.status();
    return s.cn105.transport_status;
  },
};
