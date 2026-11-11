# Qingping external room-temperature reference

Production firmware only. Open **Admin → External Room Temperature**, the
standalone section immediately above OTA Update. Changes take effect immediately
and are independent of the Settings form's Save and Reboot action.
The modal explains the feature, scans on Refresh, displays nearby readable Qingping
FDCD sensors, and binds only the device explicitly selected by the user. Reopening
shows the saved binding, latest temperature/humidity, battery, RSSI and reading age.
Unbind removes the saved address without modifying the thermometer.

- Qingping FDCD service UUID: `0000fdcd-0000-1000-8000-00805f9b34fb`.
- This UUID is common to these sensors; use the embedded device MAC as the stable
  identifier, not the Mac computer's CoreBluetooth peripheral UUID.
- Tested broadcast fixture: Qingping Temp RH Lite, model `0x10`,
  `08103f1780342d580104e3009001020155` (22.7°C, 40.0% RH, 85% battery).
- Passive BLE scanning, with a bounded cache of 16 recently discovered devices.
  No GATT connection, pairing, or changes to the sensor firmware are performed.
  Encrypted and non-FDCD formats are not supported by this decoder.
- Only the binding address is persisted (`qingping/address` in NVS). Readings and
  their timestamps are RAM-only. A new advertisement is required after binding
  and after every restart; discovery itself never enables external control.
- Fresh readings are retained for **five minutes**. An unchanged temperature
  advertisement renews freshness; CN105 keepalive transmissions do not.
- CN105 external reference (`0x07`) is resent every 20 seconds. The value is
  quantized to 0.5°C; current HomeKit temperature retains sensor 0.1°C precision.
  No humidity service is added to HomeKit. Setpoint, power and mode are untouched.
- Missing, stale, unbound, paused, or control-ineligible readings request internal
  sensor restoration. The supported injection range is 8.5–50°C. Recovery retries
  on queue-full / missing ACK, and occurs once on boot to clear residual overrides.
- Remote-reference commands use the existing UART worker and distinct request
  IDs / ACK tracking; they do not satisfy ordinary Web/Lua SET confirmations.
- WiFi BLE provisioning takes exclusive Bluetooth ownership. The scanner stops
  and deinitializes before provisioning starts, then resumes after it is stopped.

An ACK verifies delivery, not that a particular indoor unit adopted the external
reference. Actual heating/cooling and fallback behavior require testing on that
indoor unit. The firmware reports the requested command state rather than claiming
physical sensor selection has been independently confirmed.

## Local API

`GET /api/qingping` returns binding, freshness, control eligibility and discovery
data. `POST /api/qingping/scan` requests a 30-second scan. `POST /api/qingping/bind`
accepts URL-encoded `address=58:2D:34:80:17:3F` only for a recently seen device.
`DELETE /api/qingping/bind` removes the saved binding and requests restoration.
These changes are immediate and do not require Save and Reboot.

## Tests

From the project root:

```sh
clang++ -std=c++17 -Wall -Wextra -I components/qingping/include components/qingping/qingping_parser.cpp components/qingping/tests/parser_test.cpp -o /tmp/kiri-qingping-parser-test
/tmp/kiri-qingping-parser-test
clang++ -std=c++17 -Wall -Wextra -I components/qingping/tests/host_stubs -I components/core_cn105/tests/host_stubs -I components/qingping/include -I components/core_cn105/include components/qingping/tests/service_test.cpp components/qingping/qingping_parser.cpp -o /tmp/kiri-qingping-service-test
/tmp/kiri-qingping-service-test
clang++ -std=c++17 -I components/core_cn105/tests/host_stubs -I components/core_cn105/include components/core_cn105/cn105_core.cpp components/core_cn105/tests/host_test.cpp -o /tmp/kiri-cn105-host-test
/tmp/kiri-cn105-host-test
```

The service test compiles the actual production worker against host-only NVS,
clock, Bluetooth and transport fakes. It verifies explicit binding, cold-start
freshness, keepalive, exact timeout, queue-full/ACK retry, provisioning handoff,
HomeKit source selection and unbinding. It is not a radio or physical AC test.

`webui/tests/qingping.test.mjs` exercises desktop/mobile modal flows in headless
Chrome against mocked API responses. Use an existing Playwright installation,
optionally set `PLAYWRIGHT_MODULE` to its `index.mjs`, then run from `webui/`:

```sh
node tests/qingping.test.mjs
npx tsc --noEmit
```
