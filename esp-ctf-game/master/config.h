#pragma once

// WLAN des Masters (Access Point). Das Handy verbindet sich damit (Web-UI http://192.168.4.1),
// die Nodes nur kurz fuer Firmware-Updates. Gespielt wird per ESP-NOW auf demselben Kanal.
// WICHTIG: WIFI_SSID / WIFI_PASS muessen in der config.h der Nodes identisch sein!
#define WIFI_SSID        "ESP-CTF-Game"
#define WIFI_PASS        ""              // open network (sonst mind. 8 Zeichen!)

// WLAN-Kanal: 0 = automatisch – beim ERSTEN Start wird der freieste der Kanaele 1/6/11
// gesucht und gespeichert (Web-UI: "WLAN-Kanal neu waehlen"); sonst fester Kanal 1-13
#define WIFI_CHANNEL     0
// Max. gleichzeitige WLAN-Verbindungen am Master (ESP32: hoechstens 10).
// Handys + je eine Node waehrend ihres Firmware-Updates (alte v1/v2-Nodes belegen ebenfalls Plaetze)
#define AP_MAX_CONN      10

#define MAX_NODES        16        // IDs 1..16 (fest je Node-MAC, wird gespeichert; ESP-NOW: max. 19)
#define NODE_TIMEOUT_MS  15000UL   // 15s ohne Lebenszeichen -> Node gilt als offline

// Master als Spieler-Station (eigener LED-Balken + Taster direkt am Master)
// 0 = aus (Standard): der Master verhaelt sich exakt wie bisher (kein Zusatz-Code).
// 1 = an: der Master ist zusaetzlich eine "lokale Node" – feste Nummer (eigene MAC, wie jede
//     Node gespeichert, belegt also eine der MAX_NODES-Nummern), immer verbunden, ohne Funk.
//     Verdrahtung wie bei einer Node: WS2812B-Balken DIN ueber 300 Ohm an MASTER_NEO_PIN,
//     +5V an VIN, GND, 470-1000 uF am Balken; Taster zwischen MASTER_BUTTON_PIN und GND (100 nF parallel).
//     Netzteil >= 1 A, kurzes dickes Kabel mit Zugentlastung; Taster + Balken am besten in einem
//     eigenen Kaestchen an 1-2 m Kabel (Spieler schlagen sonst auf den Master).
#define MASTER_IS_NODE        0
#define MASTER_NEO_PIN        13   // D13
#define MASTER_BUTTON_PIN     18   // D18 (interner Pull-up, Taster nach GND)
#define MASTER_NEO_COUNT      8
#define MASTER_NEO_BRIGHTNESS 45   // 0-255 – bewusst niedrig: schont das Netzteil des Masters
#define MASTER_LAT_MS         6    // Fairness: lokale Drucke so viel spaeter werten (Funk-Nodes ~3-8 ms)

// OTA / Stabilitaet
#define OTA_PASSWORD     ""        // Passwort fuer Update per Arduino IDE ueber WLAN ("" = keins)
#define USE_WATCHDOG     1         // 1 = Master startet neu, falls die Firmware haengt
#define WDT_TIMEOUT_S    30        // Watchdog-Zeit in Sekunden

// CTF
#define CTF_TEAMS        2              // 2, 3, or 4
#define CTF_DURATION_S   180            // seconds

// Bomb Defusal
#define BOMB_SEQ_LEN     5              // steps to disarm
#define BOMB_DURATION_S  120
#define BOMB_STEP_MS     1200           // ms per step in sequence reveal

// Reaction Game
#define REACT_ROUNDS_DEFAULT  10
#define REACT_TIMEOUT_MS      5000      // ms until round times out
#define REACT_DELAY_MIN_MS    600       // min pause between rounds
#define REACT_DELAY_MAX_MS    3000      // max pause (random)
#define MAX_HIGHSCORES        10

// King of the Hill
#define KING_THRONE_MOVE_S  30

// Minesweeper
#define MINE_COUNT_DEFAULT  2

// Knockout
#define KNOCK_LIVES_DEFAULT 3

// Color Hunt
#define COLORHUNT_ROUNDS    8

// Whack-a-Mole
#define WHAM_ROUNDS_DEFAULT  15
#define WHAM_TIMEOUT_MS      3000UL
#define WHAM_DELAY_MIN_MS    400
#define WHAM_DELAY_MAX_MS    1800

// Rundenabstand (Reaktion / Whack-a-Mole / Knockout)
// Zeit in Sekunden, die nach jedem Treffer/Aussetzer als Pflichtpause gilt,
// damit Spieler den vorherigen Gewinner-Node nicht einfach "nachmachen" koennen.
// Einstellbar im Web-UI (0-30s). 0 = sofort wie bisher.
#define ROUND_DELAY_S        10

// Game History
#define MAX_HISTORY  20
