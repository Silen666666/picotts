/*
 * ESP CTF Game – Master Controller
 * ESP8266 + ESP32 kompatibel
 *
 * Handy: WLAN "ESP-CTF-Game" → Browser http://192.168.4.1
 * Seriell (115200): 1=CTF  2=Memory  3=Bomb  4=Reaktion  0=Stop  s=Status
 */

#ifdef ESP32
  #include <WiFi.h>
  #include <WebServer.h>
  WebServer webServer(80);
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  ESP8266WebServer webServer(80);
#endif
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
uint8_t  cfgTeams    = CTF_TEAMS;
uint16_t cfgDuration = CTF_DURATION_S;
uint8_t  cfgSeqLen   = BOMB_SEQ_LEN;
uint16_t cfgBombDur  = BOMB_DURATION_S;
uint8_t  cfgReactRounds = REACT_ROUNDS_DEFAULT;

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
bool     bombOver;

// Reaktion
uint8_t  reactRound     = 0;
uint8_t  reactTarget    = 0;
uint8_t  reactLastTarget = 0;
uint32_t reactLitAt     = 0;
bool     reactRoundDone = true;
uint32_t reactNextAt    = 0;

// ─────────────────────────────────────────────────────────────
// Netzwerk
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
  for (uint8_t i=1;i<=nodeCount;i++) { if(nodes[i].active) setLED(i,color,anim); delay(15); }
}

// ─────────────────────────────────────────────────────────────
// CTF
// ─────────────────────────────────────────────────────────────
static const uint8_t TEAM_COLORS[] = {COL_WHITE,COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW};
static const char*   TEAM_NAMES[]  = {"Neutral","ROT","BLAU","GRUEN","GELB"};

void ctfStart() {
  if (nodeCount<2) { Serial.println("[CTF] Mindestens 2 Nodes."); return; }
  gameMode=GAME_CTF; gameEndTime=millis()+(uint32_t)cfgDuration*1000;
  for (uint8_t i=1;i<=nodeCount;i++) { ctfTeam[i]=0; setLED(i,COL_WHITE,ANIM_SOLID); delay(30); }
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
  memTotalPairs=n/2; memFoundPairs=0;
  memPending[0]=memPending[1]=-1; memHideAt=0;
  uint8_t pool[MAX_NODES];
  for (uint8_t i=0;i<memTotalPairs;i++) { pool[2*i]=PAIR_PALETTE[i%7]; pool[2*i+1]=PAIR_PALETTE[i%7]; }
  randomSeed(millis());
  for (int i=n-1;i>0;i--) { int j=random(0,i+1); uint8_t t=pool[i];pool[i]=pool[j];pool[j]=t; }
  for (uint8_t i=1;i<=n;i++) { memColor[i]=pool[i-1]; memMatched[i]=false; }
  for (uint8_t i=n+1;i<=nodeCount;i++) setLED(i,COL_OFF,ANIM_SOLID);
  for (uint8_t i=1;i<=n;i++) { setLED(i,memColor[i],ANIM_SOLID); delay(30); }
  delay(2000);
  for (uint8_t i=1;i<=n;i++) { setLED(i,COL_OFF,ANIM_SOLID); delay(30); }
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
      if (memFoundPairs==memTotalPairs) { delay(400); allLED(COL_GREEN,ANIM_BLINK_FAST); gameMode=GAME_IDLE; }
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
  uint8_t len=min((uint8_t)cfgSeqLen,(uint8_t)16);
  for (uint8_t s=0;s<len;s++) {
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
  for (uint8_t i=0;i<len;i++) { uint8_t p=random(0,cnt-i); bombSeq[i]=avail[p]; avail[p]=avail[cnt-i-1]; }
  allLED(COL_OFF,ANIM_SOLID); delay(200);
  setLED(bombNode,COL_RED,ANIM_BLINK_FAST);
  bombShowSequence();
  for (uint8_t i=1;i<=nodeCount;i++) { setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID); delay(20); }
}
void bombOnButton(uint8_t id) {
  if (bombOver) return;
  if (id==bombNode) { bombShowSequence(); for (uint8_t i=1;i<=nodeCount;i++) { setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID); delay(20); } return; }
  uint8_t len=min((uint8_t)cfgSeqLen,(uint8_t)16);
  if (id==bombSeq[bombStep]) {
    setLED(id,COL_GREEN,ANIM_FLASH); bombStep++;
    if (bombStep==len) { bombOver=true; allLED(COL_GREEN,ANIM_BLINK_SLOW); gameMode=GAME_IDLE; Serial.println("[BOMB] ENTSCHAERFT!"); }
  } else { bombOver=true; allLED(COL_RED,ANIM_BLINK_FAST); gameMode=GAME_IDLE; Serial.println("[BOMB] BOOM!"); }
}
void bombUpdate() {
  if (bombOver||(long)(millis()-gameEndTime)<0) return;
  bombOver=true; allLED(COL_RED,ANIM_BLINK_FAST); gameMode=GAME_IDLE; Serial.println("[BOMB] ZEIT UM!");
}

// ─────────────────────────────────────────────────────────────
// Reaktionsspiel
// ─────────────────────────────────────────────────────────────

void reactInsertHighScore(uint8_t id) {
  if (players[id].name[0]==0) return;
  if (players[id].points==0) return;
  // Find insert position (sorted by points desc, then bestMs asc)
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

void reactStartRound() {
  if (reactRound >= cfgReactRounds) {
    // Spiel beendet
    uint8_t winner=1;
    for (uint8_t i=2;i<=nodeCount;i++) if(players[i].points>players[winner].points) winner=i;
    Serial.println("[REACT] === ENDE ===");
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  %s: %u Pkt  Best: %ums\n", players[i].name, players[i].points, players[i].bestMs);
    // Highscores aktualisieren
    for (uint8_t i=1;i<=nodeCount;i++) reactInsertHighScore(i);
    // Sieger-Animation
    allLED(COL_OFF, ANIM_SOLID); delay(200);
    setLED(winner, COL_GREEN, ANIM_BLINK_FAST);
    for (uint8_t i=1;i<=nodeCount;i++) if(i!=winner) setLED(i,COL_RED,ANIM_SOLID);
    gameMode = GAME_IDLE;
    return;
  }

  // Zufälligen Target-Node wählen (nicht denselben wie letztes Mal)
  uint8_t tries=0;
  do { reactTarget=random(1,nodeCount+1); tries++; }
  while (reactTarget==reactLastTarget && nodeCount>1 && tries<20);
  reactLastTarget = reactTarget;

  // LEDs setzen: Target blinkt gelb, alle anderen aus
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

  // Countdown: 3× blinken
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
    // Gewonnen!
    reactRoundDone = true;
    players[id].points++;
    if (players[id].bestMs==0 || ms<players[id].bestMs) players[id].bestMs=ms;

    Serial.printf("[REACT] %s: %ums → %u Pkt\n", players[id].name, ms, players[id].points);

    setLED(id, COL_GREEN, ANIM_FLASH);
    for (uint8_t i=1;i<=nodeCount;i++) if(i!=id) setLED(i,COL_RED,ANIM_BLINK_FAST);
    reactNextAt = millis() + random(REACT_DELAY_MIN_MS, REACT_DELAY_MAX_MS);
  }
  // Falscher Node → ignorieren (kein Strafpunkt)
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
// UDP empfangen
// ─────────────────────────────────────────────────────────────
void handleUDP() {
  int size=udp.parsePacket();
  if (size<(int)sizeof(Packet)) return;
  Packet p; udp.read((uint8_t*)&p,sizeof(p));
  IPAddress remoteIP=udp.remoteIP();

  switch(p.type) {
    case PKT_REGISTER:
      for (uint8_t i=1;i<=nodeCount;i++) {
        if (nodes[i].ip==remoteIP) { nodes[i].lastSeen=millis(); sendPkt(remoteIP,PKT_ACK,i,i); return; }
      }
      if (nodeCount>=MAX_NODES) return;
      nodeCount++;
      nodes[nodeCount].ip       = remoteIP;
      nodes[nodeCount].active   = true;
      nodes[nodeCount].lastSeen = millis();
      if (players[nodeCount].name[0]==0) snprintf(players[nodeCount].name,20,"Spieler %u",nodeCount);
      sendPkt(remoteIP,PKT_ACK,nodeCount,nodeCount);
      Serial.printf("[REG] Node %u (%s)\n",nodeCount,remoteIP.toString().c_str());
      break;

    case PKT_PING:
      if (p.nodeId>=1&&p.nodeId<=nodeCount) nodes[p.nodeId].lastSeen=millis();
      break;

    case PKT_BUTTON: {
      uint8_t id=p.nodeId;
      if (id<1||id>nodeCount) return;
      nodes[id].lastSeen=millis();
      Serial.printf("[BTN] Node %u\n",id);
      if      (gameMode==GAME_CTF)      ctfOnButton(id);
      else if (gameMode==GAME_MEMORY)   memOnButton(id);
      else if (gameMode==GAME_BOMB)     bombOnButton(id);
      else if (gameMode==GAME_REACTION) reactOnButton(id);
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
.btn-start{background:#238636;color:#fff}.btn-stop{background:#b91c1c;color:#fff}.btn-save{background:#0f3460;color:#a8dadc}
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
table{width:100%;border-collapse:collapse;font-size:.88rem}
th{color:#8b949e;font-weight:600;padding:4px 6px;text-align:left;border-bottom:1px solid #30363d}
td{padding:4px 6px;border-bottom:1px solid #21262d}
.gold{color:#f0883e}.silver{color:#8b949e}.bronze{color:#cd7f32}
.rnd{text-align:center;font-size:1.1rem;font-weight:700;margin:6px 0;color:#a8dadc}
</style></head>
<body>
<h1>&#127918; ESP CTF Game</h1>

<div class="card">
  <h2>Status</h2>
  <div class="row"><span>Spielmodus</span><span id="modeName" class="badge idle">Idle</span></div>
  <div class="row"><span>Nodes</span><strong id="nodeCount">0</strong></div>
  <div class="nodes-grid" id="nodesGrid"></div>
  <div class="timer hidden" id="timer"></div>
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
  </select>
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
  <button class="btn btn-start" onclick="startGame()">&#9654; STARTEN</button>
  <button class="btn btn-stop" onclick="stopGame()">&#9632; STOPPEN</button>
</div>

<div class="card hidden" id="reactLiveCard">
  <h2>&#9889; Reaktionsspiel live</h2>
  <div class="rnd" id="reactRoundDisp"></div>
  <div id="reactLiveScores"></div>
</div>

<div class="card hidden" id="ctfScoreCard">
  <h2>&#127987; Punktestand CTF</h2>
  <div id="ctfScores"></div>
</div>

<div class="card hidden" id="hsCard">
  <h2>&#127942; Bestenliste</h2>
  <table><thead><tr><th>#</th><th>Name</th><th>Punkte</th><th>Beste Zeit</th></tr></thead>
  <tbody id="hsTbody"></tbody></table>
  <button class="btn btn-stop" style="margin-top:10px;font-size:.82rem;padding:8px" onclick="clearScores()">&#128465; Bestenliste l&ouml;schen</button>
</div>

<script>
var modeNames=['','CTF','Memory','Bomb','Reaktion'];
var tc=['#8b949e','#f85149','#388bfd','#3fb950','#e3b341'];
var knownNodes=0;

function modeChanged(){
  var m=document.getElementById('mode').value;
  document.getElementById('ctfOpts').classList.toggle('hidden',m!='1');
  document.getElementById('bombOpts').classList.toggle('hidden',m!='3');
  document.getElementById('reactOpts').classList.toggle('hidden',m!='4');
}

function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});}

function startGame(){
  var m=document.getElementById('mode').value;
  var q='mode='+m;
  if(m=='1') q+='&teams='+document.getElementById('teams').value+'&duration='+document.getElementById('duration').value;
  if(m=='3') q+='&seqLen='+document.getElementById('seqLen').value+'&duration='+document.getElementById('bombDur').value;
  if(m=='4') q+='&rounds='+document.getElementById('reactRounds').value;
  post('/start',q);
}
function stopGame(){post('/stop','');}

function saveNames(){
  var q='';
  for(var i=1;i<=16;i++){var el=document.getElementById('n'+i);if(el)q+='&n'+i+'='+encodeURIComponent(el.value||'');}
  post('/names',q.slice(1)).then(function(){var b=document.querySelector('.btn-save');b.textContent='&#10003; Gespeichert';setTimeout(function(){b.innerHTML='&#128190; Speichern';},1500);});
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
    // Nodes
    document.getElementById('nodeCount').textContent=d.nodes;
    if(d.nodes!==knownNodes){knownNodes=d.nodes;updateNameInputs(d.nodes);}
    var g=document.getElementById('nodesGrid');g.innerHTML='';
    for(var i=1;i<=Math.max(d.nodes,1);i++){var dot=document.createElement('div');dot.className='nd'+(i<=d.nodes?' on':'');dot.textContent=i;g.appendChild(dot);}

    // Badge
    var mb=document.getElementById('modeName');
    mb.textContent=d.running?modeNames[d.mode]+' läuft':'Idle';
    mb.className='badge '+(d.running?'running':'idle');

    // Timer
    var tr=document.getElementById('timer');
    if(d.running&&d.timeLeft>0){tr.classList.remove('hidden');tr.textContent=fmtTime(d.timeLeft);tr.className='timer'+(d.timeLeft<30?' crit':d.timeLeft<60?' warn':'');}
    else tr.classList.add('hidden');

    // CTF scores
    var cs=document.getElementById('ctfScoreCard');
    if(d.running&&d.mode==1&&d.scores){
      cs.classList.remove('hidden');
      var sv=document.getElementById('ctfScores');sv.innerHTML='';
      var mx=1;for(var k in d.scores)if(d.scores[k]>mx)mx=d.scores[k];
      var tn=['','ROT','BLAU','GRUEN','GELB'];
      for(var t=1;t<=4;t++){if(d.scores[t]===undefined)continue;
        var row=document.createElement('div');row.className='srow';
        var pct=mx>0?Math.round(d.scores[t]/mx*100):0;
        row.innerHTML='<div class="sname" style="color:'+tc[t]+';min-width:60px">'+tn[t]+'</div>'
          +'<div class="bar-wrap"><div class="bar-fill" style="background:'+tc[t]+';width:'+pct+'%"></div></div>'
          +'<div class="spts">'+d.scores[t]+'</div>';
        sv.appendChild(row);}
    } else cs.classList.add('hidden');

    // Reaktion live
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
          +'<div class="sms">'+(p.bestMs?p.bestMs+'ms':'–')+'</div>';
        rs.appendChild(row);});
    } else rc.classList.add('hidden');

    // Highscores
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
  }).catch(function(){});
}
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
  if (gameMode!=GAME_IDLE) { long r=(long)(gameEndTime-millis()); timeLeft=(r>0)?(uint32_t)r/1000:0; }

  String j="{";
  j+="\"nodes\":"+String(nodeCount)+",";
  j+="\"mode\":"+String(gameMode)+",";
  j+="\"running\":"; j+=(gameMode!=GAME_IDLE?"true":"false"); j+=",";
  j+="\"timeLeft\":"+String(timeLeft);

  if (gameMode==GAME_CTF) {
    j+=",\"scores\":{";
    for (uint8_t t=1;t<=cfgTeams;t++) {
      uint8_t s=0; for(uint8_t i=1;i<=nodeCount;i++) if(ctfTeam[i]==t) s++;
      j+="\""+String(t)+"\":"+String(s); if(t<cfgTeams) j+=",";
    }
    j+="}";
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

  // Highscores immer mitsenden (max 10)
  j+=",\"highscores\":[";
  for (uint8_t i=0;i<highScoreCount;i++) {
    j+="{\"name\":\""+String(highScores[i].name)+"\",\"points\":"+String(highScores[i].points)+",\"bestMs\":"+String(highScores[i].bestMs)+"}";
    if (i<highScoreCount-1) j+=",";
  }
  j+="]}";

  webServer.sendHeader("Access-Control-Allow-Origin","*");
  webServer.send(200,"application/json",j);
}

void webHandleStart() {
  if (!webServer.hasArg("mode")) { webServer.send(400,"text/plain","missing mode"); return; }
  uint8_t m=webServer.arg("mode").toInt();
  if (webServer.hasArg("teams"))    cfgTeams    =constrain(webServer.arg("teams").toInt(),2,4);
  if (webServer.hasArg("duration")) cfgDuration =constrain(webServer.arg("duration").toInt(),30,600);
  if (webServer.hasArg("seqLen"))   cfgSeqLen   =constrain(webServer.arg("seqLen").toInt(),3,12);
  if (webServer.hasArg("duration")&&m==3) cfgBombDur=constrain(webServer.arg("duration").toInt(),30,300);
  if (webServer.hasArg("rounds"))   cfgReactRounds=constrain(webServer.arg("rounds").toInt(),3,20);
  gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID); delay(100);
  if      (m==GAME_CTF)      ctfStart();
  else if (m==GAME_MEMORY)   memStart();
  else if (m==GAME_BOMB)     bombStart();
  else if (m==GAME_REACTION) reactStart();
  webServer.send(200,"text/plain","OK");
}

void webHandleStop() {
  gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID);
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
  else if (c=='0') { gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID); Serial.println("Gestoppt."); }
  else if (c=='s') {
    Serial.printf("Modus:%u Nodes:%u\n",gameMode,nodeCount);
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  Node%u '%s': %s\n",i,players[i].name,nodes[i].active?"OK":"offline");
    if (highScoreCount>0) {
      Serial.println("Highscores:");
      for (uint8_t i=0;i<highScoreCount;i++)
        Serial.printf("  %u. %s – %u Pkt, %ums\n",i+1,highScores[i].name,highScores[i].points,highScores[i].bestMs);
    }
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
  if (strlen(WIFI_PASS)>0) WiFi.softAP(WIFI_SSID, WIFI_PASS);
  else WiFi.softAP(WIFI_SSID);
  Serial.printf("AP: %s  IP: %s\n", WIFI_SSID, WiFi.softAPIP().toString().c_str());

  udp.begin(UDP_PORT);

  webServer.on("/",            webHandleRoot);
  webServer.on("/status",      webHandleStatus);
  webServer.on("/start",  HTTP_POST, webHandleStart);
  webServer.on("/stop",   HTTP_POST, webHandleStop);
  webServer.on("/names",  HTTP_POST, webHandleNames);
  webServer.on("/clearscores", HTTP_POST, webHandleClearScores);
  webServer.begin();

  Serial.println("Web: http://192.168.4.1");
  Serial.println("Seriell: 1=CTF 2=Memory 3=Bomb 4=Reaktion 0=Stop s=Status");
}

void loop() {
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

  if      (gameMode==GAME_CTF)      ctfUpdate();
  else if (gameMode==GAME_MEMORY)   memUpdate();
  else if (gameMode==GAME_BOMB)     bombUpdate();
  else if (gameMode==GAME_REACTION) reactUpdate();
}
