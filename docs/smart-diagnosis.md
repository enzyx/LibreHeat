# Smart Diagnosis decoding

The LG app's "Smart Diagnosis" asks the heater for a 69-byte diagnosis block and
sends it to LG's server, which returns a verdict. This firmware requests the block
over the UART and decodes it **locally**, using an index-to-fault mapping derived
from observed server responses. This mapping was not obtained from UART captures
alone; its provenance and validation limits are described below.

## 1. Getting the block

Request (service 02, read):

```text
04 00 00 00 65 02 02 01 02 e6 00 f6 f9        field 920 (MT_SDS_REQUEST) = 0
```

Reply (route A7, service 02, opcode 04):

```text
04 00 00 00 a7 02 04 <seq> 48  e6 50 45  <69 bytes>  <crc>
                               \_____/\_/
                       field 921, width 1 = 0x45 = 69 = block length
```

Field 921 (`MT_SDS_RESPONSE`) holds the block length, and the raw block follows
it. **Don't parse the block as TLVs.** It's the same byte sequence LG's cloud
stores: the uploaded `diagData` is Base64 of these raw bytes.

Example of a healthy unit:

```text
00 82 82 81 ea 81 00 00 a0 79 3e 72 03 e6 00 00 ... (69 bytes)
```

## 2. Block layout

| Byte | Meaning |
|---|---|
| 0 | low 8 bits of the diagnosis **index** |
| 8, bit 7 | bit 8 of the index (adds 256) |
| 9 | product ID, **121** (`0x79`) for the WH27S. Other values are rejected. |
| 1–7, 10–68 | sensor/state data; meaning unknown (changing them didn't affect the verdict) |

```text
index = block[0] | ((block[8] & 0x80) << 1)        // 0..511
```

## 3. Index → verdict

The index is looked up in the table below. Indices without a mapped fault
(including 256, observed on a healthy unit) produce **"No problem found"**.

| Index | Code | Component | Summary |
|---|---|---|---|
| 1 | CH 01 | Temperature sensor | Ambient temp sensor is not working |
| 2 | CH 02 | Temperature sensor | Upper tank temp sensor is not working |
| 4 | CH 04 | Drain | Condensate drain is blocked |
| 5 | CH 05 | Etc. | PCB communication error |
| 6 | CH 06 | Temperature sensor | Lower tank temp sensor is not working |
| 8 | CH 08 | Water leak | Water leakage is detected |
| 9 | CH 09 | Etc. | EEPROM error |
| 12 | CH 12 | Temperature sensor | Mid pipe temp sensor is not working |
| 19 | CH 19 | Heater | Upper element is not working |
| 20 | CH 20 | Heater | Lower element is not working |
| 21 | CH 21 | Voltage | DC peak (IPM fault) |
| 22 | CH 22 | Voltage | CT 2 (max. current) |
| 23 | CH 23 | Voltage | DC link low/high voltage |
| 26 | CH 26 | Compressor | DC comp position error, locking |
| 27 | CH 27 | Voltage | PSC fault |
| 29 | CH 29 | Compressor | Comp phase over-current |
| 32 | CH 32 | Temperature sensor | Discharge pipe is overheated |
| 41 | CH 41 | Temperature sensor | Discharge pipe temp sensor is not working |
| 46 | CH 46 | Temperature sensor | Suction pipe temp sensor is not working |
| 61 | CH 61 | Temperature sensor | Condenser pipe temp is high |
| 65 | CH 65 | Heat sink | Heat sink temp sensor is not working |
| 67 | CH 67 | Fan motor | BLDC fan motor is locked |
| 250 | CH 250 | Water shortage | No or not enough water in tank |
| 251 | CH 251 | Water temperature | Water temp is too low |
| 252 | CH 252 | Water temperature | Water temp is too high |
| 253 | CH 253 | Water temperature | Water temp is too low |
| 254 | CH 254 | Water temperature | ECO (safety cut-out) has tripped |
| 255 | CH 255 | Water temperature | Water temp is too high |

The CH codes match the heater's panel fault codes (e.g. CH 41 = discharge-pipe
sensor). The same condition also shows up in the separate status field 545 /
`Error Code`.

## 4. Reference decoder

C++ (as used on the ESP, [src/wh27s_diagnosis.h](../src/wh27s_diagnosis.h)):

```cpp
DiagnosisResult decode_smart_diagnosis(const std::vector<uint8_t> &data) {
  if (data.size() != 69 || data[9] != 121) return {};          // unsupported block
  DiagnosisResult r; r.valid = true;
  r.index = data[0] | ((data[8] & 0x80U) << 1);
  for (const auto &rule : DIAGNOSIS_RULES) if (rule.index == r.index) r.rule = &rule;
  return r;                                                     // no rule -> "No problem found"
}
```

Python:

```python
def decode(block: bytes) -> str:
    if len(block) != 69 or block[9] != 121:
        return "Unsupported diagnosis block"
    index = block[0] | ((block[8] & 0x80) << 1)
    rule = RULES.get(index)            # {index: (code, description)} from the table above
    return f"{rule[0]}: {rule[1]}" if rule else "No problem found"
```

## 5. Provenance and validation

- The app's audible-diagnosis API (`POST …/devices/{id}/audible-diag`, body
  `{"diagData": base64(raw 69 bytes)}`) was queried with modified diagnosis blocks.
  Echo checks confirmed the submitted bytes in the responses.
- In these experiments, changing individual bytes affected the verdict only for
  byte 0, bit 7 of byte 8, and the product ID in byte 9.
- All **512 indices** were submitted: 28 gave specific fault rules and 484 gave the
  standard "no problem" verdict. The local decoder reproduced **620 recorded
  server responses**.
- The ESP never contacts LG; the table is compiled into the firmware.
- Server-response agreement does not validate physical fault detection. The
  corresponding hardware faults were not individually induced and confirmed.

## 6. Limits

- The table maps observed **server verdicts**, not physical sensor values. The
  meaning and scaling of bytes 1–7 and 10–68 remain unverified.
- Indices 251/253 and 252/255 gave equivalent server descriptions. Their
  relationship to the panel's L1/L2 and H1/H2 codes remains unverified.
- The mapping was derived using product ID 121. Other LG water heaters may use
  different IDs and mappings.
