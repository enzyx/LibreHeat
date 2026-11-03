# Development and tests

## Project layout

```text
lg-wh27s.yaml           device configuration (Wi-Fi, OTA, pins, network)
wh27s.yaml              package: all entities and controls
src/wh27s_uart.h        frame codec, CRC, TLV decoder, read session, control transactions
src/wh27s_diagnosis.h   Smart Diagnosis rule table and decoder
src/wh27s_esphome.h     ESPHome glue: UART I/O, ACKs, polling, TCP debug bridge
tests/                  native C++ tests and a compile-only validation config
docs/                   protocol and Smart Diagnosis documentation
```

## Tests

Run from the repository root, with ESPHome installed as described in the
[software setup](../README.md#software-setup).

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
  tests/test_wh27s_uart.cpp -o /tmp/test_wh27s_uart && /tmp/test_wh27s_uart
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
  tests/test_wh27s_diagnosis.cpp -o /tmp/test_wh27s_diagnosis && /tmp/test_wh27s_diagnosis
esphome compile tests/validation.yaml   # never flash this
```

The UART tests replay frames captured from the original LG modem, including the
on/off sequence.

## UART debug bridge

Turning on `UART Bridge Mode` hands the raw UART to a single TCP client on port
6638 (for example `socket://<ip>:6638` with pyserial). Normal polling pauses while
the bridge is open, and it closes after 15 s without traffic or after 5 minutes.
The client has to acknowledge heater reports itself; see [protocol reference](protocol.md).