/*
 * ESP CTF Game – Master Controller
 * ESP8266 + ESP32 kompatibel
 *
 * Handy: WLAN "ESP-CTF-Game" → Browser http://192.168.4.1
 * Seriell (115200):
 *   1=CTF  2=Memory  3=Bomb  4=Reaktion  5=Simon  6=HotPotato
 *   7=KingHill  8=TugWar  9=Minesweeper  k=Knockout  h=ColorHunt  w=WhackaMole
 *   0=Stop  s=Status
 */

#ifdef ESP32
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Update.h>
  WebServer webServer(80);
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266HTTPUpdateServer.h>
  ESP8266WebServer webServer(80);
  ESP8266HTTPUpdateServer httpUpdater;
#endif
#include <ArduinoOTA.h>
#include <WiFiUdp.h>

#include "config.h"
#include "protocol.h"

// ─────────────────────────────────────────────────────────────
// Node-Registry
// ─────────────────────────────────────────────────────────────
struct NodeInfo { IPAddress ip; bool active; uint32_t lastSeen; };
NodeInfo nodes[MAX_NODES + 1];
uint8_t  nodeCount = 0;

WiFiUDP   udp;
IPAddress bcastIP(192, 168, 4, 255);

// ─────────────────────────────────────────────────────────────
// Spieler-Namen & Highscores
// ─────────────────────────────────────────────────────────────
struct PlayerScore {
  char     name[20];
  uint16_t points;
  uint32_t bestMs;
};
PlayerScore players[MAX_NODES + 1];

struct HighScore {
  char     name[20];
  uint16_t points;
  uint32_t bestMs;
};
HighScore highScores[MAX_HIGHSCORES];
uint8_t   highScoreCount = 0;

// ─────────────────────────────────────────────────────────────
// Spielzustand
// ─────────────────────────────────────────────────────────────
uint8_t  gameMode    = GAME_IDLE;
uint32_t gameEndTime = 0;

// Konfigurierbare Parameter
uint8_t  cfgTeams        = CTF_TEAMS;
uint16_t cfgDuration     = CTF_DURATION_S;
uint8_t  cfgSeqLen       = BOMB_SEQ_LEN;
uint16_t cfgBombDur      = BOMB_DURATION_S;
uint8_t  cfgReactRounds  = REACT_ROUNDS_DEFAULT;
uint8_t  cfgMines        = MINE_COUNT_DEFAULT;
uint8_t  cfgKnockLives   = KNOCK_LIVES_DEFAULT;
uint8_t  cfgHuntRounds   = COLORHUNT_ROUNDS;

// CTF
uint8_t ctfTeam[MAX_NODES + 1];

// Memory
uint8_t  memColor[MAX_NODES + 1];
bool     memMatched[MAX_NODES + 1];
int8_t   memPending[2];
uint32_t memHideAt;
uint8_t  memTotalPairs, memFoundPairs;

// Bomb
uint8_t  bombNode, bombStep;
uint8_t  bombSeq[16];
uint8_t  bombSeqLen;   // tatsaechlich verwendete Sequenzlaenge (<= verfuegbare Nodes)
bool     bombOver;

// Reaktion
uint8_t  reactRound      = 0;
uint8_t  reactTarget     = 0;
uint8_t  reactLastTarget = 0;
uint32_t reactLitAt      = 0;
bool     reactRoundDone  = true;
uint32_t reactNextAt     = 0;

// Simon Says
uint8_t  simonSeq[20];
uint8_t  simonLen;
uint8_t  simonStep;
bool     simonShowing;
uint32_t simonTimer;
uint8_t  simonShowIdx;
uint8_t  simonHighScore;

// Hot Potato
uint8_t  potatoHolder;
uint8_t  potatoLives[MAX_NODES + 1];
bool     potatoActive[MAX_NODES + 1];
uint32_t potatoExplodeAt;
uint32_t potatoMaxTimer;
uint8_t  potatoActiveCnt;

// King of the Hill
uint8_t  kingThrone;
uint8_t  kingHolder;
uint32_t kingHoldTime[MAX_NODES + 1];
uint32_t kingLastCapture;
uint32_t kingNextMove;

// Tug of War
int16_t  tugScore;

// Minesweeper
bool     mineField[MAX_NODES + 1];
bool     mineRevealed[MAX_NODES + 1];
uint8_t  mineLives;
uint8_t  mineScore;
uint8_t  mineSafeCount;

// Knockout
bool     knockActive[MAX_NODES + 1];
bool     knockPressed[MAX_NODES + 1];
uint8_t  knockLives[MAX_NODES + 1];
uint32_t knockLitAt;
uint32_t knockGraceAt;
uint8_t  knockTarget;
uint8_t  knockRemaining;
bool     knockRoundDone;
uint32_t knockNextAt;
uint8_t  knockRound;

// Color Hunt
uint8_t  huntColors[MAX_NODES + 1];
uint8_t  huntTarget;
uint8_t  huntScores[MAX_NODES + 1];
uint8_t  huntRound;
bool     huntRoundActive;
uint32_t huntNextAt;

// Whack-a-Mole
uint8_t  whamRound;
uint8_t  whamTarget;
uint8_t  whamScores[MAX_NODES + 1];
uint32_t whamLitAt;
uint32_t whamNextAt;
bool     whamRoundDone;
uint8_t  cfgWhamRounds   = WHAM_ROUNDS_DEFAULT;

// Spielverlauf
struct GameRecord { uint8_t mode; char winner[20]; uint16_t score; };
GameRecord gameHistory[MAX_HISTORY];
uint8_t    historyCount  = 0;
uint8_t    cfgMaxHistory = MAX_HISTORY;
uint16_t   totalGames    = 0;

// ─────────────────────────────────────────────────────────────
// Netzwerk-Helfer
// ─────────────────────────────────────────────────────────────
void sendPkt(IPAddress ip, uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  Packet p; p.type=type; p.nodeId=nid;
  p.data[0]=d0;p.data[1]=d1;p.data[2]=d2;
  p.data[3]=d3;p.data[4]=d4;p.data[5]=d5;
  udp.beginPacket(ip, UDP_PORT);
  udp.write((uint8_t*)&p, sizeof(p));
  udp.endPacket();
}

void setLED(uint8_t id, uint8_t color, uint8_t anim) {
  if (id<1||id>nodeCount||!nodes[id].active) return;
  sendPkt(nodes[id].ip, PKT_SET_LED, id, color, anim);
}

void allLED(uint8_t color, uint8_t anim) {
  for (uint8_t i=1;i<=nodeCount;i++) { if(nodes[i].active) setLED(i,color,anim); }
}

void setBar(uint8_t id, uint8_t color, uint8_t count, uint8_t bg) {
  if (id<1||id>nodeCount||!nodes[id].active) return;
  sendPkt(nodes[id].ip, PKT_SET_BAR, id, color, count, bg);
}

void setSplit(uint8_t id, uint8_t colorA, uint8_t countA, uint8_t colorB) {
  if (id<1||id>nodeCount||!nodes[id].active) return;
  sendPkt(nodes[id].ip, PKT_SET_SPLIT, id, colorA, countA, colorB);
}

void allBar(uint8_t color, uint8_t count, uint8_t bg) {
  for (uint8_t i=1;i<=nodeCount;i++) { if(nodes[i].active) setBar(i,color,count,bg); }
}

void allSplit(uint8_t colorA, uint8_t countA, uint8_t colorB) {
  for (uint8_t i=1;i<=nodeCount;i++) { if(nodes[i].active) setSplit(i,colorA,countA,colorB); }
}

// ─────────────────────────────────────────────────────────────
// CTF
// ─────────────────────────────────────────────────────────────
static const uint8_t TEAM_COLORS[] = {COL_WHITE,COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW};
static const char*   TEAM_NAMES[]  = {"Neutral","ROT","BLAU","GRUEN","GELB"};

void ctfStart() {
  if (nodeCount<2) { Serial.println("[CTF] Mindestens 2 Nodes."); return; }
  gameMode=GAME_CTF; gameEndTime=millis()+(uint32_t)cfgDuration*1000;
  for (uint8_t i=1;i<=nodeCount;i++) { ctfTeam[i]=0; setLED(i,COL_WHITE,ANIM_SOLID); }
  Serial.printf("[CTF] Start: %u Nodes %us %u Teams\n",nodeCount,cfgDuration,cfgTeams);
}

void ctfOnButton(uint8_t id) {
  ctfTeam[id]=(ctfTeam[id]%cfgTeams)+1;
  setLED(id,TEAM_COLORS[ctfTeam[id]],ANIM_FLASH);
  Serial.printf("[CTF] Node%u → %s\n",id,TEAM_NAMES[ctfTeam[id]]);
}

void ctfUpdate() {
  if ((long)(millis()-gameEndTime)<0) return;
  uint8_t score[5]={0};
  for (uint8_t i=1;i<=nodeCount;i++) if(ctfTeam[i]>0&&ctfTeam[i]<=4) score[ctfTeam[i]]++;
  uint8_t winner=1;
  for (uint8_t t=2;t<=cfgTeams;t++) if(score[t]>score[winner]) winner=t;
  for (uint8_t t=1;t<=cfgTeams;t++) Serial.printf("[CTF] %s: %u\n",TEAM_NAMES[t],score[t]);
  addHistory(GAME_CTF, TEAM_NAMES[winner], score[winner]);
  allLED(TEAM_COLORS[winner],ANIM_BLINK_FAST);
  gameMode=GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// Memory
// ─────────────────────────────────────────────────────────────
static const uint8_t PAIR_PALETTE[] = {COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW,COL_PURPLE,COL_CYAN,COL_ORANGE};

void memStart() {
  if (nodeCount<2) { Serial.println("[MEM] Mindestens 2 Nodes."); return; }
  gameMode=GAME_MEMORY;
  uint8_t n=(nodeCount%2==0)?nodeCount:nodeCount-1;
  memTotalPairs=n/2;
  if (memTotalPairs>7) { memTotalPairs=7; n=14; }  // Palette hat nur 7 Farben -> sonst nicht unterscheidbar
  memFoundPairs=0;
  memPending[0]=memPending[1]=-1; memHideAt=0;
  uint8_t pool[MAX_NODES];
  for (uint8_t i=0;i<memTotalPairs;i++) { pool[2*i]=PAIR_PALETTE[i%7]; pool[2*i+1]=PAIR_PALETTE[i%7]; }
  randomSeed(millis());
  for (int i=n-1;i>0;i--) { int j=random(0,i+1); uint8_t t=pool[i];pool[i]=pool[j];pool[j]=t; }
  for (uint8_t i=1;i<=n;i++) { memColor[i]=pool[i-1]; memMatched[i]=false; }
  for (uint8_t i=n+1;i<=nodeCount;i++) setLED(i,COL_OFF,ANIM_SOLID);
  for (uint8_t i=1;i<=n;i++) { setLED(i,memColor[i],ANIM_SOLID); }
  delay(2000);
  for (uint8_t i=1;i<=n;i++) { setLED(i,COL_OFF,ANIM_SOLID); }
  Serial.printf("[MEM] %u Paare\n",memTotalPairs);
}

void memOnButton(uint8_t id) {
  uint8_t n=2*memTotalPairs;
  if (id>n||memMatched[id]||memHideAt!=0) return;
  if (memPending[0]==id||memPending[1]==id) return;
  setLED(id,memColor[id],ANIM_SOLID);
  if (memPending[0]<0) { memPending[0]=id; }
  else {
    memPending[1]=id;
    if (memColor[memPending[0]]==memColor[id]) {
      memMatched[memPending[0]]=memMatched[id]=true;
      setLED(memPending[0],COL_GREEN,ANIM_PULSE); setLED(id,COL_GREEN,ANIM_PULSE);
      memFoundPairs++; memPending[0]=memPending[1]=-1;
      if (memFoundPairs==memTotalPairs) { delay(400); allLED(COL_GREEN,ANIM_BLINK_FAST); gameMode=GAME_IDLE; addHistory(GAME_MEMORY,"Geloest",memTotalPairs); }
    } else { memHideAt=millis()+1500; }
  }
}

void memUpdate() {
  if (memHideAt==0||(long)(millis()-memHideAt)<0) return;
  memHideAt=0;
  if (memPending[0]>=0) { setLED(memPending[0],COL_OFF,ANIM_SOLID); memPending[0]=-1; }
  if (memPending[1]>=0) { setLED(memPending[1],COL_OFF,ANIM_SOLID); memPending[1]=-1; }
}

// ─────────────────────────────────────────────────────────────
// Bomb
// ─────────────────────────────────────────────────────────────
static const uint8_t SEQ_COLORS[] = {COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW,COL_PURPLE,COL_CYAN,COL_ORANGE,COL_WHITE};

void bombShowSequence() {
  for (uint8_t s=0;s<bombSeqLen;s++) {
    setLED(bombSeq[s],SEQ_COLORS[s%8],ANIM_SOLID); delay(BOMB_STEP_MS);
    setLED(bombSeq[s],COL_OFF,ANIM_SOLID); delay(200);
  }
}

void bombStart() {
  if (nodeCount<3) { Serial.println("[BOMB] Mindestens 3 Nodes."); return; }
  gameMode=GAME_BOMB; bombStep=0; bombOver=false;
  gameEndTime=millis()+(uint32_t)cfgBombDur*1000;
  bombNode=random(1,nodeCount+1);
  uint8_t avail[MAX_NODES],cnt=0;
  for (uint8_t i=1;i<=nodeCount;i++) if(i!=bombNode) avail[cnt++]=i;
  uint8_t len=min((uint8_t)cfgSeqLen,cnt);
  if (len>16) len=16;
  bombSeqLen=len;
  for (uint8_t i=0;i<len;i++) { uint8_t p=random(0,cnt-i); bombSeq[i]=avail[p]; avail[p]=avail[cnt-i-1]; }
  allLED(COL_OFF,ANIM_SOLID); delay(200);
  setLED(bombNode,COL_RED,ANIM_BLINK_FAST);
  bombShowSequence();
  for (uint8_t i=1;i<=nodeCount;i++) { setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID); }
}

void bombOnButton(uint8_t id) {
  if (bombOver) return;
  if (id==bombNode) { bombShowSequence(); for (uint8_t i=1;i<=nodeCount;i++) { setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID); } return; }
  if (id==bombSeq[bombStep]) {
    setLED(id,COL_GREEN,ANIM_FLASH); bombStep++;
    if (bombStep==bombSeqLen) { bombOver=true; allLED(COL_GREEN,ANIM_BLINK_SLOW); gameMode=GAME_IDLE; Serial.println("[BOMB] ENTSCHAERFT!"); addHistory(GAME_BOMB,"Entschaerft",bombSeqLen); }
  } else { bombOver=true; allLED(COL_RED,ANIM_BLINK_FAST); gameMode=GAME_IDLE; Serial.println("[BOMB] BOOM!"); addHistory(GAME_BOMB,"Explodiert",bombStep); }
}

void bombUpdate() {
  if (bombOver||(long)(millis()-gameEndTime)<0) return;
  bombOver=true; allLED(COL_RED,ANIM_BLINK_FAST); gameMode=GAME_IDLE; Serial.println("[BOMB] ZEIT UM!"); addHistory(GAME_BOMB,"Zeit abgelaufen",bombStep);
}

// ─────────────────────────────────────────────────────────────
// Reaktionsspiel
// ─────────────────────────────────────────────────────────────
void reactInsertHighScore(uint8_t id) {
  if (players[id].name[0]==0) return;
  if (players[id].points==0) return;
  int8_t pos = -1;
  for (uint8_t j=0; j<highScoreCount; j++) {
    if (players[id].points > highScores[j].points ||
       (players[id].points == highScores[j].points && players[id].bestMs < highScores[j].bestMs)) {
      pos = j; break;
    }
  }
  if (pos < 0 && highScoreCount < MAX_HIGHSCORES) pos = highScoreCount;
  if (pos < 0) return;
  uint8_t end = min(highScoreCount, (uint8_t)(MAX_HIGHSCORES-1));
  for (uint8_t j=end; j>(uint8_t)pos; j--) highScores[j]=highScores[j-1];
  strncpy(highScores[pos].name, players[id].name, 19);
  highScores[pos].name[19]=0;
  highScores[pos].points = players[id].points;
  highScores[pos].bestMs = players[id].bestMs;
  if (highScoreCount < MAX_HIGHSCORES) highScoreCount++;
}

void addHistory(uint8_t mode, const char* winner, uint16_t score) {
  totalGames++;
  uint8_t cap = (cfgMaxHistory < MAX_HISTORY) ? cfgMaxHistory : MAX_HISTORY;
  if (historyCount < cap) {
    gameHistory[historyCount].mode  = mode;
    strncpy(gameHistory[historyCount].winner, winner, 19);
    gameHistory[historyCount].winner[19] = 0;
    gameHistory[historyCount].score = score;
    historyCount++;
  } else {
    for (uint8_t i=0; i<cap-1; i++) gameHistory[i] = gameHistory[i+1];
    gameHistory[cap-1].mode  = mode;
    strncpy(gameHistory[cap-1].winner, winner, 19);
    gameHistory[cap-1].winner[19] = 0;
    gameHistory[cap-1].score = score;
  }
}

void reactStartRound() {
  if (reactRound >= cfgReactRounds) {
    uint8_t winner=1;
    for (uint8_t i=2;i<=nodeCount;i++) if(players[i].points>players[winner].points) winner=i;
    Serial.println("[REACT] === ENDE ===");
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  %s: %u Pkt  Best: %ums\n", players[i].name, players[i].points, players[i].bestMs);
    for (uint8_t i=1;i<=nodeCount;i++) reactInsertHighScore(i);
    addHistory(GAME_REACTION, players[winner].name, players[winner].points);
    allLED(COL_OFF, ANIM_SOLID); delay(200);
    setLED(winner, COL_GREEN, ANIM_BLINK_FAST);
    for (uint8_t i=1;i<=nodeCount;i++) if(i!=winner) setLED(i,COL_RED,ANIM_SOLID);
    gameMode = GAME_IDLE;
    return;
  }
  uint8_t tries=0;
  do { reactTarget=random(1,nodeCount+1); tries++; }
  while (reactTarget==reactLastTarget && nodeCount>1 && tries<20);
  reactLastTarget = reactTarget;
  for (uint8_t i=1;i<=nodeCount;i++) {
    setLED(i, (i==reactTarget)?COL_YELLOW:COL_OFF, (i==reactTarget)?ANIM_BLINK_FAST:ANIM_SOLID);
    delay(10);
  }
  reactLitAt     = millis();
  reactRoundDone = false;
  reactRound++;
  Serial.printf("[REACT] Runde %u/%u – Node %u leuchtet!\n", reactRound, cfgReactRounds, reactTarget);
}

void reactStart() {
  if (nodeCount<2) { Serial.println("[REACT] Mindestens 2 Nodes."); return; }
  gameMode       = GAME_REACTION;
  reactRound     = 0;
  reactLastTarget = 0;
  for (uint8_t i=1;i<=nodeCount;i++) {
    players[i].points=0;
    players[i].bestMs=0;
    if (players[i].name[0]==0) snprintf(players[i].name,20,"Node %u",i);
  }
  Serial.println("[REACT] Countdown...");
  for (uint8_t c=3;c>0;c--) {
    allLED(COL_WHITE,ANIM_SOLID); delay(400);
    allLED(COL_OFF,ANIM_SOLID);   delay(300);
  }
  reactRoundDone = true;
  reactNextAt    = millis() + random(REACT_DELAY_MIN_MS, REACT_DELAY_MAX_MS);
}

void reactOnButton(uint8_t id) {
  if (reactRoundDone || gameMode!=GAME_REACTION) return;
  uint32_t ms = millis() - reactLitAt;
  if (id == reactTarget) {
    reactRoundDone = true;
    players[id].points++;
    if (players[id].bestMs==0 || ms<players[id].bestMs) players[id].bestMs=ms;
    Serial.printf("[REACT] %s: %ums → %u Pkt\n", players[id].name, ms, players[id].points);
    setLED(id, COL_GREEN, ANIM_FLASH);
    for (uint8_t i=1;i<=nodeCount;i++) if(i!=id) setLED(i,COL_RED,ANIM_BLINK_FAST);
    reactNextAt = millis() + random(REACT_DELAY_MIN_MS, REACT_DELAY_MAX_MS);
  }
}

void reactUpdate() {
  if (gameMode!=GAME_REACTION) return;
  uint32_t now=millis();
  if (reactRoundDone) {
    if (now>=reactNextAt) reactStartRound();
  } else if (now-reactLitAt > REACT_TIMEOUT_MS) {
    reactRoundDone=true;
    Serial.println("[REACT] Timeout – niemand gedrückt");
    allLED(COL_ORANGE, ANIM_BLINK_SLOW);
    reactNextAt = now + 1500;
  }
}

// ─────────────────────────────────────────────────────────────
// Simon Says (Game 5)
// ─────────────────────────────────────────────────────────────
void simonNextRound() {
  // Sequenz ist voll (Array-Groesse 20) -> Spiel als gewonnen beenden
  if (simonLen >= 20) {
    Serial.println("[SIMON] Maximale Sequenz erreicht - gewonnen!");
    addHistory(GAME_SIMON, "Simon (max)", simonLen);
    allLED(COL_GREEN, ANIM_BLINK_FAST);
    gameMode = GAME_IDLE;
    return;
  }
  // Extend sequence by 1
  simonSeq[simonLen] = random(1, nodeCount+1);
  simonLen++;
  simonShowIdx  = 0;
  simonShowing  = true;
  simonTimer    = millis();
  // Turn all off before show
  allLED(COL_OFF, ANIM_SOLID);
  Serial.printf("[SIMON] Runde %u – zeige Sequenz\n", simonLen);
}

void simonStart() {
  if (nodeCount<2) { Serial.println("[SIMON] Mindestens 2 Nodes."); return; }
  gameMode      = GAME_SIMON;
  simonLen      = 0;
  simonStep     = 0;
  simonHighScore = 0;
  randomSeed(millis());
  allLED(COL_OFF, ANIM_SOLID);
  delay(300);
  simonNextRound();
}

void simonOnButton(uint8_t id) {
  if (simonShowing) return; // ignore during show phase
  if (id == simonSeq[simonStep]) {
    setLED(id, COL_GREEN, ANIM_FLASH);
    simonStep++;
    if (simonStep == simonLen) {
      // Completed this round
      Serial.printf("[SIMON] Runde %u korrekt!\n", simonLen);
      if (simonLen > simonHighScore) simonHighScore = simonLen;
      delay(600);
      simonStep = 0;
      simonNextRound();
    }
  } else {
    // Wrong press – game over
    Serial.printf("[SIMON] Falsch! Erreichte Runde: %u\n", simonLen);
    addHistory(GAME_SIMON, "Simon", simonLen);
    allLED(COL_RED, ANIM_BLINK_FAST);
    delay(1500);
    allLED(COL_OFF, ANIM_SOLID);
    gameMode = GAME_IDLE;
  }
}

void simonUpdate() {
  if (!simonShowing) return;
  uint32_t now = millis();
  uint32_t elapsed = now - simonTimer;

  // Each step: 600ms ON, 200ms OFF = 800ms per step
  uint8_t step = simonShowIdx;
  if (step >= simonLen) {
    // Show phase done – go to input phase
    simonShowing = false;
    simonStep    = 0;
    allLED(COL_OFF, ANIM_SOLID);
    Serial.println("[SIMON] Eingabephase");
    return;
  }

  uint32_t stepStart = (uint32_t)step * 800UL;
  uint32_t onEnd     = stepStart + 600UL;
  uint32_t offEnd    = stepStart + 800UL;

  if (elapsed < onEnd) {
    // Light this node green
    setLED(simonSeq[step], COL_GREEN, ANIM_SOLID);
  } else if (elapsed < offEnd) {
    // Turn off
    setLED(simonSeq[step], COL_OFF, ANIM_SOLID);
  } else {
    // Advance to next step
    simonShowIdx++;
  }
}

// ─────────────────────────────────────────────────────────────
// Hot Potato (Game 6)
// ─────────────────────────────────────────────────────────────
void potatoShowLives(uint8_t id) {
  // Show remaining lives as green LEDs on this node
  setBar(id, COL_GREEN, potatoLives[id], COL_OFF);
}

void potatoSetHolder(uint8_t id) {
  // Previous holder back to lives display
  if (potatoHolder >= 1 && potatoHolder <= nodeCount) {
    potatoShowLives(potatoHolder);
  }
  potatoHolder = id;
  setLED(id, COL_ORANGE, ANIM_BLINK_FAST);
  // New explode time
  uint32_t timer = potatoMaxTimer;
  if (timer < 2000) timer = 2000;
  potatoExplodeAt = millis() + timer;
  Serial.printf("[POTATO] Holder: Node %u (Timer: %ums)\n", id, timer);
}

void potatoCountActive() {
  potatoActiveCnt = 0;
  for (uint8_t i=1;i<=nodeCount;i++) if(potatoActive[i]) potatoActiveCnt++;
}

void potatoStart() {
  if (nodeCount<2) { Serial.println("[POTATO] Mindestens 2 Nodes."); return; }
  gameMode       = GAME_HOTPOTATO;
  potatoMaxTimer = 8000;
  randomSeed(millis());
  for (uint8_t i=1;i<=nodeCount;i++) {
    potatoLives[i]  = 3;
    potatoActive[i] = nodes[i].active;
  }
  potatoCountActive();
  // Pick random starting holder
  uint8_t startHolder = random(1, nodeCount+1);
  while (!potatoActive[startHolder]) startHolder = (startHolder % nodeCount) + 1;
  potatoHolder = 0;
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (nodes[i].active) potatoShowLives(i);
  }
  delay(500);
  potatoSetHolder(startHolder);
  Serial.printf("[POTATO] Start! %u Spieler\n", potatoActiveCnt);
}

void potatoOnButton(uint8_t id) {
  if (id != potatoHolder) return; // only holder can pass
  // Reduce maxTimer
  if (potatoMaxTimer > 2200) potatoMaxTimer -= 200;
  // Pick random other active node
  uint8_t candidates[MAX_NODES];
  uint8_t cnt = 0;
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (potatoActive[i] && i!=id) candidates[cnt++]=i;
  }
  if (cnt == 0) return;
  uint8_t next = candidates[random(0, cnt)];
  potatoSetHolder(next);
}

void potatoUpdate() {
  if ((long)(millis() - potatoExplodeAt) < 0) return;
  // Explosion! Holder loses a life
  uint8_t h = potatoHolder;
  potatoLives[h]--;
  Serial.printf("[POTATO] Node %u Explosion! Leben: %u\n", h, potatoLives[h]);
  setLED(h, COL_RED, ANIM_BLINK_FAST);
  delay(800);
  if (potatoLives[h] == 0) {
    potatoActive[h] = false;
    setLED(h, COL_OFF, ANIM_SOLID);
    Serial.printf("[POTATO] Node %u ausgeschieden!\n", h);
    potatoCountActive();
    if (potatoActiveCnt <= 1) {
      // Find winner
      for (uint8_t i=1;i<=nodeCount;i++) {
        if (potatoActive[i]) { setLED(i, COL_GREEN, ANIM_BLINK_FAST); break; }
      }
      Serial.println("[POTATO] Spiel beendet!");
      gameMode = GAME_IDLE;
      return;
    }
  } else {
    potatoShowLives(h);
    delay(500);
  }
  // Pass to random active node
  uint8_t candidates[MAX_NODES];
  uint8_t cnt = 0;
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (potatoActive[i] && i!=h) candidates[cnt++]=i;
  }
  if (cnt == 0) { gameMode=GAME_IDLE; return; }
  uint8_t next = candidates[random(0, cnt)];
  potatoHolder = 0; // reset so potatoSetHolder doesn't try to restore old holder's LED
  // Show remaining players' lives
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (potatoActive[i] && i!=next) potatoShowLives(i);
  }
  potatoSetHolder(next);
}

// ─────────────────────────────────────────────────────────────
// King of the Hill (Game 7)
// ─────────────────────────────────────────────────────────────
void kingMoveThrone() {
  // Pick a new random throne (different from current)
  uint8_t newThrone;
  uint8_t tries = 0;
  do { newThrone = random(1, nodeCount+1); tries++; }
  while (newThrone == kingThrone && nodeCount > 1 && tries < 20);
  // Flash all white
  allLED(COL_WHITE, ANIM_SOLID);
  delay(300);
  // Restore state
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (i == kingHolder && i != newThrone) {
      setLED(i, COL_PURPLE, ANIM_SOLID); // holder gets purple when off throne
    } else if (i != newThrone) {
      setLED(i, COL_OFF, ANIM_SOLID);
    }
    delay(15);
  }
  kingThrone = newThrone;
  setLED(kingThrone, COL_YELLOW, ANIM_PULSE);
  kingNextMove = millis() + (uint32_t)KING_THRONE_MOVE_S * 1000UL;
  Serial.printf("[KING] Thron bewegt zu Node %u\n", kingThrone);
}

void kingStart() {
  if (nodeCount<2) { Serial.println("[KING] Mindestens 2 Nodes."); return; }
  gameMode       = GAME_KINGHILL;
  gameEndTime    = millis() + (uint32_t)cfgDuration * 1000UL;
  kingHolder     = 0;
  randomSeed(millis());
  for (uint8_t i=0;i<=MAX_NODES;i++) kingHoldTime[i]=0;
  kingThrone     = random(1, nodeCount+1);
  kingLastCapture = millis();
  kingNextMove   = millis() + (uint32_t)KING_THRONE_MOVE_S * 1000UL;
  allLED(COL_OFF, ANIM_SOLID);
  setLED(kingThrone, COL_YELLOW, ANIM_PULSE);
  Serial.printf("[KING] Start! Thron: Node %u Dauer: %us\n", kingThrone, cfgDuration);
}

void kingOnButton(uint8_t id) {
  if (id != kingThrone) return; // only throne node matters
  uint32_t now = millis();
  // Accumulate hold time for previous holder
  if (kingHolder >= 1 && kingHolder <= nodeCount) {
    kingHoldTime[kingHolder] += (now - kingLastCapture);
  }
  kingHolder = id;
  kingLastCapture = now;
  setLED(kingThrone, COL_GREEN, ANIM_SOLID);
  Serial.printf("[KING] Node %u hat den Thron!\n", id);
}

void kingUpdate() {
  uint32_t now = millis();
  // Accumulate hold time
  if (kingHolder >= 1 && kingHolder <= nodeCount) {
    kingHoldTime[kingHolder] += (now - kingLastCapture);
    kingLastCapture = now;
  }
  // Move throne?
  if ((long)(now - kingNextMove) >= 0) {
    kingMoveThrone();
    kingHolder = 0;
    kingLastCapture = millis();
  }
  // Game over?
  if ((long)(now - gameEndTime) >= 0) {
    uint8_t winner = 1;
    for (uint8_t i=2;i<=nodeCount;i++) if(kingHoldTime[i]>kingHoldTime[winner]) winner=i;
    Serial.printf("[KING] Spiel beendet! Gewinner: Node %u (%s) mit %ums\n",
      winner, players[winner].name, kingHoldTime[winner]);
    addHistory(GAME_KINGHILL, players[winner].name, kingHoldTime[winner]/1000);
    allLED(COL_OFF, ANIM_SOLID);
    setLED(winner, COL_YELLOW, ANIM_BLINK_FAST);
    gameMode = GAME_IDLE;
  }
}

// ─────────────────────────────────────────────────────────────
// Tug of War (Game 8)
// ─────────────────────────────────────────────────────────────
void tugUpdateDisplay() {
  // tugScore: 0=all Blue, 100=all Red; starts at 50
  // Split bar: (tugScore*8/100) red LEDs, rest blue
  uint8_t redCount = (uint8_t)((tugScore * 8) / 100);
  if (redCount > 8) redCount = 8;
  allSplit(COL_RED, redCount, COL_BLUE);
}

void tugStart() {
  if (nodeCount<2) { Serial.println("[TUG] Mindestens 2 Nodes."); return; }
  gameMode   = GAME_TUGWAR;
  gameEndTime = millis() + (uint32_t)cfgDuration * 1000UL;
  tugScore   = 50;
  // Show teams: lower half red, upper half blue
  uint8_t half = nodeCount / 2;
  for (uint8_t i=1;i<=nodeCount;i++) {
    setLED(i, (i<=half)?COL_RED:COL_BLUE, ANIM_SOLID);
  }
  delay(800);
  tugUpdateDisplay();
  Serial.printf("[TUG] Start! Team A: Nodes 1-%u (ROT), Team B: Nodes %u-%u (BLAU)\n",
    half, half+1, nodeCount);
}

void tugOnButton(uint8_t id) {
  uint8_t half = nodeCount / 2;
  if (id <= half) {
    tugScore++; if (tugScore>100) tugScore=100;
  } else {
    tugScore--; if (tugScore<0) tugScore=0;
  }
  tugUpdateDisplay();
  if (tugScore >= 100) {
    Serial.println("[TUG] Team A (ROT) gewinnt!");
    allLED(COL_RED, ANIM_BLINK_FAST);
    addHistory(GAME_TUGWAR, "ROT", tugScore);
    gameMode = GAME_IDLE;
  } else if (tugScore <= 0) {
    Serial.println("[TUG] Team B (BLAU) gewinnt!");
    allLED(COL_BLUE, ANIM_BLINK_FAST);
    addHistory(GAME_TUGWAR, "BLAU", 100-tugScore);
    gameMode = GAME_IDLE;
  }
}

void tugUpdate() {
  if ((long)(millis()-gameEndTime)<0) return;
  if (tugScore > 50) {
    Serial.println("[TUG] Zeit! Team A (ROT) gewinnt!"); allLED(COL_RED, ANIM_BLINK_FAST);
    addHistory(GAME_TUGWAR, tugScore>50?"ROT":(tugScore<50?"BLAU":"Unentschieden"), (uint16_t)abs(tugScore-50));
  } else if (tugScore < 50) {
    Serial.println("[TUG] Zeit! Team B (BLAU) gewinnt!"); allLED(COL_BLUE, ANIM_BLINK_FAST);
    addHistory(GAME_TUGWAR, tugScore>50?"ROT":(tugScore<50?"BLAU":"Unentschieden"), (uint16_t)abs(tugScore-50));
  } else {
    Serial.println("[TUG] Zeit! Unentschieden!"); allLED(COL_WHITE, ANIM_BLINK_SLOW);
    addHistory(GAME_TUGWAR, "Unentschieden", 0);
  }
  gameMode = GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// Minesweeper (Game 9)
// ─────────────────────────────────────────────────────────────
void mineRefreshDisplay() {
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (!nodes[i].active) continue;
    if (mineRevealed[i]) {
      setLED(i, mineField[i] ? COL_RED : COL_GREEN, ANIM_SOLID);
    } else {
      // dim white for unrevealed – use bar with 2 white LEDs out of 8 as "dim"
      setBar(i, COL_WHITE, 2, COL_OFF);
    }
    delay(15);
  }
}

void mineShowLivesAll() {
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (!nodes[i].active) continue;
    setBar(i, COL_GREEN, mineLives, COL_OFF);
    delay(15);
  }
  delay(600);
  mineRefreshDisplay();
}

void mineStart() {
  if (nodeCount<3) { Serial.println("[MINE] Mindestens 3 Nodes."); return; }
  gameMode   = GAME_MINESWEEPER;
  mineLives  = 3;
  mineScore  = 0;
  uint8_t mines = cfgMines;
  if (mines >= nodeCount) mines = nodeCount - 1;

  // Init
  for (uint8_t i=1;i<=nodeCount;i++) {
    mineField[i]    = false;
    mineRevealed[i] = false;
  }
  mineSafeCount = nodeCount - mines;

  // Place mines randomly
  uint8_t pool[MAX_NODES];
  uint8_t cnt = 0;
  for (uint8_t i=1;i<=nodeCount;i++) pool[cnt++]=i;
  randomSeed(millis());
  for (int i=cnt-1;i>0;i--) { int j=random(0,i+1); uint8_t t=pool[i];pool[i]=pool[j];pool[j]=t; }
  for (uint8_t i=0;i<mines;i++) mineField[pool[i]]=true;

  mineRefreshDisplay();
  Serial.printf("[MINE] Start! %u Minen, %u sichere Nodes\n", mines, mineSafeCount);
}

void mineOnButton(uint8_t id) {
  if (mineRevealed[id]) return;
  mineRevealed[id] = true;
  if (mineField[id]) {
    // Mine!
    setLED(id, COL_RED, ANIM_BLINK_FAST);
    mineLives--;
    Serial.printf("[MINE] Node %u = MINE! Leben: %u\n", id, mineLives);
    delay(600);
    if (mineLives == 0) {
      // Game over
      allLED(COL_RED, ANIM_BLINK_FAST);
      Serial.println("[MINE] Game Over!");
      addHistory(GAME_MINESWEEPER, "Explodiert", mineScore);
      gameMode = GAME_IDLE;
      return;
    }
    mineShowLivesAll();
  } else {
    // Safe
    setLED(id, COL_GREEN, ANIM_SOLID);
    mineScore++;
    Serial.printf("[MINE] Node %u sicher! Punkte: %u/%u\n", id, mineScore, mineSafeCount);
    if (mineScore == mineSafeCount) {
      // Win!
      allLED(COL_GREEN, ANIM_BLINK_FAST);
      Serial.println("[MINE] Alle sicheren Nodes gefunden! Gewonnen!");
      addHistory(GAME_MINESWEEPER, "Geloest", mineScore);
      gameMode = GAME_IDLE;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Knockout (Game 10)
// ─────────────────────────────────────────────────────────────
void knockCountRemaining() {
  knockRemaining = 0;
  for (uint8_t i=1;i<=nodeCount;i++) if(knockActive[i]) knockRemaining++;
}

void knockStartRound() {
  knockRound++;
  // Pick random active target
  uint8_t cands[MAX_NODES]; uint8_t cnt=0;
  for (uint8_t i=1;i<=nodeCount;i++) if(knockActive[i]) cands[cnt++]=i;
  if (cnt == 0) { gameMode=GAME_IDLE; return; }
  knockTarget = cands[random(0,cnt)];
  // Reset pressed flags
  for (uint8_t i=1;i<=nodeCount;i++) knockPressed[i]=false;
  // Light target yellow, others off
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (!knockActive[i]) continue;
    setLED(i, (i==knockTarget)?COL_YELLOW:COL_OFF, (i==knockTarget)?ANIM_BLINK_FAST:ANIM_SOLID);
    delay(10);
  }
  knockLitAt    = millis();
  knockRoundDone = false;
  knockGraceAt  = 0;
  Serial.printf("[KNOCK] Runde %u – Node %u leuchtet!\n", knockRound, knockTarget);
}

void knockStart() {
  if (nodeCount<2) { Serial.println("[KNOCK] Mindestens 2 Nodes."); return; }
  gameMode      = GAME_KNOCKOUT;
  knockRound    = 0;
  knockRoundDone = true;
  knockGraceAt  = 0;   // wichtig: alte Gnadenfrist verwerfen, sonst sofortiger Lebensabzug
  knockNextAt   = millis() + 2000;
  for (uint8_t i=1;i<=nodeCount;i++) {
    knockActive[i]  = nodes[i].active;
    knockLives[i]   = cfgKnockLives;
    knockPressed[i] = false;
  }
  knockCountRemaining();
  // Show lives bars
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (knockActive[i]) setBar(i, COL_GREEN, knockLives[i], COL_OFF);
    delay(15);
  }
  delay(800);
  Serial.printf("[KNOCK] Start! %u Spieler, %u Leben\n", knockRemaining, cfgKnockLives);
}

void knockOnButton(uint8_t id) {
  if (gameMode!=GAME_KNOCKOUT || !knockActive[id]) return;
  if (!knockRoundDone) {
    // Runde laeuft: Druck zaehlt, erster Treffer startet die Gnadenfrist
    knockPressed[id] = true;
    if (id == knockTarget) {
      setLED(id, COL_GREEN, ANIM_FLASH);
      Serial.printf("[KNOCK] Node %u als erstes!\n", id);
      knockGraceAt   = millis() + 2000;
      knockRoundDone = true;
    } else {
      setLED(id, COL_WHITE, ANIM_SOLID); // Druck bestaetigen
    }
  } else if (knockGraceAt != 0) {
    // Gnadenfrist: weitere Spieler koennen noch druecken und Strafe vermeiden
    if (!knockPressed[id]) {
      knockPressed[id] = true;
      setLED(id, COL_WHITE, ANIM_SOLID);
    }
  }
}

void knockUpdate() {
  uint32_t now = millis();
  if (knockRoundDone) {
    // If grace period running, check if it expired
    if (knockGraceAt != 0 && (long)(now - knockGraceAt) >= 0) {
      // Penalize those who didn't press
      bool eliminated = false;
      for (uint8_t i=1;i<=nodeCount;i++) {
        if (!knockActive[i]) continue;
        if (!knockPressed[i]) {
          knockLives[i]--;
          Serial.printf("[KNOCK] Node %u zu langsam! Leben: %u\n", i, knockLives[i]);
          if (knockLives[i] == 0) {
            knockActive[i] = false;
            setLED(i, COL_OFF, ANIM_SOLID);
            Serial.printf("[KNOCK] Node %u ausgeschieden!\n", i);
            eliminated = true;
          } else {
            setBar(i, COL_GREEN, knockLives[i], COL_OFF);
          }
        }
      }
      knockCountRemaining();
      knockGraceAt = 0;
      if (knockRemaining <= 1) {
        // Find winner
        uint8_t knockWinner = 0;
        for (uint8_t i=1;i<=nodeCount;i++) {
          if (knockActive[i]) { knockWinner=i; setLED(i, COL_GREEN, ANIM_BLINK_FAST); break; }
        }
        if (knockRemaining == 0) allLED(COL_WHITE, ANIM_BLINK_SLOW); // draw
        Serial.println("[KNOCK] Spiel beendet!");
        if (knockRemaining == 1 && knockWinner > 0)
          addHistory(GAME_KNOCKOUT, players[knockWinner].name, knockRound);
        else
          addHistory(GAME_KNOCKOUT, "Unentschieden", knockRound);
        gameMode = GAME_IDLE;
        return;
      }
      knockNextAt = now + random(REACT_DELAY_MIN_MS, REACT_DELAY_MAX_MS);
    } else if (knockGraceAt == 0 && (long)(now - knockNextAt) >= 0) {
      knockStartRound();
    }
  } else {
    // Round in progress – check timeout
    if (now - knockLitAt > REACT_TIMEOUT_MS) {
      knockRoundDone = true;
      knockGraceAt   = now; // immediate penalty
      Serial.println("[KNOCK] Timeout!");
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Color Hunt (Game 11)
// ─────────────────────────────────────────────────────────────
void huntStartRound() {
  huntRound++;
  if (huntRound > cfgHuntRounds) {
    // Game over – find winner
    uint8_t winner = 1;
    for (uint8_t i=2;i<=nodeCount;i++) if(huntScores[i]>huntScores[winner]) winner=i;
    Serial.println("[HUNT] Spiel beendet!");
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  Node %u (%s): %u Pkt\n", i, players[i].name, huntScores[i]);
    addHistory(GAME_COLORHUNT, players[winner].name, huntScores[winner]);
    allLED(COL_OFF, ANIM_SOLID);
    setLED(winner, COL_GREEN, ANIM_BLINK_FAST);
    gameMode = GAME_IDLE;
    return;
  }

  // Flash each node its color for 1500ms
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (nodes[i].active) setLED(i, huntColors[i], ANIM_SOLID);
    delay(15);
  }
  delay(1500);

  // All go dim white
  for (uint8_t i=1;i<=nodeCount;i++) {
    if (!nodes[i].active) continue;
    setBar(i, COL_WHITE, 2, COL_OFF);
    delay(15);
  }

  // Node 1 blinks target color
  // Pick a random target color that exists among the nodes
  uint8_t targetColor = huntColors[huntTarget]; // huntTarget assigned in start
  // Blink node 1 with target color for 2s, then dim
  if (nodeCount >= 1 && nodes[1].active) {
    setLED(1, targetColor, ANIM_BLINK_FAST);
  }
  huntRoundActive = true;
  huntNextAt      = millis() + 2000; // display node shows for 2s
  Serial.printf("[HUNT] Runde %u/%u – Zielfarbe: %u\n", huntRound, cfgHuntRounds, targetColor);
}

void huntStart() {
  if (nodeCount<2) { Serial.println("[HUNT] Mindestens 2 Nodes."); return; }
  gameMode   = GAME_COLORHUNT;
  huntRound  = 0;
  huntRoundActive = false;
  randomSeed(millis());
  for (uint8_t i=1;i<=nodeCount;i++) {
    huntScores[i] = 0;
    huntColors[i] = PAIR_PALETTE[random(0, 7)]; // assign random color from palette
  }
  // Pick a target node whose color players must match (node 1 is display node)
  // Target is a different node each round – pick randomly
  huntTarget = (nodeCount >= 2) ? 2 : 1;
  huntNextAt = millis() + 1000;
  Serial.printf("[HUNT] Start! %u Runden\n", cfgHuntRounds);
  huntStartRound();
}

void huntOnButton(uint8_t id) {
  if (!huntRoundActive) return;
  uint8_t targetColor = huntColors[huntTarget];
  if (huntColors[id] == targetColor && id != 1) {
    // Correct!
    huntScores[id]++;
    huntRoundActive = false;
    setLED(id, COL_GREEN, ANIM_BLINK_FAST);
    for (uint8_t i=1;i<=nodeCount;i++) {
      if (i!=id && nodes[i].active) setLED(i, COL_RED, ANIM_SOLID);
      delay(10);
    }
    Serial.printf("[HUNT] Node %u korrekt! Punkte: %u\n", id, huntScores[id]);
    // Pick new target for next round
    uint8_t newTarget;
    uint8_t tries=0;
    do { newTarget=random(2,nodeCount+1); tries++; }
    while (newTarget==huntTarget && nodeCount>2 && tries<20);
    huntTarget = newTarget;
    huntNextAt = millis() + 1500;
  } else if (id != 1) {
    // Wrong – flash red briefly
    setLED(id, COL_RED, ANIM_BLINK_FAST);
    Serial.printf("[HUNT] Node %u falsch!\n", id);
    huntNextAt = millis() + 200; // small delay re-allow same round
  }
}

void huntUpdate() {
  uint32_t now = millis();
  if (!huntRoundActive && (long)(now - huntNextAt) >= 0) {
    huntStartRound();
  } else if (huntRoundActive && (long)(now - huntNextAt) >= 0) {
    // Display timeout – node 1 goes dim
    if (nodeCount >= 1 && nodes[1].active) {
      setBar(1, COL_WHITE, 2, COL_OFF);
    }
    huntNextAt = now + 30000; // don't fire again for a while
  }
}

// ─────────────────────────────────────────────────────────────
// Whack-a-Mole (Game 12)
// ─────────────────────────────────────────────────────────────
static uint8_t whamLastTarget = 0;

void whamStartRound() {
  if (whamRound >= cfgWhamRounds) {
    uint8_t winner=1;
    for (uint8_t i=2;i<=nodeCount;i++) if(whamScores[i]>whamScores[winner]) winner=i;
    Serial.println("[WHAM] === ENDE ===");
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  %s: %u Treffer\n", players[i].name, whamScores[i]);
    allLED(COL_OFF, ANIM_SOLID); delay(200);
    setLED(winner, COL_GREEN, ANIM_BLINK_FAST);
    for (uint8_t i=1;i<=nodeCount;i++) if(i!=winner) setLED(i,COL_RED,ANIM_SOLID);
    addHistory(GAME_WHACKAMOLE, players[winner].name, whamScores[winner]);
    gameMode = GAME_IDLE;
    return;
  }
  uint8_t tries=0;
  do { whamTarget=random(1,nodeCount+1); tries++; }
  while (whamTarget==whamLastTarget && nodeCount>1 && tries<20);
  whamLastTarget = whamTarget;
  for (uint8_t i=1;i<=nodeCount;i++) {
    setLED(i,(i==whamTarget)?COL_YELLOW:COL_OFF,(i==whamTarget)?ANIM_BLINK_FAST:ANIM_SOLID);
    delay(10);
  }
  whamLitAt     = millis();
  whamRoundDone = false;
  whamRound++;
  Serial.printf("[WHAM] Runde %u/%u - Node %u leuchtet!\n", whamRound, cfgWhamRounds, whamTarget);
}

void whamStart() {
  if (nodeCount<2) { Serial.println("[WHAM] Mindestens 2 Nodes."); return; }
  gameMode      = GAME_WHACKAMOLE;
  whamRound     = 0;
  whamRoundDone = true;
  whamLastTarget = 0;
  randomSeed(millis());
  for (uint8_t i=1;i<=nodeCount;i++) {
    whamScores[i]=0; players[i].points=0;
    if (players[i].name[0]==0) snprintf(players[i].name,20,"Node %u",i);
    setLED(i,COL_OFF,ANIM_SOLID); delay(10);
  }
  for (uint8_t c=3;c>0;c--) {
    allLED(COL_WHITE,ANIM_SOLID); delay(400);
    allLED(COL_OFF,ANIM_SOLID);   delay(300);
  }
  whamNextAt = millis() + random(WHAM_DELAY_MIN_MS, WHAM_DELAY_MAX_MS);
  Serial.printf("[WHAM] Start! %u Runden\n", cfgWhamRounds);
}

void whamOnButton(uint8_t id) {
  if (whamRoundDone || gameMode!=GAME_WHACKAMOLE) return;
  if (id == whamTarget) {
    whamRoundDone = true;
    whamScores[id]++;
    players[id].points++;
    Serial.printf("[WHAM] %s trifft! Punkte: %u\n", players[id].name, whamScores[id]);
    setLED(id, COL_GREEN, ANIM_FLASH);
    for (uint8_t i=1;i<=nodeCount;i++) if(i!=id) setLED(i,COL_OFF,ANIM_SOLID);
    whamNextAt = millis() + random(WHAM_DELAY_MIN_MS, WHAM_DELAY_MAX_MS);
  } else {
    setLED(id, COL_RED, ANIM_BLINK_FAST);
  }
}

void whamUpdate() {
  if (gameMode!=GAME_WHACKAMOLE) return;
  uint32_t now = millis();
  if (whamRoundDone) {
    if ((long)(now - whamNextAt) >= 0) whamStartRound();
  } else if (now - whamLitAt > WHAM_TIMEOUT_MS) {
    whamRoundDone = true;
    setLED(whamTarget, COL_RED, ANIM_BLINK_FAST);
    Serial.printf("[WHAM] Timeout! Node %u nicht getroffen.\n", whamTarget);
    whamNextAt = millis() + random(WHAM_DELAY_MIN_MS, WHAM_DELAY_MAX_MS);
  }
}

// ─────────────────────────────────────────────────────────────
// UDP empfangen
// ─────────────────────────────────────────────────────────────
void handleUDP() {
  int size=udp.parsePacket();
  if (size<(int)sizeof(Packet)) return;
  Packet p; udp.read((uint8_t*)&p,sizeof(p));
  IPAddress remoteIP=udp.remoteIP();

  switch(p.type) {
    case PKT_REGISTER:
      // Bereits bekannte IP? → gleiche ID bestätigen (Re-Registrierung / Retry)
      for (uint8_t i=1;i<=nodeCount;i++) {
        if (nodes[i].ip==remoteIP) {
          nodes[i].active   = true;
          nodes[i].lastSeen = millis();
          delay(random(0,30));  // kurzes Jitter damit ACKs sich nicht überschneiden
          sendPkt(remoteIP,PKT_ACK,i,i);
          Serial.printf("[REG] Node %u re-registriert (%s)\n",i,remoteIP.toString().c_str());
          return;
        }
      }
      if (nodeCount>=MAX_NODES) { Serial.println("[REG] MAX_NODES erreicht!"); return; }
      nodeCount++;
      nodes[nodeCount].ip       = remoteIP;
      nodes[nodeCount].active   = true;
      nodes[nodeCount].lastSeen = millis();
      if (players[nodeCount].name[0]==0) snprintf(players[nodeCount].name,20,"Spieler %u",nodeCount);
      delay(random(0,30));  // Jitter gegen gleichzeitige ACK-Kollision
      sendPkt(remoteIP,PKT_ACK,nodeCount,nodeCount);
      Serial.printf("[REG] Node %u neu (%s)\n",nodeCount,remoteIP.toString().c_str());
      break;

    case PKT_PING:
      if (p.nodeId>=1&&p.nodeId<=nodeCount) nodes[p.nodeId].lastSeen=millis();
      break;

    case PKT_BUTTON: {
      uint8_t id=p.nodeId;
      if (id<1||id>nodeCount) return;
      nodes[id].lastSeen=millis();
      Serial.printf("[BTN] Node %u\n",id);
      if      (gameMode==GAME_CTF)         ctfOnButton(id);
      else if (gameMode==GAME_MEMORY)      memOnButton(id);
      else if (gameMode==GAME_BOMB)        bombOnButton(id);
      else if (gameMode==GAME_REACTION)    reactOnButton(id);
      else if (gameMode==GAME_SIMON)       simonOnButton(id);
      else if (gameMode==GAME_HOTPOTATO)   potatoOnButton(id);
      else if (gameMode==GAME_KINGHILL)    kingOnButton(id);
      else if (gameMode==GAME_TUGWAR)      tugOnButton(id);
      else if (gameMode==GAME_MINESWEEPER) mineOnButton(id);
      else if (gameMode==GAME_KNOCKOUT)    knockOnButton(id);
      else if (gameMode==GAME_COLORHUNT)   huntOnButton(id);
      else if (gameMode==GAME_WHACKAMOLE)   whamOnButton(id);
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Web-UI HTML
// ─────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP CTF Game</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#e6edf3;padding:12px;max-width:480px;margin:auto}
h1{text-align:center;color:#f0883e;margin:16px 0 18px;font-size:1.5rem}
.card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:16px;margin-bottom:12px}
.card h2{color:#8b949e;font-size:.75rem;text-transform:uppercase;letter-spacing:.1em;margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0}
.badge{padding:3px 10px;border-radius:20px;font-size:.82rem;font-weight:600}
.idle{background:#21262d;color:#8b949e}.running{background:#0f5132;color:#3fb950}
label{display:block;color:#8b949e;font-size:.83rem;margin:9px 0 3px}
select,input[type=number],input[type=text]{width:100%;padding:8px 11px;background:#21262d;border:1px solid #30363d;border-radius:7px;color:#e6edf3;font-size:.93rem}
.hidden{display:none}
.btn{width:100%;padding:12px;border:none;border-radius:8px;font-size:.97rem;font-weight:700;cursor:pointer;margin-top:7px}
.btn-start{background:#238636;color:#fff}.btn-stop{background:#b91c1c;color:#fff}.btn-save{background:#0f3460;color:#a8dadc}.btn-reset{background:#6e40c9;color:#fff}
.name-row{display:flex;align-items:center;gap:8px;margin:5px 0}
.node-lbl{min-width:60px;font-size:.82rem;color:#8b949e;font-weight:600}
.srow{display:flex;align-items:center;gap:6px;margin:5px 0}
.srow .rank{width:22px;font-size:.8rem;color:#8b949e}
.srow .sname{flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.srow .spts{width:36px;text-align:right;font-weight:700}
.srow .sms{width:54px;text-align:right;font-size:.78rem;color:#8b949e}
.bar-wrap{flex:1;height:8px;background:#21262d;border-radius:4px;overflow:hidden}
.bar-fill{height:100%;border-radius:4px;transition:width .4s}
.timer{font-size:1.9rem;font-weight:700;text-align:center;font-variant-numeric:tabular-nums;margin:5px 0}
.timer.warn{color:#e3b341}.timer.crit{color:#f85149;animation:p .5s infinite}
@keyframes p{0%,100%{opacity:1}50%{opacity:.3}}
.nodes-grid{display:flex;flex-wrap:wrap;gap:5px;margin-top:8px}
.nd{width:34px;height:34px;border-radius:7px;background:#21262d;border:1px solid #30363d;display:flex;align-items:center;justify-content:center;font-size:.72rem;font-weight:700;color:#8b949e}
.nd.on{background:#0f5132;border-color:#3fb950;color:#3fb950}
.nd.off{background:#3d1518;border-color:#f85149;color:#f85149}
.pl{display:flex;align-items:center;gap:8px;margin:5px 0;font-size:.88rem}
.pl .pdot{width:10px;height:10px;border-radius:50%;flex-shrink:0;box-shadow:0 0 5px currentColor}
.pl .pdot.on{background:#3fb950;color:#3fb950}
.pl .pdot.off{background:#f85149;color:#f85149}
.pl .pname{flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pl .pstat{font-size:.76rem;color:#8b949e}
.pl.dead .pname{color:#6e7681}
.info-line{text-align:center;font-size:.95rem;font-weight:600;color:#a8dadc;margin:8px 0 2px;padding:8px;background:#0d1117;border-radius:8px}
table{width:100%;border-collapse:collapse;font-size:.88rem}
th{color:#8b949e;font-weight:600;padding:4px 6px;text-align:left;border-bottom:1px solid #30363d}
td{padding:4px 6px;border-bottom:1px solid #21262d}
.gold{color:#f0883e}.silver{color:#8b949e}.bronze{color:#cd7f32}
.hist-mode{font-size:.75rem;color:#8b949e;margin-right:4px}
.rnd{text-align:center;font-size:1.1rem;font-weight:700;margin:6px 0;color:#a8dadc}
details{margin:10px 0 4px}
summary{cursor:pointer;color:#a8dadc;font-size:.88rem;font-weight:600;padding:6px 0;user-select:none}
.instr{background:#0d1117;border-radius:8px;padding:10px 12px;margin-top:6px;color:#c9d1d9;font-size:.84rem;line-height:1.55}
.cnc{display:flex;flex-wrap:wrap;gap:8px}
.cnchip{display:flex;flex-direction:column;align-items:center;min-width:54px;padding:6px 8px;border-radius:8px;background:#0d1117;border:2px solid #30363d}
.cnchip .dot{width:22px;height:22px;border-radius:50%;border:1px solid #00000055;margin-bottom:4px}
.cnchip .lbl{font-size:.72rem;color:#c9d1d9;max-width:60px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.cnchip.off{opacity:.45}
</style></head>
<body>
<h1>&#127918; ESP CTF Game</h1>

<div class="card">
  <h2>Status</h2>
  <div class="row"><span>Spielmodus</span><span id="modeName" class="badge idle">Idle</span></div>
  <div class="row"><span>Nodes verbunden</span><strong id="nodeCount">0 / 0</strong></div>
  <div class="nodes-grid" id="nodesGrid"></div>
  <div id="nodeNames"></div>
  <div class="timer hidden" id="timer"></div>
  <div class="info-line hidden" id="infoLine"></div>
</div>

<div class="card hidden" id="namesCard">
  <h2>&#128100; Spieler-Namen</h2>
  <div id="nameInputs"></div>
  <button class="btn btn-save" onclick="saveNames()">&#128190; Speichern</button>
</div>

<div class="card">
  <h2>&#9881; Einstellungen</h2>
  <label>Spielmodus</label>
  <select id="mode" onchange="modeChanged()">
    <option value="1">&#127987; Capture the Flag</option>
    <option value="2">&#129504; Memory &ndash; Paare finden</option>
    <option value="3">&#128163; Bombenentsch&auml;rfung</option>
    <option value="4">&#9889; Reaktionsspiel</option>
    <option value="5">&#127922; Simon Says</option>
    <option value="6">&#129359; Hei&szlig;e Kartoffel</option>
    <option value="7">&#128081; King of the Hill</option>
    <option value="8">&#129308; Tauziehen</option>
    <option value="9">&#128163; Minesweeper</option>
    <option value="10">&#128293; Knockout</option>
    <option value="11">&#127752; Farbjagd</option>
    <option value="12">&#127992; Whack-a-Mole</option>
  </select>

  <details id="instrDetails">
    <summary>&#8505; Spielanleitung anzeigen</summary>
    <div class="instr" id="instrText"></div>
  </details>

  <div id="ctfOpts">
    <label>Anzahl Teams</label>
    <select id="teams"><option value="2">2 Teams</option><option value="3">3 Teams</option><option value="4">4 Teams</option></select>
    <label>Dauer (Sekunden)</label>
    <input type="number" id="duration" value="180" min="30" max="600" step="30">
  </div>
  <div id="bombOpts" class="hidden">
    <label>Sequenzl&auml;nge</label>
    <input type="number" id="seqLen" value="5" min="3" max="12">
    <label>Zeitlimit (Sekunden)</label>
    <input type="number" id="bombDur" value="120" min="30" max="300" step="30">
  </div>
  <div id="reactOpts" class="hidden">
    <label>Anzahl Runden</label>
    <select id="reactRounds">
      <option value="5">5 Runden</option>
      <option value="10" selected>10 Runden</option>
      <option value="15">15 Runden</option>
      <option value="20">20 Runden</option>
    </select>
  </div>
  <div id="kingOpts" class="hidden">
    <label>Dauer (Sekunden)</label>
    <input type="number" id="kingDuration" value="180" min="60" max="600" step="30">
  </div>
  <div id="tugOpts" class="hidden">
    <label>Dauer (Sekunden)</label>
    <input type="number" id="tugDuration" value="120" min="30" max="600" step="30">
  </div>
  <div id="mineOpts" class="hidden">
    <label>Anzahl Minen</label>
    <input type="number" id="mines" value="2" min="1" max="8">
  </div>
  <div id="knockOpts" class="hidden">
    <label>Startleben</label>
    <input type="number" id="klives" value="3" min="1" max="5">
  </div>
  <div id="huntOpts" class="hidden">
    <label>Anzahl Runden</label>
    <input type="number" id="hrounds" value="8" min="3" max="20">
  </div>
  <div id="whamOpts" class="hidden">
    <label>Anzahl Runden</label>
    <select id="whamRounds">
      <option value="10">10 Runden</option>
      <option value="15" selected>15 Runden</option>
      <option value="20">20 Runden</option>
      <option value="30">30 Runden</option>
    </select>
  </div>
  <div id="generalOpts">
    <label>Spielverlauf speichern (Anzahl)</label>
    <input type="number" id="historySize" value="20" min="3" max="20">
  </div>
  <button class="btn btn-start" onclick="startGame()">&#9654; STARTEN</button>
  <button class="btn btn-stop" onclick="stopGame()">&#9632; STOPPEN</button>
  <button class="btn btn-reset" onclick="resetNodes()">&#128260; NODES RECONNECT</button>
  <a href="/update" class="btn" style="background:#0f3460;color:#a8dadc;text-decoration:none;text-align:center">&#128640; OTA Update</a>
</div>

<div class="card hidden" id="reactLiveCard">
  <h2>&#9889; Reaktionsspiel live</h2>
  <div class="rnd" id="reactRoundDisp"></div>
  <div id="reactLiveScores"></div>
</div>

<div class="card hidden" id="ctfScoreCard">
  <h2>&#127987; CTF &ndash; Eroberte Nodes pro Team</h2>
  <div id="ctfScores"></div>
  <h3 style="margin:14px 0 6px;font-size:.95rem;color:#a8dadc">Aktuelle Farbe je Node</h3>
  <div id="ctfNodeColors" class="cnc"></div>
</div>

<div class="card hidden" id="liveScoreCard">
  <h2 id="liveScoreTitle">&#127919; Spielstand</h2>
  <div id="liveScores"></div>
</div>

<div class="card hidden" id="hsCard">
  <h2>&#127942; Bestenliste</h2>
  <table><thead><tr><th>#</th><th>Name</th><th>Punkte</th><th>Beste Zeit</th></tr></thead>
  <tbody id="hsTbody"></tbody></table>
  <button class="btn btn-stop" style="margin-top:10px;font-size:.82rem;padding:8px" onclick="clearScores()">&#128465; Bestenliste l&ouml;schen</button>
</div>

<div class="card hidden" id="histCard">
  <h2>&#128196; Spielverlauf</h2>
  <table><thead><tr><th>#</th><th>Spiel</th><th>Gewinner</th><th>Punkte</th></tr></thead>
  <tbody id="histTbody"></tbody></table>
</div>

<script>
var modeNames=['Idle','CTF','Memory','Bomb','Reaktion','Simon Says','Heisse Kartoffel','King of the Hill','Tauziehen','Minesweeper','Knockout','Farbjagd','Whack-a-Mole'];
var tc=['#8b949e','#f85149','#388bfd','#3fb950','#e3b341'];
var knownNodes=0;

var instrs={
  1:"Nodes durch Druecken fuer dein Team beanspruchen. Button wechselt Farbe (neutral&rarr;rot&rarr;blau). Nach der Zeit gewinnt das Team mit den meisten Nodes.",
  2:"Nodes leuchten 2s auf &ndash; merke dir die Farben. Druecke zwei gleich-farbige Nodes nacheinander. Kein Treffer? Beide gehen wieder aus.",
  3:"Ein Node blinkt ROT = Bombe! Die Sequenz der anderen Nodes zeigt die Reihenfolge zum Entschaerfen. Bombe druecken = Sequenz nochmal zeigen.",
  4:"Ein zufaelliger Node leuchtet GELB. Wer zuerst drueckt, bekommt einen Punkt. Reaktionszeit wird gemessen.",
  5:"Simon zeigt eine Farb-Sequenz (Nodes leuchten nacheinander gruen auf). Wiederhole die Reihenfolge durch Druecken. Wird jede Runde laenger &ndash; bis du einen Fehler machst.",
  6:"Ein Node haelt die Heisse Kartoffel (orange blinkend). Druecken gibt sie weiter. Wer sie beim Alarm haelt, verliert ein Leben. 3 Leben = 3 Balken.",
  7:"Der GOLDENE Node ist der Thron. Druecken = du haeltst ihn. Haltezeit = Punkte. Der Thron wandert alle 30s zu einem anderen Node weiter!",
  8:"Ungerade Nodes = ROT, gerade = BLAU. Jeder Druck verschiebt den Balken. Erste Farbe, die alle 8 LEDs fuellt, gewinnt!",
  9:"Manche Nodes sind Minen. Druecke Nodes frei: Gruen = sicher (+Punkt), Rot = Mine (-Leben). 3 Leben insgesamt. Alle sicheren Nodes finden = Sieg!",
  10:"Wie Reaktion, aber mit Elimination. Ein Node leuchtet gelb &ndash; 2s Gnadenfrist nach dem ersten Treffer. Wer nicht drueckt, verliert ein Leben. Letzter gewinnt!",
  11:"Nodes zeigen kurz ihre zugewiesene Farbe. Dann: Node 1 blinkt die ZIELFARBE. Wer zuerst den passenden Node drueckt, bekommt einen Punkt. Meiste Punkte nach allen Runden gewinnt.",
  12:"Nodes leuchten zufaellig gelb auf. Wer zuerst den leuchtenden Node drueckt, bekommt einen Punkt. Falscher Druck = kurz Rot. Nach allen Runden gewinnt der mit den meisten Treffern."
};

function modeChanged(){
  var m=parseInt(document.getElementById('mode').value);
  document.getElementById('ctfOpts').classList.toggle('hidden',m!==1);
  document.getElementById('bombOpts').classList.toggle('hidden',m!==3);
  document.getElementById('reactOpts').classList.toggle('hidden',m!==4);
  document.getElementById('kingOpts').classList.toggle('hidden',m!==7);
  document.getElementById('tugOpts').classList.toggle('hidden',m!==8);
  document.getElementById('mineOpts').classList.toggle('hidden',m!==9);
  document.getElementById('knockOpts').classList.toggle('hidden',m!==10);
  document.getElementById('huntOpts').classList.toggle('hidden',m!==11);
  document.getElementById('whamOpts').classList.toggle('hidden',m!==12);
  var instrEl=document.getElementById('instrText');
  if(instrs[m]){instrEl.innerHTML=instrs[m];document.getElementById('instrDetails').classList.remove('hidden');}
  else{document.getElementById('instrDetails').classList.add('hidden');}
}

function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});}

function startGame(){
  var m=document.getElementById('mode').value;
  var q='mode='+m;
  if(m==='1') q+='&teams='+document.getElementById('teams').value+'&duration='+document.getElementById('duration').value;
  if(m==='3') q+='&seqLen='+document.getElementById('seqLen').value+'&duration='+document.getElementById('bombDur').value;
  if(m==='4') q+='&rounds='+document.getElementById('reactRounds').value;
  if(m==='7') q+='&duration='+document.getElementById('kingDuration').value;
  if(m==='8') q+='&duration='+document.getElementById('tugDuration').value;
  if(m==='9') q+='&mines='+document.getElementById('mines').value;
  if(m==='10') q+='&klives='+document.getElementById('klives').value;
  if(m==='11') q+='&hrounds='+document.getElementById('hrounds').value;
  if(m==='12') q+='&whamrounds='+document.getElementById('whamRounds').value;
  q+='&history='+document.getElementById('historySize').value;
  post('/start',q);
}
function stopGame(){post('/stop','');}
function resetNodes(){post('/reset','').then(function(){var b=document.querySelector('.btn-reset');var orig=b.innerHTML;b.textContent='Gesendet...';setTimeout(function(){b.innerHTML=orig;},1500);});}

function saveNames(){
  var q='';
  for(var i=1;i<=16;i++){var el=document.getElementById('n'+i);if(el)q+='&n'+i+'='+encodeURIComponent(el.value||'');}
  post('/names',q.slice(1)).then(function(){var b=document.querySelector('.btn-save');b.textContent='✓ Gespeichert';setTimeout(function(){b.innerHTML='&#128190; Speichern';},1500);});
}

function clearScores(){post('/clearscores','').then(function(){document.getElementById('hsTbody').innerHTML='';document.getElementById('hsCard').classList.add('hidden');});}

function updateNameInputs(n){
  var c=document.getElementById('nameInputs');
  while(c.children.length<n){
    var idx=c.children.length+1;
    var d=document.createElement('div');d.className='name-row';
    d.innerHTML='<span class="node-lbl">Node '+idx+'</span><input type="text" id="n'+idx+'" placeholder="Spieler '+idx+'" maxlength="18">';
    c.appendChild(d);
  }
  document.getElementById('namesCard').classList.toggle('hidden',n===0);
}

function fmtTime(s){if(s<=0)return'0:00';return Math.floor(s/60)+':'+(s%60<10?'0':'')+s%60;}

function updateStatus(){
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    var nl=d.nodeList||[];
    var online=0;nl.forEach(function(n){if(n.on)online++;});
    document.getElementById('nodeCount').textContent=online+' / '+d.nodes;
    if(d.nodes!==knownNodes){knownNodes=d.nodes;updateNameInputs(d.nodes);}

    // Node-Chips: gruen=verbunden, rot=offline
    var g=document.getElementById('nodesGrid');g.innerHTML='';
    if(nl.length===0){var ph=document.createElement('div');ph.className='nd';ph.textContent='–';g.appendChild(ph);}
    nl.forEach(function(n){var dot=document.createElement('div');dot.className='nd'+(n.on?' on':' off');dot.textContent=n.id;dot.title=n.name+(n.on?' (verbunden)':' (offline)');g.appendChild(dot);});

    // Spielerliste mit Verbindungsstatus
    var nn=document.getElementById('nodeNames');nn.innerHTML='';
    nl.forEach(function(n){
      var row=document.createElement('div');row.className='pl'+(n.on?'':' dead');
      row.innerHTML='<span class="pdot '+(n.on?'on':'off')+'"></span>'
        +'<span class="pname">'+n.name+'</span>'
        +'<span class="pstat">'+(n.on?'verbunden':'offline')+'</span>';
      nn.appendChild(row);
    });

    // Info-Zeile (Bombe/Simon/Tauziehen/Minesweeper)
    var il=document.getElementById('infoLine');
    if(d.running&&d.info&&d.info.length>0){il.classList.remove('hidden');il.textContent=d.info;}
    else il.classList.add('hidden');

    var mb=document.getElementById('modeName');
    mb.textContent=d.running?(modeNames[d.mode]||'Spiel')+' laeuft':'Idle';
    mb.className='badge '+(d.running?'running':'idle');

    var tr=document.getElementById('timer');
    if(d.running&&d.timeLeft>0){tr.classList.remove('hidden');tr.textContent=fmtTime(d.timeLeft);tr.className='timer'+(d.timeLeft<30?' crit':d.timeLeft<60?' warn':'');}
    else tr.classList.add('hidden');

    var cs=document.getElementById('ctfScoreCard');
    if(d.running&&d.mode==1&&d.scores){
      cs.classList.remove('hidden');
      var sv=document.getElementById('ctfScores');sv.innerHTML='';
      var mx=1;for(var k in d.scores)if(d.scores[k]>mx)mx=d.scores[k];
      var tn=['','ROT','BLAU','GRUEN','GELB'];
      for(var t=1;t<=4;t++){if(d.scores[t]===undefined)continue;
        var row=document.createElement('div');row.className='srow';
        var pct=mx>0?Math.round(d.scores[t]/mx*100):0;
        var cnt=d.scores[t];
        row.innerHTML='<div class="sname" style="color:'+tc[t]+';min-width:60px">'+tn[t]+'</div>'
          +'<div class="bar-wrap"><div class="bar-fill" style="background:'+tc[t]+';width:'+pct+'%"></div></div>'
          +'<div class="spts">'+cnt+' Node'+(cnt==1?'':'s')+'</div>';
        sv.appendChild(row);}
      if(d.neutral!==undefined&&d.neutral>0){
        var nr=document.createElement('div');nr.className='srow';
        nr.innerHTML='<div class="sname" style="color:#8b949e;min-width:60px">Neutral</div>'
          +'<div class="bar-wrap"><div class="bar-fill" style="background:#8b949e;width:'+(mx>0?Math.round(d.neutral/mx*100):0)+'%"></div></div>'
          +'<div class="spts">'+d.neutral+' Node'+(d.neutral==1?'':'s')+'</div>';
        sv.appendChild(nr);}
      // Aktuelle Farbe je Node
      var nc=document.getElementById('ctfNodeColors');nc.innerHTML='';
      nl.forEach(function(n){
        var t=(n.team===undefined)?0:n.team;
        var chip=document.createElement('div');chip.className='cnchip'+(n.on?'':' off');
        chip.innerHTML='<div class="dot" style="background:'+tc[t]+'"></div>'
          +'<div class="lbl">'+n.id+'. '+n.name+'</div>';
        chip.title=n.name+' -> '+(['Neutral','ROT','BLAU','GRUEN','GELB'][t]||'?');
        nc.appendChild(chip);});
    } else cs.classList.add('hidden');

    // Generischer Live-Spielstand (Memory, Kartoffel, King, Knockout, Farbjagd)
    var lc=document.getElementById('liveScoreCard');
    if(d.running&&d.scoreLabel&&d.scoreLabel.length>0&&nl.length>0){
      lc.classList.remove('hidden');
      document.getElementById('liveScoreTitle').innerHTML='&#127919; Spielstand &ndash; '+d.scoreLabel;
      var lv=document.getElementById('liveScores');lv.innerHTML='';
      var arr=nl.slice().sort(function(a,b){return b.v-a.v;});
      var mxv=1;arr.forEach(function(n){if(n.v>mxv)mxv=n.v;});
      arr.forEach(function(n,i){
        var row=document.createElement('div');row.className='srow';
        var pct=mxv>0?Math.round(n.v/mxv*100):0;
        var col=!n.on?'#6e7681':i==0?'#3fb950':i==1?'#388bfd':'#8b949e';
        row.innerHTML='<div class="rank">'+(i+1)+'</div>'
          +'<div class="sname"'+(n.on?'':' style="color:#6e7681"')+'>'+n.name+(n.on?'':' &#9888;')+'</div>'
          +'<div class="bar-wrap"><div class="bar-fill" style="background:'+col+';width:'+pct+'%"></div></div>'
          +'<div class="spts">'+n.v+'</div>';
        lv.appendChild(row);});
    } else lc.classList.add('hidden');

    var rc=document.getElementById('reactLiveCard');
    if(d.running&&d.mode==4&&d.reactScores){
      rc.classList.remove('hidden');
      document.getElementById('reactRoundDisp').textContent='Runde '+d.round+' / '+d.totalRounds;
      var rs=document.getElementById('reactLiveScores');rs.innerHTML='';
      var sorted=d.reactScores.slice().sort(function(a,b){return b.points-a.points||(a.bestMs||9999)-(b.bestMs||9999);});
      var mx2=sorted.length>0?Math.max(sorted[0].points,1):1;
      sorted.forEach(function(p,i){
        var row=document.createElement('div');row.className='srow';
        var pct=Math.round(p.points/mx2*100);
        var col=i==0?'#3fb950':i==1?'#388bfd':'#8b949e';
        row.innerHTML='<div class="rank">'+(i+1)+'</div>'
          +'<div class="sname">'+p.name+'</div>'
          +'<div class="bar-wrap"><div class="bar-fill" style="background:'+col+';width:'+pct+'%"></div></div>'
          +'<div class="spts">'+p.points+'</div>'
          +'<div class="sms">'+(p.bestMs?p.bestMs+'ms':'&ndash;')+'</div>';
        rs.appendChild(row);});
    } else rc.classList.add('hidden');

    if(d.highscores&&d.highscores.length>0){
      document.getElementById('hsCard').classList.remove('hidden');
      var tb=document.getElementById('hsTbody');tb.innerHTML='';
      var medals=['gold','silver','bronze'];
      d.highscores.forEach(function(h,i){
        var tr2=document.createElement('tr');
        var cls=i<3?' class="'+medals[i]+'"':'';
        tr2.innerHTML='<td'+cls+'>'+(i+1)+'</td><td>'+h.name+'</td><td style="font-weight:700">'+h.points+'</td><td>'+h.bestMs+'ms</td>';
        tb.appendChild(tr2);});
    }
    var hc=document.getElementById('histCard');
    if(d.history&&d.history.length>0){
      hc.classList.remove('hidden');
      var hb=document.getElementById('histTbody');hb.innerHTML='';
      d.history.forEach(function(h,i){
        var tr3=document.createElement('tr');
        tr3.innerHTML='<td>'+(d.totalGames-i)+'</td>'
          +'<td><span class="hist-mode">'+modeNames[h.mode]+'</span></td>'
          +'<td style="font-weight:600">'+h.winner+'</td>'
          +'<td>'+h.score+'</td>';
        hb.appendChild(tr3);});
    } else hc.classList.add('hidden');
  }).catch(function(){});
}

// Initialize instruction text for default mode
modeChanged();
setInterval(updateStatus,1000);
updateStatus();
</script>
</body></html>
)rawliteral";

// ─────────────────────────────────────────────────────────────
// Web-Routen
// ─────────────────────────────────────────────────────────────
void webHandleRoot() { webServer.send_P(200,"text/html",INDEX_HTML); }

void webHandleStatus() {
  uint32_t timeLeft=0;
  if (gameMode!=GAME_IDLE && gameEndTime!=0) {
    long r=(long)(gameEndTime-millis());
    timeLeft=(r>0)?(uint32_t)r/1000:0;
  }

  String j="{";
  j+="\"nodes\":"+String(nodeCount)+",";
  j+="\"mode\":"+String(gameMode)+",";
  j+="\"running\":"; j+=(gameMode!=GAME_IDLE?"true":"false"); j+=",";
  j+="\"timeLeft\":"+String(timeLeft);

  // Pro-Node-Liste: Verbindungsstatus + spielspezifischer Punktestand
  const char* scoreLabel="";
  switch (gameMode) {
    case GAME_MEMORY:    scoreLabel="Gefunden"; break;
    case GAME_HOTPOTATO: scoreLabel="Leben";    break;
    case GAME_KINGHILL:  scoreLabel="Sekunden"; break;
    case GAME_KNOCKOUT:  scoreLabel="Leben";    break;
    case GAME_COLORHUNT:   scoreLabel="Punkte";  break;
    case GAME_WHACKAMOLE:  scoreLabel="Treffer"; break;
  }
  j+=",\"scoreLabel\":\""+String(scoreLabel)+"\"";
  j+=",\"nodeList\":[";
  for (uint8_t i=1;i<=nodeCount;i++) {
    uint16_t v=0;
    switch (gameMode) {
      case GAME_MEMORY:    v=memMatched[i]?1:0; break;
      case GAME_HOTPOTATO: v=potatoLives[i];    break;
      case GAME_KINGHILL: {
        uint32_t hold=kingHoldTime[i];
        if (kingHolder==i && kingLastCapture!=0) hold+=millis()-kingLastCapture;
        v=hold/1000; break;
      }
      case GAME_KNOCKOUT:  v=knockLives[i];  break;
      case GAME_COLORHUNT:  v=huntScores[i];  break;
      case GAME_WHACKAMOLE: v=whamScores[i];  break;
    }
    j+="{\"id\":"+String(i)+",\"name\":\""+String(players[i].name)+"\",";
    j+="\"on\":"; j+=(nodes[i].active?"true":"false"); j+=",";
    j+="\"team\":"+String(gameMode==GAME_CTF?ctfTeam[i]:0)+",";
    j+="\"v\":"+String(v)+"}";
    if (i<nodeCount) j+=",";
  }
  j+="]";

  // Info-Zeile fuer Spiele mit gemeinsamem Zustand (kein Pro-Node-Score)
  String info="";
  if (gameMode==GAME_BOMB) {
    info="Bombe bei Node "+String(bombNode)+" - Schritt "+String(bombStep)+"/"+String(cfgSeqLen);
  } else if (gameMode==GAME_SIMON) {
    info="Simon-Sequenz: Laenge "+String(simonLen);
  } else if (gameMode==GAME_TUGWAR) {
    if      (tugScore>50) info="Tauziehen: ROT fuehrt";
    else if (tugScore<50) info="Tauziehen: BLAU fuehrt";
    else                  info="Tauziehen: Gleichstand";
  } else if (gameMode==GAME_MINESWEEPER) {
    info="Sicher gedeckt: "+String(mineScore)+"/"+String(mineSafeCount)+" - Leben: "+String(mineLives);
  }
  j+=",\"info\":\""+info+"\"";

  if (gameMode==GAME_CTF) {
    j+=",\"scores\":{";
    for (uint8_t t=1;t<=cfgTeams;t++) {
      uint8_t s=0; for(uint8_t i=1;i<=nodeCount;i++) if(ctfTeam[i]==t) s++;
      j+="\""+String(t)+"\":"+String(s); if(t<cfgTeams) j+=",";
    }
    j+="}";
    uint8_t neu=0; for(uint8_t i=1;i<=nodeCount;i++) if(ctfTeam[i]==0) neu++;
    j+=",\"neutral\":"+String(neu);
  }
  if (gameMode==GAME_REACTION) {
    j+=",\"round\":"+String(reactRound);
    j+=",\"totalRounds\":"+String(cfgReactRounds);
    j+=",\"reactScores\":[";
    for (uint8_t i=1;i<=nodeCount;i++) {
      j+="{\"id\":"+String(i)+",\"name\":\""+String(players[i].name)+"\",";
      j+="\"points\":"+String(players[i].points)+",\"bestMs\":"+String(players[i].bestMs)+"}";
      if (i<nodeCount) j+=",";
    }
    j+="]";
  }

  j+=",\"highscores\":[";
  for (uint8_t i=0;i<highScoreCount;i++) {
    j+="{\"name\":\""+String(highScores[i].name)+"\",\"points\":"+String(highScores[i].points)+",\"bestMs\":"+String(highScores[i].bestMs)+"}";
    if (i<highScoreCount-1) j+=",";
  }
  j+="]";

  j+=",\"history\":[";
  uint8_t cap2 = (cfgMaxHistory < MAX_HISTORY) ? cfgMaxHistory : MAX_HISTORY;
  uint8_t hc = (historyCount < cap2) ? historyCount : cap2;
  for (int8_t i=(int8_t)hc-1; i>=0; i--) {
    j+="{\"mode\":"+String(gameHistory[i].mode)+",\"winner\":\""+String(gameHistory[i].winner)+"\",\"score\":"+String(gameHistory[i].score)+"}";
    if (i>0) j+=",";
  }
  j+="]";
  j+=",\"totalGames\":"+String(totalGames);
  j+="}";

  webServer.sendHeader("Access-Control-Allow-Origin","*");
  webServer.send(200,"application/json",j);
}

void webHandleStart() {
  if (!webServer.hasArg("mode")) { webServer.send(400,"text/plain","missing mode"); return; }
  uint8_t m=webServer.arg("mode").toInt();
  if (webServer.hasArg("teams"))    cfgTeams      = constrain(webServer.arg("teams").toInt(),2,4);
  if (webServer.hasArg("duration")) cfgDuration   = constrain(webServer.arg("duration").toInt(),30,600);
  if (webServer.hasArg("seqLen"))   cfgSeqLen     = constrain(webServer.arg("seqLen").toInt(),3,12);
  if (webServer.hasArg("duration")&&m==3) cfgBombDur = constrain(webServer.arg("duration").toInt(),30,300);
  if (webServer.hasArg("rounds"))   cfgReactRounds = constrain(webServer.arg("rounds").toInt(),3,20);
  if (webServer.hasArg("mines"))    cfgMines       = constrain(webServer.arg("mines").toInt(),1,nodeCount>1?nodeCount-1:1);
  if (webServer.hasArg("klives"))   cfgKnockLives  = constrain(webServer.arg("klives").toInt(),1,5);
  if (webServer.hasArg("hrounds"))  cfgHuntRounds  = constrain(webServer.arg("hrounds").toInt(),3,20);
  if (webServer.hasArg("whamrounds")) cfgWhamRounds = constrain(webServer.arg("whamrounds").toInt(),5,30);
  if (webServer.hasArg("history"))    cfgMaxHistory  = constrain(webServer.arg("history").toInt(),3,20);
  // duration used by king and tug too
  if (webServer.hasArg("duration")&&m==7) cfgDuration = constrain(webServer.arg("duration").toInt(),30,600);
  if (webServer.hasArg("duration")&&m==8) cfgDuration = constrain(webServer.arg("duration").toInt(),30,600);

  gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID); delay(100);
  if      (m==GAME_CTF)        ctfStart();
  else if (m==GAME_MEMORY)     memStart();
  else if (m==GAME_BOMB)       bombStart();
  else if (m==GAME_REACTION)   reactStart();
  else if (m==GAME_SIMON)      simonStart();
  else if (m==GAME_HOTPOTATO)  potatoStart();
  else if (m==GAME_KINGHILL)   kingStart();
  else if (m==GAME_TUGWAR)     tugStart();
  else if (m==GAME_MINESWEEPER) mineStart();
  else if (m==GAME_KNOCKOUT)   knockStart();
  else if (m==GAME_COLORHUNT)  huntStart();
  else if (m==GAME_WHACKAMOLE)  whamStart();
  webServer.send(200,"text/plain","OK");
}

void webHandleStop() {
  gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID);
  webServer.send(200,"text/plain","OK");
}

void resetNodes() {
  gameMode = GAME_IDLE;
  // Broadcast PKT_RESET so connected nodes clear their ID
  Packet p; p.type=PKT_RESET; p.nodeId=0xFF;
  memset(p.data,0,sizeof(p.data));
  udp.beginPacket(bcastIP, UDP_PORT);
  udp.write((uint8_t*)&p, sizeof(p));
  udp.endPacket();
  delay(50);
  // Clear registry
  for (uint8_t i=1;i<=nodeCount;i++) {
    nodes[i].active   = false;
    nodes[i].lastSeen = 0;
  }
  nodeCount = 0;
  Serial.println("[RESET] Node-Liste geleert, Nodes re-registrieren sich.");
}

void webHandleReset() {
  resetNodes();
  webServer.send(200,"text/plain","OK");
}

void webHandleNames() {
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    String key="n"+String(i);
    if (webServer.hasArg(key)) {
      String v=webServer.arg(key);
      v.trim();
      if (v.length()>0) {
        strncpy(players[i].name, v.c_str(), 19);
        players[i].name[19]=0;
      } else {
        snprintf(players[i].name,20,"Spieler %u",i);
      }
    }
  }
  webServer.send(200,"text/plain","OK");
}

void webHandleClearScores() {
  highScoreCount=0;
  memset(highScores,0,sizeof(highScores));
  historyCount=0; totalGames=0;
  webServer.send(200,"text/plain","OK");
}

// ─────────────────────────────────────────────────────────────
// Serielles Menü
// ─────────────────────────────────────────────────────────────
void handleSerial() {
  if (!Serial.available()) return;
  char c=Serial.read();
  if      (c=='1') ctfStart();
  else if (c=='2') memStart();
  else if (c=='3') bombStart();
  else if (c=='4') reactStart();
  else if (c=='5') simonStart();
  else if (c=='6') potatoStart();
  else if (c=='7') kingStart();
  else if (c=='8') tugStart();
  else if (c=='9') mineStart();
  else if (c=='k') knockStart();
  else if (c=='h') huntStart();
  else if (c=='w') whamStart();
  else if (c=='0') { gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID); Serial.println("Gestoppt."); }
  else if (c=='r') { resetNodes(); }
  else if (c=='s') {
    Serial.printf("Modus:%u Nodes:%u\n",gameMode,nodeCount);
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  Node%u '%s': %s\n",i,players[i].name,nodes[i].active?"OK":"offline");
    if (highScoreCount>0) {
      Serial.println("Highscores:");
      for (uint8_t i=0;i<highScoreCount;i++)
        Serial.printf("  %u. %s - %u Pkt, %ums\n",i+1,highScores[i].name,highScores[i].points,highScores[i].bestMs);
    }
  }
}

// ─────────────────────────────────────────────────────────────
// OTA-Update (Master) via Browser – http://192.168.4.1/update
// ─────────────────────────────────────────────────────────────
#ifdef ESP32
void webHandleUpdateForm() {
  webServer.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OTA Update</title>"
    "<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;display:flex;"
    "flex-direction:column;align-items:center;padding:30px}"
    "h2{color:#a8dadc}form{display:flex;flex-direction:column;gap:12px;width:100%;max-width:400px}"
    "input[type=file]{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:8px;color:#c9d1d9}"
    "button{background:#238636;color:#fff;border:none;border-radius:8px;padding:12px;font-size:1rem;cursor:pointer}"
    "a{color:#a8dadc;font-size:.9rem}</style></head><body>"
    "<h2>&#128640; Master OTA-Update</h2>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin' required>"
    "<button type='submit'>Firmware hochladen</button>"
    "</form><br><a href='/'>&#8592; Zurueck</a></body></html>");
}

void webHandleUpdateUpload() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
    if (!Update.begin()) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[OTA] Fertig: %u Bytes\n", upload.totalSize);
    else Update.printError(Serial);
  }
}

void webHandleUpdateResult() {
  bool ok = !Update.hasError();
  webServer.send(200, "text/html",
    String("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='8;url=/'>"
    "<title>OTA</title><style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;"
    "display:flex;flex-direction:column;align-items:center;padding:40px}</style></head><body>")
    + (ok ? "<h2>&#9989; Update erfolgreich! Neustart...</h2>" : "<h2>&#10060; Update fehlgeschlagen!</h2>")
    + "</body></html>");
  if (ok) { delay(500); ESP.restart(); }
}
#endif

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP CTF Game – Master ===");

  WiFi.mode(WIFI_AP);
  // Max 10 Verbindungen (ESP32-Default ist 4, reicht nicht fuer mehrere Nodes)
  WiFi.softAP(WIFI_SSID, strlen(WIFI_PASS)>0 ? WIFI_PASS : nullptr, 1, 0, 10);
  Serial.printf("AP: %s  IP: %s  maxConn:10\n", WIFI_SSID, WiFi.softAPIP().toString().c_str());

  udp.begin(UDP_PORT);

  webServer.on("/",            webHandleRoot);
  webServer.on("/status",      webHandleStatus);
  webServer.on("/start",  HTTP_POST, webHandleStart);
  webServer.on("/stop",   HTTP_POST, webHandleStop);
  webServer.on("/names",  HTTP_POST, webHandleNames);
  webServer.on("/clearscores", HTTP_POST, webHandleClearScores);
  webServer.on("/reset",       HTTP_POST, webHandleReset);
#ifdef ESP32
  webServer.on("/update", HTTP_GET,  webHandleUpdateForm);
  webServer.on("/update", HTTP_POST, webHandleUpdateResult, webHandleUpdateUpload);
#else
  httpUpdater.setup(&webServer, "/update");
#endif
  webServer.begin();

  // ArduinoOTA (fuer Flashen via Arduino IDE Netzwerk-Port)
  ArduinoOTA.setHostname("ctf-master");
  ArduinoOTA.onStart([]()  { Serial.println("[OTA] Start");  });
  ArduinoOTA.onEnd([]()    { Serial.println("[OTA] Fertig"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Fehler %u\n", e); });
  ArduinoOTA.begin();

  Serial.println("Web: http://192.168.4.1");
  Serial.println("OTA: http://192.168.4.1/update  (Firmware .bin hochladen)");
  Serial.println("Seriell: 1=CTF 2=Memory 3=Bomb 4=Reaktion 5=Simon 6=HotPotato");
  Serial.println("         7=KingHill 8=TugWar 9=Minesweeper k=Knockout h=ColorHunt w=WhackaMole");
  Serial.println("         0=Stop s=Status r=Reset/Reconnect");
}

void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();
  handleUDP();
  handleSerial();

  static uint32_t lastCheck=0;
  if (millis()-lastCheck>2000) {
    lastCheck=millis();
    for (uint8_t i=1;i<=nodeCount;i++) {
      bool was=nodes[i].active;
      nodes[i].active=(millis()-nodes[i].lastSeen)<NODE_TIMEOUT_MS;
      if (was&&!nodes[i].active) Serial.printf("[WARN] Node %u offline\n",i);
    }
  }

  if      (gameMode==GAME_CTF)        ctfUpdate();
  else if (gameMode==GAME_MEMORY)     memUpdate();
  else if (gameMode==GAME_BOMB)       bombUpdate();
  else if (gameMode==GAME_REACTION)   reactUpdate();
  else if (gameMode==GAME_SIMON)      simonUpdate();
  else if (gameMode==GAME_HOTPOTATO)  potatoUpdate();
  else if (gameMode==GAME_KINGHILL)   kingUpdate();
  else if (gameMode==GAME_TUGWAR)     tugUpdate();
  // GAME_MINESWEEPER: event-driven, no timer update needed
  else if (gameMode==GAME_KNOCKOUT)   knockUpdate();
  else if (gameMode==GAME_COLORHUNT)  huntUpdate();
  else if (gameMode==GAME_WHACKAMOLE)   whamUpdate();
}
