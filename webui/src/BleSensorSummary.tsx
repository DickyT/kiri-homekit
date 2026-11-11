// Kiri Bridge — shared BLE readout for Control status and Admin device info.
import { useEffect, useState } from "preact/hooks";
import { lastFetchAt, status } from "./store";

export function BleSensorSummary({ variant = "stat" }: { variant?: "stat" | "spec-row" }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const timer = window.setInterval(() => {
      if (!document.hidden) setNow(Date.now());
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);
  const s = status.value;
  const sensor = s?.ble_sensor;
  const elapsed = lastFetchAt.value > 0 ? Math.max(0, Math.max(now, Date.now()) - lastFetchAt.value) : 0;
  const ageMs = Math.max(0, sensor?.age_ms ?? 0) + elapsed;
  const received = !!sensor?.bound && !!sensor.received;
  const connected = received && sensor.online && !sensor.paused && ageMs < (sensor.timeout_ms || 300000);
  const temperature = connected && Number.isFinite(sensor.temperature_c)
    ? `${sensor.temperature_c.toFixed(1)}°C / ${(sensor.temperature_c * 1.8 + 32).toFixed(1)}°F`
    : "Not connected";
  // Prefer the device clock; use the browser receipt time when it has no valid clock.
  const clock = (s?.server_time_unix_ms ?? 0) >= 1704067200000
    ? s!.server_time_unix_ms! : lastFetchAt.value;
  const timestamp = received && clock > 0 ? clock - sensor.age_ms : 0;
  const ageText = ageMs < 1000 ? "just now" : ageMs < 60000 ? `${Math.floor(ageMs / 1000)}s ago` : `${Math.floor(ageMs / 60000)}m ${Math.floor(ageMs / 1000) % 60}s ago`;
  const lastReceived = timestamp > 0
    ? `${new Date(timestamp).toLocaleString()} (${ageText}${connected ? "" : "; not connected"})`
    : "Not connected";
  return <>
    <div class={variant}><span class="key">BLE Sensor Temperature</span><span class="val">{temperature}</span></div>
    <div class={variant}><span class="key">Last BLE Temperature Received</span><span class="val">{lastReceived}</span></div>
  </>;
}
