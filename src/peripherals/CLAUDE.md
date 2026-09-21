# src/peripherals/ — Hardware Peripherals

Hardware peripheral drivers implementing `hu_peripheral_t` vtable. Supports Arduino (serial JSON), STM32/Nucleo (probe-rs), and Raspberry Pi (GPIO/SPI).

## Key Files

- `arduino.c` — Serial JSON protocol for Arduino boards
- `stm32.c` — probe-rs CLI for STM32/Nucleo flash and debug
- `rpi.c` — Raspberry Pi GPIO and SPI access
- `factory.c` — Peripheral registry and creation

## Rules

- `HU_IS_TEST`: mock all hardware I/O, no serial/GPIO access
- Non-Linux platforms must return `HU_ERR_NOT_SUPPORTED` (not silent 0)
- Use `read`/`write` vtable methods for all I/O
