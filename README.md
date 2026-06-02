# Kiri Bridge

CN105 HomeKit controller for Mitsubishi heat pumps.

Kiri Bridge is an ESP32 firmware and hardware kit for bringing supported
Mitsubishi heat pumps into Apple HomeKit through the indoor unit's CN105 port.
It is local-first, browser-provisioned, and designed to be recoverable without
command line tools.

- Website: <https://kiri.dkt.moe>
- Wiki and setup guides: <https://kiri.dkt.moe/wiki.html>
- WiFi setup: <https://kiri.dkt.moe/ble_provisioning.html>
- Firmware recovery: <https://kiri.dkt.moe/flash.html>
- Firmware releases: <https://github.com/DickyT/kiri-homekit/releases/>
- Source: <https://github.com/DickyT/kiri-homekit>

## What It Does

- Controls supported Mitsubishi heat pumps through the CN105 connector.
- Exposes the heat pump to Apple HomeKit.
- Provides a local WebUI at `http://<device-ip>:8080/`.
- Supports browser-based BLE WiFi provisioning.
- Supports browser-based firmware recovery with `.kiri` packages.
- Runs locally without a required cloud service.

## Hardware Kit

The intended user path assumes a prebuilt Kiri Bridge kit:

- M5Stack ATOM Lite or AtomS3 Lite controller
- two CN105 cable options
- pre-flashed Kiri Bridge installer or firmware
- browser-based setup and recovery flow

Order links are not public yet. The website currently shows an order interest
modal with a contact email.

## For Buyers

If you bought a Kiri Bridge, start with the website:

- Website: <https://kiri.dkt.moe>
- Setup and recovery wiki: <https://kiri.dkt.moe/wiki.html>
- WiFi setup tool: <https://kiri.dkt.moe/ble_provisioning.html>
- USB recovery flasher: <https://kiri.dkt.moe/flash.html>

Buyer-facing setup, recovery, and USB update instructions live on the website
so the docs only need to be maintained in one place.

## Firmware Releases

Release packages are published as `.kiri` files:

- `kiri_installer_<version>.kiri`: recovery and first-time restore path.
- `kiri_bridge_<version>.kiri`: production firmware.

Users should download release packages from
<https://github.com/DickyT/kiri-homekit/releases/>. Building from source is for
developers and is not required for normal installation.

## Current Status

Kiri Bridge is actively developed. The current ESP-IDF firmware includes:

- real CN105 UART transport enabled by default
- Apple HomeKit bridge
- local WebUI
- BLE WiFi provisioning
- installer/recovery firmware
- browser flasher and provisioning pages
- SPIFFS-backed diagnostics and logs

Final long-run validation across more Mitsubishi indoor unit models is still
ongoing.

## Repository Layout

- [`site`](./site): static website pages for `kiri.dkt.moe`
- [`main`](./main): production ESP-IDF app entrypoint
- [`components`](./components): firmware components
- [`debug_apps/cn105_probe`](./debug_apps/cn105_probe): installer/probe firmware
- [`partitions.csv`](./partitions.csv): 4MB flash partition table
- [`build.py`](./build.py): project build/export helper
- [`PLAN.md`](./PLAN.md): current engineering notes
- [`CODEX_GUIDE.md`](./CODEX_GUIDE.md): local development guide
- [`original_version`](./original_version): reference implementation submodule
- [`external/esp-homekit-sdk`](./external/esp-homekit-sdk): HomeKit SDK submodule

## Development Notes

This project targets ESP-IDF and Espressif ESP HomeKit SDK. The production
firmware and installer firmware intentionally have separate WebUIs today:

- production WebUI lives in `components/web/pages`
- installer WebUI is embedded in `debug_apps/cn105_probe/main/app_main.cpp`

When changing settings, OTA, HomeKit pairing, safety copy, or CN105 behavior,
check whether both UIs need the same behavior.

## Support

For installation and hardware questions, see [SUPPORT.md](./SUPPORT.md).

For bug reports, please use GitHub Issues and include firmware version, hardware
details, Mitsubishi indoor unit model if known, and logs when available.

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](./CONTRIBUTING.md)
before opening a pull request.

## License

Kiri Bridge firmware source is licensed under the GNU General Public License
version 3. See [LICENSE](./LICENSE).
