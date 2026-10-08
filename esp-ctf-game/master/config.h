#pragma once

// WiFi AP created by the master
#define WIFI_SSID        "ESP-CTF-Game"
#define WIFI_PASS        ""              // open network (sonst mind. 8 Zeichen!)

// WLAN-Kanal: 0 = automatisch (beim Start wird der freieste der Kanaele
// 1/6/11 gesucht), sonst fester Kanal 1-13
#define WIFI_CHANNEL     0
// Max. gleichzeitige WLAN-Verbindungen am Master (ESP32: hoechstens 10)
// => max. 9 Nodes + 1 Handy
#define AP_MAX_CONN      10

#define MAX_NODES        16        // IDs 1..16 (fest je Node-MAC, wird gespeichert)
#define NODE_TIMEOUT_MS  15000UL   // 15s – toleriert kurze WLAN-Aussetzer

// OTA / Stabilitaet
#define OTA_PASSWORD     ""        // Passwort fuer Update per Arduino IDE ueber WLAN ("" = keins)
#define USE_WATCHDOG     1         // ESP32: 1 = Master startet neu, falls die Firmware haengt
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
