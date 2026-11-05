# Glaze documentation

Start with the [top-level README](../README.md) for building and running.

## Hardware

- [Built-in piezo buzzer](hardware/BUZZER.md)
- [LCD startup and physical display controls](hardware/DISPLAY.md)
- [Battery, charging, idle power and power off](hardware/POWER.md)
- [Physical NAND artifacts and recovery](hardware/PHYSICAL_NAND.md)
- [BE-300 NAND boot initialization audit](hardware/NAND_BOOT_AUDIT.md)
- [Masked ROM handoff and protected boot content](hardware/NAND_ROM_HANDOFF.md)
- [USB development on the BE-300](hardware/USB.md)
- [Networking through the USB cable](hardware/USB_NETWORK.md)
- [Wi-Fi and network security](hardware/WIFI.md)
- [CF Bluetooth discovery and serial](hardware/BLUETOOTH.md)
- [Veo Photo Traveler CF camera](hardware/VEO.md)
- [Ambicom DGCAM-CF camera storage](hardware/AMBICOM.md)
- [WL54-CF wireless networking](hardware/WL54CF.md)
- [Eye-Fi X2 diagnostics and networking feasibility](hardware/EYEFI.md)

## Operating system

- [Loadable drivers and hardware-dependent services](os/DRIVERS_SERVICES.md)
- [Native shared libraries](os/DYNAMIC_LIBRARIES.md)
- [Over-the-air OS updates](os/OTA.md)
- [Copy, cut and paste](os/CLIPBOARD.md)
- [Handwriting input](os/HANDWRITING.md)
- [Hard icons](os/HARD_ICONS.md)
- [Device entropy](os/ENTROPY.md)
- [Rendering and flash performance](os/PERFORMANCE.md)

## Built-in apps

- [Browser JavaScript and frames](apps/BROWSER.md) and [its Web APIs](apps/BROWSER_WEB_APIS.md)
- [Glaze App Store](apps/APP_STORE.md)
- [Task Manager](apps/TASK_MANAGER.md)
- [Original BE-300 (Windows CE) applications](apps/WINCE.md) and [launching them from NAND](apps/WINCE_NAND.md)

## Development

- [Glaze application SDK](dev/SDK.md)
- [GlazeBoot: booting Glaze from Windows CE](dev/GLAZEBOOT.md)
- [Network development debugging](dev/NETWORK_DEBUG.md)
- [Screen mirroring and remote control](dev/MIRRORING.md)

## Other repositories

[Glaze repositories](REPOS.md) maps all five, their sibling layout and build order. The console emulators and their guides are in [glaze-emulators](https://github.com/jroark/glaze-emulators),
Chat, Doom and SomaFM's in [glaze-apps](https://github.com/jroark/glaze-apps), and the
website and App Store publishing guides in [glaze-web](https://github.com/jroark/glaze-web), and the
desktop installer in [glaze-installer](https://github.com/jroark/glaze-installer).
