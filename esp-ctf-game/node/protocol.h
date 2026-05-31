#pragma once
#include <stdint.h>

#define UDP_PORT 4210

#define PKT_REGISTER   0x01
#define PKT_ACK        0x02
#define PKT_BUTTON     0x03
#define PKT_SET_LED    0x04
#define PKT_GAME_START 0x05
#define PKT_GAME_OVER  0x06
#define PKT_PING       0x07

#define COL_OFF     0
#define COL_RED     1
#define COL_BLUE    2
#define COL_GREEN   3
#define COL_YELLOW  4
#define COL_WHITE   5
#define COL_PURPLE  6
#define COL_CYAN    7
#define COL_ORANGE  8

#define ANIM_SOLID      0
#define ANIM_BLINK_SLOW 1
#define ANIM_BLINK_FAST 2
#define ANIM_PULSE      3
#define ANIM_FLASH      4

#define GAME_IDLE     0
#define GAME_CTF      1
#define GAME_MEMORY   2
#define GAME_BOMB     3
#define GAME_REACTION 4

struct Packet {
  uint8_t type;
  uint8_t nodeId;
  uint8_t data[6];
};
