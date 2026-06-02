# Installer / Probe App

This is a standalone ESP-IDF installer firmware for first-run setup and CN105
hardware validation.

It replaces the old serial-only CN105 probe. The app now provides:

- BLE Wi-Fi provisioning through Espressif's mobile provisioning app or Web
  Bluetooth using Security 1 and PoP `abcd1234`.
- A no-password installer SoftAP on every boot. The AP SSID matches the BLE
  provisioning name, such as `PROV_KIRI_90`.
- An installer WebUI on port `8080` over SoftAP and over the provisioned STA
  network.
- CN105 auto-probing from the browser using user-selected RX/TX GPIO pins.
- LED GPIO color test for WS2812-style status LEDs.
- Full-overwrite writes to the formal firmware's `device_cfg` NVS namespace.
- OTA upload of the formal firmware app binary.

The installer uses the same OTA partition layout as the formal firmware:

- `nvs`
- `otadata`
- `phy_init`
- `ota_0`
- `ota_1`
- `spiffs`

## UI Maintenance Note

The installer WebUI is not code-level reused from the production WebUI yet.
Production UI source lives under `components/web/pages`, while this installer
UI is embedded in `main/app_main.cpp`. If you change Device Settings, CN105
settings, OTA upload/apply behavior, or safety warning copy in either place,
check whether the other UI needs the same update.

## Build And Flash

From the repository root:

```bash
./build.py --app installer build
./build.py --app installer flash-auto --monitor
```

Every successful build also exports a packaged firmware set into:

```text
firmware_exports/<version>/
```

The four flash images are renamed as:

```text
<project_name>_<version>_<0xOFFSET>.bin
```

The legacy app alias still works:

```bash
./build.py --app cn105-probe build
```

## First-Run Flow

1. Flash the installer firmware.
2. Connect either through the installer SoftAP or through BLE provisioning.
3. For SoftAP setup, join `PROV_KIRI_XX` with no password, then manually open
   `http://192.168.4.1:8080/`.
4. For BLE setup, use Espressif's mobile provisioning app or the browser BLE
   pairing page with the BLE service name printed in serial logs and Security 1.
5. The WebUI is available at `http://<installer-ip>:8080/`.
6. Run CN105 auto-probe with the physical RX/TX GPIO pins.
7. Test the status LED GPIO if needed.
8. Save step 1 so the installer writes the full `device_cfg` NVS set.
9. Optionally run the step 2 CN105 smoke test.
10. Upload the formal firmware app binary from the exported package:
   `firmware_exports/<version>/kiri_bridge_<version>_0x20000.bin`
11. Click `Reboot and Apply OTA`.

After reboot, the formal firmware reads the NVS values written by the installer
and starts normally with the same partition table.

## CN105 Probe Notes

The auto-probe walks:

- `2400 / 4800 / 9600`
- push-pull
- push-pull + RX pullup
- open-drain + RX pullup
- open-drain + TX/RX pullup
- CONNECT `0x5A` and `0x5B`

Only legal CN105 responses are treated as a positive match:

- `0x7A / 0x7B` connect acknowledgements
- `0x62` info responses
- `0x61` SET acknowledgements

Echoed request packets are counted as RX bytes but are not treated as a legal
CN105 response.
