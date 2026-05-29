/*
 * ESP CTF Game – Master Controller
 * ESP8266 + ESP32 kompatibel
 *
 * Handy-Konfiguration:
 *   1. Handy mit WLAN "ESP-CTF-Game" verbinden (kein Passwort)
 *   2. Browser öffnen → http://192.168.4.1
 *
 * Serielle Befehle (115200 Baud, als Backup):
 *   1=CTF  2=Memory  3=Bomb  0=Stop  s=Status
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
// Konfigurierbare Spielparameter (via Web-UI änderbar)
// ─────────────────────────────────────────────────────────────
uint8_t  cfgTeams    = CTF_TEAMS;
uint16_t cfgDuration = CTF_DURATION_S;
uint8_t  cfgSeqLen   = BOMB_SEQ_LEN;
uint16_t cfgBombDur  = BOMB_DURATION_S;

// ─────────────────────────────────────────────────────────────
// Node-Registry
// ─────────────────────────────────────────────────────────────
struct NodeInfo { IPAddress ip; bool active; uint32_t lastSeen; };
NodeInfo nodes[MAX_NODES + 1];
uint8_t  nodeCount = 0;

WiFiUDP  udp;
IPAddress bcastIP(192, 168, 4, 255);

// ─────────────────────────────────────────────────────────────
// Spielzustand
// ─────────────────────────────────────────────────────────────
uint8_t  gameMode    = GAME_IDLE;
uint32_t gameEndTime = 0;

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

// ─────────────────────────────────────────────────────────────
// Netzwerk-Hilfsfunktionen
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

void broadcast(uint8_t type, uint8_t nid,
               uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
               uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  sendPkt(bcastIP, type, nid, d0, d1, d2, d3, d4, d5);
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
static const char*   TEAM_NAMES[]  = {"Neutral","ROT","BLAU","GRÜN","GELB"};

void ctfStart() {
  if (nodeCount<2) { Serial.println("[CTF] Mindestens 2 Nodes nötig."); return; }
  gameMode=GAME_CTF; gameEndTime=millis()+(uint32_t)cfgDuration*1000;
  for (uint8_t i=1;i<=nodeCount;i++) { ctfTeam[i]=0; setLED(i,COL_WHITE,ANIM_SOLID); delay(30); }
  Serial.printf("[CTF] Start: %u Nodes, %us, %u Teams\n",nodeCount,cfgDuration,cfgTeams);
}

void ctfOnButton(uint8_t id) {
  ctfTeam[id]=(ctfTeam[id]%cfgTeams)+1;
  setLED(id,TEAM_COLORS[ctfTeam[id]],ANIM_FLASH);
  Serial.printf("[CTF] Node %u → %s\n",id,TEAM_NAMES[ctfTeam[id]]);
}

void ctfUpdate() {
  if ((long)(millis()-gameEndTime)<0) return;
  uint8_t score[5]={0};
  for (uint8_t i=1;i<=nodeCount;i++) if(ctfTeam[i]>0&&ctfTeam[i]<=4) score[ctfTeam[i]]++;
  uint8_t winner=1;
  for (uint8_t t=2;t<=cfgTeams;t++) if(score[t]>score[winner]) winner=t;
  Serial.println("[CTF] === ZEIT UM ===");
  for (uint8_t t=1;t<=cfgTeams;t++) Serial.printf("  %s: %u Nodes\n",TEAM_NAMES[t],score[t]);
  Serial.printf("  SIEGER: %s!\n",TEAM_NAMES[winner]);
  allLED(TEAM_COLORS[winner],ANIM_BLINK_FAST);
  gameMode=GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// Memory
// ─────────────────────────────────────────────────────────────
static const uint8_t PAIR_PALETTE[] = {COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW,COL_PURPLE,COL_CYAN,COL_ORANGE};

void memStart() {
  if (nodeCount<2) { Serial.println("[MEM] Mindestens 2 Nodes nötig."); return; }
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
  Serial.println("[MEM] Zeige Paare...");
  for (uint8_t i=1;i<=n;i++) { setLED(i,memColor[i],ANIM_SOLID); delay(30); }
  delay(2000);
  for (uint8_t i=1;i<=n;i++) { setLED(i,COL_OFF,ANIM_SOLID); delay(30); }
  Serial.printf("[MEM] Start: %u Nodes, %u Paare\n",n,memTotalPairs);
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
      Serial.printf("[MEM] Treffer! %u/%u\n",memFoundPairs,memTotalPairs);
      if (memFoundPairs==memTotalPairs) { delay(400); allLED(COL_GREEN,ANIM_BLINK_FAST); Serial.println("[MEM] ALLE PAARE!"); gameMode=GAME_IDLE; }
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
  uint8_t seqLen=min((uint8_t)cfgSeqLen,(uint8_t)16);
  for (uint8_t s=0;s<seqLen;s++) {
    setLED(bombSeq[s],SEQ_COLORS[s%8],ANIM_SOLID); delay(BOMB_STEP_MS);
    setLED(bombSeq[s],COL_OFF,ANIM_SOLID); delay(200);
  }
}

void bombStart() {
  if (nodeCount<3) { Serial.println("[BOMB] Mindestens 3 Nodes nötig."); return; }
  gameMode=GAME_BOMB; bombStep=0; bombOver=false;
  gameEndTime=millis()+(uint32_t)cfgBombDur*1000;
  bombNode=random(1,nodeCount+1);
  uint8_t avail[MAX_NODES],cnt=0;
  for (uint8_t i=1;i<=nodeCount;i++) if(i!=bombNode) avail[cnt++]=i;
  uint8_t seqLen=min((uint8_t)cfgSeqLen,cnt);
  for (uint8_t i=0;i<seqLen;i++) { uint8_t p=random(0,cnt-i); bombSeq[i]=avail[p]; avail[p]=avail[cnt-i-1]; }
  allLED(COL_OFF,ANIM_SOLID); delay(200);
  setLED(bombNode,COL_RED,ANIM_BLINK_FAST);
  Serial.println("[BOMB] Zeige Sequenz...");
  bombShowSequence();
  for (uint8_t i=1;i<=nodeCount;i++) { setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID); delay(20); }
  Serial.printf("[BOMB] Start! Bombe=Node%u, %us\n",bombNode,cfgBombDur);
}

void bombOnButton(uint8_t id) {
  if (bombOver) return;
  if (id==bombNode) {
    Serial.println("[BOMB] Sequenz-Wiederholung");
    bombShowSequence();
    for (uint8_t i=1;i<=nodeCount;i++) { setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID); delay(20); }
    return;
  }
  uint8_t seqLen=min((uint8_t)cfgSeqLen,(uint8_t)16);
  if (id==bombSeq[bombStep]) {
    setLED(id,COL_GREEN,ANIM_FLASH); bombStep++;
    Serial.printf("[BOMB] Schritt %u/%u OK\n",bombStep,seqLen);
    if (bombStep==seqLen) { bombOver=true; Serial.println("[BOMB] ENTSCHÄRFT!"); allLED(COL_GREEN,ANIM_BLINK_SLOW); gameMode=GAME_IDLE; }
  } else {
    bombOver=true; Serial.println("[BOMB] FALSCH – BOOM!");
    allLED(COL_RED,ANIM_BLINK_FAST); gameMode=GAME_IDLE;
  }
}

void bombUpdate() {
  if (bombOver||(long)(millis()-gameEndTime)<0) return;
  bombOver=true; Serial.println("[BOMB] ZEIT UM – BOOM!");
  allLED(COL_RED,ANIM_BLINK_FAST); gameMode=GAME_IDLE;
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
      nodeCount++; nodes[nodeCount]={remoteIP,true,millis()};
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
      if      (gameMode==GAME_CTF)    ctfOnButton(id);
      else if (gameMode==GAME_MEMORY) memOnButton(id);
      else if (gameMode==GAME_BOMB)   bombOnButton(id);
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Web-UI HTML (im Flash-Speicher)
// ─────────────────────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP CTF Game</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,sans-serif;background:#0d1117;color:#e6edf3;padding:12px;max-width:480px;margin:auto}
  h1{text-align:center;color:#f0883e;margin:16px 0 20px;font-size:1.6rem}
  .card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:16px;margin-bottom:14px}
  .card h2{color:#8b949e;font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;margin-bottom:12px}
  .row{display:flex;justify-content:space-between;align-items:center;padding:4px 0}
  .badge{padding:4px 12px;border-radius:20px;font-size:.85rem;font-weight:600}
  .idle{background:#21262d;color:#8b949e}
  .running{background:#0f5132;color:#3fb950}
  label{display:block;color:#8b949e;font-size:.85rem;margin:10px 0 4px}
  select,input[type=number]{width:100%;padding:9px 12px;background:#21262d;border:1px solid #30363d;border-radius:8px;color:#e6edf3;font-size:.95rem}
  .hidden{display:none}
  .btn{width:100%;padding:13px;border:none;border-radius:9px;font-size:1rem;font-weight:700;cursor:pointer;margin-top:8px;transition:opacity .15s}
  .btn:active{opacity:.7}
  .btn-start{background:#238636;color:#fff}
  .btn-stop{background:#b91c1c;color:#fff}
  #scores .srow{display:flex;align-items:center;gap:8px;margin:6px 0}
  #scores .srow .name{width:70px;font-weight:600}
  #scores .srow .bar{flex:1;height:10px;border-radius:5px;transition:width .5s}
  #scores .srow .val{width:30px;text-align:right;font-size:.9rem}
  .pulse{animation:pulse 1s infinite}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
  .nodes-grid{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
  .node-dot{width:36px;height:36px;border-radius:8px;background:#21262d;border:1px solid #30363d;display:flex;align-items:center;justify-content:center;font-size:.75rem;font-weight:700;color:#8b949e}
  .node-dot.active{background:#0f5132;border-color:#3fb950;color:#3fb950}
  .timer{font-size:2rem;font-weight:700;text-align:center;font-variant-numeric:tabular-nums;margin:4px 0}
  .timer.warn{color:#e3b341}
  .timer.crit{color:#f85149;animation:pulse .5s infinite}
</style></head>
<body>
<h1>&#127918; ESP CTF Game</h1>

<div class="card">
  <h2>Status</h2>
  <div class="row"><span>Spielmodus</span><span id="modeName" class="badge idle">Idle</span></div>
  <div class="row"><span>Nodes verbunden</span><strong id="nodeCount">0</strong></div>
  <div class="nodes-grid" id="nodesGrid"></div>
  <div class="timer" id="timer" style="display:none"></div>
</div>

<div class="card">
  <h2>Einstellungen</h2>
  <label>Spielmodus</label>
  <select id="mode" onchange="modeChanged()">
    <option value="1">&#127987; Capture the Flag</option>
    <option value="2">&#129504; Memory &ndash; Paare finden</option>
    <option value="3">&#128163; Bombenentsch&auml;rfung</option>
  </select>

  <div id="ctfOpts">
    <label>Anzahl Teams</label>
    <select id="teams">
      <option value="2">2 Teams (Rot vs. Blau)</option>
      <option value="3">3 Teams</option>
      <option value="4">4 Teams</option>
    </select>
    <label>Spieldauer (Sekunden)</label>
    <input type="number" id="duration" value="180" min="30" max="600" step="30">
  </div>

  <div id="bombOpts" class="hidden">
    <label>Sequenzl&auml;nge (Schritte)</label>
    <input type="number" id="seqLen" value="5" min="3" max="12">
    <label>Zeitlimit (Sekunden)</label>
    <input type="number" id="bombDur" value="120" min="30" max="300" step="30">
  </div>

  <button class="btn btn-start" onclick="startGame()">&#9654; SPIEL STARTEN</button>
  <button class="btn btn-stop"  onclick="stopGame()">&#9632; STOPPEN</button>
</div>

<div class="card" id="scoresCard" style="display:none">
  <h2>Punktestand</h2>
  <div id="scores"></div>
</div>

<script>
var modeNames=['','CTF','Memory','Bomb'];
var teamColors=['#8b949e','#f85149','#388bfd','#3fb950','#e3b341'];

function modeChanged(){
  var m=document.getElementById('mode').value;
  document.getElementById('ctfOpts').classList.toggle('hidden',m!='1');
  document.getElementById('bombOpts').classList.toggle('hidden',m!='3');
}

function post(url,body){
  return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
}

function startGame(){
  var m=document.getElementById('mode').value;
  var q='mode='+m;
  if(m=='1') q+='&teams='+document.getElementById('teams').value+'&duration='+document.getElementById('duration').value;
  if(m=='3') q+='&seqLen='+document.getElementById('seqLen').value+'&duration='+document.getElementById('bombDur').value;
  post('/start',q);
}

function stopGame(){ post('/stop',''); }

function fmtTime(s){
  if(s<=0) return '0:00';
  return Math.floor(s/60)+':'+(s%60<10?'0':'')+s%60;
}

function updateStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    // Node count + grid
    document.getElementById('nodeCount').textContent=d.nodes;
    var g=document.getElementById('nodesGrid');
    g.innerHTML='';
    for(var i=1;i<=Math.max(d.nodes,1);i++){
      var dot=document.createElement('div');
      dot.className='node-dot'+(i<=d.nodes?' active':'');
      dot.textContent=i;
      g.appendChild(dot);
    }
    // Mode badge
    var mb=document.getElementById('modeName');
    mb.textContent=d.running?modeNames[d.mode]+' läuft':'Idle';
    mb.className='badge '+(d.running?'running':'idle');
    // Timer
    var tr=document.getElementById('timer');
    if(d.running&&d.timeLeft>0){
      tr.style.display='block';
      tr.textContent=fmtTime(d.timeLeft);
      tr.className='timer'+(d.timeLeft<30?' crit':d.timeLeft<60?' warn':'');
    } else { tr.style.display='none'; }
    // Scores (CTF only)
    var sc=document.getElementById('scoresCard');
    var sv=document.getElementById('scores');
    if(d.running&&d.mode==1&&d.scores){
      sc.style.display='block'; sv.innerHTML='';
      var max=1;
      for(var k in d.scores) if(d.scores[k]>max) max=d.scores[k];
      var tnames=['','ROT','BLAU','GRÜN','GELB'];
      for(var t=1;t<=4;t++){
        if(!d.scores[t]&&d.scores[t]!==0) continue;
        var row=document.createElement('div'); row.className='srow';
        var pct=max>0?Math.round(d.scores[t]/max*100):0;
        row.innerHTML='<div class="name" style="color:'+teamColors[t]+'">'+tnames[t]+'</div>'
          +'<div class="bar" style="background:'+teamColors[t]+';width:'+pct+'%"></div>'
          +'<div class="val">'+d.scores[t]+'</div>';
        sv.appendChild(row);
      }
    } else if(!d.running) { sc.style.display='none'; }
  }).catch(e=>{});
}
setInterval(updateStatus,1000);
updateStatus();
</script>
</body></html>
)rawliteral";

// ─────────────────────────────────────────────────────────────
// Web-Routen
// ─────────────────────────────────────────────────────────────
void webHandleRoot() {
  webServer.send_P(200, "text/html", INDEX_HTML);
}

void webHandleStatus() {
  uint32_t timeLeft = 0;
  if (gameMode != GAME_IDLE) {
    long remaining = (long)(gameEndTime - millis());
    timeLeft = (remaining > 0) ? (uint32_t)remaining / 1000 : 0;
  }

  String json = "{";
  json += "\"nodes\":" + String(nodeCount) + ",";
  json += "\"mode\":"  + String(gameMode)  + ",";
  json += "\"running\":" + String(gameMode != GAME_IDLE ? "true" : "false") + ",";
  json += "\"timeLeft\":" + String(timeLeft);

  if (gameMode == GAME_CTF) {
    json += ",\"scores\":{";
    for (uint8_t t = 1; t <= cfgTeams; t++) {
      uint8_t score = 0;
      for (uint8_t i = 1; i <= nodeCount; i++) if (ctfTeam[i] == t) score++;
      json += "\"" + String(t) + "\":" + String(score);
      if (t < cfgTeams) json += ",";
    }
    json += "}";
  }
  json += "}";

  webServer.sendHeader("Access-Control-Allow-Origin", "*");
  webServer.send(200, "application/json", json);
}

void webHandleStart() {
  if (webServer.hasArg("mode")) {
    uint8_t m = webServer.arg("mode").toInt();
    if (webServer.hasArg("teams"))    cfgTeams    = constrain(webServer.arg("teams").toInt(),    2, 4);
    if (webServer.hasArg("duration")) cfgDuration = constrain(webServer.arg("duration").toInt(), 30, 600);
    if (webServer.hasArg("seqLen"))   cfgSeqLen   = constrain(webServer.arg("seqLen").toInt(),   3, 12);
    if (webServer.hasArg("duration") && m == 3)
                                      cfgBombDur  = constrain(webServer.arg("duration").toInt(), 30, 300);
    gameMode = GAME_IDLE; allLED(COL_OFF, ANIM_SOLID); delay(100);
    if      (m == GAME_CTF)    ctfStart();
    else if (m == GAME_MEMORY) memStart();
    else if (m == GAME_BOMB)   bombStart();
  }
  webServer.send(200, "text/plain", "OK");
}

void webHandleStop() {
  gameMode = GAME_IDLE;
  allLED(COL_OFF, ANIM_SOLID);
  Serial.println("Spiel gestoppt.");
  webServer.send(200, "text/plain", "OK");
}

// ─────────────────────────────────────────────────────────────
// Serielles Menü (Backup)
// ─────────────────────────────────────────────────────────────
void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if      (c == '1') ctfStart();
  else if (c == '2') memStart();
  else if (c == '3') bombStart();
  else if (c == '0') { gameMode=GAME_IDLE; allLED(COL_OFF,ANIM_SOLID); Serial.println("Gestoppt."); }
  else if (c == 's') {
    Serial.printf("Modus:%u  Nodes:%u\n",gameMode,nodeCount);
    for (uint8_t i=1;i<=nodeCount;i++)
      Serial.printf("  Node%u: %s\n",i,nodes[i].active?"aktiv":"offline");
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
  Serial.printf("WiFi-AP: %s  IP: %s\n", WIFI_SSID, WiFi.softAPIP().toString().c_str());

  udp.begin(UDP_PORT);
  Serial.println("UDP Port: " + String(UDP_PORT));

  webServer.on("/",       webHandleRoot);
  webServer.on("/status", webHandleStatus);
  webServer.on("/start",  HTTP_POST, webHandleStart);
  webServer.on("/stop",   HTTP_POST, webHandleStop);
  webServer.begin();
  Serial.println("Webserver: http://192.168.4.1");
  Serial.println("→ Handy mit 'ESP-CTF-Game' verbinden, dann Browser öffnen!");
}

void loop() {
  webServer.handleClient();
  handleUDP();
  handleSerial();

  // Node Timeout-Prüfung
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 2000) {
    lastCheck = millis();
    for (uint8_t i=1;i<=nodeCount;i++) {
      bool was=nodes[i].active;
      nodes[i].active=(millis()-nodes[i].lastSeen)<NODE_TIMEOUT_MS;
      if (was&&!nodes[i].active) Serial.printf("[WARN] Node %u Timeout\n",i);
    }
  }

  if      (gameMode==GAME_CTF)    ctfUpdate();
  else if (gameMode==GAME_MEMORY) memUpdate();
  else if (gameMode==GAME_BOMB)   bombUpdate();
}
