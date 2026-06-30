// Kiri Bridge — Control page.
// Preserves the draft-lock behavior: while the user edits the form,
// polled status from the server does not overwrite their pending input.

import { useEffect, useState, useRef } from "preact/hooks";
import type { JSX } from "preact";
import { status, fetchStatusOnce, deviceName } from "../store";
import { Section, Field, Btn } from "../components";
import { navigate } from "../router";
import { HomeKitPairingTile } from "../HomeKitPairingTile";
import { api } from "../api";
import type { Cn105MockState } from "../types";

type FormState = {
  power: string;
  mode: string;
  temp: string;
  fan: string;
  vane: string;
  wide: string;
};

const FORM_DEFAULT: FormState = { power: "OFF", mode: "AUTO", temp: "77", fan: "AUTO", vane: "AUTO", wide: "|" };
const AC_CAP_DEFAULT = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
const AC_CAP_UP_DOWN_AIRFLOW = 1 << 3;
const AC_CAP_LEFT_RIGHT_AIRFLOW = 1 << 4;

function fromMock(m: Cn105MockState): FormState {
  return {
    power: m.power,
    mode: m.mode,
    temp: String(m.target_temperature_f),
    fan: m.fan,
    vane: m.vane,
    wide: m.wide_vane,
  };
}

function sameForm(a: FormState, b: FormState): boolean {
  return a.power === b.power && a.mode === b.mode && a.temp === b.temp &&
    a.fan === b.fan && a.vane === b.vane && a.wide === b.wide;
}

export function ControlPage(): JSX.Element {
  const [form, setForm] = useState<FormState>(FORM_DEFAULT);
  const [draftLocked, setDraftLocked] = useState(false);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState("Loading…");
  const draftRef = useRef(draftLocked);
  draftRef.current = draftLocked;

  // Keep form in sync with server unless user is editing.
  useEffect(() => {
    const s = status.value;
    if (!s) return;
    const remote = fromMock(s.cn105.mock_state);
    if (draftRef.current) {
      // Auto-clear lock once server reflects user's last commit.
      if (sameForm(form, remote)) setDraftLocked(false);
      return;
    }
    setForm(remote);
    if (!busy && msg === "Loading…") setMsg("Ready");
  }, [status.value, busy, msg]);

  function update<K extends keyof FormState>(key: K, value: string): void {
    setForm((f) => ({ ...f, [key]: value }));
    setDraftLocked(true);
    setMsg("Local draft (not sent yet)");
  }

  async function refresh(force = false): Promise<void> {
    if (busy) return;
    setMsg(force ? "Querying CN105…" : "Loading…");
    await fetchStatusOnce(force);
    setMsg(draftRef.current ? "Local draft (not sent yet)" : "Ready");
  }

  async function send(): Promise<void> {
    if (busy) return;
    const previous = form;
    setBusy(true);
    setMsg("Sending…");
    try {
      const params = new URLSearchParams();
      params.set("power", form.power);
      params.set("mode", form.mode);
      params.set("temperature_f", form.temp);
      params.set("fan", form.fan);
      if (supportsUpDownAirflow) params.set("vane", form.vane);
      if (supportsLeftRightAirflow) params.set("wide_vane", form.wide);
      const j = await api.buildSet(params);
      if (j.ok) {
        setDraftLocked(false);
        if (j.mock_state) setForm(fromMock(j.mock_state));
        setMsg("Sent");
        setTimeout(() => fetchStatusOnce(), 300);
        return;
      }
      setForm(previous);
      setDraftLocked(false);
      setMsg("Send failed: " + (j.error ?? "unknown"));
    } catch (e: any) {
      setForm(previous);
      setDraftLocked(false);
      setMsg("Send failed: " + (e?.message ?? e));
    } finally {
      setBusy(false);
    }
  }

  function openHomeKitAdmin(): void {
    navigate("admin");
    window.setTimeout(() => {
      document.getElementById("homekit")?.scrollIntoView({ behavior: "smooth", block: "start" });
    }, 0);
  }

  const s = status.value;
  const m = s?.cn105.mock_state;
  const acCapabilities = s?.config?.ac_capabilities ?? AC_CAP_DEFAULT;
  const supportsUpDownAirflow = (acCapabilities & AC_CAP_UP_DOWN_AIRFLOW) !== 0;
  const supportsLeftRightAirflow = (acCapabilities & AC_CAP_LEFT_RIGHT_AIRFLOW) !== 0;
  const wifiText = s?.wifi.connected
    ? `${s.wifi.ip ?? "--"} · ${s.wifi.rssi ?? "--"} dBm`
    : "Disconnected";
  const transportText = s?.cn105.transport === "real"
    ? (s.cn105.transport_status.connected ? "Connected" : "Connecting…")
    : "Mock";
  const hkText = s?.homekit.started
    ? `${s.homekit.paired_controllers} paired`
    : "Not started";

  return (
    <main>
      <div class="control-hero">
        <div class="control-hero-text">
          <h1>{deviceName.value}</h1>
          <div class="subtitle">Live state from the indoor unit. Set values and press Send to push them over CN105.</div>
          <Btn variant="primary" compact={false} onClick={openHomeKitAdmin} class="control-hero-pair-button">View HomeKit Pairing Code</Btn>
        </div>
        <HomeKitPairingTile setupCode={s?.homekit.setup_code} setupPayload={s?.homekit.setup_payload} />
      </div>

      <Section title="Status">
        <div class="stats">
          <div class="stat"><span class="key">Power</span><span class={"val " + (m?.power === "ON" ? "on" : "off")}>{m?.power ?? "--"}</span></div>
          <div class="stat"><span class="key">Mode</span><span class="val">{m?.mode ?? "--"}</span></div>
          <div class="stat"><span class="key">Set Temp</span><span class="val">{m ? `${m.target_temperature_f}°F` : "--"}</span></div>
          <div class="stat"><span class="key">Room Temp</span><span class="val">{m ? `${m.room_temperature_f}°F` : "--"}</span></div>
          <div class="stat"><span class="key">Fan</span><span class="val">{m?.fan ?? "--"}</span></div>
          <div class="stat"><span class="key">WiFi</span><span class="val">{wifiText}</span></div>
          <div class="stat"><span class="key">CN105</span><span class="val">{transportText}</span></div>
          <div class="stat"><span class="key">HomeKit</span><span class="val">{hkText}</span></div>
        </div>
      </Section>

      <Section title="Set">
        <div class="row">
          <Field label="Power">
            <select value={form.power} disabled={busy} onChange={(e) => update("power", (e.target as HTMLSelectElement).value)}>
              <option>OFF</option><option>ON</option>
            </select>
          </Field>
          <Field label="Mode">
            <select value={form.mode} disabled={busy} onChange={(e) => update("mode", (e.target as HTMLSelectElement).value)}>
              <option>COOL</option><option>HEAT</option><option>DRY</option><option>FAN</option><option>AUTO</option>
            </select>
          </Field>
        </div>
        <div class={supportsUpDownAirflow ? "row3" : "row"}>
          <Field label="Temperature °F">
            <input type="number" min="50" max="88" step="1" value={form.temp} disabled={busy} onInput={(e) => update("temp", (e.target as HTMLInputElement).value)} />
          </Field>
          <Field label="Fan Speed">
            <select value={form.fan} disabled={busy} onChange={(e) => update("fan", (e.target as HTMLSelectElement).value)}>
              <option>AUTO</option><option>QUIET</option><option>1</option><option>2</option><option>3</option><option>4</option>
            </select>
          </Field>
          {supportsUpDownAirflow && (
            <Field label="Up/Down Airflow (Horizontal Flap)">
              <select value={form.vane} disabled={busy} onChange={(e) => update("vane", (e.target as HTMLSelectElement).value)}>
                <option>AUTO</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>SWING</option>
              </select>
            </Field>
          )}
        </div>
        {supportsLeftRightAirflow && (
          <Field label="Left/Right Airflow (Vertical Vanes)">
            <select value={form.wide} disabled={busy} onChange={(e) => update("wide", (e.target as HTMLSelectElement).value)}>
              <option value="|">Center</option>
              <option value="<<">Far Left</option>
              <option value="<">Left</option>
              <option value=">">Right</option>
              <option value=">>">Far Right</option>
              <option value="<>">Wide</option>
              <option value="SWING">SWING</option>
            </select>
          </Field>
        )}
        <div class="btns">
          <Btn variant="primary" disabled={busy} onClick={send}>Send</Btn>
          <Btn disabled={busy} onClick={() => refresh(true)}>Refresh</Btn>
        </div>
        <pre>{msg}</pre>
      </Section>

    </main>
  );
}
