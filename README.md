# THERMAX – Cold Plate Cooling Optimization

Low-cost cold plate test rig for EV battery / power-electronics cooling. An **Arduino Nano** reads two flow sensors and two thermocouples, and sends the readings to an **ESP32**, which serves a live web dashboard.

- **Test section:** aluminium plate 235 × 170 × 10 mm, Ø3 mm coolant passages
- **Measured:** inlet/outlet coolant flow, inlet/outlet coolant temperature (pressure inlet/outlet: optional)
- **Calculated:** coolant temperature rise ΔT, heat removal rate Q̇ = ṁ · Cₚ · (T_out − T_in), pressure drop ΔP = P_in − P_out

---

## Project Layout

| File | Board | Purpose |
| :--- | :--- | :--- |
| `src/nano_main.cpp` | Arduino Nano | Reads sensors, sends one JSON line per second to the ESP32 on D8 |
| `src/esp32_main.cpp` | ESP32 | Receives data on GPIO16, hosts the THERMAX web dashboard |
| `src/main.cpp` | — | Older Nano sketch, not built |
| `include/secrets.example.h` | ESP32 | Template for WiFi credentials |

---

## Wiring – Arduino Nano Sensors

| Sensor / Module | Wire / Pin | Arduino Nano Pin | Function / Notes |
| :--- | :--- | :--- | :--- |
| **Flow Sensor 1 (Inlet)** | **RED** | **`5V`** | 5V Power rail |
| | **BLACK** | **`GND`** | Ground |
| | **YELLOW** | **`D2`** | **Hardware Interrupt 0 (`INT0`)** |
| **Flow Sensor 2 (Outlet)** | **RED** | **`5V`** | 5V Power rail |
| | **BLACK** | **`GND`** | Ground |
| | **YELLOW** | **`D3`** | **Hardware Interrupt 1 (`INT1`)** |
| **Thermocouple 1 – Inlet (MAX6675)** | **VCC** / **GND** | **`5V`** / **`GND`** | Power & Ground |
| | **SCK** (Clock) | **`D6`** | Shared Clock line |
| | **SO** (Data Out) | **`D4`** | Shared Data Out line |
| | **CS** (Chip Select) | **`D5`** | Dedicated CS for Sensor 1 |
| **Thermocouple 2 – Outlet (MAX6675)** | **VCC** / **GND** | **`5V`** / **`GND`** | Power & Ground |
| | **SCK** (Clock) | **`D6`** | Shared Clock line |
| | **SO** (Data Out) | **`D4`** | Shared Data Out line |
| | **CS** (Chip Select) | **`D7`** | Dedicated CS for Sensor 2 |

## Wiring – Nano → ESP32 Link

| Nano | ESP32 | Notes |
| :--- | :--- | :--- |
| `D8` (SoftwareSerial TX, 9600 baud) | `GPIO16` (RX2) | Through a voltage divider (5 V → 3.3 V) |
| `GND` | `GND` | **Required** – shared ground |
| `5V` | `VIN` / `5V` | Only if the Nano is not powered by USB |

```
Nano D8 ──[1kΩ]──┬── ESP32 GPIO16
                 │
               [2kΩ]
                 │
                GND
```

---

## Setup

1. Copy `include/secrets.example.h` to `include/secrets.h` and enter your WiFi name and password (2.4 GHz network only).
2. Upload both boards:

```bash
# Arduino Nano (default environment)
pio run -e nanoatmega328 -t upload

# ESP32
pio run -e esp32dev -t upload

# Serial monitors (115200 baud)
pio device monitor -e nanoatmega328
pio device monitor -e esp32dev
```

## Dashboard

- On your WiFi: the IP printed in the ESP32 serial monitor, or `http://thermax.local`
- Fallback hotspot: join `THERMAX-Dashboard`, open `http://192.168.4.1`

Features: live flow/temperature cards and trends, ΔT and Q̇, pressure drop (when sensors are fitted), steady-state detection, test-run recording at different flow rates/heater powers with a Q̇ & ΔP vs flow chart, ΔT sensor zeroing, and CSV export.

### Adding pressure sensors

Have the Nano append `"p1"` (inlet) and `"p2"` (outlet) in kPa to its JSON line. The dashboard shows ΔP automatically. Place the taps immediately upstream and downstream of the plate, not across the whole loop.

---

Riswandh S S · richsanj0@gmail.com
