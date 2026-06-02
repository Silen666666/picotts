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
#define PKT_SET_BAR    0x08  // Master→Node: set N LEDs to color, rest to bgColor
                              // data[0]=color, data[1]=count(0-8), data[2]=bgColor
#define PKT_SET_SPLIT  0x09  // Master→Node: split bar
                              // data[0]=colorA, data[1]=countA(0-8), data[2]=colorB

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
#define ANIM_BAR        5   // N LEDs solid color, rest bgColor (used with PKT_SET_BAR)
#define ANIM_SPLIT      6   // left N=colorA, rest=colorB (used with PKT_SET_SPLIT)

// Game modes
#define GAME_IDLE        0
#define GAME_CTF         1
#define GAME_MEMORY      2
#define GAME_BOMB        3
#define GAME_REACTION    4
#define GAME_SIMON       5
#define GAME_HOTPOTATO   6
#define GAME_KINGHILL    7
#define GAME_TUGWAR      8
#define GAME_MINESWEEPER 9
#define GAME_KNOCKOUT    10
#define GAME_COLORHUNT   11
#define GAME_WHACKAMOLE  12

struct Packet {
  uint8_t type;
  uint8_t nodeId;
  uint8_t data[6];
};
