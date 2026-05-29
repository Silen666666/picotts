/*
 * ESP CTF Game – Master Controller
 * Compatible with ESP8266 and ESP32.
 *
 * Architecture:
 *   Master creates a WiFi AP. Nodes connect as clients.
 *   All communication is via UDP (broadcast + unicast).
 *
 * Serial commands (115200 baud):
 *   1  – Start CTF (Capture the Flag)
 *   2  – Start Memory (pair matching)
 *   3  – Start Bomb Defusal
 *   0  – Stop / return to idle
 *   s  – Show current status
 */

#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiUdp.h>

#include "config.h"
#include "protocol.h"

// ─────────────────────────────────────────────────────────────
// Node registry
// ─────────────────────────────────────────────────────────────

struct NodeInfo {
  IPAddress ip;
  bool      active;
  uint32_t  lastSeen;
};

NodeInfo nodes[MAX_NODES + 1];   // index 0 unused; nodes are 1..nodeCount
uint8_t  nodeCount = 0;

WiFiUDP udp;
IPAddress bcastIP(192, 168, 4, 255);

// ─────────────────────────────────────────────────────────────
// Game state
// ─────────────────────────────────────────────────────────────

uint8_t  gameMode    = GAME_IDLE;
uint32_t gameEndTime = 0;

// CTF
uint8_t ctfTeam[MAX_NODES + 1];

// Memory
uint8_t  memColor[MAX_NODES + 1];
bool     memMatched[MAX_NODES + 1];
int8_t   memPending[2];   // indices of currently revealed nodes
uint32_t memHideAt;       // millis() when to hide a non-matching pair
uint8_t  memTotalPairs;
uint8_t  memFoundPairs;

// Bomb
uint8_t  bombNode;
uint8_t  bombSeq[BOMB_SEQ_LEN];
uint8_t  bombStep;
bool     bombOver;

// ─────────────────────────────────────────────────────────────
// Network helpers
// ─────────────────────────────────────────────────────────────

void sendPkt(IPAddress ip, uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  Packet p;
  p.type = type; p.nodeId = nid;
  p.data[0]=d0; p.data[1]=d1; p.data[2]=d2;
  p.data[3]=d3; p.data[4]=d4; p.data[5]=d5;
  udp.beginPacket(ip, UDP_PORT);
  udp.write((uint8_t*)&p, sizeof(p));
  udp.endPacket();
}

void broadcast(uint8_t type, uint8_t nid,
               uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
               uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  sendPkt(bcastIP, type, nid, d0, d1, d2, d3, d4, d5);
}

void setLED(uint8_t id, uint8_t color, uint8_t anim) {
  if (id < 1 || id > nodeCount || !nodes[id].active) return;
  sendPkt(nodes[id].ip, PKT_SET_LED, id, color, anim);
}

void allLED(uint8_t color, uint8_t anim) {
  for (uint8_t i = 1; i <= nodeCount; i++) {
    if (nodes[i].active) setLED(i, color, anim);
    delay(15);
  }
}

// ─────────────────────────────────────────────────────────────
// CTF helpers
// ─────────────────────────────────────────────────────────────

static const uint8_t TEAM_COLORS[] = {COL_WHITE, COL_RED, COL_BLUE, COL_GREEN, COL_YELLOW};
static const char*   TEAM_NAMES[]  = {"Neutral","RED","BLUE","GREEN","YELLOW"};

void ctfStart() {
  if (nodeCount < 2) { Serial.println("[CTF] Need at least 2 nodes."); return; }
  gameMode    = GAME_CTF;
  gameEndTime = millis() + (uint32_t)CTF_DURATION_S * 1000;
  for (uint8_t i = 1; i <= nodeCount; i++) {
    ctfTeam[i] = 0;
    setLED(i, COL_WHITE, ANIM_SOLID);
    delay(30);
  }
  Serial.printf("[CTF] Started: %u nodes, %ds\n", nodeCount, CTF_DURATION_S);
}

void ctfOnButton(uint8_t id) {
  ctfTeam[id] = (ctfTeam[id] % CTF_TEAMS) + 1;
  setLED(id, TEAM_COLORS[ctfTeam[id]], ANIM_FLASH);
  Serial.printf("[CTF] Node %u → %s\n", id, TEAM_NAMES[ctfTeam[id]]);
}

void ctfUpdate() {
  if ((long)(millis() - gameEndTime) < 0) return;
  uint8_t score[5] = {0};
  for (uint8_t i = 1; i <= nodeCount; i++)
    if (ctfTeam[i] > 0 && ctfTeam[i] <= 4) score[ctfTeam[i]]++;

  uint8_t winner = 1;
  for (uint8_t t = 2; t <= CTF_TEAMS; t++)
    if (score[t] > score[winner]) winner = t;

  Serial.println("[CTF] === TIME UP ===");
  for (uint8_t t = 1; t <= CTF_TEAMS; t++)
    Serial.printf("  %s: %u nodes\n", TEAM_NAMES[t], score[t]);
  Serial.printf("  WINNER: %s!\n", TEAM_NAMES[winner]);

  allLED(TEAM_COLORS[winner], ANIM_BLINK_FAST);
  gameMode = GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// Memory helpers
// ─────────────────────────────────────────────────────────────

// Color palette for pairs (skip COL_WHITE to keep as "unknown")
static const uint8_t PAIR_PALETTE[] = {
  COL_RED, COL_BLUE, COL_GREEN, COL_YELLOW, COL_PURPLE, COL_CYAN, COL_ORANGE
};
#define PALETTE_SIZE 7

void memStart() {
  if (nodeCount < 2) { Serial.println("[MEM] Need at least 2 nodes."); return; }
  gameMode = GAME_MEMORY;
  uint8_t n = (nodeCount % 2 == 0) ? nodeCount : nodeCount - 1;
  memTotalPairs = n / 2;
  memFoundPairs = 0;
  memPending[0] = memPending[1] = -1;
  memHideAt = 0;

  // Build shuffled color array
  uint8_t pool[MAX_NODES];
  for (uint8_t i = 0; i < memTotalPairs; i++) {
    pool[2*i]   = PAIR_PALETTE[i % PALETTE_SIZE];
    pool[2*i+1] = PAIR_PALETTE[i % PALETTE_SIZE];
  }
  // Fisher-Yates shuffle
  randomSeed(millis());
  for (int i = n - 1; i > 0; i--) {
    int j = random(0, i + 1);
    uint8_t tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
  }
  for (uint8_t i = 1; i <= n; i++) {
    memColor[i]   = pool[i - 1];
    memMatched[i] = false;
  }
  // Extra node off
  for (uint8_t i = n + 1; i <= nodeCount; i++) setLED(i, COL_OFF, ANIM_SOLID);

  // Brief reveal
  Serial.println("[MEM] Showing pairs for 2s...");
  for (uint8_t i = 1; i <= n; i++) { setLED(i, memColor[i], ANIM_SOLID); delay(30); }
  delay(2000);
  for (uint8_t i = 1; i <= n; i++) { setLED(i, COL_OFF, ANIM_SOLID); delay(30); }

  Serial.printf("[MEM] Started: %u nodes, %u pairs\n", n, memTotalPairs);
}

void memOnButton(uint8_t id) {
  uint8_t n = 2 * memTotalPairs;
  if (id > n || memMatched[id]) return;
  // Ignore taps during the hide-delay phase
  if (memHideAt != 0) return;
  // Already showing this node
  if (memPending[0] == id || memPending[1] == id) return;

  setLED(id, memColor[id], ANIM_SOLID);

  if (memPending[0] < 0) {
    memPending[0] = id;
  } else {
    memPending[1] = id;
    if (memColor[memPending[0]] == memColor[id]) {
      // Match!
      memMatched[memPending[0]] = true;
      memMatched[id]            = true;
      setLED(memPending[0], COL_GREEN, ANIM_PULSE);
      setLED(id, COL_GREEN, ANIM_PULSE);
      memFoundPairs++;
      memPending[0] = memPending[1] = -1;
      Serial.printf("[MEM] Match! %u/%u\n", memFoundPairs, memTotalPairs);
      if (memFoundPairs == memTotalPairs) {
        delay(400);
        allLED(COL_GREEN, ANIM_BLINK_FAST);
        Serial.println("[MEM] ALL PAIRS FOUND!");
        gameMode = GAME_IDLE;
      }
    } else {
      // No match – schedule hide in 1.5 s
      memHideAt = millis() + 1500;
    }
  }
}

void memUpdate() {
  if (memHideAt == 0 || (long)(millis() - memHideAt) < 0) return;
  memHideAt = 0;
  if (memPending[0] >= 0) { setLED(memPending[0], COL_OFF, ANIM_SOLID); memPending[0] = -1; }
  if (memPending[1] >= 0) { setLED(memPending[1], COL_OFF, ANIM_SOLID); memPending[1] = -1; }
}

// ─────────────────────────────────────────────────────────────
// Bomb Defusal helpers
// ─────────────────────────────────────────────────────────────

// Color codes for sequence display (one color = one node number visually)
static const uint8_t SEQ_COLORS[] = {
  COL_RED, COL_BLUE, COL_GREEN, COL_YELLOW, COL_PURPLE,
  COL_CYAN, COL_ORANGE, COL_WHITE
};

void bombShowSequence() {
  Serial.print("[BOMB] Sequence: ");
  for (uint8_t i = 0; i < BOMB_SEQ_LEN; i++) Serial.printf("%u ", bombSeq[i]);
  Serial.println();

  for (uint8_t s = 0; s < BOMB_SEQ_LEN; s++) {
    uint8_t nid = bombSeq[s];
    setLED(nid, SEQ_COLORS[s % 8], ANIM_SOLID);
    delay(BOMB_STEP_MS);
    setLED(nid, COL_OFF, ANIM_SOLID);
    delay(200);
  }
}

void bombStart() {
  if (nodeCount < 3) { Serial.println("[BOMB] Need at least 3 nodes."); return; }
  gameMode    = GAME_BOMB;
  gameEndTime = millis() + (uint32_t)BOMB_DURATION_S * 1000;
  bombStep = 0;
  bombOver = false;

  // Random bomb node
  bombNode = random(1, nodeCount + 1);

  // Random sequence (excluding bomb node, no repeats)
  uint8_t available[MAX_NODES], cnt = 0;
  for (uint8_t i = 1; i <= nodeCount; i++)
    if (i != bombNode) available[cnt++] = i;

  uint8_t seqLen = min((uint8_t)BOMB_SEQ_LEN, cnt);
  for (uint8_t i = 0; i < seqLen; i++) {
    uint8_t pick = random(0, cnt - i);
    bombSeq[i] = available[pick];
    available[pick] = available[cnt - i - 1];
  }

  // All neutral
  allLED(COL_OFF, ANIM_SOLID);
  delay(200);

  // Show bomb
  setLED(bombNode, COL_RED, ANIM_BLINK_FAST);

  // Show sequence on non-bomb nodes
  Serial.println("[BOMB] Revealing sequence...");
  bombShowSequence();

  // All sequence nodes dim-white, bomb stays blinking
  for (uint8_t i = 1; i <= nodeCount; i++) {
    if (i == bombNode) setLED(i, COL_RED, ANIM_BLINK_FAST);
    else               setLED(i, COL_WHITE, ANIM_SOLID);
    delay(20);
  }
  Serial.printf("[BOMB] Started! Bomb=node%u, %ds\n", bombNode, BOMB_DURATION_S);
}

void bombOnButton(uint8_t id) {
  if (bombOver) return;

  if (id == bombNode) {
    // Re-show sequence as hint
    Serial.println("[BOMB] Hint requested – replaying sequence");
    bombShowSequence();
    for (uint8_t i = 1; i <= nodeCount; i++) {
      if (i == bombNode) setLED(i, COL_RED, ANIM_BLINK_FAST);
      else               setLED(i, COL_WHITE, ANIM_SOLID);
      delay(20);
    }
    return;
  }

  if (id == bombSeq[bombStep]) {
    setLED(id, COL_GREEN, ANIM_FLASH);
    bombStep++;
    Serial.printf("[BOMB] Step %u/%u OK\n", bombStep, BOMB_SEQ_LEN);
    if (bombStep == BOMB_SEQ_LEN) {
      bombOver = true;
      Serial.println("[BOMB] DEFUSED!");
      allLED(COL_GREEN, ANIM_BLINK_SLOW);
      gameMode = GAME_IDLE;
    }
  } else {
    bombOver = true;
    Serial.printf("[BOMB] WRONG! Expected node %u, got node %u – BOOM!\n",
                  bombSeq[bombStep], id);
    allLED(COL_RED, ANIM_BLINK_FAST);
    gameMode = GAME_IDLE;
  }
}

void bombUpdate() {
  if (bombOver || (long)(millis() - gameEndTime) < 0) return;
  bombOver = true;
  Serial.println("[BOMB] TIME'S UP – BOOM!");
  allLED(COL_RED, ANIM_BLINK_FAST);
  gameMode = GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// UDP receive handler
// ─────────────────────────────────────────────────────────────

void handleUDP() {
  int size = udp.parsePacket();
  if (size < (int)sizeof(Packet)) return;

  Packet p;
  udp.read((uint8_t*)&p, sizeof(p));
  IPAddress remoteIP = udp.remoteIP();

  switch (p.type) {
    case PKT_REGISTER: {
      // Check if already registered
      for (uint8_t i = 1; i <= nodeCount; i++) {
        if (nodes[i].ip == remoteIP) {
          nodes[i].lastSeen = millis();
          sendPkt(remoteIP, PKT_ACK, i, i);
          return;
        }
      }
      if (nodeCount >= MAX_NODES) return;
      nodeCount++;
      nodes[nodeCount].ip       = remoteIP;
      nodes[nodeCount].active   = true;
      nodes[nodeCount].lastSeen = millis();
      sendPkt(remoteIP, PKT_ACK, nodeCount, nodeCount);
      Serial.printf("[REG] Node %u registered (%s)\n",
                    nodeCount, remoteIP.toString().c_str());
      break;
    }
    case PKT_PING: {
      uint8_t id = p.nodeId;
      if (id >= 1 && id <= nodeCount) nodes[id].lastSeen = millis();
      break;
    }
    case PKT_BUTTON: {
      uint8_t id = p.nodeId;
      if (id < 1 || id > nodeCount) return;
      nodes[id].lastSeen = millis();
      Serial.printf("[BTN] Node %u pressed\n", id);
      if      (gameMode == GAME_CTF)    ctfOnButton(id);
      else if (gameMode == GAME_MEMORY) memOnButton(id);
      else if (gameMode == GAME_BOMB)   bombOnButton(id);
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Serial menu
// ─────────────────────────────────────────────────────────────

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == '1')      ctfStart();
  else if (c == '2') memStart();
  else if (c == '3') bombStart();
  else if (c == '0') {
    gameMode = GAME_IDLE;
    allLED(COL_OFF, ANIM_SOLID);
    Serial.println("Game stopped.");
  }
  else if (c == 's') {
    Serial.printf("Mode: %u | Nodes: %u\n", gameMode, nodeCount);
    for (uint8_t i = 1; i <= nodeCount; i++)
      Serial.printf("  Node %u: %s\n", i, nodes[i].active ? "active" : "lost");
  }
}

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP CTF Game – Master ===");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, *WIFI_PASS ? WIFI_PASS : nullptr);
  Serial.printf("AP: %s  IP: %s\n", WIFI_SSID, WiFi.softAPIP().toString().c_str());

  udp.begin(UDP_PORT);
  Serial.println("UDP listening on port " + String(UDP_PORT));
  Serial.println("Commands: 1=CTF  2=Memory  3=Bomb  0=Stop  s=Status");
}

void loop() {
  handleUDP();
  handleSerial();

  // Node timeout check
  static uint32_t lastTimeoutCheck = 0;
  if (millis() - lastTimeoutCheck > 2000) {
    lastTimeoutCheck = millis();
    for (uint8_t i = 1; i <= nodeCount; i++) {
      bool wasActive = nodes[i].active;
      nodes[i].active = (millis() - nodes[i].lastSeen) < NODE_TIMEOUT_MS;
      if (wasActive && !nodes[i].active)
        Serial.printf("[WARN] Node %u timed out\n", i);
    }
  }

  if      (gameMode == GAME_CTF)    ctfUpdate();
  else if (gameMode == GAME_MEMORY) memUpdate();
  else if (gameMode == GAME_BOMB)   bombUpdate();
}
