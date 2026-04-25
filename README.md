# ESP32-S3-ETH-8DI-8RO-C

Firmware for the Waveshare `ESP32-S3-ETH-8DI-8RO-C` industrial controller board. The hardware includes 8 digital inputs, 8 relay outputs, Ethernet, Wi-Fi/BLE, USB, and CAN support. This project currently brings up the relay-output side of the board, initializes the W5500 Ethernet interface, and exposes relay control over OSC using TCP.

## What It Does

- Initializes the board RGB LED and buzzer.
- Initializes the TCA9554 I2C expander used to drive 8 relays.
- Runs a startup relay test across all 8 channels.
- Starts Ethernet on the `ESP32-S3-ETH-8DI-8RO-C` using the W5500.
- Builds a unique hostname from the ESP32 MAC address in the form `relay8-XXXXXX`.
- Listens for OSC messages on TCP port `53000`.
- Supports three relay actions per channel:
  - `0` = turn relay off
  - `1` = turn relay on
  - `2` = trigger relay for 1 second, then turn it back off

## OSC Control

The firmware subscribes to OSC addresses matching:

```text
/relay/*
```

The final path segment is treated as the relay number from `1` to `8`.

Examples:

```text
/relay/1  -> control relay 1
/relay/8  -> control relay 8
```

Payload values:

```text
0 = OFF
1 = ON
2 = TRIGGER for 1 second
```

When a command is received, the RGB LED briefly indicates activity:

- Red for OFF
- Green for ON
- Blue for timed trigger

## Hardware Notes

This project is configured for a custom PlatformIO board definition targeting the Waveshare `ESP32-S3-ETH-8DI-8RO-C`:

- MCU: `ESP32-S3`
- Framework: `Arduino`
- Ethernet controller: `W5500`
- Flash size: `16MB`
- USB CDC enabled on boot

Board capabilities called out by Waveshare include:

- `8` digital inputs
- `8` relay outputs
- Ethernet via `W5500`
- `Wi-Fi` and `Bluetooth LE`
- `CAN`
- USB Type-C for power, flashing, and debugging

Relevant pin assignments in the current code:

- W5500 SPI: `SCK=15`, `MISO=14`, `MOSI=13`, `CS=16`
- W5500 control: `IRQ=12`, `RST=39`, `PHY addr=1`
- I2C expander: `SDA=42`, `SCL=41`
- RGB LED: `GPIO38`
- Buzzer: `GPIO46`

In the current firmware, relay outputs are driven through a `TCA9554` I2C GPIO expander at address `0x20`. The digital input and CAN features of the board are not yet used by `src/main.cpp`.

## Project Layout

```text
src/main.cpp            Application entry point and OSC relay handling
src/WS_ETH.*            Ethernet bring-up for the W5500
src/WS_Relay.*          Relay control helpers
src/WS_TCA9554PWR.*     I2C GPIO expander access
src/I2C_Driver.*        I2C initialization and register I/O
src/WS_GPIO.*           RGB LED and buzzer helpers
platformio.ini          PlatformIO environment
boards/                 Custom board definition
```

## Building And Flashing

This project uses PlatformIO.

```bash
pio run
pio run -t upload
pio device monitor
```

The configured environment is:

```text
esp32-s3-eth-8di-8ro-c
```

The main external dependency declared in `platformio.ini` is:

- `hideakitai/ArduinoOSC`

## Startup Sequence

On boot, the firmware:

1. Starts serial output at `115200`.
2. Initializes GPIO and I2C.
3. Initializes the relay subsystem.
4. Performs a short relay test on channels 1 through 8.
5. Starts Ethernet and waits briefly for a DHCP lease.
6. Starts the OSC TCP listener on port `53000`.

If Ethernet is not ready within the initial timeout, the firmware continues running and keeps servicing Ethernet events in the main loop.

## Development Notes

- The relay control API in the board support files uses the existing `Relay_Open` / `Relay_Closs` naming from the source.
- `src/main.cpp` currently focuses on relay output control. Although the board name suggests digital inputs are available, input handling is not implemented in the current application logic.
