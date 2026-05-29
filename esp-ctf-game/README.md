# ESP CTF Game

Multiplayer physical game system using ESP8266 / ESP32 nodes with RGB LEDs and buttons.

## Hardware

### Per Node
| Part | Notes |
|---|---|
| ESP8266 (NodeMCU/Wemos) or ESP32 | Both supported |
| WS2812B NeoPixel LED (recommended) | Or simple RGB LED (common-cathode/anode) |
| Momentary push button | Any tactile button |
| 10 kΩ resistor (optional) | If not using internal pull-up |

### Master
One additional ESP (same type) acts as the master. It needs no LED/button, just power + serial for the menu.

---

## Wiring

### Node – NeoPixel (recommended)

```
ESP8266 NodeMCU:            ESP32:
  D4 (GPIO2) ──► DIN        GPIO13 ──► DIN
  3.3V / 5V  ──► VCC        3.3V   ──► VCC
  GND        ──► GND        GND    ──► GND

  D3 (GPIO0) ──► Button ──► GND
```

### Node – Simple RGB LED (common-cathode)

```
ESP8266:          ESP32:
  D5 ──[R]──► R    GPIO25
  D6 ──[G]──► G    GPIO26
  D7 ──[B]──► B    GPIO27
  GND        ── Cathode (–)
```
Use 47–100 Ω resistors on each color pin.

---

## Software Setup

### Dependencies
Install via Arduino Library Manager:
- **Adafruit NeoPixel** (only if using `LED_NEOPIXEL`)

### Board Support
- ESP8266: install *ESP8266 Arduino Core* (Board Manager URL: `http://arduino.esp8266.com/stable/package_esp8266com_index.json`)
- ESP32: install *ESP32 Arduino Core* (Board Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)

### Flash the Master
1. Open `master/master.ino` in Arduino IDE
2. Select your board (ESP8266 or ESP32)
3. Flash

### Flash each Node
1. Open `node/node.ino` in Arduino IDE
2. In `node/config.h` select your LED type (uncomment one `#define`)
3. Adjust pin numbers if needed
4. Flash – all nodes use identical firmware; IDs are assigned at runtime

---

## Gameplay

### Starting a game (Serial Monitor, 115200 baud)

Connect to the master's serial port and type:

| Key | Action |
|-----|--------|
| `1` | Start **CTF** |
| `2` | Start **Memory** |
| `3` | Start **Bomb Defusal** |
| `0` | Stop current game |
| `s` | Show status |

---

### Game 1 – Capture the Flag (CTF)

- All nodes start **white** (neutral).
- **Press** a node → it changes to the next team's color (cycling: White → Red → Blue → White…).
- Players run around and physically press nodes to capture them for their team.
- After **3 minutes** the team with the most captured nodes wins.
- Winner's color flashes on all nodes.

> Config: `CTF_TEAMS`, `CTF_DURATION_S` in `master/config.h`

---

### Game 2 – Memory (Pair Matching)

- All nodes briefly flash their hidden color for **2 seconds**, then go dark.
- Players take turns pressing nodes to reveal them.
- If two revealed nodes share the same color → **match**! They stay lit green.
- If they don't match → both go dark after 1.5 s.
- Game ends when all pairs are found.

> Supports 2–14 nodes (must be even; odd count ignores the last node).

---

### Game 3 – Bomb Defusal

- One randomly chosen node becomes the **bomb** (fast red blink).
- The master reveals the **disarm sequence**: nodes light up one by one in order.
- Players must press the nodes in that exact order.
- Pressing the **bomb node** replays the sequence as a hint.
- Wrong press → **BOOM!** (all nodes flash red).
- Correct sequence → **DEFUSED!** (all nodes blink green).
- Timer: **2 minutes**.

> Config: `BOMB_SEQ_LEN`, `BOMB_DURATION_S` in `master/config.h`

---

## Extending

- **More teams** – change `CTF_TEAMS` (max 4).
- **More nodes** – change `MAX_NODES` (tested up to 16).
- **Longer sequences** – change `BOMB_SEQ_LEN`.
- **Custom colors/brightness** – edit `COLORS[]` in `node.ino` or `strip.setBrightness()`.
