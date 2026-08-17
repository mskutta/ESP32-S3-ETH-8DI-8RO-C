# SpookIO

Firmware for the Waveshare ESP32-S3-ETH-8DI-8RO-C industrial controller. It provides configurable digital-input OSC output and relay control through OSC, Art-Net, or sACN.

## Features

- Eight debounced digital inputs with configurable Open/Closed OSC messages.
- One configurable message per input state, with one-shot or repeat delivery.
- IPv4 and optional discovered QLab targets; UDP or length-prefixed TCP OSC output.
- Eight relays, each independently controlled by OSC, Art-Net, or sACN.
- Three fixed OSC rules per relay—OFF, ON, and PULSE—with configurable matching and pulse duration.
- Art-Net UDP `6454` and sACN UDP `5568` input.
- Configurable DMX universe/channel and ON/OFF hysteresis thresholds.
- DMX packet-loss timeout, defaulting to two seconds and turning DMX relays OFF.
- HTTP configuration page at `http://<device-ip>/` on port 80.
- Versioned NVS persistence, validation, live status, test controls, and restore defaults.

## Default OSC Commands

The default configuration preserves the original relay commands:

```text
/relay/1 0  -> relay 1 OFF
/relay/1 1  -> relay 1 ON
/relay/1 2  -> relay 1 PULSE for one second
```

The default OSC listener uses TCP and UDP port `53000`. The web interface can change that port and all relay/input routing rules.

## Web Configuration

Open the device IP address in a browser. The page provides human-oriented forms for global settings, digital inputs, and relays, with conditional fields and inline validation. A read-only JSON preview is available for inspection. Save validates and applies the complete configuration atomically. The page also provides live input/relay status, per-message tests, relay rule tests, and a confirmed restore-defaults action.

QLab discovery is optional. When enabled, the device browses `_qlab._udp`, displays discovered services through the status API, and QLab-targeted DIN messages fan out to discovered services. Targets are matched by mDNS hostname, retained through short discovery gaps, and expire after 30 seconds without a valid refresh. TCP OSC sends run in a background queue with a single 200 ms connection/write attempt; failed sends are rate-limited in the serial log.

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
