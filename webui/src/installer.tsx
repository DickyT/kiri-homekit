// Kiri Bridge — installer setup SPA.
// Single self-contained bundle inlined into the installer firmware. The
// installer hotspot may not have internet access, so this imports nothing
// external at runtime.

import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import type { ComponentChildren, JSX } from "preact";
import "./styles.css";
import { validateAndUpload } from "./lib/ota";
import type { OtaUploadResult } from "./lib/ota";
import { OtaConfirmModal, RebootingModal } from "./components";

// ---------- types ----------

type InstallerStatus = {
  ok?: boolean;
  version?: string;
  wifi: { connected: boolean; ssid?: string; ip?: string; ap_ssid: string; ap_ip: string };
  provisioning: { service_name: string; security: string; pop: string };
  board?: {
    id?: string;
    name?: string;
    target?: string;
    defaults?: { led_pin?: number; rx_pin?: number; tx_pin?: number };
  };
  defaults: Record<string, string | number | boolean>;
  probe?: { ran: boolean; found: boolean; rx_pin?: number; tx_pin?: number; baud?: number };
};

type SettingsForm = Record<string, string>;

const FIELD_KEYS = [
  "device_name", "wifi_ssid", "wifi_pass",
  "hk_code", "hk_setupid", "hk_mfr", "hk_model", "hk_serial",
  "rx_pin", "tx_pin", "led_pin", "baud", "data_bits", "parity",
  "stop_bits", "rx_pull", "tx_od", "use_real",
  "poll_on", "poll_off", "log_level",
] as const;

const CN105_KEYS = ["rx_pin", "tx_pin", "baud", "data_bits", "parity", "stop_bits", "rx_pull", "tx_od"] as const;
const DEFAULTS: SettingsForm = {
  device_name: "Kiri Bridge",
  wifi_ssid: "YOUR_WIFI_SSID",
  wifi_pass: "YOUR_WIFI_PASSWORD",
  hk_code: "1111-2233",
  hk_setupid: "DKT1",
  hk_mfr: "dkt smart home",
  hk_model: "Kiri Bridge",
  hk_serial: "KIRI-BRIDGE",
  led_pin: "27",
  log_level: "error",
  poll_on: "15000",
  poll_off: "60000",
  use_real: "1",
  rx_pin: "26",
  tx_pin: "32",
  baud: "2400",
  data_bits: "8",
  parity: "E",
  stop_bits: "1",
  rx_pull: "1",
  tx_od: "0",
};

// ---------- helpers ----------

function cn105Summary(f: SettingsForm): string {
  return `${f["data_bits"]}${f["parity"]}${f["stop_bits"]} ${f["baud"]} RX G${f["rx_pin"]} TX G${f["tx_pin"]}`;
}

function randomHomeKitCode(): string {
  const b = new Uint8Array(8);
  crypto.getRandomValues(b);
  const d = Array.from(b).map((x) => String(x % 10));
  return d.slice(0, 4).join("") + "-" + d.slice(4).join("");
}

function boardDefault(status: InstallerStatus | null, key: "led_pin" | "rx_pin" | "tx_pin", fallback: string): string {
  const value = status?.board?.defaults?.[key];
  return value === undefined ? fallback : String(value);
}

// ---------- installer app ----------

function InstallerApp(): JSX.Element {
  const [form, setForm] = useState<SettingsForm>({ ...DEFAULTS });
  const [status, setStatus] = useState<InstallerStatus | null>(null);
  const [statusError, setStatusError] = useState<string | null>(null);
  const [step, setStep] = useState<1 | 2 | 3>(1);
  const [step1Out, setStep1Out] = useState("Save Step 1 to continue.");
  const [probeOut, setProbeOut] = useState("Optional test has not run.");
  const [otaOut, setOtaOut] = useState("Save Step 1, then continue from Step 2.");
  const [otaProgress, setOtaProgress] = useState<number | null>(null);
  const [otaModal, setOtaModal] = useState<OtaUploadResult | null>(null);
  const [rebooting, setRebooting] = useState(false);
  const [cn105Open, setCn105Open] = useState(false);
  const [cn105Snapshot, setCn105Snapshot] = useState<SettingsForm | null>(null);
  const rebootTimer = useRef<number | undefined>(undefined);
  const otaInputRef = useRef<HTMLInputElement>(null);

  function update(key: string, value: string): void {
    setForm((prev) => ({ ...prev, [key]: value }));
  }

  // Initial status load.
  useEffect(() => {
    fetch("/api/status")
      .then((r) => r.json())
      .then((j: InstallerStatus) => {
        setStatus(j);
        setStatusError(null);
        setForm((prev) => {
          const next = { ...prev };
          if (j.defaults) {
            for (const [k, v] of Object.entries(j.defaults)) {
              if (k in next) next[k] = String(v);
            }
            next.use_real = j.defaults.use_real ? "1" : "0";
            next.rx_pull = j.defaults.rx_pull ? "1" : "0";
            next.tx_od = j.defaults.tx_od ? "1" : "0";
          }
          if (next.hk_code === DEFAULTS.hk_code) next.hk_code = randomHomeKitCode();
          if (j.probe?.found) {
            if (j.probe.rx_pin !== undefined) next.rx_pin = String(j.probe.rx_pin);
            if (j.probe.tx_pin !== undefined) next.tx_pin = String(j.probe.tx_pin);
            if (j.probe.baud !== undefined) next.baud = String(j.probe.baud);
          }
          return next;
        });
      })
      .catch((e) => setStatusError("Load failed: " + (e?.message ?? e)));
  }, []);

  useEffect(() => () => {
    if (rebootTimer.current !== undefined) window.clearTimeout(rebootTimer.current);
  }, []);

  function buildParams(): URLSearchParams {
    const p = new URLSearchParams();
    for (const k of FIELD_KEYS) p.set(k, form[k] ?? "");
    return p;
  }

  async function ledTest(): Promise<void> {
    try {
      const j = await (await fetch("/api/led-test", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({ led_pin: form.led_pin ?? boardDefault(status, "led_pin", DEFAULTS.led_pin) }),
      })).json();
      setStep1Out(JSON.stringify(j, null, 2));
    } catch (e: any) {
      setStep1Out("LED test failed: " + (e?.message ?? e));
    }
  }

  async function saveStep1(): Promise<void> {
    const ssid = form.wifi_ssid?.trim() ?? "";
    const pass = form.wifi_pass?.trim() ?? "";
    if (!ssid || ssid === "YOUR_WIFI_SSID" || !pass || pass === "YOUR_WIFI_PASSWORD") {
      setStep1Out("Enter a real WiFi SSID and password. Don't keep the placeholder values.");
      document.getElementById("wifi_ssid")?.focus();
      return;
    }
    setStep1Out("Saving Step 1 settings to NVS…");
    try {
      const j = await (await fetch("/api/write-settings", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: buildParams(),
      })).json();
      setStep1Out(JSON.stringify(j, null, 2));
      if (j.ok) {
        setStep(2);
        setProbeOut("Step 1 saved. Run a CN105 probe, or continue to OTA.");
        setTimeout(() => document.getElementById("step2")?.scrollIntoView({ behavior: "smooth", block: "start" }), 50);
      }
    } catch (e: any) {
      setStep1Out("Save failed: " + (e?.message ?? e));
    }
  }

  async function probe(): Promise<void> {
    setProbeOut("Probing. This may take a few dozen seconds…");
    try {
      const j = await (await fetch("/api/probe", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({
          rx_pin: form.rx_pin ?? boardDefault(status, "rx_pin", DEFAULTS.rx_pin),
          tx_pin: form.tx_pin ?? boardDefault(status, "tx_pin", DEFAULTS.tx_pin),
        }),
      })).json();
      setProbeOut(JSON.stringify(j, null, 2));
      if (j.ok && j.result) update("baud", String(j.result.baud));
    } catch (e: any) {
      setProbeOut("Probe failed: " + (e?.message ?? e));
    }
  }

  function backToStep1(): void {
    setStep(1);
    setTimeout(() => document.getElementById("step1")?.scrollIntoView({ behavior: "smooth", block: "start" }), 50);
  }

  function continueToOta(): void {
    setStep(3);
    setOtaOut("Select the production .kiri firmware package. Upload starts automatically.");
    setTimeout(() => document.getElementById("step3")?.scrollIntoView({ behavior: "smooth", block: "start" }), 50);
  }

  async function uploadFirmware(file: File): Promise<void> {
    setOtaProgress(0);
    setOtaOut("Validating " + file.name + "…");
    try {
      const result = await validateAndUpload(file, {
        acceptVariants: ["app", "installer"],
        onProgress: (pct) => setOtaProgress(pct),
      });
      setOtaProgress(null);
      setOtaModal(result);
      setOtaOut("Upload complete. Confirm in the modal to reboot.");
    } catch (e: any) {
      setOtaProgress(null);
      setOtaOut("OTA upload failed: " + (e?.message ?? e));
    }
  }

  function cancelOta(): void {
    setOtaModal(null);
    setOtaOut("Upload canceled. Select the firmware again to retry.");
  }

  function applyOta(): void {
    setOtaModal(null);
    setRebooting(true);
    fetch("/api/ota/apply", { method: "POST" }).catch(() => {});
    if (rebootTimer.current === undefined) {
      rebootTimer.current = window.setTimeout(() => {
        location.href = `http://${location.hostname}:8080/`;
      }, 5000);
    }
  }

  function openCn105(): void { setCn105Snapshot({ ...form }); setCn105Open(true); }
  function closeCn105(keep: boolean): void {
    if (!keep && cn105Snapshot) {
      const restored = { ...form };
      for (const k of CN105_KEYS) restored[k] = cn105Snapshot[k] ?? "";
      setForm(restored);
    }
    setCn105Snapshot(null);
    setCn105Open(false);
  }

  // ---------- render ----------

  const wifiState = status ? (status.wifi.connected ? "ok" : "bad") : "warn";
  const wifiText = !status
    ? "checking…"
    : status.wifi.connected
      ? `${status.wifi.ssid ?? "STA"} · ${status.wifi.ip ?? "--"}`
      : "Not connected";

  return (
    <>
      <nav class="nav-bar">
        <div class="nav-inner">
          <span class="brand">
            <span class="brand-mark" aria-hidden="true" />
            Kiri Bridge<span class="brand-page">installer</span>
          </span>
        </div>
      </nav>
      <main>
        <h1>Installer<span class="accent">.</span></h1>
        <div class="subtitle">Configure WiFi and device settings, optionally probe CN105, then upload the production firmware.</div>

        <div class="status-row">
          {statusError && <span class="text-bad">{statusError}</span>}
          {!statusError && (
            <>
              <span class="stat">
                <span class={"dot " + wifiState} />
                <span class="key">Wi-Fi STA</span>
                <span class={wifiState === "ok" ? "text-ok" : (wifiState === "bad" ? "text-bad" : "text-warn")}>{wifiText}</span>
              </span>
              <span class="stat">
                <span class="dot ok" />
                <span class="key">Installer AP</span>
                <span class="text-ok">{status?.wifi.ap_ssid ?? "--"} · {status?.wifi.ap_ip ?? "--"}</span>
              </span>
              <span class="stat">
                <span class="dot ok" />
                <span class="key">BLE</span>
                <span class="text-ok">{status?.provisioning.service_name ?? "--"} · PoP {status?.provisioning.pop ?? "--"}</span>
              </span>
            </>
          )}
        </div>

        <Step n={1} title="Device settings" hint="Saved to the same NVS namespace as production firmware. Saving here doesn't reboot the installer." locked={step !== 1} id="step1">
          <div class="grid2">
            <Field label="Device Name"><input id="wifi_ssid_anchor" type="text" value={form.device_name} onInput={(e) => update("device_name", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="WiFi SSID"><input id="wifi_ssid" type="text" autocomplete="off" autocapitalize="none" spellcheck={false} value={form.wifi_ssid} onInput={(e) => update("wifi_ssid", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="WiFi Password"><input type="text" autocomplete="off" autocapitalize="none" spellcheck={false} value={form.wifi_pass} onInput={(e) => update("wifi_pass", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="HomeKit Pairing Code">
              <div class="inline-control">
                <input type="text" inputmode="numeric" autocomplete="off" value={form.hk_code} onInput={(e) => update("hk_code", (e.target as HTMLInputElement).value)} />
                <button class="btn compact" type="button" onClick={() => update("hk_code", randomHomeKitCode())}>Random</button>
              </div>
            </Field>
            <Field label="HomeKit Setup ID"><input type="text" maxLength={4} value={form.hk_setupid} onInput={(e) => update("hk_setupid", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="HomeKit Manufacturer"><input type="text" value={form.hk_mfr} onInput={(e) => update("hk_mfr", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="HomeKit Model"><input type="text" value={form.hk_model} onInput={(e) => update("hk_model", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="HomeKit Serial"><input type="text" value={form.hk_serial} onInput={(e) => update("hk_serial", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="Status LED GPIO"><input type="number" value={form.led_pin} onInput={(e) => update("led_pin", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="Log Level">
              <select value={form.log_level} onChange={(e) => update("log_level", (e.target as HTMLSelectElement).value)}>
                <option value="error">Error</option><option value="warn">Warn</option><option value="info">Info</option><option value="debug">Debug</option><option value="verbose">Verbose</option>
              </select>
            </Field>
            <Field label="On Polling (ms)"><input type="number" min={1000} step={1000} value={form.poll_on} title="How often production firmware queries CN105 while the AC is on. Lower = faster sync, more traffic." onInput={(e) => update("poll_on", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="Off Polling (ms)"><input type="number" min={5000} step={1000} value={form.poll_off} title="How often production firmware queries CN105 while the AC is off. Usually longer to avoid noise." onInput={(e) => update("poll_off", (e.target as HTMLInputElement).value)} /></Field>
            <Field label="CN105 Mode">
              <select value={form.use_real} onChange={(e) => update("use_real", (e.target as HTMLSelectElement).value)}>
                <option value="1">Real CN105</option><option value="0">Mock</option>
              </select>
            </Field>
            <Field label="CN105 Advanced">
              <button class="btn config-summary" type="button" onClick={openCn105}>{cn105Summary(form)}</button>
            </Field>
          </div>
          <div class="btns">
            <button class="btn" type="button" onClick={ledTest}>LED Test</button>
            <button class="btn primary" type="button" onClick={saveStep1}>Save and Continue →</button>
          </div>
          <pre>{step1Out}</pre>
        </Step>

        <Step n={2} title="CN105 smoke test (optional)" hint="If the AC is connected, run a safe CONNECT/INFO probe using the saved pins and baud. Skip if you don't have hardware nearby." locked={step !== 2} id="step2">
          <div class="btns">
            <button class="btn" type="button" onClick={backToStep1}>← Back</button>
            <button class="btn" type="button" onClick={probe}>Run probe</button>
            <button class="btn primary" type="button" onClick={continueToOta}>Continue →</button>
          </div>
          <pre>{probeOut}</pre>
        </Step>

        <Step n={3} title="OTA upload" hint={<>Choose the versioned production <code>.kiri</code> firmware package from the Kiri Bridge release. Upload starts on selection.</>} locked={step !== 3} id="step3">
          <input ref={otaInputRef} class="file-input-hidden" type="file" accept=".kiri" onChange={(e) => {
            const f = (e.target as HTMLInputElement).files?.[0];
            (e.target as HTMLInputElement).value = "";
            if (f) uploadFirmware(f);
          }} />
          <button class="btn primary file-pick-button" type="button" onClick={() => otaInputRef.current?.click()}>
            Choose Firmware
          </button>
          {otaProgress !== null && <progress value={otaProgress} max={100} style={{ marginTop: "12px" }} />}
          <pre>{otaOut}</pre>
        </Step>

        <footer class="site-footer">
          <div class="footer-brand">
            <span class="footer-mark" aria-hidden="true" />
            <span><strong>Kiri Bridge</strong> — open-source HomeKit controller for Mitsubishi heat pumps</span>
          </div>
          <div class="footer-meta">
            <a href="https://kiri.dkt.moe" target="_blank" rel="noopener noreferrer">kiri.dkt.moe</a>
            <span>{status?.board?.name ?? "Unknown board"} ({status?.board?.target ?? "unknown target"})</span>
            <span>v<b>{status?.version ?? "--"}</b></span>
          </div>
        </footer>
      </main>

      <OtaConfirmModal
        result={otaModal}
        onConfirm={applyOta}
        onCancel={cancelOta}
      />
      <RebootingModal open={rebooting} />

      {cn105Open && (
        <div class="modal" role="dialog" aria-modal="true">
          <div class="modal-backdrop" onClick={() => closeCn105(false)} />
          <div class="modal-panel wide">
            <div class="modal-header"><div><h2>CN105 Advanced</h2><div class="subtitle modal-subtitle">Serial line settings. Confirm only updates the local Step 1 draft; click Save and Continue afterward to write NVS.</div></div></div>
            <div class="danger-banner"><strong>Dangerous</strong>If CN105 works now, don't change these unless you're debugging hardware.</div>
            <div class="grid2">
              <Field label="RX GPIO"><input type="number" min={0} step={1} value={form.rx_pin} onInput={(e) => update("rx_pin", (e.target as HTMLInputElement).value)} /></Field>
              <Field label="TX GPIO"><input type="number" min={0} step={1} value={form.tx_pin} onInput={(e) => update("tx_pin", (e.target as HTMLInputElement).value)} /></Field>
              <Field label="Baud Rate">
                <select value={form.baud} onChange={(e) => update("baud", (e.target as HTMLSelectElement).value)}>
                  <option value="2400">2400</option><option value="4800">4800</option><option value="9600">9600</option>
                </select>
              </Field>
              <Field label="Data Bits"><select value={form.data_bits} onChange={(e) => update("data_bits", (e.target as HTMLSelectElement).value)}><option value="8">8</option></select></Field>
              <Field label="Parity">
                <select value={form.parity} onChange={(e) => update("parity", (e.target as HTMLSelectElement).value)}>
                  <option value="E">Even</option><option value="N">None</option><option value="O">Odd</option>
                </select>
              </Field>
              <Field label="Stop Bits"><select value={form.stop_bits} onChange={(e) => update("stop_bits", (e.target as HTMLSelectElement).value)}><option value="1">1</option><option value="2">2</option></select></Field>
              <Field label="RX Pullup"><select value={form.rx_pull} onChange={(e) => update("rx_pull", (e.target as HTMLSelectElement).value)}><option value="1">On</option><option value="0">Off</option></select></Field>
              <Field label="TX Open Drain"><select value={form.tx_od} onChange={(e) => update("tx_od", (e.target as HTMLSelectElement).value)}><option value="0">Off</option><option value="1">On</option></select></Field>
            </div>
            <div class="modal-actions">
              <button class="btn primary" type="button" onClick={() => closeCn105(true)}>Confirm</button>
              <button class="btn" type="button" onClick={() => closeCn105(false)}>Cancel</button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}

// ---------- step subcomponent ----------

function Step({ n, title, hint, locked, id, children }: {
  n: number; title: string; hint?: JSX.Element | string; locked: boolean; id: string;
  children: ComponentChildren;
}): JSX.Element {
  return (
    <section class={"installer-step" + (locked ? " locked" : "")} id={id}>
      <div class="installer-step-num">{String(n).padStart(2, "0")}</div>
      <div>
        <h2>{title}</h2>
        {hint && <div class="installer-step-hint">{hint}</div>}
        {children}
      </div>
    </section>
  );
}

function Field({ label, children }: { label: string; children: ComponentChildren }): JSX.Element {
  return (
    <label class="field">
      <span class="field-label">{label}</span>
      {children}
    </label>
  );
}

// ---------- mount ----------

const root = document.getElementById("app");
if (root) {
  root.textContent = "";
  render(<InstallerApp />, root);
}
