#pragma once

// WiFi AP created by the master
#define WIFI_SSID        "ESP-CTF-Game"
#define WIFI_PASS        ""              // open network

#define MAX_NODES        16
#define NODE_TIMEOUT_MS  8000UL

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

// Game History
#define MAX_HISTORY  20
