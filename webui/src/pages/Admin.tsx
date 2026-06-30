// Kiri Bridge — Admin page.
// Device info, settings (with CN105 advanced sub-modal), OTA, NVS editor, HomeKit QR, maintenance.

import { useEffect, useState, useRef, useCallback } from "preact/hooks";
import type { JSX } from "preact";
import { Section, Field, Btn, Modal, OtaConfirmModal, RebootingModal } from "../components";
import { HomeKitPairingTile } from "../HomeKitPairingTile";
import { api } from "../api";
import { status, fetchStatusOnce } from "../store";
import type { Status, DeviceConfig, TransportStatus, BoardInfo, AutomationStatus } from "../types";
import { validateAndUpload } from "../lib/ota";
import type { OtaUploadResult } from "../lib/ota";

// ----- formatters -----

function formatHomeKitCode(code: string | undefined): string {
  const digits = String(code ?? "").replace(/\D/g, "");
  if (digits.length !== 8) return code || "--";
  return `${digits.slice(0, 4)}-${digits.slice(4)}`;
}
function normalizeHomeKitCode(code: string | undefined): string {
  return String(code ?? "").replace(/\D/g, "");
}
function formatUptime(ms: number): string {
  const t = Math.max(0, Math.floor(ms / 1000));
  const d = Math.floor(t / 86400);
  const h = Math.floor((t % 86400) / 3600);
  const m = Math.floor((t % 3600) / 60);
  const s = t % 60;
  const pad = (n: number) => String(n).padStart(2, "0");
  return d > 0 ? `${d}d ${pad(h)}:${pad(m)}:${pad(s)}` : `${pad(h)}:${pad(m)}:${pad(s)}`;
}
function formatBootTime(unixMs: number): string {
  if (!unixMs) return "--";
  const d = new Date(unixMs);
  return Number.isNaN(d.getTime()) ? "--" : d.toLocaleString();
}
function signalIcon(rssi: number | undefined): string {
  if (typeof rssi !== "number") return "▱▱▱";
  if (rssi >= -55) return "▰▰▰";
  if (rssi >= -67) return "▰▰▱";
  if (rssi >= -75) return "▰▱▱";
  return "▱▱▱";
}
function provisioningStage(p: { stage?: string; active?: boolean } | undefined): string {
  if (!p) return "Disabled";
  const stage = p.stage ?? "idle";
  const map: Record<string, string> = {
    starting: "Starting BLE provisioning",
    waiting: "Waiting for phone provisioning",
    connecting: "Connecting to new WiFi",
    connected: "Connected, rebooting soon",
    failed: "New WiFi connection failed",
    "timed-out": "Provisioning window timed out",
    "save-failed": "Failed to save new WiFi",
    "start-failed": "BLE provisioning failed to start",
    "init-failed": "BLE provisioning failed to initialize",
  };
  return map[stage] ?? (p.active ? "BLE provisioning active" : "Inactive");
}
function formatDuration(ms: number): string {
  const t = Math.max(0, Math.ceil((ms || 0) / 1000));
  return `${Math.floor(t / 60)}m ${String(t % 60).padStart(2, "0")}s`;
}
// ----- settings field config -----

const CN105_ADVANCED_KEYS = [
  "cfg-cn105-rx-pin", "cfg-cn105-tx-pin", "cfg-cn105-baud",
  "cfg-cn105-data-bits", "cfg-cn105-parity", "cfg-cn105-stop-bits",
  "cfg-cn105-rx-pullup", "cfg-cn105-tx-open-drain",
] as const;

const HVAC_TARGET_OPTIONS = [
  { value: "0", bit: 1 << 0, label: "Auto", detail: "Target value 0" },
  { value: "1", bit: 1 << 1, label: "Heat", detail: "Target value 1" },
  { value: "2", bit: 1 << 2, label: "Cool", detail: "Target value 2" },
] as const;
const AC_CAP_TARGET_MASK = (1 << 0) | (1 << 1) | (1 << 2);
const AC_CAP_UP_DOWN_AIRFLOW = 1 << 3;
const AC_CAP_LEFT_RIGHT_AIRFLOW = 1 << 4;
const AC_CAP_KNOWN_MASK = AC_CAP_TARGET_MASK | AC_CAP_UP_DOWN_AIRFLOW | AC_CAP_LEFT_RIGHT_AIRFLOW;
const AC_CAP_DEFAULT = AC_CAP_KNOWN_MASK;
const HK_MAP_UP_DOWN_TILT = 1 << 0;
const HK_MAP_LEFT_RIGHT_TILT = 1 << 1;
const HK_MAP_UP_DOWN_SWING = 1 << 2;
const HK_MAP_LEFT_RIGHT_SWING = 1 << 3;
const HK_MAP_KNOWN_MASK = HK_MAP_UP_DOWN_TILT | HK_MAP_LEFT_RIGHT_TILT | HK_MAP_UP_DOWN_SWING | HK_MAP_LEFT_RIGHT_SWING;

const HOMEKIT_MAPPING_OPTIONS = [
  { bit: HK_MAP_UP_DOWN_TILT, label: "Up/Down Tilt Tile", detail: "Adds a HomeKit slat service for the horizontal flap's fixed up/down position." },
  { bit: HK_MAP_LEFT_RIGHT_TILT, label: "Left/Right Tilt Tile", detail: "Adds a HomeKit slat service for the vertical vanes' fixed left/right position." },
  { bit: HK_MAP_UP_DOWN_SWING, label: "Up/Down Swing Tile", detail: "Adds a HomeKit switch dedicated to horizontal flap oscillation." },
  { bit: HK_MAP_LEFT_RIGHT_SWING, label: "Left/Right Swing Tile", detail: "Adds a HomeKit switch dedicated to vertical vane oscillation." },
] as const;

type SettingsForm = Record<string, string>;

const DEFAULT_AUTOMATION_SCRIPT = `-- Kiri Bridge automation.lua
-- Save, enable Automation, then test with the real AC/HomeKit/WebUI.

function on_power_off(state, previous)
  if previous and previous.up_down_airflow then
    kv.set("last_up_down_airflow", previous.up_down_airflow)
  end
end

function on_power_on(state, previous)
  local airflow = kv.get("last_up_down_airflow")
  if airflow and airflow ~= "" then
    ac.set_up_down_airflow(airflow)
  end
end

function on_state_changed(state, previous)
  -- Example:
  -- if state.power and state.room_temp_f > state.target_temp_f + 3 then
  --   ac.set_fan(3)
  -- end
end
`;

function defaultSettings(): SettingsForm {
  return {
    "cfg-device-name": "",
    "cfg-wifi-ssid": "",
    "cfg-wifi-password": "",
    "cfg-homekit-code": "",
    "cfg-homekit-setup-id": "",
    "cfg-homekit-manufacturer": "",
    "cfg-homekit-model": "",
    "cfg-homekit-serial": "",
    "cfg-homekit-separate-airflow-tile": "1",
    "cfg-homekit-advanced-mapping": "0",
    "cfg-ac-capabilities": String(AC_CAP_DEFAULT),
    "cfg-led-pin": "27",
    "cfg-cn105-mode": "real",
    "cfg-log-level": "info",
    "cfg-poll-active": "15000",
    "cfg-poll-off": "60000",
    "cfg-cn105-rx-pin": "26",
    "cfg-cn105-tx-pin": "32",
    "cfg-cn105-baud": "2400",
    "cfg-cn105-data-bits": "8",
    "cfg-cn105-parity": "E",
    "cfg-cn105-stop-bits": "1",
    "cfg-cn105-rx-pullup": "1",
    "cfg-cn105-tx-open-drain": "0",
  };
}
function settingsFromConfig(cfg: DeviceConfig | undefined, deviceFallback: string, board?: BoardInfo): SettingsForm {
  const defaults = board?.defaults ?? {};
  return {
    "cfg-device-name": cfg?.device_name ?? deviceFallback,
    "cfg-wifi-ssid": cfg?.wifi_ssid ?? "",
    "cfg-wifi-password": "",
    "cfg-homekit-code": formatHomeKitCode(cfg?.homekit_code),
    "cfg-homekit-setup-id": cfg?.homekit_setup_id ?? "",
    "cfg-homekit-manufacturer": cfg?.homekit_manufacturer ?? "",
    "cfg-homekit-model": cfg?.homekit_model ?? "",
    "cfg-homekit-serial": cfg?.homekit_serial ?? "",
    "cfg-homekit-separate-airflow-tile": cfg?.homekit_separate_airflow_tile === false ? "0" : "1",
    "cfg-homekit-advanced-mapping": String(normalizeHomeKitMapping(cfg?.homekit_advanced_mapping)),
    "cfg-ac-capabilities": String(normalizeAcCapabilities(cfg?.ac_capabilities, cfg?.homekit_hvac_modes)),
    "cfg-led-pin": String(cfg?.led_pin ?? defaults.led_pin ?? 27),
    "cfg-cn105-mode": cfg?.cn105_mode ?? "real",
    "cfg-log-level": cfg?.log_level ?? "info",
    "cfg-poll-active": String(cfg?.poll_active_ms ?? 15000),
    "cfg-poll-off": String(cfg?.poll_off_ms ?? 60000),
    "cfg-cn105-rx-pin": String(cfg?.cn105_rx_pin ?? defaults.cn105_rx_pin ?? 26),
    "cfg-cn105-tx-pin": String(cfg?.cn105_tx_pin ?? defaults.cn105_tx_pin ?? 32),
    "cfg-cn105-baud": String(cfg?.cn105_baud ?? 2400),
    "cfg-cn105-data-bits": String(cfg?.cn105_data_bits ?? 8),
    "cfg-cn105-parity": cfg?.cn105_parity ?? "E",
    "cfg-cn105-stop-bits": String(cfg?.cn105_stop_bits ?? 1),
    "cfg-cn105-rx-pullup": cfg?.cn105_rx_pullup === false ? "0" : "1",
    "cfg-cn105-tx-open-drain": cfg?.cn105_tx_open_drain ? "1" : "0",
  };
}
function cn105Summary(f: SettingsForm): string {
  return `${f["cfg-cn105-data-bits"]}${f["cfg-cn105-parity"]}${f["cfg-cn105-stop-bits"]} ${f["cfg-cn105-baud"]} RX G${f["cfg-cn105-rx-pin"]} TX G${f["cfg-cn105-tx-pin"]}`;
}

function normalizeHvacModes(value: string | undefined): string {
  const values = new Set<string>();
  for (const token of String(value ?? "").split(",")) {
    const v = token.trim();
    if (v === "0" || v === "1" || v === "2") values.add(v);
  }
  const ordered = HVAC_TARGET_OPTIONS.map((o) => o.value).filter((v) => values.has(v));
  return ordered.length ? ordered.join(",") : "0,1,2";
}

function normalizeAcCapabilities(value: number | string | undefined, fallbackHvacModes?: string): number {
  let caps = typeof value === "number" ? value : Number.parseInt(String(value ?? ""), 10);
  if (!Number.isFinite(caps)) {
    caps = AC_CAP_DEFAULT;
    if (fallbackHvacModes) {
      const targetBits = normalizeHvacModes(fallbackHvacModes).split(",").reduce((acc, v) => acc | (1 << Number(v)), 0);
      caps = (caps & ~AC_CAP_TARGET_MASK) | targetBits;
    }
  }
  caps &= AC_CAP_KNOWN_MASK;
  if ((caps & AC_CAP_TARGET_MASK) === 0) caps |= AC_CAP_TARGET_MASK;
  return caps;
}

function normalizeHomeKitMapping(value: number | string | undefined): number {
  const mapping = typeof value === "number" ? value : Number.parseInt(String(value ?? ""), 10);
  return Number.isFinite(mapping) ? mapping & HK_MAP_KNOWN_MASK : 0;
}

function hvacModeSetFromCapabilities(caps: number): Set<string> {
  const selected = new Set<string>();
  for (const option of HVAC_TARGET_OPTIONS) {
    if ((caps & option.bit) !== 0) selected.add(option.value);
  }
  return selected;
}

function hvacModeSummary(caps: number): string {
  const selected = hvacModeSetFromCapabilities(caps);
  return HVAC_TARGET_OPTIONS
    .filter((o) => selected.has(o.value))
    .map((o) => `${o.label} (${o.value})`)
    .join(", ");
}

function hvacModeCompactSummary(caps: number): string {
  const selected = hvacModeSetFromCapabilities(caps);
  return HVAC_TARGET_OPTIONS
    .filter((o) => selected.has(o.value))
    .map((o) => o.label)
    .join(" / ");
}

function hvacCurrentSummary(caps: number): string {
  const selected = hvacModeSetFromCapabilities(caps);
  const current = ["Inactive (0)", "Idle (1)"];
  if (selected.has("0") || selected.has("1")) current.push("Heating (2)");
  if (selected.has("0") || selected.has("2")) current.push("Cooling (3)");
  return current.join(", ");
}

function airflowSummary(caps: number): string {
  const names = [];
  if ((caps & AC_CAP_UP_DOWN_AIRFLOW) !== 0) names.push("Up/Down");
  if ((caps & AC_CAP_LEFT_RIGHT_AIRFLOW) !== 0) names.push("Left/Right");
  return names.length ? names.join(" + ") : "Hidden";
}

function acCapabilitiesSummary(value: string | undefined): string {
  const caps = normalizeAcCapabilities(value);
  return `${hvacModeCompactSummary(caps)} · ${airflowSummary(caps)}`;
}

function homeKitMappingSummary(value: string | undefined): string {
  const mapping = normalizeHomeKitMapping(value);
  if (mapping === 0) return "None · Legacy Swing behavior";
  const names = HOMEKIT_MAPPING_OPTIONS.filter((option) => (mapping & option.bit) !== 0).map((option) => option.label.replace(" Tile", ""));
  return names.join(" · ");
}

// ----- transport pre helper -----

function renderTransport(t: TransportStatus | undefined): string {
  if (!t) return "No transport status available";
  return [
    "Phase: " + t.phase,
    "Connected: " + t.connected,
    "Connect Attempts: " + t.connect_attempts,
    "Poll Cycles: " + t.poll_cycles,
    "RX Packets: " + t.rx_packets + " / Errors: " + t.rx_errors,
    "TX Packets: " + t.tx_packets,
    "Sets Pending: " + t.sets_pending,
    t.last_error ? "Last Error: " + t.last_error : "",
  ].filter(Boolean).join("\n");
}

function renderAutomationLog(a: AutomationStatus | null): string {
  if (!a) return "Loading automation status…";
  const lines = [
    `enabled: ${a.enabled ? "yes" : "no"}`,
    `script: ${a.script_exists ? `${a.script_size}/${a.script_max_bytes} bytes` : "not saved"}`,
    `runs: ${a.run_count}`,
  ];
  if (!a.last_set) {
    lines.push("last run: none yet");
    lines.push("tip: save + enable, then change AC state from HomeKit, WebUI, or the Mitsubishi remote.");
    return lines.join("\n");
  }
  lines.push(`last hook: ${a.last_hook || "--"}`);
  lines.push(`last result: ${a.last_ok ? "ok" : "error"}`);
  lines.push(`message: ${a.last_message || "--"}`);
  lines.push(`script loaded: ${a.last_script_loaded ? "yes" : "no"}`);
  lines.push(`hook found: ${a.last_hook_found ? "yes" : "no"}`);
  lines.push(`instruction limit: ${a.last_instruction_limit_hit ? "hit" : "ok"}`);
  lines.push(`peak heap: ${a.last_peak_bytes} bytes`);
  if (a.last_actions?.length) {
    lines.push("actions:");
    for (const action of a.last_actions) {
      const value = action.value !== undefined && action.value !== "" ? ` value=${action.value}` : "";
      const intValue = action.int_value !== undefined ? ` int=${action.int_value}` : "";
      lines.push(`  - ${action.type}${value}${intValue}`);
    }
  } else {
    lines.push("actions: none");
  }
  return lines.join("\n");
}

// ----- main page component -----

type NoticeState = { title: string; body: string };

export function AdminPage(): JSX.Element {
  const s = status.value;
  const [settings, setSettings] = useState<SettingsForm>(defaultSettings);
  const [settingsLoaded, setSettingsLoaded] = useState(false);
  const [settingsDirty, setSettingsDirty] = useState(false);
  const [savedHomeKitDisplayMode, setSavedHomeKitDisplayMode] = useState("1");
  const [savedHomeKitMapping, setSavedHomeKitMapping] = useState(0);
  const [savedAcCapabilities, setSavedAcCapabilities] = useState(AC_CAP_DEFAULT);
  const [savedAdvanced, setSavedAdvanced] = useState<SettingsForm | null>(null);
  const [advancedDirty, setAdvancedDirty] = useState(false);
  const [cn105Open, setCn105Open] = useState(false);
  const [cn105Snapshot, setCn105Snapshot] = useState<SettingsForm | null>(null);
  const [capabilitiesOpen, setCapabilitiesOpen] = useState(false);
  const [capabilitiesSnapshot, setCapabilitiesSnapshot] = useState<string | null>(null);
  const [mappingOpen, setMappingOpen] = useState(false);
  const [mappingSnapshot, setMappingSnapshot] = useState<string | null>(null);
  const [otaModalState, setOtaModalState] = useState<OtaUploadResult | null>(null);
  const [otaApplying, setOtaApplying] = useState(false);
  const [otaApplyStatus, setOtaApplyStatus] = useState("");
  const [otaUploading, setOtaUploading] = useState(false);
  const [otaProgress, setOtaProgress] = useState(0);
  const [otaShowProgress, setOtaShowProgress] = useState(false);
  const [otaMsg, setOtaMsg] = useState("");
  const [otaMsgError, setOtaMsgError] = useState(false);
  const [nvsOpen, setNvsOpen] = useState(false);
  const [nvsBody, setNvsBody] = useState<string>("Loading…");
  const [nvsMsg, setNvsMsg] = useState<string>("");
  const [nvsBusy, setNvsBusy] = useState(false);
  const [automation, setAutomation] = useState<AutomationStatus | null>(null);
  const [automationScript, setAutomationScript] = useState(DEFAULT_AUTOMATION_SCRIPT);
  const [automationLoaded, setAutomationLoaded] = useState(false);
  const [automationDirty, setAutomationDirty] = useState(false);
  const [automationBusy, setAutomationBusy] = useState(false);
  const [automationMsg, setAutomationMsg] = useState("");
  const [notice, setNotice] = useState<NoticeState | null>(null);
  const [rebooting, setRebooting] = useState(false);
  const [transport, setTransport] = useState<TransportStatus | undefined>(undefined);
  const [maintMsg, setMaintMsg] = useState("");
  const [tick, setTick] = useState(0);
  const rebootTimer = useRef<number | undefined>(undefined);
  const otaInputRef = useRef<HTMLInputElement>(null);
  const currentAcCapabilities = normalizeAcCapabilities(settings["cfg-ac-capabilities"]);
  const currentHomeKitMapping = normalizeHomeKitMapping(settings["cfg-homekit-advanced-mapping"]);
  const separateHomeKitDisplay = (settings["cfg-homekit-separate-airflow-tile"] ?? "1") === "1";
  const homeKitPresentationChanged =
    (settings["cfg-homekit-separate-airflow-tile"] ?? "1") !== savedHomeKitDisplayMode ||
    currentHomeKitMapping !== savedHomeKitMapping ||
    (currentAcCapabilities & AC_CAP_TARGET_MASK) !== (savedAcCapabilities & AC_CAP_TARGET_MASK);

  // Bootstrap settings from first status that arrives.
  useEffect(() => {
    if (settingsLoaded || !s) return;
    const initialSettings = settingsFromConfig(s.config, s.device, s.board);
    setSettings(initialSettings);
    setSavedHomeKitDisplayMode(initialSettings["cfg-homekit-separate-airflow-tile"] ?? "1");
    setSavedHomeKitMapping(normalizeHomeKitMapping(initialSettings["cfg-homekit-advanced-mapping"]));
    setSavedAcCapabilities(normalizeAcCapabilities(initialSettings["cfg-ac-capabilities"]));
    setTransport(s.cn105.transport_status);
    setSettingsLoaded(true);
  }, [s]);

  // Re-tick once a second so uptime label refreshes between polls.
  useEffect(() => {
    const id = window.setInterval(() => setTick((t) => t + 1), 1000);
    return () => clearInterval(id);
  }, []);

  useEffect(() => () => {
    if (rebootTimer.current !== undefined) window.clearTimeout(rebootTimer.current);
  }, []);

  // Keep transport pre in sync with polled status.
  useEffect(() => {
    if (s) setTransport(s.cn105.transport_status);
  }, [s]);

  useEffect(() => {
    if (automationLoaded) return;
    refreshAutomation();
  }, [automationLoaded]);

  function update(key: string, value: string): void {
    setSettings((prev) => ({ ...prev, [key]: value }));
    setSettingsDirty(true);
  }

  function beginRebootFlow(action?: () => void): void {
    setNotice(null);
    setRebooting(true);
    action?.();
    if (rebootTimer.current === undefined) {
      rebootTimer.current = window.setTimeout(() => window.location.reload(), 5000);
    }
  }

  function openCn105(): void {
    setCn105Snapshot({ ...settings });
    setCn105Open(true);
  }
  function closeCn105(keep: boolean): void {
    if (keep) {
      const before = cn105Snapshot;
      const changed = before ? CN105_ADVANCED_KEYS.some((k) => settings[k] !== before[k]) : false;
      if (changed) {
        setAdvancedDirty(true);
        setSettingsDirty(true);
      }
    } else if (cn105Snapshot) {
      const restored = { ...settings };
      for (const k of CN105_ADVANCED_KEYS) restored[k] = cn105Snapshot[k] ?? "";
      setSettings(restored);
    }
    setCn105Snapshot(null);
    setCn105Open(false);
  }

  function openCapabilities(): void {
    setCapabilitiesSnapshot(settings["cfg-ac-capabilities"] ?? String(AC_CAP_DEFAULT));
    setCapabilitiesOpen(true);
  }

  function closeCapabilities(keep: boolean): void {
    if (!keep && capabilitiesSnapshot !== null) {
      setSettings((prev) => ({ ...prev, "cfg-ac-capabilities": capabilitiesSnapshot }));
    }
    setCapabilitiesSnapshot(null);
    setCapabilitiesOpen(false);
  }

  function updateAcCapabilities(nextCaps: number): void {
    update("cfg-ac-capabilities", String(normalizeAcCapabilities(nextCaps)));
  }

  function toggleHvacMode(bit: number): void {
    let caps = currentAcCapabilities;
    if ((caps & bit) !== 0) {
      if ((caps & AC_CAP_TARGET_MASK) === bit) return;
      caps &= ~bit;
    } else {
      caps |= bit;
    }
    updateAcCapabilities(caps);
  }

  function toggleCapability(bit: number): void {
    updateAcCapabilities(currentAcCapabilities ^ bit);
  }

  function openMapping(): void {
    setMappingSnapshot(settings["cfg-homekit-advanced-mapping"] ?? "0");
    setMappingOpen(true);
  }

  function closeMapping(keep: boolean): void {
    if (!keep && mappingSnapshot !== null) {
      setSettings((prev) => ({ ...prev, "cfg-homekit-advanced-mapping": mappingSnapshot }));
    }
    setMappingSnapshot(null);
    setMappingOpen(false);
  }

  function toggleHomeKitMapping(bit: number): void {
    update("cfg-homekit-advanced-mapping", String((currentHomeKitMapping ^ bit) & HK_MAP_KNOWN_MASK));
  }

  async function refreshTransport(): Promise<void> {
    try { setTransport(await api.transportStatus()); }
    catch (e: any) { setTransport(undefined); }
  }

  async function refreshAutomation(): Promise<void> {
    setAutomationMsg("Loading automation…");
    try {
      const [nextStatus, script] = await Promise.all([
        api.automationStatus(),
        api.automationScript(),
      ]);
      setAutomation(nextStatus);
      setAutomationScript(script || DEFAULT_AUTOMATION_SCRIPT);
      setAutomationDirty(false);
      setAutomationLoaded(true);
      setAutomationMsg(nextStatus.script_exists ? "Automation loaded." : "No script saved yet. Edit the template and save when ready.");
    } catch (e: any) {
      setAutomationMsg("Automation load failed: " + (e?.message ?? e));
    }
  }

  async function saveAutomation(): Promise<void> {
    setAutomationBusy(true);
    setAutomationMsg("Saving automation script…");
    try {
      const next = await api.saveAutomationScript(automationScript);
      setAutomation(next);
      setAutomationDirty(false);
      setAutomationMsg("Automation script saved. Enable it, then test with the real AC.");
    } catch (e: any) {
      setAutomationMsg("Save failed: " + (e?.message ?? e));
    } finally {
      setAutomationBusy(false);
    }
  }

  async function toggleAutomation(enabled: boolean): Promise<void> {
    setAutomationBusy(true);
    setAutomationMsg(enabled ? "Enabling automation…" : "Disabling automation…");
    try {
      const next = await api.setAutomationEnabled(enabled);
      setAutomation(next);
      setAutomationMsg(enabled ? "Automation enabled. Change AC state to test it." : "Automation disabled.");
    } catch (e: any) {
      setAutomationMsg("Toggle failed: " + (e?.message ?? e));
    } finally {
      setAutomationBusy(false);
    }
  }

  async function deleteAutomation(): Promise<void> {
    if (!confirm("Delete automation.lua from this device?")) return;
    setAutomationBusy(true);
    setAutomationMsg("Deleting automation script…");
    try {
      const next = await api.deleteAutomationScript();
      setAutomation(next);
      setAutomationScript(DEFAULT_AUTOMATION_SCRIPT);
      setAutomationDirty(false);
      setAutomationMsg("Automation script deleted. Template restored locally only.");
    } catch (e: any) {
      setAutomationMsg("Delete failed: " + (e?.message ?? e));
    } finally {
      setAutomationBusy(false);
    }
  }

  async function saveConfig(): Promise<void> {
    const code = normalizeHomeKitCode(settings["cfg-homekit-code"]?.trim());
    if (code && code.length !== 8) {
      setNotice({ title: "Save Failed", body: "HomeKit pairing code must be 8 digits, eg 1111-2222." });
      return;
    }
    const params = new URLSearchParams();
    params.set("device_name", settings["cfg-device-name"]?.trim() || "Kiri Bridge");
    params.set("wifi_ssid", settings["cfg-wifi-ssid"]?.trim() ?? "");
    if (settings["cfg-wifi-password"]) params.set("wifi_password", settings["cfg-wifi-password"]!);
    if (code) params.set("homekit_code", code);
    params.set("homekit_manufacturer", settings["cfg-homekit-manufacturer"]?.trim() ?? "");
    params.set("homekit_model", settings["cfg-homekit-model"]?.trim() ?? "");
    params.set("homekit_serial", settings["cfg-homekit-serial"]?.trim() ?? "");
    params.set("homekit_setup_id", (settings["cfg-homekit-setup-id"] ?? "").trim().toUpperCase());
    params.set("homekit_separate_airflow_tile", settings["cfg-homekit-separate-airflow-tile"] ?? "0");
    params.set("homekit_advanced_mapping", String(currentHomeKitMapping));
    params.set("ac_capabilities", String(currentAcCapabilities));
    params.set("led_pin", settings["cfg-led-pin"] ?? "");
    params.set("cn105_mode", settings["cfg-cn105-mode"] ?? "real");
    for (const k of CN105_ADVANCED_KEYS) {
      const apiKey = k.replace("cfg-", "").replace(/-/g, "_");
      params.set(apiKey, settings[k] ?? "");
    }
    params.set("log_level", settings["cfg-log-level"] ?? "info");
    params.set("poll_active_ms", settings["cfg-poll-active"] ?? "");
    params.set("poll_off_ms", settings["cfg-poll-off"] ?? "");

    setMaintMsg("Saving settings. The device will reboot when successful…");
    try {
      const j = await api.saveConfig(params);
      if (!j.ok) {
        setMaintMsg("");
        setNotice({ title: "Save Failed", body: j.error ?? j.message ?? "The device rejected this save." });
        return;
      }
      beginRebootFlow(() => { api.reboot().catch(() => { }); });
    } catch (e: any) {
      setMaintMsg("");
      setNotice({ title: "Save Failed", body: "Request failed: " + (e?.message ?? e) });
    }
  }

  async function maintenance(action: () => Promise<{ ok: boolean; message?: string; error?: string; rebooting?: boolean }>, label: string, prompt: string): Promise<void> {
    if (!confirm(prompt)) return;
    setMaintMsg(label + " running…");
    try {
      const j = await action();
      if (j.ok && j.rebooting) {
        setMaintMsg("");
        beginRebootFlow();
        return;
      }
      setMaintMsg((j.ok ? "Done: " : "Failed: ") + (j.message ?? label));
      setTimeout(fetchStatusOnce, 800);
    } catch (e: any) {
      setMaintMsg(label + " request failed: " + (e?.message ?? e));
    }
  }

  async function reboot(): Promise<void> {
    if (!confirm("Reboot the device?")) return;
    beginRebootFlow(() => { api.reboot().catch(() => { }); });
  }

  // ----- OTA -----

  async function uploadOta(file: File): Promise<void> {
    if (otaUploading) return;
    setOtaUploading(true);
    setOtaShowProgress(true);
    setOtaProgress(0);
    setOtaMsg(`Validating package: ${file.name}`);
    setOtaMsgError(false);

    try {
      const result = await validateAndUpload(file, {
        acceptVariants: ["app"],
        onProgress: (pct) => setOtaProgress(pct),
      });
      setOtaUploading(false);
      setOtaShowProgress(false);
      setOtaMsg("");
      setOtaApplyStatus("");
      setOtaModalState(result);
    } catch (e: any) {
      setOtaUploading(false);
      setOtaShowProgress(false);
      setOtaMsg("OTA upload failed: " + (e?.message ?? e));
      setOtaMsgError(true);
    }
  }

  async function confirmOta(): Promise<void> {
    if (!otaModalState) return;
    setOtaApplying(true);
    setOtaApplyStatus("Applying OTA. The device will reboot and this page will refresh in 5 seconds.");
    try {
      const r = await fetch("/api/ota/apply", { method: "POST" });
      if (!r.ok) {
        let err = "HTTP " + r.status;
        try { const j = await r.json(); err = j.error ?? err; } catch { }
        setOtaApplying(false);
        setOtaApplyStatus("OTA apply failed. Check the error, then retry or upload again.");
        setOtaMsg("OTA apply failed: " + err);
        setOtaMsgError(true);
        return;
      }
    } catch {
      // Reboot can close the connection before we see a response.
    }
    setOtaModalState(null);
    setOtaApplying(false);
    beginRebootFlow();
  }

  // ----- NVS editor -----

  async function openNvs(): Promise<void> {
    setNvsOpen(true);
    setNvsMsg("Reading device_cfg…");
    setNvsBody("Loading…");
    setNvsBusy(false);
    try {
      const text = await api.deviceCfgRead();
      setNvsBody(text);
      setNvsMsg("Edit carefully. Cancel writes nothing.");
    } catch (e: any) {
      setNvsBody("");
      setNvsMsg("Read failed: " + (e?.message ?? e));
    }
  }

  async function saveNvs(): Promise<void> {
    let parsed: unknown;
    try { parsed = JSON.parse(nvsBody); }
    catch (e: any) { setNvsMsg("JSON format error: " + (e?.message ?? e)); return; }
    setNvsBusy(true);
    setNvsMsg("Writing NVS…");
    try {
      const j = await api.deviceCfgWrite(JSON.stringify(parsed));
      if (!j.ok) {
        setNvsBusy(false);
        setNvsMsg("Write failed: " + (j.error ?? j.message ?? "unknown"));
        return;
      }
      setNvsOpen(false);
      beginRebootFlow(() => { api.reboot().catch(() => { }); });
    } catch (e: any) {
      setNvsBusy(false);
      setNvsMsg("Write failed: " + (e?.message ?? e));
    }
  }

  if (!s) {
    return <main><h1>Admin</h1><div class="subtitle">Loading…</div></main>;
  }

  const wifiInfo = `${s.wifi.ssid ?? "--"} | ${s.wifi.ip ?? "0.0.0.0"} | ${signalIcon(s.wifi.rssi)} ${s.wifi.rssi ?? "--"} dBm | BSSID ${s.wifi.bssid ?? "--"}`;
  const uptimeMs = (s.uptime_ms ?? 0) + tick * 0; // tick triggers re-render only
  const bootUnix = Date.now() - uptimeMs;

  return (
    <main>
      <h1>Admin</h1>
      <div class="subtitle">Device info, settings, OTA, and maintenance. All saves write NVS and reboot.</div>

      <Section title="Device">
        <div class="spec-row"><span class="key">Name</span><span class="val">{s.device}</span></div>
        <div class="spec-row"><span class="key">Version</span><span class="val">{s.version ?? "--"}</span></div>
        <div class="spec-row"><span class="key">Runtime</span><span class="val">{s.cn105.transport === "real" ? "Real CN105" : "Mock CN105"}</span></div>
        <div class="spec-row"><span class="key">Boot Time</span><span class="val">{formatBootTime(bootUnix)}</span></div>
        <div class="spec-row"><span class="key">Uptime</span><span class="val">{formatUptime(uptimeMs + (Date.now() - bootUnix - uptimeMs))}</span></div>
        <div class="spec-row"><span class="key">WiFi</span><span class="val">{wifiInfo}</span></div>
        <div class="spec-row"><span class="key">MAC</span><span class="val">{s.wifi.mac ?? "--"}</span></div>
        <div class="spec-row"><span class="key">Storage</span><span class="val">{s.filesystem.used_bytes + " / " + s.filesystem.total_bytes + " bytes"}</span></div>
      </Section>

      <div id="homekit">
        <Section title="HomeKit">
          <div class="homekit-summary">
            <div class="homekit-info">
              <div class="spec-row"><span class="key">Status</span><span class="val">{s.homekit.started ? "Started" : "Not started"}</span></div>
              <div class="spec-row"><span class="key">Paired</span><span class="val">{s.homekit.paired_controllers}</span></div>
              <div class="spec-row"><span class="key">Model</span><span class="val">{s.homekit.model ?? "--"}</span></div>
              <div class="spec-row"><span class="key">Firmware</span><span class="val">{s.homekit.firmware_revision ?? "--"}</span></div>
              <div class="spec-row homekit-mobile-action"><span class="key">Pair Code</span><span class="val">{formatHomeKitCode(s.homekit.setup_code)}</span></div>
            </div>
            <HomeKitPairingTile setupCode={s.homekit.setup_code} setupPayload={s.homekit.setup_payload} />
          </div>
        </Section>
      </div>

      <Section title="CN105 Link" action={<Btn compact onClick={refreshTransport}>Refresh</Btn>}>
        <pre>{renderTransport(transport)}</pre>
      </Section>

      <div style={{ display: "none" }}>
        <Section title="Automation" action={<Btn compact disabled={automationBusy} onClick={refreshAutomation}>Refresh</Btn>}>
          <div class="subtitle">Tiny Lua hooks that run on real state changes. Save, enable, then test by changing the AC from HomeKit, WebUI, the Mitsubishi remote, or the indoor unit.</div>
          <div class="spec-row"><span class="key">State</span><span class="val">{automation?.enabled ? "Enabled" : "Disabled"}</span></div>
          <div class="spec-row"><span class="key">Script</span><span class="val">{automation?.script_exists ? `${automation.script_size} / ${automation.script_max_bytes} bytes` : "Not saved"}</span></div>
          <div class="spec-row"><span class="key">Last Hook</span><span class="val">{automation?.last_set ? (automation.last_hook || "--") : "None yet"}</span></div>
          <div class="spec-row"><span class="key">Last Result</span><span class={"val " + (automation?.last_set ? (automation.last_ok ? "on" : "text-bad") : "off")}>{automation?.last_set ? (automation.last_ok ? "OK" : "Error") : "--"}</span></div>
          <textarea
            spellcheck={false}
            autocomplete="off"
            autocapitalize="off"
            value={automationScript}
            style={{ minHeight: "300px", marginTop: "14px" }}
            onInput={(e) => {
              setAutomationScript((e.target as HTMLTextAreaElement).value);
              setAutomationDirty(true);
            }}
          />
          <div class="btns">
            <Btn variant="primary" disabled={automationBusy || !automationDirty} onClick={saveAutomation}>{automationDirty ? "Save Script *" : "Save Script"}</Btn>
            <Btn disabled={automationBusy || automationDirty || !automation?.script_exists} onClick={() => toggleAutomation(!automation?.enabled)}>{automation?.enabled ? "Disable" : "Enable"}</Btn>
            <Btn disabled={automationBusy} onClick={() => {
              setAutomationScript(DEFAULT_AUTOMATION_SCRIPT);
              setAutomationDirty(true);
              setAutomationMsg("Template restored locally. Click Save Script to write it.");
            }}>Template</Btn>
            <Btn variant="danger" disabled={automationBusy || !automation?.script_exists} onClick={deleteAutomation}>Delete</Btn>
          </div>
          {automationMsg && <div style={{ marginTop: "10px", fontSize: "13px", color: automationMsg.includes("failed") || automationMsg.includes("Failed") ? "var(--bad)" : "var(--accent)" }}>{automationMsg}</div>}
          <div class="subtitle" style={{ marginTop: "14px" }}>Runtime Log</div>
          <pre>{renderAutomationLog(automation)}</pre>
        </Section>
      </div>

      <Section title="BLE Provisioning">
        <div class="subtitle">Hold the {s.board?.name ?? "Kiri Bridge"} button (GPIO{s.provisioning.button_gpio ?? s.board?.defaults?.provisioning_button_gpio ?? 39}) for 3 seconds to open BLE provisioning for 5 minutes.</div>
        <div class="spec-row"><span class="key">State</span><span class="val">{provisioningStage(s.provisioning)}</span></div>
        <div class="spec-row"><span class="key">Service</span><span class="val">{s.provisioning.service_name ?? `GPIO${s.provisioning.button_gpio ?? s.board?.defaults?.provisioning_button_gpio ?? 39}`}</span></div>
        <div class="spec-row"><span class="key">Time Left</span><span class="val">{s.provisioning.active ? formatDuration(s.provisioning.remaining_ms ?? 0) : "--"}</span></div>
        <div class="spec-row"><span class="key">Last Result</span><span class="val">{s.provisioning.last_result ?? "--"}</span></div>
        <div class="spec-row"><span class="key">Pending WiFi</span><span class="val">{s.provisioning.pending_ssid ?? "--"}</span></div>
      </Section>

      <Section title="Settings">
        <div class="subtitle">Saving reboots the device.</div>
        <div class="grid2">
          <Field label="Device Name"><input type="text" maxLength={63} value={settings["cfg-device-name"]} onInput={(e) => update("cfg-device-name", (e.target as HTMLInputElement).value)} placeholder="Kiri Bridge" /></Field>
          <Field label="WiFi SSID"><input type="text" maxLength={32} value={settings["cfg-wifi-ssid"]} onInput={(e) => update("cfg-wifi-ssid", (e.target as HTMLInputElement).value)} placeholder="YOUR_WIFI_SSID" /></Field>
          <Field label="WiFi Password"><input type="password" maxLength={64} value={settings["cfg-wifi-password"]} onInput={(e) => update("cfg-wifi-password", (e.target as HTMLInputElement).value)} placeholder={s.config?.wifi_password_set ? "Already set; leave blank to keep" : "Not set"} /></Field>
          <Field label="HomeKit Pairing Code"><input type="text" maxLength={9} value={settings["cfg-homekit-code"]} onBlur={(e) => update("cfg-homekit-code", normalizeHomeKitCode((e.target as HTMLInputElement).value))} onInput={(e) => update("cfg-homekit-code", (e.target as HTMLInputElement).value)} placeholder="1234-5678" /></Field>
          <Field label="HomeKit Setup ID"><input type="text" maxLength={4} value={settings["cfg-homekit-setup-id"]} onInput={(e) => update("cfg-homekit-setup-id", (e.target as HTMLInputElement).value)} placeholder="DKT1" /></Field>
          <Field label="HomeKit Manufacturer"><input type="text" maxLength={63} value={settings["cfg-homekit-manufacturer"]} onInput={(e) => update("cfg-homekit-manufacturer", (e.target as HTMLInputElement).value)} placeholder="dkt smart home" /></Field>
          <Field label="HomeKit Model"><input type="text" maxLength={63} value={settings["cfg-homekit-model"]} onInput={(e) => update("cfg-homekit-model", (e.target as HTMLInputElement).value)} placeholder="Kiri Bridge" /></Field>
          <Field label="HomeKit Serial"><input type="text" maxLength={63} value={settings["cfg-homekit-serial"]} onInput={(e) => update("cfg-homekit-serial", (e.target as HTMLInputElement).value)} placeholder="KIRI-BRIDGE" /></Field>
          <Field label="HomeKit Display">
            <select value={settings["cfg-homekit-separate-airflow-tile"]} onChange={(e) => update("cfg-homekit-separate-airflow-tile", (e.target as HTMLSelectElement).value)}>
              <option value="0">Single AC tile</option>
              <option value="1">Separate airflow tile</option>
            </select>
          </Field>
          <Field label="AC Capabilities">
            <button class="btn config-summary" type="button" onClick={openCapabilities} style={{ width: "100%", justifyContent: "flex-start", textTransform: "none", letterSpacing: ".04em", fontSize: "13px" }}>
              <span class="config-summary-text">{acCapabilitiesSummary(settings["cfg-ac-capabilities"])}</span>
            </button>
          </Field>
          <Field label="Advanced HomeKit Mapping">
            <button class="btn config-summary" type="button" disabled={!separateHomeKitDisplay} onClick={openMapping} style={{ width: "100%", justifyContent: "flex-start", textTransform: "none", letterSpacing: ".04em", fontSize: "13px" }}>
              <span class="config-summary-text">{separateHomeKitDisplay ? homeKitMappingSummary(settings["cfg-homekit-advanced-mapping"]) : "Requires Separate airflow tile"}</span>
            </button>
            {!separateHomeKitDisplay && <span class="field-hint">Select Separate airflow tile above to configure extra tilt and Swing tiles.</span>}
          </Field>
          <Field label="Status LED GPIO" help="GPIO pin used for the onboard status LED. On Atom Lite this is usually GPIO27; change only if you are using different hardware.">
            <input type="number" min={0} step={1} value={settings["cfg-led-pin"]} onInput={(e) => update("cfg-led-pin", (e.target as HTMLInputElement).value)} />
          </Field>
          <Field label="Log Level">
            <select value={settings["cfg-log-level"]} onChange={(e) => update("cfg-log-level", (e.target as HTMLSelectElement).value)}>
              <option value="error">error</option><option value="warn">warn</option><option value="info">info</option><option value="debug">debug</option><option value="verbose">verbose</option>
            </select>
          </Field>
          <Field label="On Polling (ms)" help="How often Kiri queries CN105 while the AC is on. Lower values update HomeKit/Web UI faster, but create more CN105 traffic.">
            <input type="number" min={1000} step={1000} value={settings["cfg-poll-active"]} onInput={(e) => update("cfg-poll-active", (e.target as HTMLInputElement).value)} />
          </Field>
          <Field label="Off Polling (ms)" help="How often Kiri queries CN105 while the AC is off. Usually this can be slower because the indoor unit state changes less often.">
            <input type="number" min={5000} step={1000} value={settings["cfg-poll-off"]} onInput={(e) => update("cfg-poll-off", (e.target as HTMLInputElement).value)} />
          </Field>
          <Field label="CN105 Mode">
            <select value={settings["cfg-cn105-mode"]} onChange={(e) => update("cfg-cn105-mode", (e.target as HTMLSelectElement).value)}>
              <option value="real">Real CN105</option><option value="mock">Mock</option>
            </select>
          </Field>
          <Field label="CN105 Advanced">
            <button class={"btn config-summary " + (advancedDirty ? "dirty" : "")} type="button" onClick={openCn105} style={{ width: "100%", justifyContent: "flex-start", textTransform: "none", letterSpacing: ".04em", fontSize: "13px" }}>
              <span class="config-summary-text">{(advancedDirty ? "* " : "") + cn105Summary(settings)}</span>
            </button>
          </Field>
        </div>
        {homeKitPresentationChanged && (
          <div class="danger-banner" style={{ marginTop: "14px" }}>
            <strong>HomeKit re-add required</strong>
            After saving this HomeKit presentation change, remove this accessory from Apple Home and add it again so the Home app reloads the tile and mode list. Do not use Reset HomeKit on Kiri Bridge.
          </div>
        )}
        <div class="btns">
          <Btn variant="primary" disabled={!settingsDirty} onClick={saveConfig}>{settingsDirty ? "Save and Reboot *" : "Save and Reboot"}</Btn>
        </div>
      </Section>

      <Section title="OTA Update">
        <div class="subtitle">Choose a versioned <code>.kiri</code> firmware package from the Kiri Bridge release. The browser checks the package before uploading the OTA app image.</div>
        <input ref={otaInputRef} class="file-input-hidden" type="file" accept=".kiri" disabled={otaUploading} onChange={(e) => {
          const f = (e.target as HTMLInputElement).files?.[0];
          (e.target as HTMLInputElement).value = "";
          if (f) uploadOta(f);
        }} />
        <button class="btn primary file-pick-button" type="button" disabled={otaUploading} onClick={() => otaInputRef.current?.click()}>
          {otaUploading ? "Uploading…" : "Choose Firmware"}
        </button>
        {otaShowProgress && <progress value={otaProgress} max={100} style={{ marginTop: "12px" }} />}
        {otaMsg && <div style={{ marginTop: "10px", fontSize: "13px", color: otaMsgError ? "var(--bad)" : "var(--accent)" }}>{otaMsg}</div>}
      </Section>

      <Section title="Maintenance">
        <div class="subtitle">Re-pair HomeKit, clear local data, or reboot.</div>
        <div class="btns">
          <Btn onClick={reboot}>Reboot</Btn>
          <Btn variant="danger" onClick={() => maintenance(api.resetHomeKit, "Reset HomeKit", "Reset HomeKit? This clears pairings and reboots the device.")}>Reset HomeKit</Btn>
          <Btn variant="danger" onClick={() => maintenance(api.clearSpiffs, "Clear SPIFFS", "Clear all SPIFFS data? Logs and uploaded files will be deleted.")}>Clear SPIFFS</Btn>
          <Btn variant="danger" onClick={openNvs}>Edit NVS</Btn>
        </div>
        {maintMsg && <div style={{ marginTop: "10px", fontSize: "13px", color: "var(--accent)", whiteSpace: "pre-wrap" }}>{maintMsg}</div>}
      </Section>

      <OtaConfirmModal
        result={otaModalState}
        applying={otaApplying}
        status={otaApplyStatus || undefined}
        subtitle="Upload complete. Confirm to reboot into the new partition."
        onConfirm={confirmOta}
        onCancel={() => { setOtaModalState(null); setOtaMsg(""); setOtaApplyStatus(""); }}
      />

      {/* CN105 advanced modal */}
      <Modal
        open={cn105Open}
        onClose={() => closeCn105(false)}
        title="CN105 Advanced"
        subtitle="Serial line settings to the indoor unit. Confirm only updates the local draft; click Save and Reboot afterward."
        size="wide"
      >
        <div class="danger-banner"><strong>Dangerous</strong>If CN105 works now, don't change these unless you're debugging hardware.</div>
        <div class="grid2">
          <Field label="RX GPIO"><input type="number" min={0} step={1} value={settings["cfg-cn105-rx-pin"]} onInput={(e) => update("cfg-cn105-rx-pin", (e.target as HTMLInputElement).value)} /></Field>
          <Field label="TX GPIO"><input type="number" min={0} step={1} value={settings["cfg-cn105-tx-pin"]} onInput={(e) => update("cfg-cn105-tx-pin", (e.target as HTMLInputElement).value)} /></Field>
          <Field label="Baud Rate">
            <select value={settings["cfg-cn105-baud"]} onChange={(e) => update("cfg-cn105-baud", (e.target as HTMLSelectElement).value)}>
              <option value="2400">2400</option><option value="4800">4800</option><option value="9600">9600</option>
            </select>
          </Field>
          <Field label="Data Bits"><select value={settings["cfg-cn105-data-bits"]} onChange={(e) => update("cfg-cn105-data-bits", (e.target as HTMLSelectElement).value)}><option value="8">8</option></select></Field>
          <Field label="Parity">
            <select value={settings["cfg-cn105-parity"]} onChange={(e) => update("cfg-cn105-parity", (e.target as HTMLSelectElement).value)}>
              <option value="E">Even</option><option value="N">None</option><option value="O">Odd</option>
            </select>
          </Field>
          <Field label="Stop Bits"><select value={settings["cfg-cn105-stop-bits"]} onChange={(e) => update("cfg-cn105-stop-bits", (e.target as HTMLSelectElement).value)}><option value="1">1</option><option value="2">2</option></select></Field>
          <Field label="RX Pullup"><select value={settings["cfg-cn105-rx-pullup"]} onChange={(e) => update("cfg-cn105-rx-pullup", (e.target as HTMLSelectElement).value)}><option value="1">On</option><option value="0">Off</option></select></Field>
          <Field label="TX Open Drain"><select value={settings["cfg-cn105-tx-open-drain"]} onChange={(e) => update("cfg-cn105-tx-open-drain", (e.target as HTMLSelectElement).value)}><option value="0">Off</option><option value="1">On</option></select></Field>
        </div>
        <div class="modal-actions">
          <Btn variant="primary" onClick={() => closeCn105(true)}>Confirm</Btn>
          <Btn onClick={() => closeCn105(false)}>Cancel</Btn>
        </div>
      </Modal>

      {/* AC capabilities modal */}
      <Modal
        open={capabilitiesOpen}
        onClose={() => closeCapabilities(false)}
        title="AC Capabilities"
        subtitle="Describe what this indoor unit supports. Kiri hides unsupported Web UI controls and advertises selected HomeKit modes."
        size="wide"
      >
        <div class="danger-banner">
          <strong>Apple Home cache</strong>
          If you change HomeKit HVAC modes, save and reboot first, then remove and re-add the accessory in Apple Home. Airflow-only changes just update the Web UI. You do not need to reset HomeKit inside Kiri Bridge.
        </div>
        <div class="subtitle" style={{ marginTop: "14px" }}>HomeKit HVAC Modes</div>
        <div class="grid2">
          {HVAC_TARGET_OPTIONS.map((option) => {
            const checked = (currentAcCapabilities & option.bit) !== 0;
            const disabled = checked && (currentAcCapabilities & AC_CAP_TARGET_MASK) === option.bit;
            return (
              <Field key={option.value} label={option.label}>
                <label style={{ display: "flex", alignItems: "center", gap: "10px", minHeight: "42px" }}>
                  <input
                    type="checkbox"
                    checked={checked}
                    disabled={disabled}
                    onChange={() => toggleHvacMode(option.bit)}
                  />
                  <span>{option.detail}</span>
                </label>
              </Field>
            );
          })}
        </div>
        <div class="subtitle" style={{ marginTop: "14px" }}>Airflow Controls</div>
        <div class="grid2">
          <Field label="Up/Down Airflow">
            <label style={{ display: "flex", alignItems: "center", gap: "10px", minHeight: "42px" }}>
              <input
                type="checkbox"
                checked={(currentAcCapabilities & AC_CAP_UP_DOWN_AIRFLOW) !== 0}
                onChange={() => toggleCapability(AC_CAP_UP_DOWN_AIRFLOW)}
              />
              <span>Horizontal flap control in the Web UI</span>
            </label>
          </Field>
          <Field label="Left/Right Airflow">
            <label style={{ display: "flex", alignItems: "center", gap: "10px", minHeight: "42px" }}>
              <input
                type="checkbox"
                checked={(currentAcCapabilities & AC_CAP_LEFT_RIGHT_AIRFLOW) !== 0}
                onChange={() => toggleCapability(AC_CAP_LEFT_RIGHT_AIRFLOW)}
              />
              <span>Vertical vane control in the Web UI</span>
            </label>
          </Field>
        </div>
        <div class="subtitle" style={{ marginTop: "14px" }}>
          Target values: {hvacModeSummary(currentAcCapabilities)}
        </div>
        <div class="subtitle" style={{ marginTop: "8px" }}>
          Current values exposed: {hvacCurrentSummary(currentAcCapabilities)}
        </div>
        <div class="subtitle" style={{ marginTop: "8px" }}>
          Web airflow controls: {airflowSummary(currentAcCapabilities)}
        </div>
        <div class="modal-actions">
          <Btn variant="primary" onClick={() => closeCapabilities(true)}>Confirm</Btn>
          <Btn onClick={() => closeCapabilities(false)}>Cancel</Btn>
        </div>
      </Modal>

      {/* Advanced HomeKit mapping modal */}
      <Modal
        open={mappingOpen}
        onClose={() => closeMapping(false)}
        title="Advanced HomeKit Mapping"
        subtitle="Add optional HomeKit services for each airflow axis. With no options selected, Separate airflow tile behaves exactly like the legacy implementation."
        size="wide"
      >
        <div class="danger-banner">
          <strong>Apple Home cache</strong>
          Adding or removing these services may require removing and re-adding Kiri Bridge in Apple Home after Save and Reboot. Do not use Reset HomeKit inside Kiri Bridge.
        </div>
        <div class="grid2" style={{ marginTop: "14px" }}>
          {HOMEKIT_MAPPING_OPTIONS.map((option) => (
            <Field key={option.bit} label={option.label}>
              <label style={{ display: "flex", alignItems: "flex-start", gap: "10px", minHeight: "58px" }}>
                <input
                  type="checkbox"
                  checked={(currentHomeKitMapping & option.bit) !== 0}
                  onChange={() => toggleHomeKitMapping(option.bit)}
                />
                <span>{option.detail}</span>
              </label>
            </Field>
          ))}
        </div>
        <div class="info-banner" style={{ marginTop: "18px" }}>
          <strong>Original Swing control</strong>
          {(currentHomeKitMapping & (HK_MAP_UP_DOWN_SWING | HK_MAP_LEFT_RIGHT_SWING)) === (HK_MAP_UP_DOWN_SWING | HK_MAP_LEFT_RIGHT_SWING)
            ? "Both dedicated Swing switches are enabled, so the original Swing control is removed from the AC/Airflow service."
            : (currentHomeKitMapping & HK_MAP_UP_DOWN_SWING) !== 0
              ? "The original Swing control remains visible, but it controls only Left/Right Swing when that airflow axis is supported. Up/Down Swing belongs exclusively to its dedicated switch."
              : (currentHomeKitMapping & HK_MAP_LEFT_RIGHT_SWING) !== 0
                ? "The original Swing control remains visible, but it controls only Up/Down Swing when that airflow axis is supported. Left/Right Swing belongs exclusively to its dedicated switch."
                : "No dedicated Swing switches are enabled. The original Swing control continues to operate every supported airflow axis."}
        </div>
        <div class="modal-actions">
          <Btn variant="primary" onClick={() => closeMapping(true)}>Confirm</Btn>
          <Btn onClick={() => closeMapping(false)}>Cancel</Btn>
        </div>
      </Modal>

      {/* NVS editor modal */}
      <Modal
        open={nvsOpen}
        onClose={() => !nvsBusy && setNvsOpen(false)}
        title="Edit NVS"
        subtitle={<>Editing the <code>device_cfg</code> namespace directly. Bad values can brick the device. Cancel writes nothing.</>}
        size="wide"
      >
        <div class="danger-banner"><strong>High risk</strong>If the device is working now, don't edit raw NVS unless you're prepared to recover over USB.</div>
        <textarea class="nvs-editor" spellcheck={false} autocomplete="off" autocapitalize="off" value={nvsBody} onInput={(e) => setNvsBody((e.target as HTMLTextAreaElement).value)} />
        {nvsMsg && <div style={{ marginTop: "10px", fontSize: "13px", color: "var(--accent)" }}>{nvsMsg}</div>}
        <div class="modal-actions">
          <Btn variant="danger" disabled={nvsBusy} onClick={saveNvs}>Save and Reboot</Btn>
          <Btn disabled={nvsBusy} onClick={() => setNvsOpen(false)}>Cancel</Btn>
        </div>
      </Modal>

      {/* Notice / restart modal */}
      <Modal
        open={notice !== null}
        onClose={() => setNotice(null)}
        title={notice?.title ?? "Action"}
        size="narrow"
        actions={<Btn onClick={() => setNotice(null)}>OK</Btn>}
      >
        <div class="subtitle">{notice?.body ?? ""}</div>
      </Modal>
      <RebootingModal open={rebooting} />
    </main>
  );
}
