# SpookIO

Firmware for the Waveshare ESP32-S3-ETH-8DI-8RO-C industrial controller. It provides configurable digital-input OSC output and relay control through OSC, Art-Net, or sACN.

## Features

- Eight debounced digital inputs with configurable Open/Closed OSC messages.
- One configurable message per input state, with one-shot or repeat delivery.
- IPv4 and optional discovered QLab targets; UDP or OSC 1.1 SLIP TCP output.
- Eight relays, each independently controlled by OSC, Art-Net, or sACN.
- Three fixed OSC rules per relay—OFF, ON, and PULSE—with configurable matching and pulse duration.
- Art-Net UDP `6454` and sACN UDP `5568` input.
- Configurable DMX universe/channel and ON/OFF hysteresis thresholds.
- DMX packet-loss timeout, defaulting to two seconds and turning DMX relays OFF.
- HTTP configuration page at `http://<device-ip>/` on port 80.
- OSC 1.1 SLIP TCP input alongside UDP, and browser firmware uploads over Ethernet.
- Versioned NVS persistence, validation, live status, test controls, and restore defaults.

## Default OSC Commands

The default configuration preserves the original relay commands:

```text
/relay/1 0  -> relay 1 OFF
/relay/1 1  -> relay 1 ON
/relay/1 2  -> relay 1 PULSE for one second
```

The default OSC listener uses TCP and UDP port `53000`. The web interface can change that port and all relay/input routing rules.

For TCP input, configure QLab (or another sender) for **OSC 1.1 / SLIP**,
matching the linear actuator controller. Frames begin and end with `0xC0`;
payload `0xC0` and `0xDB` bytes are escaped as `0xDB 0xDC` and `0xDB 0xDD`.
TCP input accepts two simultaneous clients, up to 1024 decoded bytes per
message, and closes connections with an incomplete frame after two seconds.
Malformed or oversized SLIP frames are discarded through the next END byte.
Disconnects, Ethernet loss, port changes, and restored defaults reset TCP input.
Length-prefixed OSC 1.0 TCP is no longer used. Both TCP input and DIN TCP output
use OSC 1.1 SLIP, including Send test and discovered QLab targets. UDP remains
unchanged. Existing TCP output receivers must support SLIP.

## Web Configuration

Open the device IP address in a browser. The page provides human-oriented forms for global settings, digital inputs, and relays, with conditional fields and inline validation. A read-only JSON preview is available for inspection. Save validates and applies the complete configuration atomically. The page also provides live input/relay status, per-message tests, relay rule tests, and a confirmed restore-defaults action.

QLab discovery is optional. When enabled, the device browses `_qlab._udp`, displays discovered services through the status API, and QLab-targeted DIN messages fan out to discovered services. Targets are matched by mDNS hostname, retained through short discovery gaps, and expire after 30 seconds without a valid refresh. TCP OSC sends run in a background queue with one connection attempt and no retry. The connection timeout is 200 ms; failed sends are rate-limited in the serial log.

## Hardware

- MCU: ESP32-S3
- Ethernet: W5500
- Flash: 16 MB
- Relay expander: TCA9554 at `0x20`
- W5500 SPI: `SCK=15`, `MISO=14`, `MOSI=13`, `CS=16`
- W5500 control: `IRQ=12`, `RST=39`, PHY address `1`
- I2C: `SDA=42`, `SCL=41`
- RGB LED: GPIO `38`
- Buzzer: GPIO `46`
- Digital inputs: GPIO `4` through `11`

Digital inputs use an active-low contact convention: an input that is not connected to ground is `Open` (GPIO HIGH), while an input connected to ground is `Closed` (GPIO LOW). The public status API and web interface expose only `Open` or `Closed`; the raw GPIO level is an internal detail.

## Project Layout

```text
src/main.cpp            Protocol handlers, DIN routing, and web API
src/WS_Config.*         Persistent configuration model and validation
src/WS_ETH.*            W5500 Ethernet bring-up
src/WS_MDNS.*           Device advertisement and optional QLab discovery
src/WS_Relay.*          Relay control helpers
src/WS_TCA9554PWR.*     Relay expander access
src/WS_GPIO.*           RGB LED and buzzer helpers
boards/                 Custom board and partition definitions
```

## Building and Flashing

```bash
pio run
pio run -t upload
pio device monitor
```

The PlatformIO environment is `esp32-s3-eth-8di-8ro-c`. Dependencies include ArduinoOSC and ArduinoJson.

## Ethernet firmware updates

**Install this version once over USB with `pio run -t upload`.** This installs
`partitions.csv`, containing two 7 MiB application slots and a dedicated 64 KiB
`config` NVS partition. Uploading only an
application binary cannot migrate the previous single-slot partition layout.
The original NVS location and both application slot locations are retained.
Settings migrate from the original NVS partition into `config` on first boot,
leaving the original copy intact. The larger partition has room for both the
old configuration blob and its replacement during an atomic save. A normal
USB upload and subsequent Ethernet updates preserve saved settings. Do not
erase the flash if you want to retain settings. Devices with the earlier OTA
layout but without `config` also need one USB upload to install this table.

After USB installation, open `http://<device-ip>/`, find **Firmware update**,
and select this project's `.pio/build/esp32-s3-eth-8di-8ro-c/firmware.bin`.
The page asks for confirmation, displays upload progress, and reports the
result. A successful update reboots after the response. Upload the application
`firmware.bin`, not a bootloader, partition table, merged image, or firmware
from a different project.

The equivalent API is `PUT /api/update` with a raw `application/octet-stream`
body and a positive, exact Content-Length no larger than the inactive slot:

```bash
curl --fail-with-body -H 'Content-Type: application/octet-stream' -H 'Expect:' \
  --upload-file .pio/build/esp32-s3-eth-8di-8ro-c/firmware.bin \
  http://<device-ip>/api/update
```

Uploads stream in chunks of at most 512 bytes to the inactive slot. They abort
after 120 seconds total or five seconds without data. Chunked transfer and
multipart uploads are unsupported. HTTP serves one request at a time, so status,
configuration, and DIN polling pause during an upload. Already queued outbound
DIN sends may finish.

Before flash writes, the controller inhibits relay control, cancels pulses,
clears DMX state, discards pending relay input, and turns all eight relays off.
An unsuccessful relay-off write rejects the upload before writing firmware.
Failed, invalid, or interrupted uploads abort without rebooting or selecting
the new image. Relays successfully turned off stay off until fresh commands
arrive; previous relay states and pulses are not restored. Pending control
traffic is discarded when maintenance ends. Successful uploads hold relay
control inhibited until reboot, when relays initialize off.

Uploads have no password; use the intended show-control network. USB recovery
remains available. Automatic rollback after an installed image fails to boot
is not provided.

## Validation

Run `python3 scripts/test_native.py` for the production SLIP receiver, OSC
packet bounds checker, firmware lifecycle, and HTTP upload adapter with fake
network/flash backends under AddressSanitizer and UndefinedBehaviorSanitizer.
Tests include two TCP clients, fragmentation, escaping, coalesced frames,
malformed/oversized frames, timeout/reconnect/port reset, upload retries,
relay-off/flash/validation failures, disconnects, idle and total deadlines,
invalid upload headers, and reboot signaling only after successful validation.
The native suite also exercises configuration migration from legacy NVS,
repeated saves, and preservation of the previous settings after write failures.
Run `node scripts/test_web_ui.cjs` to check the embedded browser upload controls,
including confirmation, progress, success, failure, and connection errors.

Hardware acceptance: send `/relay/1 1`, `/relay/1 0`, and `/relay/1 2` from
QLab using TCP OSC 1.1 / SLIP and repeat via UDP. Change the listening port and
restore defaults. With a relay on and a pulse pending, upload firmware and
verify every relay turns off and stays off throughout upload. Interrupt an
upload and verify fresh commands work afterward; then complete an update,
verify reboot, and confirm saved settings survive. Perform these checks with
loads suitable for a bench test.
