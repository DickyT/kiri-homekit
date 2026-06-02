// Kiri Bridge — top nav (desktop) + bottom floating pill (mobile) + footer.

import type { JSX } from "preact";
import { useComputed } from "@preact/signals";
import { useState } from "preact/hooks";
import { currentPage, navigate, type PageId } from "./router";
import { pollingActive, pollingMode, setPollingMode, status, statusError, type PollingMode } from "./store";

const TABS: { id: PageId; label: string; svg: JSX.Element }[] = [
  {
    id: "control",
    label: "Control",
    svg: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
        <path d="M3 9l9-7 9 7v11a2 2 0 01-2 2H5a2 2 0 01-2-2z" />
        <polyline points="9 22 9 12 15 12 15 22" />
      </svg>
    ),
  },
  {
    id: "logs",
    label: "Logs",
    svg: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
        <path d="M4 19h16" /><path d="M4 15h16" /><path d="M4 11h16" /><path d="M4 7h10" />
      </svg>
    ),
  },
  {
    id: "admin",
    label: "Admin",
    svg: (
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
        <circle cx="12" cy="12" r="3" />
        <path d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 010 2.83 2 2 0 01-2.83 0l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 01-4 0v-.09A1.65 1.65 0 009 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 01-2.83-2.83l.06-.06A1.65 1.65 0 004.68 15a1.65 1.65 0 00-1.51-1H3a2 2 0 010-4h.09A1.65 1.65 0 004.6 9a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 012.83-2.83l.06.06A1.65 1.65 0 009 4.68a1.65 1.65 0 001-1.51V3a2 2 0 014 0v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 012.83 2.83l-.06.06A1.65 1.65 0 0019.4 9a1.65 1.65 0 001.51 1H21a2 2 0 010 4h-.09a1.65 1.65 0 00-1.51 1z" />
      </svg>
    ),
  },
];

export function Tabs(): JSX.Element {
  const active = currentPage.value;
  return (
    <nav class="tabs" aria-label="Main navigation">
      {TABS.map(({ id, label, svg }) => (
        <button
          key={id}
          class={"tab-button" + (active === id ? " active" : "")}
          onClick={() => navigate(id)}
          aria-current={active === id ? "page" : undefined}
        >
          {svg}{label}
        </button>
      ))}
      <PollingControl />
    </nav>
  );
}

export function AppTopBar(): JSX.Element {
  const meta = useComputed(() => {
    const s = status.value;
    if (!s) return { wifi: "Wi-Fi --", cn105: "CN105 --" };
    return {
      wifi: s.wifi.connected ? `Wi-Fi ${s.wifi.ip ?? "--"}` : "Wi-Fi offline",
      cn105: s.cn105.transport === "real"
        ? `CN105 ${s.cn105.transport_status.connected ? "connected" : "connecting"}`
        : "CN105 mock",
    };
  });
  return (
    <nav class="nav-bar" aria-label="Kiri Bridge">
      <div class="nav-inner">
        <span class="brand">
          <span class="brand-mark" aria-hidden="true" />
          Kiri Bridge<span class="brand-page">web UI</span>
        </span>
        <PollingControl placement="top" />
        <span class="app-nav-meta" aria-label="Device status">
          <span>{meta.value.wifi}</span>
          <span>{meta.value.cn105}</span>
        </span>
      </div>
    </nav>
  );
}

function PollingControl({ placement = "tabs" }: { placement?: "tabs" | "top" }): JSX.Element {
  const [pollMenuOpen, setPollMenuOpen] = useState(false);
  const poll = useComputed(() => {
    const mode = pollingMode.value;
    const label = mode === 0 ? "Off" : `${mode / 1000}s`;
    return {
      label: statusError.value ? "Offline" : `Live ${label}`,
      detail: statusError.value ? "error" : (mode === 0 ? "manual" : (pollingActive.value ? "active" : "paused")),
      cls: statusError.value ? "bad" : (mode === 0 || !pollingActive.value ? "off" : "ok"),
    };
  });
  function choosePolling(mode: PollingMode): void {
    setPollingMode(mode);
    setPollMenuOpen(false);
  }
  return (
    <span class={"poll-control poll-control-" + placement}>
      <button
        class={"poll-button " + poll.value.cls}
        type="button"
        onClick={() => setPollMenuOpen((open) => !open)}
        aria-haspopup="menu"
        aria-expanded={pollMenuOpen}
      >
        <span>{poll.value.label}</span>
        <small>{poll.value.detail}</small>
      </button>
      {pollMenuOpen && (
        <span class="poll-menu" role="menu">
          <button type="button" class={pollingMode.value === 5000 ? "active" : ""} onClick={() => choosePolling(5000)}>5s</button>
          <button type="button" class={pollingMode.value === 15000 ? "active" : ""} onClick={() => choosePolling(15000)}>15s</button>
          <button type="button" class={pollingMode.value === 0 ? "active" : ""} onClick={() => choosePolling(0)}>Off</button>
        </span>
      )}
    </span>
  );
}

export function SiteFooter(): JSX.Element {
  const meta = useComputed(() => {
    const s = status.value;
    const board = s?.board?.name ?? "Unknown board";
    const target = s?.board?.target ?? "unknown target";
    return {
      version: s?.version ?? "--",
      board: `${board} (${target})`,
    };
  });
  return (
    <footer class="site-footer">
      <div class="footer-brand">
        <span class="footer-mark" aria-hidden="true" />
        <span><strong>Kiri Bridge</strong> — open-source HomeKit controller for Mitsubishi heat pumps</span>
      </div>
      <div class="footer-meta">
        <a href="https://kiri.dkt.moe" target="_blank" rel="noopener noreferrer">kiri.dkt.moe</a>
        <span>{meta.value.board}</span>
        <span>v<b>{meta.value.version}</b></span>
      </div>
    </footer>
  );
}
