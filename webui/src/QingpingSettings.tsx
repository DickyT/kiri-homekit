// Kiri Bridge — https://kiri.dkt.moe — GPL-3.0
import { useEffect, useRef, useState } from "preact/hooks";
import { Section, Modal } from "./components";
interface Sensor {
  address: string; service_uuid: string; model: number; temperature_c: number;
  humidity: number; battery: number; rssi: number; age_ms: number;
}
interface SensorStatus {
  ok: boolean; error?: string; bound: boolean; address: string; online: boolean;
  received: boolean; age_ms: number; temperature_c: number; humidity: number; battery: number;
  scanning: boolean; paused: boolean; control_usable: boolean; command_pending: boolean;
  message: string; devices: Sensor[]; rssi?: number; ble_error?: number;
}
const age = (ms: number) => ms < 1000 ? "just now" : ms < 60000 ? `${Math.floor(ms / 1000)}s ago` : `${Math.floor(ms / 60000)}m ${Math.floor(ms / 1000) % 60}s ago`;
const temperature = (c: number) => `${c.toFixed(1)}°C / ${(c * 1.8 + 32).toFixed(1)}°F`;
export function QingpingSettings() {
  const [open, setOpen] = useState(false);
  const [step, setStep] = useState("loading");
  const [info, setInfo] = useState<SensorStatus | null>(null);
  const [selected, setSelected] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const generation = useRef(0);
  const busyRef = useRef(false);
  async function request(path = "/api/qingping", method = "GET", address?: string) {
    const response = await fetch(path, { method, cache: "no-store", headers: address ? { "Content-Type": "application/x-www-form-urlencoded" } : undefined,
      body: address ? new URLSearchParams({ address }) : undefined });
    const result = await response.json() as SensorStatus;
    if (!response.ok || !result.ok) throw new Error(result.error || `HTTP ${response.status}`);
    return result;
  }
  async function show() {
    const g = ++generation.current;
    setOpen(true); setStep("loading"); setError(""); setSelected("");
    try {
      const result = await request();
      if (generation.current !== g) return;
      setInfo(result); setStep(result.bound ? "paired" : "intro");
    } catch (e) { if (generation.current === g) { setError(String(e)); setStep("intro"); } }
  }
  function close() { if (busyRef.current) return; generation.current++; setOpen(false); }
  useEffect(() => {
    if (!open) return;
    let stopped = false, inflight = false;
    const timer = window.setInterval(async () => {
      if (document.hidden || !document.hasFocus() || inflight || busyRef.current) return;
      inflight = true;
      const g = generation.current;
      try { const result = await request(); if (!stopped && g === generation.current && !busyRef.current) setInfo(result); }
      catch (e) { if (!stopped && g === generation.current && !busyRef.current) setError(String(e)); }
      finally { inflight = false; }
    }, 2000);
    return () => { stopped = true; window.clearInterval(timer); };
  }, [open]);
  async function action(kind: "scan" | "bind" | "remove") {
    if (busyRef.current) return;
    busyRef.current = true; generation.current++; setBusy(true); setError("");
    try {
      const result = await request(kind === "scan" ? "/api/qingping/scan" : "/api/qingping/bind", kind === "remove" ? "DELETE" : "POST", kind === "bind" ? selected : undefined);
      setInfo(result);
      if (kind === "scan") { setStep("scan"); setSelected(""); }
      else if (kind === "bind") setStep("success");
      else { setStep("intro"); setSelected(""); }
    } catch (e) { setError(String(e)); }
    finally { busyRef.current = false; setBusy(false); }
  }
  const live = !!info?.bound && info.received;
  return <>
    <Section title="External Room Temperature">
      <div class="subtitle">Bind a Qingping BLE sensor for CN105 control and HomeKit room-temperature reporting. Binding and unbinding take effect immediately; no Save and Reboot is needed.</div>
      <div class="btns"><button type="button" class="btn primary" onClick={show}>Configure Room Sensor</button></div>
    </Section>
    <Modal open={open} onClose={close} title="Qingping Room Sensor" subtitle="Use a selected room sensor instead of the indoor unit's temperature reference." size="wide">
      {step === "loading" && <p>Loading sensor status…</p>}
      {error && <div class="danger-banner" role="alert">{error}</div>}
      {!!info?.ble_error && <div class="danger-banner" role="alert">Bluetooth scanner error: {info.ble_error}. Close the WiFi provisioning window and refresh to retry.</div>}
      {info?.paused && <div class="info-banner"><strong>WiFi provisioning in progress</strong>Sensor scanning resumes after the BLE provisioning window closes.</div>}
      {step === "intro" && <>
        <h3>01 — Find your room sensor</h3>
        <p>Place your Qingping sensor away from direct airflow and heat sources. Kiri listens to its broadcasts; no Bluetooth connection or phone pairing is needed.</p>
        <div class="info-banner"><strong>Five-minute timeout</strong>Only your selected sensor is used. After five minutes without fresh data, Kiri marks it Offline and requests the AC's internal sensor. Reception automatically resumes external control. HomeKit reports the selected sensor's temperature while it is active; humidity is not exposed.</div>
        <p>Supports readable Qingping FDCD temperature/humidity broadcasts, including the tested Temp RH Lite. Encrypted and other broadcast formats are not listed.</p>
        <div class="modal-actions"><button type="button" class="btn primary" disabled={busy || info?.paused} onClick={() => action("scan")}>{busy ? "Starting…" : "Refresh Nearby Sensors"}</button><button type="button" class="btn" onClick={close}>Cancel</button></div>
      </>}
      {step === "scan" && <>
        <h3>02 — Select one sensor</h3>
        <p>{info?.scanning ? "Scanning for 30 seconds. Readings update automatically." : "Scan finished. Refresh to scan again."} UUID is shared; the device address identifies each sensor.</p>
        <div style={{ overflowX: "auto" }}><table class="qingping-table"><thead><tr><th>Sensor / Identifier</th><th>Live Readings</th><th>Signal / Last Seen</th></tr></thead><tbody>
          {info?.devices.map(d => <tr key={d.address}><td><label style={{ display: "flex", gap: "10px" }}><input type="radio" name="qingping-sensor" checked={selected === d.address} onChange={() => setSelected(d.address)} /><span>Qingping · model 0x{d.model.toString(16)}<br /><code>{d.address}</code><br /><small>UUID: {d.service_uuid}</small></span></label></td><td>{temperature(d.temperature_c)}<br />{d.humidity.toFixed(1)}% RH<br />Battery: {d.battery >= 0 ? `${d.battery}%` : "Unknown"}</td><td>{d.rssi} dBm<br />{age(d.age_ms)}</td></tr>)}
        </tbody></table></div>
        {!info?.devices.length && <p>No supported sensor found yet. Keep it nearby and wait, or refresh.</p>}
        <div class="modal-actions"><button type="button" class="btn primary" disabled={busy || !selected || info?.paused || !info?.devices.some(d => d.address === selected && d.age_ms < 30000)} onClick={() => action("bind")}>{busy ? "Saving…" : "Bind Selected Sensor"}</button><button type="button" class="btn" disabled={busy || info?.paused} onClick={() => action("scan")}>Refresh</button><button type="button" class="btn" onClick={close}>Cancel</button></div>
      </>}
      {(step === "paired" || step === "success") && <>
        <h3>{step === "success" ? "03 — Sensor bound successfully" : "Bound Sensor"}</h3>
        <table class="qingping-table"><tbody>
          <tr><th>Device Address</th><td><code>{info?.address}</code></td></tr>
          <tr><th>Status</th><td>{info?.paused ? "Paused for WiFi provisioning" : !info?.received ? "Waiting for a fresh reading" : info.online ? "Online" : "Offline"}</td></tr>
          <tr><th>Temperature</th><td>{live && info ? temperature(info.temperature_c) : "—"}</td></tr>
          <tr><th>Humidity</th><td>{live && info ? `${info.humidity.toFixed(1)}%` : "—"}</td></tr>
          <tr><th>Battery</th><td>{live && info && info.battery >= 0 ? `${info.battery}%` : "Unknown"}</td></tr>
          <tr><th>Signal</th><td>{info?.rssi ? `${info.rssi} dBm` : "—"}</td></tr>
          <tr><th>HomeKit Temperature Source</th><td>{info?.control_usable ? "Qingping sensor" : "Indoor unit"}</td></tr>
          <tr><th>Last Temperature Received</th><td>{live && info ? age(info.age_ms) : "Not received since binding / reboot"}</td></tr>
          <tr><th>CN105 Command</th><td>{info?.message}{info?.command_pending ? " (awaiting ACK)" : ""}</td></tr>
        </tbody></table>
        <div class="info-banner"><strong>Automatic internal-sensor fallback</strong>Timeout: five minutes. Offline, unbinding, and readings outside 8.5–50°C request the AC's internal sensor. A command ACK confirms receipt, not that the unit actually adopted that sensor.</div>
        <div class="modal-actions"><button type="button" class="btn primary" onClick={close}>Done</button><button type="button" class="btn" disabled={busy} onClick={() => setStep("remove")}>Unbind Sensor</button></div>
      </>}
      {step === "remove" && <><h3>Remove this binding?</h3><p>Kiri will stop using this sensor and request the AC's internal room-temperature sensor. The thermometer itself is unchanged.</p><div class="modal-actions"><button type="button" class="btn danger" disabled={busy} onClick={() => action("remove")}>{busy ? "Removing…" : "Unbind Sensor"}</button><button type="button" class="btn" disabled={busy} onClick={() => setStep("paired")}>Keep Sensor</button></div></>}
    </Modal>
  </>;
}
