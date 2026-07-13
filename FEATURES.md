# Kiri Bridge Features

Kiri Bridge is a local-first CN105 controller for supported Mitsubishi heat
pumps and mini splits. It runs directly on the indoor unit, speaks Apple
HomeKit without another bridge, and provides its own local WebUI, setup flow,
diagnostics, OTA updater, and recovery tools.

This page describes the user-visible behavior of the current production and
installer firmware.

## Complete CN105 Control

Kiri controls more than a thermostat setpoint. The production firmware can
read and change power, operating mode, target temperature, fan speed, and both
airflow axes when the indoor unit supports them.

<details>
<summary><strong>Power, operating mode, and temperature</strong></summary>

- Turn the indoor unit on or off from Apple Home or the local WebUI.
- Select Auto, Heat, or Cool according to the configured AC capabilities.
- Read the room temperature reported by the indoor unit.
- Set the target temperature using the same CN105 connection as the original
  Mitsubishi controls.
- Cool-only and heat-only installations can hide modes the indoor unit does
  not support.

</details>

<details>
<summary><strong>Fan speed</strong></summary>

- Supports Auto, Quiet, and the discrete fan levels exposed by CN105.
- Fan changes are synchronized between Apple Home, the WebUI, and the indoor
  unit.
- The optional separate Airflow tile makes fan speed easier to use in Apple
  Home scenes and automations.

</details>

<details>
<summary><strong>Up/down airflow</strong></summary>

- Controls the horizontal flap that directs air toward the ceiling or floor.
- Supports fixed positions, Auto, and Swing when reported by the indoor unit.
- The WebUI labels the physical part and direction explicitly to avoid the
  common horizontal-flap versus vertical-airflow naming confusion.

</details>

<details>
<summary><strong>Left/right airflow</strong></summary>

- Controls motorized vertical vanes that direct air toward either side of the
  room.
- Supports fixed positions and Swing on compatible indoor units.
- Models with manual or unsupported left/right vanes can disable the feature
  in AC Capabilities. Kiri then hides the WebUI control and does not create
  incompatible HomeKit services.

</details>

## State That Stays in Sync

The indoor unit remains the source of truth. Kiri continuously reconciles its
state with CN105 instead of assuming that a command succeeded or that no one
used the original remote.

<details>
<summary><strong>Changes from the Mitsubishi remote are detected</strong></summary>

- Kiri polls the complete CN105 information set, including the state groups
  commonly identified as `0x02`, `0x03`, and `0x06`.
- Temperature, mode, fan, power, and airflow changes made with the original
  remote are reflected back into the WebUI and Apple Home.
- Separate polling intervals can be configured for an active and an inactive
  indoor unit.
- The WebUI Refresh action asks the firmware to query CN105 before returning
  the refreshed state.

</details>

<details>
<summary><strong>Commands are confirmed, not optimistically displayed</strong></summary>

- A WebUI command is sent over CN105 and followed by immediate state queries.
- The firmware retries confirmation briefly and returns as soon as the indoor
  unit reports the requested value.
- Controls remain busy while the request is in flight, preventing duplicate
  submissions.
- If confirmation fails, the WebUI reports the error and restores the previous
  displayed value instead of leaving a false success state.

</details>

<details>
<summary><strong>Only changed fields are sent</strong></summary>

- Changing the temperature does not resend mode, fan, or vane values that the
  user did not edit.
- This prevents an unsupported airflow field from blocking an otherwise valid
  temperature or power change.
- If left/right airflow is rejected, the error explains that the indoor unit
  may not support motorized vertical vanes and points to AC Capabilities.

</details>

## Native Apple Home

Kiri speaks HomeKit directly from the ESP32. There is no required Home
Assistant instance, Homebridge server, cloud account, subscription, or vendor
app in the control path.

<details>
<summary><strong>Standard climate controls</strong></summary>

- Heater Cooler service with power, current state, target mode, current
  temperature, and target temperature.
- Fan speed and Swing characteristics mapped to CN105.
- HomeKit setup QR and manual pairing code available from the local Admin page.
- Device name, manufacturer, model, serial, setup code, and setup ID can be
  configured locally.

</details>

<details>
<summary><strong>Single AC tile or separate Airflow tile</strong></summary>

- **Separate airflow tile** is the default and exposes fan speed and Swing on a
  dedicated Fan/Airflow service.
- AC power and Fan Active stay synchronized: turning either service on or off
  changes the same indoor unit power state.
- Rotation Speed and Swing are not duplicated across the Heater Cooler and Fan
  services in separate mode, keeping apps such as Eve cleaner.
- **Single AC tile** restores the older grouped presentation for users who
  prefer every control on one accessory tile.

Apple Home decides whether available services are shown together or as
separate tiles. The firmware option controls which service layout Kiri
advertises after reboot.

</details>

<details>
<summary><strong>Advertise only supported HVAC modes</strong></summary>

- AC Capabilities can advertise any valid combination of Auto, Heat, and Cool.
- Cool-only units no longer need to expose nonfunctional Heat controls.
- Changing the advertised HomeKit mode list requires Save and Reboot, followed
  by removing and re-adding the accessory in Apple Home so Home reloads its
  cached service metadata.
- A HomeKit reset inside Kiri is not required for that refresh.

</details>

<details>
<summary><strong>Advanced airflow mapping</strong></summary>

Advanced HomeKit Mapping is available when the separate Airflow presentation
is selected:

- **Up/Down Tilt** creates a named Window Covering-style service for the fixed
  position of the horizontal flap.
- **Left/Right Tilt** creates a separately named Window Covering-style service
  for motorized vertical vanes.
- **Up/Down Swing** creates a dedicated switch for horizontal-flap oscillation.
- **Left/Right Swing** creates a dedicated switch for vertical-vane
  oscillation.

When no dedicated Swing switch is enabled, the original Swing control keeps
its legacy behavior. If one axis receives its own switch, the original Swing
control continues to operate the other supported axis. If both dedicated
Swing switches are enabled, the original combined Swing control is removed to
avoid duplication.

AC Capabilities always take precedence: disabling an airflow axis prevents its
advanced HomeKit services from being created even if an old mapping bit remains
stored in NVS.

</details>

<details>
<summary><strong>Dry and Fan-Only HomeKit switches</strong></summary>

When Separate airflow tile is enabled, Advanced HomeKit Mapping can expose
CN105 modes that Apple's Heater Cooler service cannot represent:

- **Dry Mode** starts Mitsubishi DRY/dehumidify mode.
- **Fan-Only Mode** starts Mitsubishi FAN mode without requesting heating or
  cooling.
- The switches are mutually exclusive. Enabling one turns the other off.
- Turning the active switch off restores the prior Auto, Heat, or Cool mode
  and the prior power state.
- While either special mode is active, the main AC tile reports Off because
  Heater Cooler has no Dry or Fan target state. The dedicated switch and the
  Airflow tile continue to report the real operating state.

Both mappings default to enabled, but their services are created only when
Separate airflow tile is selected and the corresponding mode remains enabled
under AC Capabilities. Changing this service layout may require removing and
re-adding Kiri Bridge in Apple Home after Save and Reboot.

</details>

## Local Web Controller

Every production Kiri Bridge hosts a responsive controller at
`http://<device-ip>:8080/`. It communicates directly with the device over the
local network.

<details>
<summary><strong>Responsive control without an app</strong></summary>

- Desktop and mobile layouts for power, mode, temperature, fan, and airflow.
- Pending commands lock the relevant controls and show progress.
- Server errors are shown in the control page instead of silently replacing
  the requested value.
- Device name is used throughout the WebUI and in page titles.
- Firmware version and hardware board are shown in the interface footer.

</details>

<details>
<summary><strong>Focus-aware live refresh</strong></summary>

- Select 5-second refresh, 15-second refresh, or Off.
- The default is 15 seconds and the preference is stored in browser
  `localStorage`.
- Initial page load always fetches current state immediately.
- Background tabs and unfocused windows stop issuing periodic requests.
- Returning after a missed interval triggers an immediate refresh and starts a
  new interval.
- Polling avoids overwriting fields while the user is editing or submitting a
  control change.

</details>

<details>
<summary><strong>Device administration</strong></summary>

- Configure WiFi, device identity, HomeKit presentation, AC capabilities,
  logging, CN105 polling, LED GPIO, and advanced UART settings.
- View CN105 link health, HomeKit status, firmware version, hardware profile,
  and provisioning state.
- Save-and-reboot flows use a visible countdown instead of leaving the browser
  in an ambiguous state.

</details>

## Guided Setup

The buyer-facing setup flow assumes a preassembled Kiri Bridge kit and does not
require a command line.

<details>
<summary><strong>WiFi hotspot setup — recommended</strong></summary>

- Installer firmware starts a `PROV_KIRI_XX` WiFi hotspot.
- Connect to it and open `http://192.168.4.1:8080/` directly.
- The installer can probe CN105, configure safe hardware settings, receive WiFi
  credentials, and install the production `.kiri` package.
- Installer defaults favor recovery: real CN105 mode and conservative error
  logging are used instead of stale values from a previous installation.

</details>

<details>
<summary><strong>BLE provisioning — optional fallback</strong></summary>

- Desktop Chrome provisioning is available through the public setup page.
- Espressif's provisioning app can be used from iOS or Android.
- Security 1 with Proof of Possession protects the provisioning exchange.
- Production firmware can reopen a temporary BLE provisioning window after a
  long press of the hardware button.
- LED patterns indicate when the device is ready to receive WiFi credentials.

</details>

<details>
<summary><strong>Installer remains recoverable</strong></summary>

- Installer SoftAP and BLE provisioning remain available after a provisioning
  attempt so credentials can be replaced without reflashing first.
- The installer WebUI remains reachable on port 8080 through its hotspot.
- The installer and production firmware share the same `.kiri` OTA validation
  component, reducing differences between first install and later updates.

</details>

## Safe Firmware Updates and Recovery

Firmware is distributed as board-specific `.kiri` packages rather than
ambiguous standalone app binaries.

<details>
<summary><strong>Validated .kiri packages</strong></summary>

- The package records the board, target chip, installer/production variant,
  project identity, version, partition layout, image addresses, sizes, and
  hashes.
- The browser validates the package before upload or USB flashing.
- The device validates the OTA image again before selecting the new partition.
- Board-specific filenames make Atom Lite and AtomS3 Lite packages explicit.
- Legacy project-name transition packages are supported for the one-time move
  from older Mitsubishi firmware branding to Kiri Bridge.

</details>

<details>
<summary><strong>Local OTA updates</strong></summary>

- Upload a `.kiri` package from the local Admin page.
- Review firmware variant, version, destination partition, and warnings before
  rebooting.
- Installer and production firmware use the same confirmation component.
- The reboot screen shows a countdown and refreshes the page after the device
  is expected to return.

</details>

<details>
<summary><strong>Browser-based USB recovery</strong></summary>

- The public WebSerial flasher reserves the selected port and verifies a
  supported ESP32 bootloader before enabling firmware selection.
- It displays chip details, MAC address, package summary, and every flash region
  before writing.
- Progress is derived from actual esptool write output.
- On success, the serial port is released and the page returns to its initial
  state while preserving the diagnostic console.
- The user is asked to restart the device manually or reconnect it to the
  indoor unit, avoiding unstable automatic WebSerial reset behavior observed
  in some macOS Chrome versions.

</details>

## Diagnostics and Maintenance

Kiri includes local tools intended to make support possible without attaching
a debugger to every installed unit.

<details>
<summary><strong>Persistent logs</strong></summary>

- SPIFFS-backed logs survive a browser refresh and can capture boot failures or
  intermittent CN105 problems.
- Log writes run asynchronously so storage does not block the control path.
- View the current log, tail it live, download historical files, or delete logs
  from the WebUI.
- Storage safeguards clear or rotate data when space is exhausted.

</details>

<details>
<summary><strong>CN105 link diagnostics</strong></summary>

- Connected state and connection phase.
- Connection attempts and polling cycles.
- RX packets, RX errors, TX packets, pending SET requests, and last transport
  error.
- Manual status refresh and configurable on/off polling intervals.

</details>

<details>
<summary><strong>Maintenance and expert settings</strong></summary>

- Reboot the controller, reset WiFi, reset HomeKit, clear logs, and manage
  stored files.
- Choose the firmware log level.
- Test and configure the status LED GPIO.
- Inspect or adjust CN105 UART settings when debugging custom hardware.
- Raw NVS editing is available as an explicitly high-risk recovery tool.

</details>

## Hardware Support

- **M5Stack Atom Lite** using the ESP32-PICO-D4.
- **M5Stack AtomS3 Lite** using the ESP32-S3.
- Board profiles provide the correct CN105 pins, LED behavior, provisioning
  button, chip metadata, and package identity.
- `build.py buildall` creates production and installer packages for both boards
  in one release build.
- The Kiri Bridge hardware kit includes one CN105 cable.

## On-device Lua Automation

Kiri can run small Lua 5.4 rules directly on the controller without depending
on Apple Home automations or another always-on server. Scripts are managed in
the local Admin page; the public [Kiri Automation Editor](https://kiri.dkt.moe/automation.html)
provides examples, API completion, syntax preflight, copy, and download.
The complete versioned contract is in [AUTOMATION.md](./AUTOMATION.md).

<details>
<summary><strong>Automation API v2</strong></summary>

- Hooks for `on_state_changed`, `on_power_on`, and `on_power_off`.
- Read power, mode, target and room temperature, fan, both airflow axes,
  operating state, compressor frequency, input power, and CN105 connection
  state.
- Set power, mode, target temperature, fan, up/down airflow, left/right
  airflow, and Swing.
- Inspect device support through `kiri.capabilities` and check
  `kiri.api_version` before using future APIs.
- Store small persistent values through NVS-backed `kv.get()`, `kv.set()`, and
  `kv.delete()`.
- Write script messages into the normal Kiri diagnostic log.

The embedded Lua 5.4 runtime omits `io`, `os`, `package`, and `debug`, uses a
32 KiB memory arena, and enforces an instruction limit so a bad script cannot
grow without limit. Scripts are capped at 12 KiB and four AC actions per hook.
Parameter and device-capability checks fail immediately with a useful error.

Saving is atomic: a new script is validated before activation and the previous
valid script is retained as a last-known-good copy. Runtime protection removes
no-op actions, suppresses automation feedback loops, disables action storms,
and records the latest 12 hook runs. Real CN105 actions run outside the callback
and report their confirmation result and retry count.

Persistent automation storage is intentionally small: 16 keys and 1 KiB total.
Writing the same value again does not touch flash.

</details>

## Learn More

- Product website: <https://kiri.dkt.moe/>
- Automation editor: <https://kiri.dkt.moe/automation.html>
- Setup and recovery guides: <https://kiri.dkt.moe/wiki.html>
- Firmware releases: <https://github.com/DickyT/kiri-homekit/releases/>
- Support: [SUPPORT.md](./SUPPORT.md)
