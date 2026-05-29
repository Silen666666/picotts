#pragma once
#include <stdint.h>

#define UDP_PORT 4210

// Packet types
#define PKT_REGISTER   0x01  // Node→Master: I'm online
#define PKT_ACK        0x02  // Master→Node: assigned ID in data[0]
#define PKT_BUTTON     0x03  // Node→Master: button pressed
#define PKT_SET_LED    0x04  // Master→Node: color=data[0], anim=data[1]
#define PKT_GAME_START 0x05  // Master→All: mode=data[0]
#define PKT_GAME_OVER  0x06  // Master→All: winner color=data[0]
#define PKT_PING       0x07  // Node→Master: keepalive

// Colors (index into COLORS[] on the node)
#define COL_OFF     0
#define COL_RED     1
#define COL_BLUE    2
#define COL_GREEN   3
#define COL_YELLOW  4
#define COL_WHITE   5
#define COL_PURPLE  6
#define COL_CYAN    7
#define COL_ORANGE  8

// Animations
#define ANIM_SOLID      0
#define ANIM_BLINK_SLOW 1   // 500 ms period
#define ANIM_BLINK_FAST 2   // 125 ms period
#define ANIM_PULSE      3   // breathing
#define ANIM_FLASH      4   // one quick flash, then solid

// Game modes
#define GAME_IDLE   0
#define GAME_CTF    1
#define GAME_MEMORY 2
#define GAME_BOMB   3

struct Packet {
  uint8_t type;
  uint8_t nodeId;
  uint8_t data[6];
};
