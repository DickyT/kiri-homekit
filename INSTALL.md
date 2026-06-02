# Kiri Bridge Installation Guide

This guide assumes you bought a prebuilt Kiri Bridge hardware kit.

You do not need to build firmware from source. You only need a phone or
computer with Bluetooth, WiFi, and a browser.

## What You Need

- Kiri Bridge hardware kit
- Mitsubishi indoor unit with a CN105 connector
- 2.4 GHz WiFi network
- Apple Home app for HomeKit pairing
- Chrome or another Web Bluetooth capable browser for browser provisioning

The Kiri Bridge kit includes an M5Stack ATOM Lite or AtomS3 Lite controller and two CN105
cable options.

## Normal Setup

1. Plug the Kiri Bridge into USB power.
2. Wait for the LED to rapidly blink blue.
3. Open <https://kiri.dkt.moe/ble_provisioning.html>.
4. Confirm that the LED is blinking blue.
5. Select the Kiri Bridge BLE device.
6. Enter your 2.4 GHz WiFi name and password.
7. Wait for provisioning to finish.
8. Open the device portal shown by the provisioning page, usually:

```text
http://<device-ip>:8080/
```

9. Follow the WebUI pairing instructions to add Kiri Bridge to Apple Home.
10. Power down the indoor unit before connecting CN105 if your installation
    process requires it.
11. Plug the CN105 cable into the indoor unit and the Kiri Bridge.
12. Power the system back on and verify control from the WebUI and Apple Home.

## LED Hints

- Rapid blue blink: ready for BLE WiFi provisioning.
- Solid green: WiFi provisioning succeeded.
- If the LED does not blink blue after power-on, restart the device once.

## Updating After Provisioning

If your device is already in installer mode and connected to WiFi, you can use
the installer WebUI to upload a current Kiri package from GitHub Releases. This
is often easier than USB recovery flashing.

If the device is not reachable, or you want a clean recovery path, follow
[FLASHING.md](./FLASHING.md) and flash the latest installer package from
GitHub Releases.

## Troubleshooting

If BLE does not show up:

- Make sure the LED is rapidly blinking blue.
- Move the phone or computer closer to the Kiri Bridge.
- Turn Bluetooth off and on again.
- Restart the Kiri Bridge.

If WiFi provisioning fails:

- Use a 2.4 GHz WiFi network.
- Re-enter the password carefully.
- Make sure the access point is not using captive portal login.
- Try provisioning again after restarting the Kiri Bridge.

If you cannot open the device portal:

- Check the IP address shown after provisioning.
- Open `http://<device-ip>:8080/`.
- Make sure your phone or computer is on the same network.

If HomeKit pairing fails:

- Keep the Kiri Bridge and phone on the same local network.
- Restart the Kiri Bridge and try pairing again.
- If the device was paired before, remove the old accessory from Apple Home.
