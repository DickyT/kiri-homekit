# Kiri Bridge Release Notes

## Current development release

This release focuses on board support, setup reliability, and HomeKit presentation.

### Highlights

- Added Atom S3 Lite support alongside Atom Lite. Build scripts now produce firmware packages for all supported boards by default.
- Added the WiFi hotspot activation path for new devices. Connect to the installer hotspot, then open `http://192.168.4.1:8080` directly to upload production firmware and finish setup.
- Updated public setup pages to recommend WiFi hotspot setup first, while keeping desktop Chrome BLE provisioning available as a fallback path.
- Clarified that the hardware kit includes one CN105 cable.

### HomeKit and Control

- Fixed airflow naming in the UI so horizontal flaps are described as up/down airflow and vertical vanes are described as left/right airflow.
- Added optional HomeKit airflow exposure through a separate Fan/Airflow service. This lets Apple Home users choose whether the AC appears as combined or separate tiles.
- Added a `HomeKit Display` setting in the Admin UI. The default remains the older single AC tile behavior; selecting `Separate airflow tile` saves the preference to NVS and applies after reboot.

### Automation

- Added a small Lua automation engine for future user-defined actions, backed by constrained RAM allocation and a small AC/KV API.
- Kept the Automation UI hidden for now while the feature is stabilized.

### Firmware Packages

- Generated `.kiri` packages remain board-specific. Choose the file matching your hardware, for example `atom-lite` or `atoms3-lite`.
- OTA and USB flashing flows continue to validate Kiri packages before writing firmware.
