# WH27S modem UART protocol

This is how the LG WH27S talks to its Wi-Fi modem, and therefore to this firmware.

Each item is marked by how well it is established:
- **verified:** sent or received on a real heater and confirmed by its reply.
- **captured:** seen on the wire between the heater and the original modem.
- **named:** taken from LG's tables, with no live confirmation.

## 1. Physical layer

| | |
|---|---|
| Interface | UART, 3.3 V TTL, non-inverted |
| Format | **9600 baud, 8N1**, no flow control |
| Roles | the modem (or ESP) is the client; the heater answers and pushes reports |

WH27S communication was captured at 9600 baud.

## 2. Frame format

```text
offset  size  field
0       1     start byte = 04
1       3     reserved   = 00 00 00
4       1     route
5       1     service
6       1     opcode
7       1     sequence
8       1     payload length N
9       N     payload
9+N     2     CRC-16, big-endian
```

- Total length is `11 + N`.
- Services `F0`–`F9` (firmware update/debug) use a 2-byte length at offsets 8–9.
  They aren't needed for normal operation.
- **CRC:** CRC-16/XMODEM (polynomial `0x1021`, initial value 0, no reflection,
  no final XOR). It covers every byte from `04` to the end of the payload.
  Check value: `"123456789"` → `0x31C3`.

```c
uint16_t crc = 0;
for (each byte b) {
  crc ^= b << 8;
  for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
}
```

### Header values

| Byte | Value | Meaning |
|---|---|---|
| route | `65` | modem → heater (requests, writes, ACKs) |
| route | `87` | heater → modem ACK |
| route | `A7` | heater → modem report |
| route | `A8` | heater → modem extended report (not used by the WH27S) |
| service | `01` | discovery / synchronisation |
| service | `02` | normal field traffic |
| opcode | `01` | modem→heater: **write**; heater→modem: capability/firmware report |
| opcode | `02` | read request |
| opcode | `04` | status / diagnosis report |
| opcode | `10` | ACK (empty payload) |

## 3. Payload: compact TLVs

A payload is a sequence of fields, each starting with a big-endian 16-bit tag:

```text
field = tag >> 6               (10 bits, 0..1023)
width = (tag >> 4) & 3         (number of value bytes that follow, 0..3)
value = tag & 0x0F             if width == 0
      = next <width> bytes     big-endian, otherwise
```

To encode, use width 0 for values 0–15 and the smallest width that fits otherwise.

| Bytes | Decoded |
|---|---|
| `7d42` | field 501 = 2 |
| `7dc1` | field 503 = 1 |
| `7e50 19` | field 505 = 25 |
| `9590 5a` | field 598 = 90 |
| `d5a0 10e0` | field 854 = 4320 |

Reports can repeat a field, for example capability records and firmware records,
so keep the order. Partial reports contain only the fields that changed; fields
that aren't present keep their previous value.

**Exception:** field 921 (Smart Diagnosis) is followed by a raw block that is not
made of TLVs; see [smart-diagnosis.md](smart-diagnosis.md).

## 4. Acknowledgements

- Acknowledge every heater report (route `A7`, service 01/02, opcode 01 or 04)
  with `04 00 00 00 65 <service> 10 <same sequence> 00 <crc>`.
- The heater acknowledges each modem request with route `87`, opcode `10`.
- An ACK only means the frame was received. It doesn't mean a setting was applied.
  A write is confirmed only by a report that carries the new value.
- Reports carry their own sequence numbers and don't echo the request's sequence.

## 5. Session start-up (verified)

| Step | Request (complete frame) | Heater reply |
|---|---|---|
| Discovery | `040000006501010102aa43e6cd` (svc 01, 681 = 3) | ACK `040000008701100100df0d` |
| Capabilities | `0400000065020201027d416a0d` (501 = 1) | opcode 01 report: 704, 707, 708, 722, 723, 725, 727/728 pairs, 730–757 |
| Status | `0400000065020202027d42c1b2` (501 = 2) | opcode 04 report with all 17 status fields |
| Firmware | `040000006502020102e4c049d7` (915 = 0) | records 916–919 (part numbers / checksums) |
| Smart Diagnosis | `040000006502020102e600f6f9` (920 = 0) | 921 + 69-byte block |

After that the heater **pushes changed fields on its own** (e.g. water temperature,
energy counter). This firmware also polls status every 30 s and requests Smart
Diagnosis every 60 s.

A read sweep found only two group values: 501 = 1 and 501 = 2. The values given
with 915 and 920 are ignored, and reading single fields by sending
`<field> = 0` doesn't work (the heater only ACKs).

## 6. Status fields (opcode 04)

| Field | Constant | Meaning | Scale |
|---|---|---|---|
| 503 | MT_IDU_ON_OFF | operation 0 = standby, 1 = on | – |
| 505 | MT_IDU_OPERATION_MODE | 25 Heat Pump (Eco), 26 Auto, 27 Turbo, 28 Vacation | – |
| 508 | MT_ROOM_CF_SWITCHING | temperature unit (0 observed with °C) | unresolved |
| 545 | MT_IDU_ERROR | panel error code, 0 = none | display code |
| 552 | MT_OUT_CURRENT | current | raw, unscaled |
| 553 | MT_OUT_VOLTAGE | voltage | raw, unscaled |
| 554 | MT_OUT_COMPSTEP | compressor step, 0 = off | raw |
| 562 / 563 | MT_OUT_ACCUMULATE_WH(_POINT) | energy counter `100·562 + 563` | ≈ 0.01 Wh/count, inferred |
| 597 | MT_ROOM_HOT_WATER_TEMPERATURE | current water temperature | ÷ 2 °C |
| 598 | MT_IDU_HOT_WATER_TEMP_SETTING | target water temperature | ÷ 2 °C |
| 649 | MT_PREHEAT_SCHEDULE_STATUS | "Schedule" flag / panel indicator | 0/1 |
| 689 | MT_PUSH_SERVICE | notification (9 filter, 11 drain, 12 T&P valve, 13 system error, …) | code |
| 691 | MT_INOUT_INSTANT_W | power input | W |
| 804 | MT_EXTRAORDINARY_OPERATION | 1 = emergency heating | 0/1 |
| 853 | MT_FILTER_REMAIN_TIME | filter time remaining (reset → 854) | unit unknown |
| 854 | MT_FILTER_MAX_TIME | filter lifetime (4320) | unit unknown |

Capability fields (opcode 01 report) are bitmasks: **model code N is bit N−1**.
Example: 756 = `0x1FD00` = notifications 9, 11–17; 722 = 30 = mode codes 2–5;
723 = `0x20000` = code 18 "not used", meaning the heater has no schedule store of
its own.

## 7. Writes (service 02, opcode 01, sequence 00)

All writes below were captured from the original modem and confirmed live with this
firmware.

### Target temperature / mode

The LG modem always sends mode and target together:

```text
7e50 <mode>  9590 <target × 2>
45 °C, Eco: 0400000065020100067e501995905a20df
46 °C, Eco: 0400000065020100067e501995905c4019
```

To change the mode, send the new mode together with the current target. To change
the target, send it together with the current mode.

### Schedule flag (field 649)

```text
on : 040000006502010002a241f772
off: 040000006502010002a240e753
```

This turns the panel's "Schedule" text on and off, and the heater reports 649 back.

### On / off (field 503) – the stock schedule sequence

The heater **only accepts `503 = 0` while 649 = 1**. When 649 = 0, the same frame
is ACKed and ignored. This logic-analyser capture of an app schedule window shows
how the LG modem does it:

| Event | Modem → heater | Heater reports |
|---|---|---|
| schedule created | `649 = 1` | 649 = 1 |
| window start | `503 = 1, 505 = mode, 510 = target×2` → `0400000065020100087dc17e50197f906cd29b` | 503 = 1 |
| window end | `503 = 0` → `0400000065020100027dc073c2` | **503 = 0**, compressor stops |
| schedule deleted | `649 = 0` | **503 = 1**, the heater switches itself back on |

Field 510 (`MT_IDU_TEMP_SETTING`) is the app's generic target field; on the "on"
frame it carries the same value as 598.

This firmware therefore implements:
- **Off:** send `649 = 1` if needed, wait for the report, then send `503 = 0`.
- **On:** send `503 = 1, 505 = <current mode>, 510 = <current target>`.

## 8. Where the LG cloud fits

During captured ThinQ schedule windows, the original modem sent the on/off writes
shown above at the scheduled boundaries. The UART commands contain no stored
schedule. In a local setup, Home Assistant automations provide the timing and
this firmware sends the corresponding writes.

## 9. Not available on this link

- **Legionella / disinfection:** toggling it on the panel produces no UART traffic
  and no change in status or diagnosis data. No writable field was found.
- **Daily/monthly energy history:** built by the cloud from the 562/563 counter. The
  heater has no history request.
- Fields 552/553/554 have no verified physical scaling.

## 10. Implementation notes

- Wait at least about 1 s between requests. Never send a write while another one
  is still waiting for confirmation.
- Confirm writes from reports. If confirmation is missing, send one status read
  (501 = 2) after 1 s and give up after 5 s. Don't blindly repeat writes.
- Don't run two transmitters (LG modem and ESP) on the line at the same time.
- Reference implementation: [src/wh27s_uart.h](../src/wh27s_uart.h) (codec, session,
  controls) and [src/wh27s_esphome.h](../src/wh27s_esphome.h) (I/O, ACKs, polling).
