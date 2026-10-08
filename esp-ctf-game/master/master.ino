/*
 * ESP CTF Game – Master Controller   (Firmware-Version: siehe protocol.h)
 * ESP32 (alle Funktionen) + ESP8266 (Grundfunktionen, ohne Firmware-Verteilung)
 *
 * Handy: WLAN "ESP-CTF-Game" → Browser http://192.168.4.1
 * Seriell (115200):
 *   1=CTF  2=Memory  3=Bomb  4=Reaktion  5=Simon  6=HotPotato
 *   7=KingHill  8=TugWar  9=Minesweeper  k=Knockout  h=ColorHunt  w=WhackaMole
 *   0=Stop  s=Status  r=Reconnect
 */

#ifdef ESP32
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Update.h>
  #include <AsyncUDP.h>
  #include <Preferences.h>
  #include <LittleFS.h>
  #include <MD5Builder.h>
  #include "esp_ota_ops.h"
  #include "esp_task_wdt.h"
  #include "esp_system.h"
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

// ── Fallbacks: aeltere config.h-Kopien kennen diese Optionen noch nicht ──
#ifndef WIFI_CHANNEL
  #define WIFI_CHANNEL 0          // 0 = automatisch
#endif
#ifndef AP_MAX_CONN
  #define AP_MAX_CONN 10
#endif
#ifndef OTA_PASSWORD
  #define OTA_PASSWORD ""
#endif
#ifndef USE_WATCHDOG
  #define USE_WATCHDOG 1
#endif
#ifndef WDT_TIMEOUT_S
  #define WDT_TIMEOUT_S 30
#endif
#ifndef NODE_TIMEOUT_MS
  #define NODE_TIMEOUT_MS 15000UL
#endif

#include "protocol.h"

#ifdef ESP32
// C-Linkage VOR den automatisch erzeugten Prototypen festlegen (Hook des Cores, siehe unten)
extern "C" bool verifyRollbackLater();
#endif

#if MAX_NODES < 2 || MAX_NODES > 32
  #error "MAX_NODES in config.h muss zwischen 2 und 32 liegen"
#endif

#define BAR_LEDS     8       // LEDs je Node (NEO_COUNT der Node-Firmware)
#define IMG_HDR_LEN  0x140   // so viele Bytes eines .bin werden vorab geprueft (inkl. CtfFwDesc)
static_assert(CTF_DESC_OFFSET + sizeof(CtfFwDesc) <= IMG_HDR_LEN, "CtfFwDesc liegt ausserhalb des Pruefbereichs");

// ─────────────────────────────────────────────────────────────
// Node-Registry: feste ID je MAC-Adresse (im NVS gespeichert)
// ─────────────────────────────────────────────────────────────
struct NodeInfo {
  uint8_t  mac[6];
  bool     known;        // Slot belegt (MAC bekannt)
  uint32_t ip;           // letzte IP (IPv4 roh, 0 = unbekannt) – IPs koennen wechseln (DHCP)!
  bool     active;       // in den letzten NODE_TIMEOUT_MS etwas gehoert
  uint32_t lastSeen;
  int8_t   rssi;         // Empfangsstaerke laut Node (dBm)
  uint8_t  fw[3];        // Firmware-Version laut STATUS (0.0.0 = unbekannt)
  uint8_t  flags;        // NODE_FLAG_*
  uint8_t  resetReason;  // esp_reset_reason() der Node (letzter Start)
  bool     statusSeen;   // STATUS seit der letzten Registrierung erhalten
  uint8_t  regCount;     // zaehlt Registrierungen (Rollout erkennt so den Neustart)
  uint8_t  btnNonce, lastBtnSeq;   // Tasten-Dedup (Sitzung + Sequenz)
  // Soll-LED-Zustand: wird wiederholt gesendet, bis die Node ihn quittiert
  uint8_t  ledType, ledA, ledB, ledC;
  uint8_t  ledSeq;       // 1..255, 0 = (noch) kein Soll-Zustand
  uint8_t  ledAcked;     // von der Node gemeldete, angewendete Seq
  uint8_t  ledTries;     // Anzahl Sendungen des aktuellen Zustands
  uint32_t ledSentAt;
  uint8_t  otaState, otaPct, otaErr;   // letzter PKT_OTA_STATUS
};
NodeInfo nodes[MAX_NODES + 1];
uint8_t  nodeCount = 0;              // hoechste bekannte ID (Luecken moeglich -> nodes[i].known pruefen)
bool     inGame[MAX_NODES + 1];      // Teilnehmer des laufenden Spiels (aktive Nodes beim Start)

#ifdef ESP32
AsyncUDP audp;
struct RxItem { Packet p; uint32_t ip; };
QueueHandle_t     rxQueue = NULL;    // AsyncUDP-Task -> loop()
volatile uint32_t rxDrops = 0;       // verworfene Pakete (Queue voll)
#else
WiFiUDP  udp;
uint32_t rxDrops = 0;
#endif
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
uint8_t  gameMode       = GAME_IDLE;
uint32_t gameEndTime    = 0;
uint32_t idleBlinkUntil = 0;   // nach Spielende: Blinken stopp nach 10s

// Konfigurierbare Parameter
uint8_t  cfgTeams        = CTF_TEAMS;
uint16_t cfgDuration     = CTF_DURATION_S;
uint8_t  cfgSeqLen       = BOMB_SEQ_LEN;
uint16_t cfgBombDur      = BOMB_DURATION_S;
uint8_t  cfgReactRounds  = REACT_ROUNDS_DEFAULT;
uint8_t  cfgMines        = MINE_COUNT_DEFAULT;
uint8_t  cfgKnockLives   = KNOCK_LIVES_DEFAULT;
uint8_t  cfgHuntRounds   = COLORHUNT_ROUNDS;
uint8_t  cfgRoundDelay   = ROUND_DELAY_S;    // Pflichtpause zwischen Runden (s)

// CTF
uint8_t  ctfTeam[MAX_NODES + 1];
uint32_t ctfLockUntil[MAX_NODES + 1];  // Sperre je Node: Zeitstempel Ablauf (0 = frei)
uint32_t ctfFeedbackUntil[MAX_NODES + 1]; // oranges "gesperrt"-Blinken bis (0 = keins)
uint8_t  ctfBarLast[MAX_NODES + 1];    // gezeigter Zustand: 0..8 Sperr-Balken, CTF_SHOW_SOLID, 255 = neu zeichnen
uint32_t ctfLockMs;                    // Sperrdauer dieses Spiels (ms, 0 = keine Sperre)

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
bool     simonShowing;                 // Sequenz muss (noch) gezeigt werden -> keine Eingaben
uint32_t simonShowAt;                  // ab hier zeigt simonUpdate() die Sequenz
uint32_t simonInputAt;                 // Beginn der Eingabephase / letzter richtiger Druck
uint8_t  simonCol[MAX_NODES + 1];      // feste Farbe je Teilnehmer
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
uint8_t  tugTeam[MAX_NODES + 1];       // 0 = spielt nicht mit, 1 = ROT, 2 = BLAU

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
uint8_t  huntDisplay;                  // Anzeige-Node (erster Teilnehmer)
bool     huntShowTarget;               // Anzeige-Node zeigt gerade die Zielfarbe
uint32_t huntRoundEnd;                 // Runden-Timeout

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
// Web-Aktionen: Handler antworten sofort und setzen nur pendingAction,
// ausgefuehrt wird in loop() -> processPending() (nie im Web-Handler!)
// ─────────────────────────────────────────────────────────────
#define ACT_NONE      0
#define ACT_START     1   // pendingArg = Spielmodus
#define ACT_STOP      2
#define ACT_RECONNECT 3   // alle Nodes neu anmelden lassen (IDs bleiben)
#define ACT_FORGET    4   // Node-Liste (MAC-Tabelle) loeschen
#define ACT_IDENTIFY  5   // pendingArg = Node-ID
#define ACT_ROLLOUT   6   // pendingArg = 1: alle neu flashen, 0: nur veraltete
#define ACT_CANCEL    7   // Rollout nach aktuellem Node beenden
#define ACT_FWDELETE  8   // gespeicherte Node-Firmware loeschen
uint8_t  pendingAction = ACT_NONE;
uint8_t  pendingArg    = 0;

// PKT_RESET per Broadcast: 3x im Abstand von ~50 ms (nicht blockierend)
uint8_t  rstBcastLeft = 0;
uint32_t rstBcastNext = 0;

// kleine IP-Tabellen: Reset-Begrenzung (1x/s je IP) und Nodes mit alter Firmware
struct IpNote { uint32_t ip; uint32_t t; uint8_t info; };
IpNote   resetNotes[8];
IpNote   legacyNodes[8];     // info = gemeldete Protokollversion (1.x: 0)

uint32_t restartAt         = 0;   // geplanter Neustart (nach Master-Update), 0 = keiner
uint8_t  masterResetReason = 0;
bool     wdtOn             = false;
uint8_t  apChannel         = 1;
uint8_t  apMaxConn         = AP_MAX_CONN;   // tatsaechlich verwendetes Limit (1..10)
bool     appValidMarked    = false;
uint32_t firstLoopAt       = 0;
bool     rolloutActive     = false;   // Node-Firmware wird gerade verteilt
String   fwMsg;                       // letzte Meldung Upload/Rollout fuer das Web-UI

#ifdef ESP32
Preferences prefs;
bool     fsOk = false;
uint32_t fsTotal = 0, fsUsed = 0;

// Gespeicherte Node-Firmware (/node.bin + /node.meta im LittleFS)
struct NodeFwInfo {
  bool     present;
  uint8_t  major, minor, patch, proto;
  uint32_t size;
  char     version[16];
  char     md5[33];
};
NodeFwInfo nodeFw;

// Upload-Zustand (Master-Update bzw. Node-Firmware). Der WebServer bearbeitet
// immer nur EINE Anfrage gleichzeitig -> ein gemeinsamer Zustand genuegt.
#define UP_NODE   1
#define UP_MASTER 2
uint8_t    upKind    = 0;
bool       upStarted = false, upFail = false, upHdrOk = false, upDone = false;
uint16_t   upHdrLen  = 0;
uint32_t   upTotal   = 0;
uint8_t    upHdr[IMG_HDR_LEN];
CtfFwDesc  upDesc;
String     upMsg;
File       upFile;
MD5Builder upMd5;

// Rollout (Node-Firmware nacheinander an die Nodes verteilen)
#define RO_IDLE 0     // naechsten Node waehlen
#define RO_SEND 1     // PKT_OTA senden, auf OTA_STATUS STARTED warten
#define RO_WAIT 2     // Node laedt/flasht, auf Neustart mit neuer Version warten
#define RO_ERR_NOANSWER 20
#define RO_ERR_TIMEOUT  21
#define RO_ERR_OFFLINE  22
#define RO_ERR_VERSION  23
bool     roCancel = false;
uint8_t  roQueue[MAX_NODES];
uint8_t  roQLen = 0, roQPos = 0;
uint8_t  roCur = 0, roPhase = RO_IDLE, roTries = 0, roRegAt = 0;
uint32_t roLastSend = 0, roPhaseAt = 0;
uint8_t  roState[MAX_NODES + 1];   // 0=-, 1=wartet, 2=laeuft, 3=fertig, 4=Fehler
uint8_t  roErr[MAX_NODES + 1];     // OTA_ERR_* bzw. RO_ERR_*

// Firmware-Kennung (wird beim Hochladen geprueft) + automatischer Rollback:
// eine neue Firmware gilt erst nach 20 s stabilem Lauf als gut.
extern "C" const CtfFwDesc ctf_fw_desc __attribute__((section(".rodata_custom_desc"), used)) =
  { CTF_DESC_MAGIC, CTF_ROLE_MASTER, PROTO_VERSION, CTF_FW_MAJOR, CTF_FW_MINOR, CTF_FW_PATCH, {0, 0, 0}, CTF_FW_VERSION };
extern "C" bool verifyRollbackLater() { return true; }
// WICHTIG: ctf_fw_desc muss im Code BENUTZT werden (MASTER_FW_STR), sonst entfernt
// der Linker (--gc-sections) die Kennung und Updates koennen sie nicht pruefen.
#define MASTER_FW_STR ctf_fw_desc.version
#else
#define MASTER_FW_STR CTF_FW_VERSION
#endif

// ─────────────────────────────────────────────────────────────
// Netzwerk-Helfer
// ─────────────────────────────────────────────────────────────
// Liefert false, wenn das Paket nicht gesendet werden konnte.
bool sendPkt(IPAddress ip, uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  Packet p; p.type=type; p.nodeId=nid;
  p.data[0]=d0;p.data[1]=d1;p.data[2]=d2;
  p.data[3]=d3;p.data[4]=d4;p.data[5]=d5;
#ifdef ESP32
  return audp.writeTo((const uint8_t*)&p, sizeof(p), ip, UDP_PORT) == sizeof(p);
#else
  udp.beginPacket(ip, UDP_PORT);
  udp.write((uint8_t*)&p, sizeof(p));
  return udp.endPacket() == 1;
#endif
}

void wdtFeed() {
#ifdef ESP32
  if (wdtOn) esp_task_wdt_reset();
#endif
}

bool nodeValid(uint8_t id) { return id >= 1 && id <= MAX_NODES && nodes[id].known; }

#ifdef ESP32
// AsyncUDP-Callback (laeuft im async_udp-Task!): nur kopieren, nichts verarbeiten.
void onUdpPacket(AsyncUDPPacket& pk) {
  if (pk.length() != sizeof(Packet) || !rxQueue) return;
  RxItem it;
  memcpy(&it.p, pk.data(), sizeof(Packet));
  it.ip = (uint32_t)pk.remoteIP();
  if (xQueueSend(rxQueue, &it, 0) != pdTRUE) rxDrops = rxDrops + 1;   // nur dieser Task schreibt
}
#endif

// ─────────────────────────────────────────────────────────────
// Zuverlaessige LED-Steuerung: der Master merkt sich je Node den
// Soll-Zustand und wiederholt ihn (40/80/160/300/300/300 ms, dann 1 s),
// bis die Node die LED-Seq per PKT_STATUS quittiert.
// ─────────────────────────────────────────────────────────────
uint8_t nextSeq(uint8_t s) { return (s >= 255) ? 1 : (uint8_t)(s + 1); }

uint32_t ledBackoff(uint8_t tries) {
  static const uint16_t BK[6] = {40, 80, 160, 300, 300, 300};
  return (tries >= 1 && tries <= 6) ? BK[tries - 1] : 1000UL;
}

void ledSend(uint8_t id) {
  NodeInfo& n = nodes[id];
  bool ok = sendPkt(IPAddress(n.ip), n.ledType, id, n.ledA, n.ledB, n.ledC, 0, 0, n.ledSeq);
  n.ledSentAt = millis();
  if (!ok)                 n.ledTries = 1;    // Senden fehlgeschlagen -> bald (40 ms) erneut
  else if (n.ledTries < 250) n.ledTries++;
}

// Neuen Soll-Zustand setzen (fuer alle BEKANNTEN Nodes, auch offline –
// er wird dann nach dem Wiederverbinden gesendet).
void ledSet(uint8_t id, uint8_t type, uint8_t a, uint8_t b, uint8_t c) {
  if (!nodeValid(id)) return;
  NodeInfo& n = nodes[id];
  // Unveraendert -> nichts tun (Ausnahme: FLASH soll erneut aufblitzen)
  if (n.ledSeq != 0 && n.ledType == type && n.ledA == a && n.ledB == b && n.ledC == c &&
      !(type == PKT_SET_LED && b == ANIM_FLASH)) return;
  n.ledType = type; n.ledA = a; n.ledB = b; n.ledC = c;
  n.ledSeq   = nextSeq(n.ledSeq);
  n.ledTries = 0;
  n.ledSentAt = 0;
  if (n.active && n.ip) ledSend(id);
}

// Wiederholt nicht quittierte Soll-Zustaende (loop + yieldDelay)
void ledService() {
  uint32_t now = millis();
  for (uint8_t i = 1; i <= nodeCount; i++) {
    NodeInfo& n = nodes[i];
    if (!n.known || !n.active || !n.ip || n.ledSeq == 0 || n.ledAcked == n.ledSeq) continue;
    if (n.ledTries == 0 || (uint32_t)(now - n.ledSentAt) >= ledBackoff(n.ledTries)) ledSend(i);
  }
}

void setLED(uint8_t id, uint8_t color, uint8_t anim)                 { ledSet(id, PKT_SET_LED,   color,  anim,   0); }
void setBar(uint8_t id, uint8_t color, uint8_t count, uint8_t bg)    { ledSet(id, PKT_SET_BAR,   color,  count,  bg); }
void setSplit(uint8_t id, uint8_t colorA, uint8_t countA, uint8_t colorB) { ledSet(id, PKT_SET_SPLIT, colorA, countA, colorB); }

void allLED(uint8_t color, uint8_t anim) {
  for (uint8_t i=1;i<=nodeCount;i++) if (nodes[i].known) setLED(i,color,anim);
}
void allBar(uint8_t color, uint8_t count, uint8_t bg) {
  for (uint8_t i=1;i<=nodeCount;i++) if (nodes[i].known) setBar(i,color,count,bg);
}
void allSplit(uint8_t colorA, uint8_t countA, uint8_t colorB) {
  for (uint8_t i=1;i<=nodeCount;i++) if (nodes[i].known) setSplit(i,colorA,countA,colorB);
}

// ─────────────────────────────────────────────────────────────
// Empfang: AsyncUDP-Queue leeren, jedes Paket mit processPacket()
// ─────────────────────────────────────────────────────────────
void netPoll(bool inAnim) {
#ifdef ESP32
  if (!rxQueue) return;
  RxItem it;
  for (uint8_t k = 0; k < 64 && xQueueReceive(rxQueue, &it, 0) == pdTRUE; k++)
    processPacket(it.p, IPAddress(it.ip), inAnim);
#else
  int sz;
  for (uint8_t k = 0; k < 32 && (sz = udp.parsePacket()) > 0; k++) {
    if (sz != (int)sizeof(Packet)) continue;   // falsche Groesse: parsePacket() verwirft den Rest
    Packet p; udp.read((uint8_t*)&p, sizeof(p));
    processPacket(p, udp.remoteIP(), inAnim);
  }
#endif
}

// Nicht-blockierendes Warten: Web, OTA, Empfang und LED-Wiederholungen laufen weiter.
// Liefert false, sobald eine Web-Aktion (Start/Stop/...) ansteht -> Aufrufer soll abbrechen.
bool yieldDelay(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    if (pendingAction != ACT_NONE) return false;
    ArduinoOTA.handle();
    webServer.handleClient();
    netPoll(true);
    ledService();
    wdtFeed();
    if (pendingAction != ACT_NONE) return false;
    yield();
  }
  return true;
}

// ─────────────────────────────────────────────────────────────
// Registry: Speichern (NVS), Registrierung, Pakete
// ─────────────────────────────────────────────────────────────
void saveMacs() {
#ifdef ESP32
  uint8_t buf[MAX_NODES * 6];
  memset(buf, 0, sizeof(buf));
  for (uint8_t i = 1; i <= MAX_NODES; i++) if (nodes[i].known) memcpy(buf + (i - 1) * 6, nodes[i].mac, 6);
  if (prefs.begin("ctf", false)) { prefs.putBytes("macs", buf, sizeof(buf)); prefs.end(); }
#endif
}

void loadMacs() {
#ifdef ESP32
  uint8_t buf[MAX_NODES * 6];
  memset(buf, 0, sizeof(buf));
  if (!prefs.begin("ctf", true)) return;     // beim allerersten Start gibt es noch nichts
  size_t len = prefs.getBytesLength("macs");
  if (len > sizeof(buf)) len = sizeof(buf);
  if (len > 0) prefs.getBytes("macs", buf, len);
  prefs.end();
  for (uint8_t i = 1; i <= MAX_NODES; i++) {
    const uint8_t* m = buf + (i - 1) * 6;
    bool zero = true;
    for (uint8_t k = 0; k < 6; k++) if (m[k]) zero = false;
    if (zero) continue;
    memcpy(nodes[i].mac, m, 6);
    nodes[i].known = true;
    if (i > nodeCount) nodeCount = i;
  }
#endif
}

void saveNames() {
#ifdef ESP32
  uint8_t buf[MAX_NODES * 20];
  for (uint8_t i = 1; i <= MAX_NODES; i++) {
    memcpy(buf + (i - 1) * 20, players[i].name, 20);
    buf[(i - 1) * 20 + 19] = 0;
  }
  if (prefs.begin("ctf", false)) { prefs.putBytes("names", buf, sizeof(buf)); prefs.end(); }
#endif
}

void loadNames() {
#ifdef ESP32
  uint8_t buf[MAX_NODES * 20];
  memset(buf, 0, sizeof(buf));
  if (!prefs.begin("ctf", true)) return;
  size_t len = prefs.getBytesLength("names");
  if (len > sizeof(buf)) len = sizeof(buf);
  if (len > 0) prefs.getBytes("names", buf, len);
  prefs.end();
  for (uint8_t i = 1; i <= MAX_NODES; i++) {
    memcpy(players[i].name, buf + (i - 1) * 20, 20);
    players[i].name[19] = 0;
  }
#endif
}

uint8_t findMac(const uint8_t* mac) {
  for (uint8_t i = 1; i <= MAX_NODES; i++)
    if (nodes[i].known && memcmp(nodes[i].mac, mac, 6) == 0) return i;
  return 0;
}

// Antwortet einer unbekannten/verwirrten Node mit PKT_RESET (max. 1x pro Sekunde je IP)
void maybeSendReset(uint32_t ip) {
  if (!ip) return;
  uint32_t now = millis();
  uint8_t slot = 0; uint32_t bestAge = 0; bool found = false;
  for (uint8_t k = 0; k < 8; k++) {
    if (resetNotes[k].ip == ip) {
      if ((uint32_t)(now - resetNotes[k].t) < 1000UL) return;
      slot = k; found = true; break;
    }
  }
  if (!found) {
    for (uint8_t k = 0; k < 8; k++) {
      uint32_t age = resetNotes[k].ip ? (uint32_t)(now - resetNotes[k].t) : 0xFFFFFFFFUL;
      if (age >= bestAge) { bestAge = age; slot = k; }
    }
  }
  resetNotes[slot].ip = ip; resetNotes[slot].t = now;
  sendPkt(IPAddress(ip), PKT_RESET, 0xFF);
}

// Node mit alter Firmware (1.x) meldet sich -> nur fuer die Warnung im Web-UI merken
void legacyNote(uint32_t ip, uint8_t proto) {
  uint32_t now = millis();
  for (uint8_t k = 0; k < 8; k++)
    if (legacyNodes[k].ip == ip) { legacyNodes[k].t = now; legacyNodes[k].info = proto; return; }
  uint8_t slot = 0; uint32_t bestAge = 0;
  for (uint8_t k = 0; k < 8; k++) {
    uint32_t age = legacyNodes[k].ip ? (uint32_t)(now - legacyNodes[k].t) : 0xFFFFFFFFUL;
    if (age >= bestAge) { bestAge = age; slot = k; }
  }
  legacyNodes[slot].ip = ip; legacyNodes[slot].t = now; legacyNodes[slot].info = proto;
  Serial.printf("[REG] Node mit alter Firmware (Protokoll %u) von %s – bitte einmal per USB neu flashen!\n",
    proto, IPAddress(ip).toString().c_str());
}

bool senderOk(uint8_t id, uint32_t ip) {
  return ip != 0 && nodeValid(id) && nodes[id].ip == ip;
}

void handleRegister(const uint8_t* mac, uint32_t ip) {
  bool zero = true, ff = true;
  for (uint8_t k = 0; k < 6; k++) { if (mac[k] != 0) zero = false; if (mac[k] != 0xFF) ff = false; }
  if (zero || ff) return;
  uint32_t now = millis();
  uint8_t id = findMac(mac);
  bool newSlot = false;
  if (id == 0) {
    // Neue Node: kleinste freie ID ...
    for (uint8_t i = 1; i <= MAX_NODES; i++) if (!nodes[i].known) { id = i; break; }
    // ... sonst den am laengsten inaktiven Slot wiederverwenden
    if (id == 0) {
      uint32_t oldest = 0;
      for (uint8_t i = 1; i <= MAX_NODES; i++) {
        if (!nodes[i].known || nodes[i].active) continue;
        uint32_t age = (uint32_t)(now - nodes[i].lastSeen);
        if (id == 0 || age > oldest) { id = i; oldest = age; }
      }
    }
    if (id == 0) {
      static uint32_t lastFullLog = 0;
      if ((uint32_t)(now - lastFullLog) > 5000UL) {
        lastFullLog = now;
        Serial.printf("[REG] Kein freier Platz (MAX_NODES=%u) – Node %s ignoriert\n", MAX_NODES, IPAddress(ip).toString().c_str());
      }
      return;
    }
    if (nodes[id].known) Serial.printf("[REG] ID %u wird neu vergeben (alte Node lange offline)\n", id);
    memset(&nodes[id], 0, sizeof(NodeInfo));
    memcpy(nodes[id].mac, mac, 6);
    nodes[id].known = true;
    inGame[id] = false;
    if (id > nodeCount) nodeCount = id;
    if (players[id].name[0] == 0) snprintf(players[id].name, 20, "Spieler %u", id);
    saveMacs();
    newSlot = true;
  }
  NodeInfo& n = nodes[id];
  // Andere Slots mit derselben IP sind veraltet (DHCP hat die IP neu vergeben)
  for (uint8_t j = 1; j <= nodeCount; j++) {
    if (j != id && nodes[j].known && nodes[j].ip == ip) {
      nodes[j].ip = 0; nodes[j].active = false;
      Serial.printf("[REG] Node %u: IP an Node %u vergeben -> offline\n", j, id);
    }
  }
  // Diese IP ist jetzt eine v2-Node -> evtl. Warnung "alte Firmware" entfernen
  for (uint8_t k = 0; k < 8; k++) if (legacyNodes[k].ip == ip) legacyNodes[k].ip = 0;
  n.ip         = ip;
  n.active     = true;
  n.lastSeen   = now;
  n.statusSeen = false;
  n.regCount++;
  n.ledAcked   = 0;      // Node hat nach der Registrierung nichts angewendet -> Soll-Zustand neu senden
  n.ledTries   = 0;
  n.ledSentAt  = 0;
  IPAddress rip(ip);
  for (uint8_t k = 0; k < 2; k++)   // doppelt gegen Paketverlust
    sendPkt(rip, PKT_ACK, id, id, PROTO_VERSION, mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[REG] Node %u %s (%s, MAC ..%02X:%02X)\n", id, newSlot ? "neu" : "angemeldet",
    rip.toString().c_str(), mac[4], mac[5]);
  // Spiel laeuft und die Node spielt nicht mit (war beim Start nicht da) -> neutraler
  // Zustand. Teilnehmer bekommen ihren Soll-Zustand automatisch neu (ledAcked = 0).
  if (gameMode != GAME_IDLE && !inGame[id])
    ledSet(id, PKT_SET_LED, (gameMode == GAME_CTF) ? COL_WHITE : COL_OFF, ANIM_SOLID, 0);
}

// EINE Verarbeitungsfunktion fuer alle Pakete (aus loop und yieldDelay).
// inAnim = true: aufgerufen waehrend einer Animation (yieldDelay) -> Tasten
// werden quittiert, aber nicht an das Spiel weitergegeben.
void processPacket(const Packet& p, IPAddress rip, bool inAnim) {
  uint32_t ip = (uint32_t)rip;
  if (!ip) return;
  uint32_t now = millis();
  uint8_t id = p.nodeId;
  switch (p.type) {
    case PKT_REGISTER:
      if (p.nodeId != PROTO_VERSION) { legacyNote(ip, p.nodeId); break; }   // alte Firmware: kein ACK
      handleRegister(p.data, ip);
      break;

    case PKT_STATUS: {
      // Unbekannt / falsche IP / keine v2-Firmware -> Node soll sich neu anmelden
      if (!senderOk(id, ip) || p.data[2] == 0) { maybeSendReset(ip); break; }
      NodeInfo& n = nodes[id];
      if (!n.active) Serial.printf("[NODE] Node %u wieder online\n", id);
      n.active      = true;
      n.lastSeen    = now;
      n.rssi        = (int8_t)p.data[1];
      n.fw[0] = p.data[2]; n.fw[1] = p.data[3]; n.fw[2] = p.data[4];
      n.flags       = p.data[5] & NODE_FLAG_MASK;
      n.resetReason = p.data[5] >> 4;
      n.statusSeen  = true;
      n.ledAcked    = p.data[0];
      sendPkt(rip, PKT_PONG, id, n.ledSeq);
      break;
    }

    case PKT_BUTTON: {
      if (!senderOk(id, ip)) { maybeSendReset(ip); break; }
      NodeInfo& n = nodes[id];
      if (!n.active) Serial.printf("[NODE] Node %u wieder online\n", id);
      n.active = true; n.lastSeen = now;
      uint8_t seq = p.data[0], nonce = p.data[1];
      sendPkt(rip, PKT_BTN_ACK, id, seq, nonce);     // IMMER quittieren (auch Wiederholungen)
      if (nonce != n.btnNonce) n.btnNonce = nonce;   // neue Sitzung der Node -> verarbeiten
      else if (seq == n.lastBtnSeq) break;            // Wiederholung desselben Drucks
      n.lastBtnSeq = seq;
      if (inAnim || rolloutActive || pendingAction != ACT_NONE) {
        Serial.printf("[BTN] Node %u (ignoriert: %s)\n", id,
          inAnim ? "Animation" : (rolloutActive ? "Firmware-Update" : "Aktion laeuft"));
        break;
      }
      Serial.printf("[BTN] Node %u\n", id);
      gameOnButton(id);
      break;
    }

    case PKT_OTA_STATUS: {
      if (!senderOk(id, ip)) break;
      uint8_t st = p.data[0];
      if (st < OTA_ST_STARTED || st > OTA_ST_SUCCESS) break;
      NodeInfo& n = nodes[id];
      n.active = true; n.lastSeen = now;
      if (st != n.otaState || p.data[1] != n.otaPct) {
        if      (st == OTA_ST_STARTED)  Serial.printf("[OTA] Node %u: Download gestartet\n", id);
        else if (st == OTA_ST_PROGRESS) Serial.printf("[OTA] Node %u: %u %%\n", id, p.data[1]);
        else if (st == OTA_ST_SUCCESS)  Serial.printf("[OTA] Node %u: geflasht, startet neu\n", id);
        else                            Serial.printf("[OTA] Node %u: FEHLER %u\n", id, p.data[2]);
      }
      n.otaState = st; n.otaPct = p.data[1]; n.otaErr = p.data[2];
      break;
    }

    default: break;
  }
}

// Node-Timeout: jede Sekunde pruefen
void nodeTimeoutCheck() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if ((uint32_t)(now - last) < 1000UL) return;
  last = now;
  for (uint8_t i = 1; i <= nodeCount; i++) {
    NodeInfo& n = nodes[i];
    if (!n.known || !n.active) continue;
    if ((uint32_t)(now - n.lastSeen) >= NODE_TIMEOUT_MS) {
      n.active = false;
      Serial.printf("[WARN] Node %u offline\n", i);
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Spiele – gemeinsame Regeln
//  - Teilnehmer = Nodes, die beim Spielstart aktiv waren (inGame[], gesetzt in
//    startGameNow()). Ziele, Sequenzen, Paare, Minen, Thron, Teams, Bombe und
//    Anzeige-Node nur unter Teilnehmern. Tasten anderer Nodes werden ignoriert
//    (gameOnButton), Spiel-LEDs nur auf Teilnehmern (partLED/partBar/partSplit).
//  - yieldDelay() liefert false, sobald eine Web-Aktion wartet. Dann wird NICHT
//    weiter gewartet: das Spiel wird ohne Pause in einen gueltigen Zustand gebracht
//    (z.B. Spielfeld zeigen) und die Funktion kehrt sofort zurueck (false an den
//    Aufrufer). Die Aktion selbst laeuft danach in loop() -> processPending().
//  - Zeiten immer ueberlauf-sicher vergleichen: (long)(now - t) >= 0 bzw.
//    (uint32_t)(now - start) >= dauer. Zeitpunkte mit "0 = aus" per tAfter().
// ─────────────────────────────────────────────────────────────
#ifndef CTF_FEEDBACK_MS
  #define CTF_FEEDBACK_MS        700UL     // CTF: oranges "gesperrt"-Blinken so lange stehen lassen
#endif
#ifndef SIMON_INPUT_TIMEOUT_MS
  #define SIMON_INPUT_TIMEOUT_MS 20000UL   // Simon: so lange ohne richtigen Druck -> Spielende
#endif
#ifndef HUNT_ROUND_TIMEOUT_MS
  #define HUNT_ROUND_TIMEOUT_MS  12000UL   // Farbjagd: Runde ohne Treffer -> naechste Runde ohne Punkte
#endif

// Zeitpunkt "jetzt + ms", nie 0 (0 bedeutet bei ctfLockUntil, memHideAt, knockGraceAt, ... "aus")
uint32_t tAfter(uint32_t ms) { return (millis() + ms) | 1UL; }

// Mindestanzahl aktiver Nodes je Spiel
uint8_t minNodesFor(uint8_t m) { return (m == GAME_BOMB || m == GAME_MINESWEEPER) ? 3 : 2; }

// Zurzeit verbundene Nodes (Pruefung vor dem Start)
uint8_t onlineCount() {
  uint8_t c = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (nodes[i].known && nodes[i].active) c++;
  return c;
}

// ── Teilnehmer (= beim Spielstart aktive Nodes) ──
bool isPart(uint8_t id) { return id >= 1 && id <= MAX_NODES && inGame[id]; }

// Anzahl Teilnehmer
uint8_t activeCount() {
  uint8_t c = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) c++;
  return c;
}

// Teilnehmer-IDs aufsteigend nach out[] (Platz fuer MAX_NODES Eintraege); liefert die Anzahl
uint8_t collectActive(uint8_t* out) {
  uint8_t c = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) out[c++] = i;
  return c;
}

// Zufaelliger Teilnehmer != exclude. Gibt es keinen anderen: exclude selbst (falls Teilnehmer), sonst 0.
uint8_t randomParticipant(uint8_t exclude) {
  uint8_t ids[MAX_NODES]; uint8_t c = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i != exclude) ids[c++] = i;
  if (c == 0) return isPart(exclude) ? exclude : 0;
  return ids[random(0, c)];
}

void shuffleIds(uint8_t* a, uint8_t n) {
  for (int i=(int)n-1;i>0;i--) { int j=random(0,i+1); uint8_t t=a[i]; a[i]=a[j]; a[j]=t; }
}

// Spiel-LEDs nur auf Teilnehmern (Nicht-Teilnehmer bleiben im Ruhezustand)
void partLED(uint8_t color, uint8_t anim) {
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) setLED(i,color,anim);
}
void partBar(uint8_t color, uint8_t count, uint8_t bg) {
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) setBar(i,color,count,bg);
}
void partSplit(uint8_t colorA, uint8_t countA, uint8_t colorB) {
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) setSplit(i,colorA,countA,colorB);
}

// Zufall: der ESP32 nutzt den Hardware-Zufallsgenerator (randomSeed() wuerde auf den
// schwaecheren Pseudo-Zufall umschalten) – nur der ESP8266 braucht einen Startwert.
void gameSeed() {
#ifndef ESP32
  randomSeed(micros() | 1UL);
#endif
}

// Spielende-Blinken (alle Teilnehmer); nach 10 s leuchten die Nodes solid weiter
void gameOverBlink(uint8_t color, uint8_t anim = ANIM_BLINK_FAST) {
  partLED(color, anim);
  idleBlinkUntil = tAfter(10000UL);
}

// Spielende mit Sieger: Sieger blinkt, alle anderen Teilnehmer solid (winnerId 0 = kein Sieger).
// Nach 10s stoppt das Blinken automatisch (Nodes leuchten solid weiter).
void finishGame(uint8_t winnerId, uint8_t winColor, uint8_t loserColor) {
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    if (!inGame[i]) continue;
    if (i==winnerId) setLED(i, winColor, ANIM_BLINK_FAST);
    else             setLED(i, loserColor, ANIM_SOLID);
  }
  idleBlinkUntil = tAfter(10000UL);
  gameMode = GAME_IDLE;
}

// Gibt die Wartezeit (ms) zurueck, die nach einer Runde vergehen muss,
// bevor die naechste startet. Besteht aus cfgRoundDelay + kleinem Zufallspuffer
// (0.5-1.5s) damit alle Spieler wirklich gleichzeitig starten.
uint32_t roundPauseMs() {
  return (uint32_t)cfgRoundDelay * 1000UL + random(500, 1500);
}

// Rundenabstand (Reaktion / Whack-a-Mole / Knockout): zuerst ~1 s das Ergebnis
// (gruen/rot/orange bzw. Leben) stehen lassen, danach zeigt ein weisser Balken von
// voll nach leer die Restzeit. Nicht-blockierend – updateRoundBar() pollt.
uint32_t roundBarStartAt = 0;   // ab hier Balken (vorher Ergebnis zeigen)
uint32_t roundBarEndAt   = 0;   // 0 = kein Balken aktiv
uint8_t  roundBarLast    = 255;

// Nodes, die den Pausen-Balken zeigen (Knockout: nur noch nicht ausgeschiedene)
bool roundBarNode(uint8_t i) {
  return inGame[i] && (gameMode != GAME_KNOCKOUT || knockActive[i]);
}

void startRoundBar(uint32_t pauseMs) {
  uint32_t hold = (pauseMs >= 2000UL) ? 1000UL : pauseMs;   // kurze Pause: nur Ergebnis zeigen
  roundBarStartAt = millis() + hold;
  roundBarEndAt   = tAfter(pauseMs);
  roundBarLast    = 255;
}

void updateRoundBar() {
  if (roundBarEndAt == 0) return;
  uint32_t now = millis();
  if ((long)(now - roundBarEndAt) >= 0) { roundBarEndAt = 0; return; }
  if ((long)(now - roundBarStartAt) < 0) return;            // Ergebnis noch zeigen
  uint32_t total   = roundBarEndAt - roundBarStartAt;
  uint32_t elapsed = now - roundBarStartAt;
  if (total == 0 || elapsed >= total) return;
  uint8_t lit = (uint8_t)(((total - elapsed) * BAR_LEDS + total - 1) / total);   // 8 .. 1
  if (lit > BAR_LEDS) lit = BAR_LEDS;
  if (lit == roundBarLast) return;
  roundBarLast = lit;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (roundBarNode(i)) setBar(i, COL_WHITE, lit, COL_OFF);
}

// ─────────────────────────────────────────────────────────────
// CTF
// ─────────────────────────────────────────────────────────────
static const uint8_t TEAM_COLORS[] = {COL_WHITE,COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW};
static const char*   TEAM_NAMES[]  = {"Neutral","ROT","BLAU","GRUEN","GELB"};
#define CTF_SHOW_SOLID 254   // ctfBarLast: Node zeigt die Teamfarbe solid (nicht gesperrt)

bool ctfStart() {
  uint8_t pc = activeCount();
  if (pc < minNodesFor(GAME_CTF)) { Serial.println("[CTF] Mindestens 2 aktive Nodes."); return false; }
  if (cfgTeams < 2) cfgTeams = 2;
  if (cfgTeams > 4) cfgTeams = 4;
  gameMode=GAME_CTF; gameEndTime=tAfter((uint32_t)cfgDuration*1000UL);
  ctfLockMs = (uint32_t)cfgRoundDelay * 1000UL;
  for (uint8_t i=0;i<=MAX_NODES;i++) {
    ctfTeam[i]=0; ctfLockUntil[i]=0; ctfFeedbackUntil[i]=0; ctfBarLast[i]=CTF_SHOW_SOLID;
  }
  partLED(COL_WHITE,ANIM_SOLID);
  Serial.printf("[CTF] Start: %u Nodes %us %u Teams Sperre:%us\n",
    pc,cfgDuration,cfgTeams,cfgRoundDelay);
  return true;
}

void ctfOnButton(uint8_t id) {
  uint32_t now = millis();
  // Sperre pruefen: Node wurde gerade eingenommen und ist noch gesperrt
  if (ctfLockUntil[id] != 0 && (long)(ctfLockUntil[id] - now) > 0) {
    Serial.printf("[CTF] Node %u gesperrt – noch %lus\n", id, (unsigned long)((ctfLockUntil[id] - now + 999) / 1000));
    // Oranges Blinken = "noch nicht"; ctfUpdate() laesst es ~700 ms stehen und zeichnet dann den Balken neu
    setLED(id, COL_ORANGE, ANIM_BLINK_FAST);
    ctfFeedbackUntil[id] = tAfter(CTF_FEEDBACK_MS);
    ctfBarLast[id] = 255;
    return;
  }
  ctfTeam[id]=(ctfTeam[id]%cfgTeams)+1;
  ctfFeedbackUntil[id] = 0;
  setLED(id, TEAM_COLORS[ctfTeam[id]], ANIM_FLASH);
  if (ctfLockMs > 0) {
    ctfLockUntil[id] = tAfter(ctfLockMs);
    ctfBarLast[id]   = 255;              // sofort vollen Sperr-Balken zeigen
  } else {
    ctfBarLast[id]   = CTF_SHOW_SOLID;   // FLASH endet ohnehin solid in der Teamfarbe
  }
  Serial.printf("[CTF] Node%u → %s%s\n", id, TEAM_NAMES[ctfTeam[id]],
    ctfLockMs>0 ? " (gesperrt)" : "");
}

void ctfUpdate() {
  uint32_t now = millis();

  // Je Teilnehmer: Sperre ablaufen lassen, "gesperrt"-Feedback halten,
  // sonst schrumpfender Balken (gesperrt) bzw. Teamfarbe solid. Gesendet wird nur bei Aenderung.
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    if (!inGame[i]) continue;
    if (ctfLockUntil[i]!=0 && (long)(now - ctfLockUntil[i]) >= 0) {
      ctfLockUntil[i]=0; ctfBarLast[i]=255;
      Serial.printf("[CTF] Node %u entsperrt\n", i);
    }
    if (ctfFeedbackUntil[i]!=0) {
      if ((long)(now - ctfFeedbackUntil[i]) < 0) continue;   // orange noch stehen lassen
      ctfFeedbackUntil[i]=0; ctfBarLast[i]=255;              // danach neu zeichnen
    }
    uint8_t want = CTF_SHOW_SOLID;
    if (ctfLockUntil[i]!=0 && ctfLockMs>0) {
      uint32_t left = ctfLockUntil[i] - now;                 // > 0 (oben geprueft)
      uint32_t lit  = (left * BAR_LEDS + ctfLockMs - 1) / ctfLockMs;
      want = (uint8_t)(lit > BAR_LEDS ? BAR_LEDS : lit);
    }
    if (want == ctfBarLast[i]) continue;   // keine Aenderung -> nichts senden
    ctfBarLast[i] = want;
    if (want == CTF_SHOW_SOLID) setLED(i, TEAM_COLORS[ctfTeam[i]], ANIM_SOLID);
    else                        setBar(i, TEAM_COLORS[ctfTeam[i]], want, COL_OFF);
  }

  if ((long)(now-gameEndTime)<0) return;
  uint8_t score[5]={0};
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && ctfTeam[i]>0 && ctfTeam[i]<=4) score[ctfTeam[i]]++;
  uint8_t winner=1; bool tie=false;
  for (uint8_t t=2;t<=cfgTeams;t++) {
    if      (score[t]>score[winner])  { winner=t; tie=false; }
    else if (score[t]==score[winner]) tie=true;
  }
  for (uint8_t t=1;t<=cfgTeams;t++) Serial.printf("[CTF] %s: %u\n",TEAM_NAMES[t],score[t]);
  if (tie || score[winner]==0) {
    Serial.println("[CTF] Unentschieden!");
    addHistory(GAME_CTF, "Unentschieden", score[winner]);
    gameOverBlink(COL_WHITE, ANIM_BLINK_SLOW);
  } else {
    addHistory(GAME_CTF, TEAM_NAMES[winner], score[winner]);
    gameOverBlink(TEAM_COLORS[winner]);
  }
  gameMode=GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// Memory
// ─────────────────────────────────────────────────────────────
static const uint8_t PAIR_PALETTE[] = {COL_RED,COL_BLUE,COL_GREEN,COL_YELLOW,COL_PURPLE,COL_CYAN,COL_ORANGE};

bool memStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_MEMORY)) { Serial.println("[MEM] Mindestens 2 aktive Nodes."); return false; }
  gameMode=GAME_MEMORY;
  memTotalPairs=pc/2;
  if (memTotalPairs>7) memTotalPairs=7;   // Palette hat nur 7 Farben -> sonst nicht unterscheidbar
  uint8_t n=memTotalPairs*2;              // so viele Teilnehmer spielen mit, der Rest bleibt aus
  memFoundPairs=0;
  memPending[0]=memPending[1]=-1; memHideAt=0;
  for (uint8_t i=0;i<=MAX_NODES;i++) { memColor[i]=COL_OFF; memMatched[i]=false; }   // COL_OFF = spielt nicht mit
  shuffleIds(ids, pc);                    // zufaellige Paare (und zufaellig, wer bei ungerader Zahl aussetzt)
  for (uint8_t k=0;k<n;k++) memColor[ids[k]]=PAIR_PALETTE[k/2];
  for (uint8_t k=0;k<pc;k++) setLED(ids[k],memColor[ids[k]],ANIM_SOLID);
  bool ok = yieldDelay(2000);             // Farben merken (bei Abbruch sofort verdecken)
  for (uint8_t k=0;k<n;k++) setLED(ids[k],COL_OFF,ANIM_SOLID);
  Serial.printf("[MEM] %u Paare\n",memTotalPairs);
  return ok;
}

void memOnButton(uint8_t id) {
  if (memColor[id]==COL_OFF||memMatched[id]||memHideAt!=0) return;
  if (memPending[0]==id||memPending[1]==id) return;
  setLED(id,memColor[id],ANIM_SOLID);
  if (memPending[0]<0) { memPending[0]=id; return; }
  memPending[1]=id;
  uint8_t a=(uint8_t)memPending[0];
  if (memColor[a]==memColor[id]) {
    memMatched[a]=memMatched[id]=true;
    setLED(a,COL_GREEN,ANIM_PULSE); setLED(id,COL_GREEN,ANIM_PULSE);
    memFoundPairs++; memPending[0]=memPending[1]=-1;
    if (memFoundPairs>=memTotalPairs) {
      Serial.println("[MEM] Alle Paare gefunden!");
      gameOverBlink(COL_GREEN); gameMode=GAME_IDLE; addHistory(GAME_MEMORY,"Geloest",memTotalPairs);
    }
  } else { memHideAt=tAfter(1500); }
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

// Entschaerf-Reihenfolge zeigen (blockierend, Tasten werden waehrenddessen ignoriert)
bool bombShowSequence() {
  for (uint8_t s=0;s<bombSeqLen;s++) {
    setLED(bombSeq[s],SEQ_COLORS[s%8],ANIM_SOLID);
    if (!yieldDelay(BOMB_STEP_MS)) return false;
    setLED(bombSeq[s],COL_OFF,ANIM_SOLID);
    if (!yieldDelay(200)) return false;
  }
  return true;
}

// Spielfeld: Bombe rot blinkend, alle anderen Teilnehmer weiss
void bombShowField() {
  for (uint8_t i=1;i<=MAX_NODES;i++)
    if (inGame[i]) setLED(i,(i==bombNode)?COL_RED:COL_WHITE,(i==bombNode)?ANIM_BLINK_FAST:ANIM_SOLID);
}

bool bombStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_BOMB)) { Serial.println("[BOMB] Mindestens 3 aktive Nodes."); return false; }
  gameMode=GAME_BOMB; bombStep=0; bombOver=false;
  gameEndTime=tAfter((uint32_t)cfgBombDur*1000UL);
  shuffleIds(ids, pc);
  bombNode=ids[0];                         // Bombe = zufaelliger Teilnehmer
  uint8_t len=cfgSeqLen;                   // Sequenz = verschiedene andere Teilnehmer
  if (len>pc-1) len=pc-1;
  if (len>(uint8_t)sizeof(bombSeq)) len=(uint8_t)sizeof(bombSeq);
  if (len<1) len=1;
  bombSeqLen=len;
  for (uint8_t i=0;i<len;i++) bombSeq[i]=ids[i+1];
  Serial.printf("[BOMB] Start: Bombe = Node %u, %u Schritte\n", bombNode, bombSeqLen);
  partLED(COL_OFF,ANIM_SOLID);
  bool ok = yieldDelay(200);
  setLED(bombNode,COL_RED,ANIM_BLINK_FAST);
  if (ok) ok = bombShowSequence();
  bombShowField();                         // auch nach Abbruch: Spielfeld steht
  return ok;
}

void bombOnButton(uint8_t id) {
  if (bombOver || bombStep>=bombSeqLen) return;
  if (id==bombNode) {                      // Bombe gedrueckt: Sequenz nochmal zeigen (blockierend)
    if (!bombShowSequence()) Serial.println("[BOMB] Anzeige abgebrochen (Web-Aktion)");
    bombShowField();
    return;
  }
  if (id==bombSeq[bombStep]) {
    setLED(id,COL_GREEN,ANIM_FLASH); bombStep++;
    if (bombStep==bombSeqLen) { bombOver=true; gameOverBlink(COL_GREEN,ANIM_BLINK_SLOW); gameMode=GAME_IDLE; Serial.println("[BOMB] ENTSCHAERFT!"); addHistory(GAME_BOMB,"Entschaerft",bombSeqLen); }
  } else { bombOver=true; gameOverBlink(COL_RED); gameMode=GAME_IDLE; Serial.println("[BOMB] BOOM!"); addHistory(GAME_BOMB,"Explodiert",bombStep); }
}

void bombUpdate() {
  if (bombOver||(long)(millis()-gameEndTime)<0) return;
  bombOver=true; gameOverBlink(COL_RED); gameMode=GAME_IDLE; Serial.println("[BOMB] ZEIT UM!"); addHistory(GAME_BOMB,"Zeit abgelaufen",bombStep);
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
    uint8_t winner=0;
    for (uint8_t i=1;i<=MAX_NODES;i++)
      if (inGame[i] && (winner==0 || players[i].points>players[winner].points)) winner=i;
    Serial.println("[REACT] === ENDE ===");
    for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i])
      Serial.printf("  %s: %u Pkt  Best: %lums\n", players[i].name, players[i].points, (unsigned long)players[i].bestMs);
    for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) reactInsertHighScore(i);
    if (winner && players[winner].points>0) addHistory(GAME_REACTION, players[winner].name, players[winner].points);
    else { winner=0; addHistory(GAME_REACTION, "Niemand", 0); }
    finishGame(winner, COL_GREEN, COL_RED);
    return;
  }
  uint8_t t = randomParticipant(reactLastTarget);
  if (t == 0) { Serial.println("[REACT] Keine Teilnehmer mehr."); gameMode=GAME_IDLE; return; }
  roundBarEndAt   = 0;
  reactTarget     = t;
  reactLastTarget = t;
  setLED(reactTarget, COL_YELLOW, ANIM_BLINK_FAST);
  reactLitAt      = millis();               // Zeitmessung ab Aufleuchten des Ziels
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i!=reactTarget) setLED(i, COL_OFF, ANIM_SOLID);
  reactRoundDone  = false;
  reactRound++;
  Serial.printf("[REACT] Runde %u/%u – Node %u leuchtet!\n", reactRound, cfgReactRounds, reactTarget);
}

bool reactStart() {
  if (activeCount() < minNodesFor(GAME_REACTION)) { Serial.println("[REACT] Mindestens 2 aktive Nodes."); return false; }
  gameMode       = GAME_REACTION;
  reactRound     = 0;
  reactTarget    = 0;
  reactLastTarget = 0;
  reactRoundDone = true;
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    players[i].points=0;
    players[i].bestMs=0;
    if (inGame[i] && players[i].name[0]==0) snprintf(players[i].name,20,"Node %u",i);
  }
  Serial.println("[REACT] Countdown...");
  bool ok = true;
  for (uint8_t c=3;c>0 && ok;c--) {
    partLED(COL_WHITE,ANIM_SOLID); ok = yieldDelay(400);
    partLED(COL_OFF,ANIM_SOLID);   if (ok) ok = yieldDelay(300);
  }
  reactNextAt    = millis() + random(REACT_DELAY_MIN_MS, REACT_DELAY_MAX_MS);
  return ok;
}

void reactOnButton(uint8_t id) {
  if (reactRoundDone || id != reactTarget) return;
  uint32_t ms = millis() - reactLitAt;
  reactRoundDone = true;
  players[id].points++;
  if (players[id].bestMs==0 || ms<players[id].bestMs) players[id].bestMs=ms;
  Serial.printf("[REACT] %s: %lums → %u Pkt\n", players[id].name, (unsigned long)ms, players[id].points);
  setLED(id, COL_GREEN, ANIM_SOLID);
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i!=id) setLED(i,COL_RED,ANIM_SOLID);
  uint32_t pause = roundPauseMs();
  startRoundBar(pause);
  reactNextAt = millis() + pause;
}

void reactUpdate() {
  uint32_t now=millis();
  updateRoundBar();
  if (reactRoundDone) {
    if ((long)(now - reactNextAt) >= 0) reactStartRound();
  } else if ((uint32_t)(now - reactLitAt) >= REACT_TIMEOUT_MS) {
    reactRoundDone=true;
    Serial.println("[REACT] Timeout – niemand gedrückt");
    partLED(COL_ORANGE, ANIM_SOLID);
    uint32_t pause = roundPauseMs();
    startRoundBar(pause);
    reactNextAt = now + pause;
  }
}

// ─────────────────────────────────────────────────────────────
// Simon Says (Game 5)
// ─────────────────────────────────────────────────────────────
// Jeder Teilnehmer hat eine feste Farbe (nach Nummer: ROT, BLAU, GRUEN …).
// Das macht die Sequenz merkbar: "ROT, BLAU, ROT, GRUEN" statt "Node 3, Node 1…"
static const uint8_t SIMON_COLORS[] = {
  COL_RED, COL_BLUE, COL_GREEN, COL_YELLOW,
  COL_PURPLE, COL_CYAN, COL_ORANGE, COL_WHITE
};

// Sequenz zeigen (blockierend, Tasten werden ignoriert). false = durch Web-Aktion unterbrochen.
bool simonShowSequence() {
  partLED(COL_OFF, ANIM_SOLID);
  if (!yieldDelay(400)) return false;
  for (uint8_t s = 0; s < simonLen; s++) {
    uint8_t id = simonSeq[s];
    setLED(id, simonCol[id], ANIM_SOLID);
    if (!yieldDelay(700)) return false;
    setLED(id, COL_OFF, ANIM_SOLID);
    if (!yieldDelay(250)) return false;
  }
  return yieldDelay(200);
}

// Naechsten Schritt anhaengen. false = Spiel ist vorbei (maximale Laenge erreicht = gewonnen).
// Die Sequenz zeigt danach simonUpdate() (nicht-blockierend geplant ueber simonShowAt).
bool simonAddStep() {
  if (simonLen >= (uint8_t)sizeof(simonSeq)) {
    Serial.println("[SIMON] Maximale Sequenz erreicht - gewonnen!");
    addHistory(GAME_SIMON, "Simon (max)", simonLen);
    gameOverBlink(COL_GREEN);
    gameMode = GAME_IDLE;
    return false;
  }
  uint8_t id = randomParticipant(0);
  if (id == 0) { Serial.println("[SIMON] Keine Teilnehmer mehr."); gameMode = GAME_IDLE; return false; }
  simonSeq[simonLen++] = id;
  simonStep    = 0;
  simonShowing = true;
  simonShowAt  = millis();
  return true;
}

bool simonStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_SIMON)) { Serial.println("[SIMON] Mindestens 2 aktive Nodes."); return false; }
  gameMode       = GAME_SIMON;
  simonLen       = 0;
  simonStep      = 0;
  simonHighScore = 0;
  simonShowing   = false;
  for (uint8_t i = 0; i <= MAX_NODES; i++) simonCol[i] = COL_OFF;
  for (uint8_t k = 0; k < pc; k++) simonCol[ids[k]] = SIMON_COLORS[k % 8];
  // Intro: alle Teilnehmer zeigen kurz ihre Farbe, damit Spieler die Zuordnung lernen
  Serial.println("[SIMON] Zeige Node-Farben...");
  for (uint8_t k = 0; k < pc; k++) setLED(ids[k], simonCol[ids[k]], ANIM_SOLID);
  bool ok = yieldDelay(2000);
  partLED(COL_OFF, ANIM_SOLID);
  if (simonAddStep()) simonShowAt = millis() + 400;   // erste Sequenz zeigt simonUpdate()
  return ok;
}

void simonOnButton(uint8_t id) {
  if (simonShowing || simonStep >= simonLen) return;
  if (id == simonSeq[simonStep]) {
    setLED(id, simonCol[id], ANIM_FLASH);
    simonStep++;
    simonInputAt = millis();
    if (simonStep == simonLen) {
      Serial.printf("[SIMON] Runde %u korrekt!\n", simonLen);
      if (simonLen > simonHighScore) simonHighScore = simonLen;
      if (simonAddStep()) simonShowAt = millis() + 800;   // kurze Pause, dann neue Sequenz
    }
    return;
  }
  Serial.printf("[SIMON] Falsch! Erreichte Laenge: %u\n", simonLen);
  addHistory(GAME_SIMON, "Simon", simonLen);
  // Falschen Node kurz rot zeigen, dann blinken alle rot
  setLED(id, COL_RED, ANIM_BLINK_FAST);
  if (!yieldDelay(800)) Serial.println("[SIMON] Anzeige abgekuerzt (Web-Aktion)");
  gameOverBlink(COL_RED);
  gameMode = GAME_IDLE;
}

void simonUpdate() {
  uint32_t now = millis();
  if (simonShowing) {
    if ((long)(now - simonShowAt) < 0) return;
    Serial.printf("[SIMON] Runde %u – zeige Sequenz\n", simonLen);
    if (!simonShowSequence()) return;   // Web-Aktion: Sequenz wird danach von vorn gezeigt
    // Eingabephase: alle Teilnehmer zeigen ihre Farbe gedimmt (2 von 8 LEDs)
    for (uint8_t i = 1; i <= MAX_NODES; i++) if (inGame[i]) setBar(i, simonCol[i], 2, COL_OFF);
    simonShowing = false;
    simonStep    = 0;
    simonInputAt = millis();
    Serial.println("[SIMON] Eingabephase");
    return;
  }
  // Eingabe-Timeout: zu lange kein richtiger Druck -> Spielende
  if ((uint32_t)(now - simonInputAt) >= SIMON_INPUT_TIMEOUT_MS) {
    Serial.printf("[SIMON] Zeit abgelaufen! Erreichte Laenge: %u\n", simonLen);
    addHistory(GAME_SIMON, "Simon (Zeit um)", simonLen);
    gameOverBlink(COL_RED);
    gameMode = GAME_IDLE;
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
  if (potatoHolder >= 1 && potatoHolder <= MAX_NODES && potatoActive[potatoHolder]) {
    potatoShowLives(potatoHolder);
  }
  potatoHolder = id;
  setLED(id, COL_ORANGE, ANIM_BLINK_FAST);
  // New explode time
  uint32_t timer = potatoMaxTimer;
  if (timer < 2000) timer = 2000;
  potatoExplodeAt = millis() + timer;
  Serial.printf("[POTATO] Holder: Node %u (Timer: %lums)\n", id, (unsigned long)timer);
}

void potatoCountActive() {
  potatoActiveCnt = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if(potatoActive[i]) potatoActiveCnt++;
}

// Zufaelliger noch lebender Spieler != exclude (0 = keiner)
uint8_t potatoRandomOther(uint8_t exclude) {
  uint8_t c[MAX_NODES]; uint8_t n = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (potatoActive[i] && i!=exclude) c[n++]=i;
  return n ? c[random(0, n)] : 0;
}

bool potatoStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_HOTPOTATO)) { Serial.println("[POTATO] Mindestens 2 aktive Nodes."); return false; }
  gameMode       = GAME_HOTPOTATO;
  potatoMaxTimer = 8000;
  potatoHolder   = 0;
  for (uint8_t i=0;i<=MAX_NODES;i++) {
    potatoActive[i] = inGame[i];
    potatoLives[i]  = inGame[i] ? 3 : 0;
  }
  potatoCountActive();
  uint8_t startHolder = ids[random(0, pc)];   // direkt aus der Teilnehmerliste -> keine Suchschleife
  for (uint8_t k=0;k<pc;k++) potatoShowLives(ids[k]);
  bool ok = yieldDelay(500);
  potatoSetHolder(startHolder);
  Serial.printf("[POTATO] Start! %u Spieler\n", potatoActiveCnt);
  return ok;
}

void potatoOnButton(uint8_t id) {
  if (id != potatoHolder) return; // only holder can pass
  // Reduce maxTimer
  if (potatoMaxTimer > 2200) potatoMaxTimer -= 200;
  // Pick random other active node
  uint8_t next = potatoRandomOther(id);
  if (next) potatoSetHolder(next);
}

void potatoUpdate() {
  if ((long)(millis() - potatoExplodeAt) < 0) return;
  uint8_t h = potatoHolder;
  if (h < 1 || h > MAX_NODES || !potatoActive[h]) {   // sollte nicht vorkommen: neu verteilen
    uint8_t n = potatoRandomOther(0);
    if (!n) { gameMode = GAME_IDLE; return; }
    potatoHolder = 0;
    potatoSetHolder(n);
    return;
  }
  // Explosion! Holder loses a life
  if (potatoLives[h] > 0) potatoLives[h]--;
  Serial.printf("[POTATO] Node %u Explosion! Leben: %u\n", h, potatoLives[h]);
  setLED(h, COL_RED, ANIM_BLINK_FAST);
  bool ok = yieldDelay(800);           // bei Abbruch (Web-Aktion) ohne weitere Pausen fortfahren
  if (potatoLives[h] == 0) {
    potatoActive[h] = false;
    setLED(h, COL_OFF, ANIM_SOLID);
    Serial.printf("[POTATO] Node %u ausgeschieden!\n", h);
    potatoCountActive();
    if (potatoActiveCnt <= 1) {
      // Find winner
      uint8_t pWinner = potatoRandomOther(0);   // der einzige Verbliebene (oder 0)
      Serial.println("[POTATO] Spiel beendet!");
      if (pWinner>0) addHistory(GAME_HOTPOTATO, players[pWinner].name, potatoLives[pWinner]);
      else           addHistory(GAME_HOTPOTATO, "Niemand", 0);
      finishGame(pWinner, COL_GREEN, COL_OFF);
      return;
    }
  } else {
    potatoShowLives(h);
    if (ok) ok = yieldDelay(500);
  }
  // Pass to random active node
  uint8_t next = potatoRandomOther(h);
  if (next == 0) { gameMode=GAME_IDLE; return; }
  potatoHolder = 0; // reset so potatoSetHolder doesn't try to restore old holder's LED
  // Show remaining players' lives
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    if (potatoActive[i] && i!=next) potatoShowLives(i);
  }
  potatoSetHolder(next);
}

// ─────────────────────────────────────────────────────────────
// King of the Hill (Game 7)
// ─────────────────────────────────────────────────────────────
// Thron wandert zu einem anderen Teilnehmer (kurz alle weiss). false = Animation abgebrochen.
bool kingMoveThrone() {
  uint8_t newThrone = randomParticipant(kingThrone);
  if (newThrone == 0) newThrone = kingThrone;
  // Flash all white
  partLED(COL_WHITE, ANIM_SOLID);
  bool ok = yieldDelay(300);
  // Restore state
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    if (!inGame[i] || i == newThrone) continue;
    setLED(i, (i == kingHolder) ? COL_PURPLE : COL_OFF, ANIM_SOLID);   // holder gets purple when off throne
  }
  kingThrone = newThrone;
  setLED(kingThrone, COL_YELLOW, ANIM_PULSE);
  kingNextMove = millis() + (uint32_t)KING_THRONE_MOVE_S * 1000UL;
  kingHolder = 0;
  kingLastCapture = millis();
  Serial.printf("[KING] Thron bewegt zu Node %u\n", kingThrone);
  return ok;
}

bool kingStart() {
  if (activeCount() < minNodesFor(GAME_KINGHILL)) { Serial.println("[KING] Mindestens 2 aktive Nodes."); return false; }
  gameMode       = GAME_KINGHILL;
  gameEndTime    = tAfter((uint32_t)cfgDuration * 1000UL);
  kingHolder     = 0;
  for (uint8_t i=0;i<=MAX_NODES;i++) kingHoldTime[i]=0;
  kingThrone     = randomParticipant(0);
  kingLastCapture = millis();
  kingNextMove   = millis() + (uint32_t)KING_THRONE_MOVE_S * 1000UL;
  partLED(COL_OFF, ANIM_SOLID);
  setLED(kingThrone, COL_YELLOW, ANIM_PULSE);
  Serial.printf("[KING] Start! Thron: Node %u Dauer: %us\n", kingThrone, cfgDuration);
  return true;
}

void kingOnButton(uint8_t id) {
  if (id != kingThrone) return; // only throne node matters
  uint32_t now = millis();
  // Accumulate hold time for previous holder
  if (kingHolder >= 1 && kingHolder <= MAX_NODES) {
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
  if (kingHolder >= 1 && kingHolder <= MAX_NODES) {
    kingHoldTime[kingHolder] += (now - kingLastCapture);
    kingLastCapture = now;
  }
  // Game over?
  if ((long)(now - gameEndTime) >= 0) {
    uint8_t winner = 0;
    for (uint8_t i=1;i<=MAX_NODES;i++)
      if (inGame[i] && (winner==0 || kingHoldTime[i]>kingHoldTime[winner])) winner=i;
    if (winner && kingHoldTime[winner]>0) {
      Serial.printf("[KING] Spiel beendet! Gewinner: Node %u (%s) mit %lums\n",
        winner, players[winner].name, (unsigned long)kingHoldTime[winner]);
      addHistory(GAME_KINGHILL, players[winner].name, kingHoldTime[winner]/1000);
    } else {
      winner = 0;
      Serial.println("[KING] Spiel beendet! Niemand hat den Thron gehalten.");
      addHistory(GAME_KINGHILL, "Niemand", 0);
    }
    finishGame(winner, COL_YELLOW, COL_OFF);
    return;
  }
  // Move throne? (false = Animation durch Web-Aktion abgekuerzt; der Thron steht trotzdem am neuen Platz)
  if ((long)(now - kingNextMove) >= 0 && !kingMoveThrone()) Serial.println("[KING] Animation abgekuerzt (Web-Aktion)");
}

// ─────────────────────────────────────────────────────────────
// Tug of War (Game 8)
// ─────────────────────────────────────────────────────────────
void tugUpdateDisplay() {
  // tugScore: 0=all Blue, 100=all Red; starts at 50
  // Split bar: (tugScore*8/100) red LEDs, rest blue
  int16_t s = tugScore;
  if (s < 0) s = 0;
  if (s > 100) s = 100;
  uint8_t redCount = (uint8_t)((s * BAR_LEDS) / 100);
  partSplit(COL_RED, redCount, COL_BLUE);
}

bool tugStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_TUGWAR)) { Serial.println("[TUG] Mindestens 2 aktive Nodes."); return false; }
  gameMode   = GAME_TUGWAR;
  gameEndTime = tAfter((uint32_t)cfgDuration * 1000UL);
  tugScore   = 50;
  // Teams: erste Haelfte der Teilnehmer (kleinere Nummern) ROT, der Rest BLAU
  uint8_t half = pc / 2;
  for (uint8_t i=0;i<=MAX_NODES;i++) tugTeam[i]=0;
  Serial.print("[TUG] Start! ROT:");
  for (uint8_t k=0;k<pc;k++) {
    tugTeam[ids[k]] = (k<half) ? 1 : 2;
    setLED(ids[k], (k<half)?COL_RED:COL_BLUE, ANIM_SOLID);
    if (k==half) Serial.print("  BLAU:");
    Serial.printf(" %u", ids[k]);
  }
  Serial.println();
  bool ok = yieldDelay(800);          // Teamfarben kurz zeigen
  tugUpdateDisplay();
  return ok;
}

void tugOnButton(uint8_t id) {
  if (tugTeam[id] == 1) {
    tugScore++; if (tugScore>100) tugScore=100;
  } else if (tugTeam[id] == 2) {
    tugScore--; if (tugScore<0) tugScore=0;
  } else return;
  tugUpdateDisplay();
  if (tugScore >= 100) {
    Serial.println("[TUG] Team ROT gewinnt!");
    gameOverBlink(COL_RED);
    addHistory(GAME_TUGWAR, "ROT", tugScore);
    gameMode = GAME_IDLE;
  } else if (tugScore <= 0) {
    Serial.println("[TUG] Team BLAU gewinnt!");
    gameOverBlink(COL_BLUE);
    addHistory(GAME_TUGWAR, "BLAU", 100-tugScore);
    gameMode = GAME_IDLE;
  }
}

void tugUpdate() {
  if ((long)(millis()-gameEndTime)<0) return;
  if (tugScore > 50) {
    Serial.println("[TUG] Zeit! Team ROT gewinnt!"); gameOverBlink(COL_RED);
    addHistory(GAME_TUGWAR, "ROT", (uint16_t)(tugScore-50));
  } else if (tugScore < 50) {
    Serial.println("[TUG] Zeit! Team BLAU gewinnt!"); gameOverBlink(COL_BLUE);
    addHistory(GAME_TUGWAR, "BLAU", (uint16_t)(50-tugScore));
  } else {
    Serial.println("[TUG] Zeit! Unentschieden!"); gameOverBlink(COL_WHITE, ANIM_BLINK_SLOW);
    addHistory(GAME_TUGWAR, "Unentschieden", 0);
  }
  gameMode = GAME_IDLE;
}

// ─────────────────────────────────────────────────────────────
// Minesweeper (Game 9)
// ─────────────────────────────────────────────────────────────
void mineRefreshDisplay() {
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    if (!inGame[i]) continue;
    if (mineRevealed[i]) {
      setLED(i, mineField[i] ? COL_RED : COL_GREEN, ANIM_SOLID);
    } else {
      // dim white for unrevealed – use bar with 2 white LEDs out of 8 as "dim"
      setBar(i, COL_WHITE, 2, COL_OFF);
    }
  }
}

// Leben kurz auf allen Teilnehmern zeigen, danach wieder das Spielfeld. false = abgebrochen.
bool mineShowLivesAll() {
  partBar(COL_GREEN, mineLives, COL_OFF);
  bool ok = yieldDelay(600);
  mineRefreshDisplay();
  return ok;
}

bool mineStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_MINESWEEPER)) { Serial.println("[MINE] Mindestens 3 aktive Nodes."); return false; }
  gameMode   = GAME_MINESWEEPER;
  mineLives  = 3;
  mineScore  = 0;
  uint8_t mines = cfgMines;
  if (mines < 1) mines = 1;
  if (mines > pc - 1) mines = pc - 1;     // mind. ein sicherer Node

  // Init
  for (uint8_t i=0;i<=MAX_NODES;i++) {
    mineField[i]    = false;
    mineRevealed[i] = false;
  }
  mineSafeCount = pc - mines;

  // Place mines randomly (nur auf Teilnehmern)
  shuffleIds(ids, pc);
  for (uint8_t k=0;k<mines;k++) mineField[ids[k]]=true;

  mineRefreshDisplay();
  Serial.printf("[MINE] Start! %u Minen, %u sichere Nodes\n", mines, mineSafeCount);
  return true;
}

void mineOnButton(uint8_t id) {
  if (mineRevealed[id]) return;
  mineRevealed[id] = true;
  if (mineField[id]) {
    // Mine!
    setLED(id, COL_RED, ANIM_BLINK_FAST);
    if (mineLives > 0) mineLives--;
    Serial.printf("[MINE] Node %u = MINE! Leben: %u\n", id, mineLives);
    bool ok = yieldDelay(600);
    if (mineLives == 0) {
      // Game over
      gameOverBlink(COL_RED);
      Serial.println("[MINE] Game Over!");
      addHistory(GAME_MINESWEEPER, "Explodiert", mineScore);
      gameMode = GAME_IDLE;
      return;
    }
    if (ok) ok = mineShowLivesAll();   // zeichnet danach selbst das Spielfeld
    else    mineRefreshDisplay();      // Web-Aktion wartet: sofort das Spielfeld
  } else {
    // Safe
    setLED(id, COL_GREEN, ANIM_SOLID);
    mineScore++;
    Serial.printf("[MINE] Node %u sicher! Punkte: %u/%u\n", id, mineScore, mineSafeCount);
    if (mineScore >= mineSafeCount) {
      // Win!
      gameOverBlink(COL_GREEN);
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
  for (uint8_t i=1;i<=MAX_NODES;i++) if(knockActive[i]) knockRemaining++;
}

// Spielende: letzter Verbliebener gewinnt, sonst Unentschieden
void knockFinish() {
  uint8_t knockWinner = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (knockActive[i]) { knockWinner=i; break; }
  Serial.println("[KNOCK] Spiel beendet!");
  if (knockRemaining == 1 && knockWinner > 0) {
    addHistory(GAME_KNOCKOUT, players[knockWinner].name, knockRound);
    finishGame(knockWinner, COL_GREEN, COL_OFF);
  } else {
    addHistory(GAME_KNOCKOUT, "Unentschieden", knockRound);
    gameOverBlink(COL_WHITE, ANIM_BLINK_SLOW); // draw
    gameMode = GAME_IDLE;
  }
}

void knockStartRound() {
  // Pick random active target
  uint8_t cands[MAX_NODES]; uint8_t cnt=0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if(knockActive[i]) cands[cnt++]=i;
  if (cnt == 0) { knockCountRemaining(); knockFinish(); return; }
  knockRound++;
  roundBarEndAt = 0;
  knockTarget = cands[random(0,cnt)];
  // Reset pressed flags
  for (uint8_t i=0;i<=MAX_NODES;i++) knockPressed[i]=false;
  // Light target yellow, others off
  setLED(knockTarget, COL_YELLOW, ANIM_BLINK_FAST);
  knockLitAt    = millis();                 // Zeitmessung ab Aufleuchten des Ziels
  for (uint8_t k=0;k<cnt;k++) if (cands[k]!=knockTarget) setLED(cands[k], COL_OFF, ANIM_SOLID);
  knockRoundDone = false;
  knockGraceAt  = 0;
  Serial.printf("[KNOCK] Runde %u – Node %u leuchtet!\n", knockRound, knockTarget);
}

bool knockStart() {
  if (activeCount() < minNodesFor(GAME_KNOCKOUT)) { Serial.println("[KNOCK] Mindestens 2 aktive Nodes."); return false; }
  gameMode      = GAME_KNOCKOUT;
  knockRound    = 0;
  knockTarget   = 0;
  knockRoundDone = true;
  knockGraceAt  = 0;   // wichtig: alte Gnadenfrist verwerfen, sonst sofortiger Lebensabzug
  knockNextAt   = millis() + 2000;
  uint8_t lives = cfgKnockLives;
  if (lives < 1) lives = 1;
  if (lives > BAR_LEDS) lives = BAR_LEDS;
  for (uint8_t i=0;i<=MAX_NODES;i++) {
    knockActive[i]  = inGame[i];
    knockLives[i]   = inGame[i] ? lives : 0;
    knockPressed[i] = false;
  }
  knockCountRemaining();
  // Show lives bars
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    if (knockActive[i]) setBar(i, COL_GREEN, knockLives[i], COL_OFF);
  }
  Serial.printf("[KNOCK] Start! %u Spieler, %u Leben\n", knockRemaining, lives);
  return yieldDelay(800);
}

void knockOnButton(uint8_t id) {
  if (!knockActive[id]) return;
  if (!knockRoundDone) {
    // Runde laeuft: Druck zaehlt, erster Treffer startet die Gnadenfrist
    knockPressed[id] = true;
    if (id == knockTarget) {
      setLED(id, COL_GREEN, ANIM_FLASH);
      Serial.printf("[KNOCK] Node %u als erstes!\n", id);
      knockGraceAt   = tAfter(2000);
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
  updateRoundBar();
  if (!knockRoundDone) {
    // Round in progress – check timeout
    if ((uint32_t)(now - knockLitAt) >= REACT_TIMEOUT_MS) {
      knockRoundDone = true;
      knockGraceAt   = now | 1UL; // immediate penalty
      Serial.println("[KNOCK] Timeout!");
    }
    return;
  }
  if (knockGraceAt != 0) {
    if ((long)(now - knockGraceAt) < 0) return;   // Gnadenfrist laeuft noch
    knockGraceAt = 0;
    // Penalize those who didn't press
    for (uint8_t i=1;i<=MAX_NODES;i++) {
      if (!knockActive[i] || knockPressed[i]) continue;
      if (knockLives[i] > 0) knockLives[i]--;
      Serial.printf("[KNOCK] Node %u zu langsam! Leben: %u\n", i, knockLives[i]);
      if (knockLives[i] == 0) {
        knockActive[i] = false;
        setLED(i, COL_OFF, ANIM_SOLID);
        Serial.printf("[KNOCK] Node %u ausgeschieden!\n", i);
      }
    }
    knockCountRemaining();
    if (knockRemaining <= 1) { knockFinish(); return; }
    // Verbliebene zeigen ihre Leben, danach Countdown-Balken bis zur naechsten Runde
    for (uint8_t i=1;i<=MAX_NODES;i++) if (knockActive[i]) setBar(i, COL_GREEN, knockLives[i], COL_OFF);
    uint32_t kpause = roundPauseMs();
    startRoundBar(kpause);
    knockNextAt = now + kpause;
    return;
  }
  if ((long)(now - knockNextAt) >= 0) knockStartRound();
}

// ─────────────────────────────────────────────────────────────
// Color Hunt (Game 11)
// ─────────────────────────────────────────────────────────────
// Anzeige-Node = erster Teilnehmer (kleinste Nummer); Ziele nur unter den anderen.

// Neues Ziel: anderer Teilnehmer als Anzeige-Node und (wenn moeglich) als das bisherige Ziel
void huntPickTarget() {
  uint8_t c[MAX_NODES]; uint8_t n = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i!=huntDisplay && i!=huntTarget) c[n++]=i;
  if (n > 0) huntTarget = c[random(0, n)];
  // sonst: nur ein weiterer Teilnehmer -> Ziel bleibt
}

void huntFinish() {
  uint8_t winner = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++)
    if (inGame[i] && (winner==0 || huntScores[i]>huntScores[winner])) winner=i;
  Serial.println("[HUNT] Spiel beendet!");
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i])
    Serial.printf("  Node %u (%s): %u Pkt\n", i, players[i].name, huntScores[i]);
  if (winner && huntScores[winner]>0) addHistory(GAME_COLORHUNT, players[winner].name, huntScores[winner]);
  else { winner=0; addHistory(GAME_COLORHUNT, "Niemand", 0); }
  finishGame(winner, COL_GREEN, COL_OFF);
}

// Neue Runde (oder Spielende). false = Farb-Anzeige durch Web-Aktion abgekuerzt (Runde laeuft trotzdem).
bool huntStartRound() {
  huntRound++;
  if (huntRound > cfgHuntRounds) { huntFinish(); return true; }
  if (!isPart(huntTarget) || huntTarget == huntDisplay) {
    Serial.println("[HUNT] Kein Ziel-Node mehr – Spiel beendet.");
    huntFinish();
    return true;
  }
  // Jeder Teilnehmer zeigt 1,5 s seine Farbe
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i]) setLED(i, huntColors[i], ANIM_SOLID);
  bool ok = yieldDelay(1500);
  // Dann alle gedimmt weiss, der Anzeige-Node blinkt 2 s die Zielfarbe
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i!=huntDisplay) setBar(i, COL_WHITE, 2, COL_OFF);
  uint8_t targetColor = huntColors[huntTarget];
  setLED(huntDisplay, targetColor, ANIM_BLINK_FAST);
  huntShowTarget  = true;
  huntRoundActive = true;
  huntNextAt      = millis() + 2000;                    // so lange zeigt der Anzeige-Node die Zielfarbe
  huntRoundEnd    = millis() + HUNT_ROUND_TIMEOUT_MS;   // Runden-Timeout
  Serial.printf("[HUNT] Runde %u/%u – Zielfarbe: %u (Anzeige: Node %u)\n", huntRound, cfgHuntRounds, targetColor, huntDisplay);
  return ok;
}

bool huntStart() {
  uint8_t ids[MAX_NODES];
  uint8_t pc = collectActive(ids);
  if (pc < minNodesFor(GAME_COLORHUNT)) { Serial.println("[HUNT] Mindestens 2 aktive Nodes."); return false; }
  gameMode   = GAME_COLORHUNT;
  huntRound  = 0;
  huntRoundActive = false;
  huntShowTarget  = false;
  huntDisplay = ids[0];                        // Anzeige-Node = erster Teilnehmer
  for (uint8_t i=0;i<=MAX_NODES;i++) { huntScores[i] = 0; huntColors[i] = COL_OFF; }
  for (uint8_t k=0;k<pc;k++) huntColors[ids[k]] = PAIR_PALETTE[random(0, 7)]; // assign random color from palette
  huntTarget = 0;
  huntPickTarget();
  huntNextAt = millis();
  Serial.printf("[HUNT] Start! %u Runden, Anzeige-Node %u\n", cfgHuntRounds, huntDisplay);
  return huntStartRound();
}

void huntOnButton(uint8_t id) {
  if (!huntRoundActive || id == huntDisplay) return;
  uint8_t targetColor = huntColors[huntTarget];
  if (huntColors[id] == targetColor) {
    // Correct!
    huntScores[id]++;
    huntRoundActive = false;
    huntShowTarget  = false;
    setLED(id, COL_GREEN, ANIM_BLINK_FAST);
    for (uint8_t i=1;i<=MAX_NODES;i++) {
      if (inGame[i] && i!=id) setLED(i, COL_RED, ANIM_SOLID);
    }
    Serial.printf("[HUNT] Node %u korrekt! Punkte: %u\n", id, huntScores[id]);
    huntPickTarget();                          // Pick new target for next round
    huntNextAt = millis() + 1500;
  } else {
    // Wrong – red blink; the display node keeps showing the target
    setLED(id, COL_RED, ANIM_BLINK_FAST);
    Serial.printf("[HUNT] Node %u falsch!\n", id);
  }
}

void huntUpdate() {
  uint32_t now = millis();
  if (!huntRoundActive) {
    // naechste Runde; false = Farb-Anzeige durch Web-Aktion abgekuerzt (die Runde laeuft trotzdem)
    if ((long)(now - huntNextAt) >= 0 && !huntStartRound()) Serial.println("[HUNT] Anzeige abgekuerzt (Web-Aktion)");
    return;
  }
  if ((long)(now - huntRoundEnd) >= 0) {
    // 12 s ohne Treffer -> naechste Runde ohne Punkte
    huntRoundActive = false;
    huntShowTarget  = false;
    Serial.println("[HUNT] Zeit um – keine Punkte");
    partLED(COL_ORANGE, ANIM_SOLID);
    huntPickTarget();
    huntNextAt = now + 1500;
    return;
  }
  if (huntShowTarget && (long)(now - huntNextAt) >= 0) {
    // Zielfarbe lange genug gezeigt -> Anzeige-Node gedimmt
    huntShowTarget = false;
    setBar(huntDisplay, COL_WHITE, 2, COL_OFF);
  }
}

// ─────────────────────────────────────────────────────────────
// Whack-a-Mole (Game 12)
// ─────────────────────────────────────────────────────────────
static uint8_t whamLastTarget = 0;

void whamStartRound() {
  if (whamRound >= cfgWhamRounds) {
    uint8_t winner=0;
    for (uint8_t i=1;i<=MAX_NODES;i++)
      if (inGame[i] && (winner==0 || whamScores[i]>whamScores[winner])) winner=i;
    Serial.println("[WHAM] === ENDE ===");
    for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i])
      Serial.printf("  %s: %u Treffer\n", players[i].name, whamScores[i]);
    if (winner && whamScores[winner]>0) addHistory(GAME_WHACKAMOLE, players[winner].name, whamScores[winner]);
    else { winner=0; addHistory(GAME_WHACKAMOLE, "Niemand", 0); }
    finishGame(winner, COL_GREEN, COL_RED);
    return;
  }
  uint8_t t = randomParticipant(whamLastTarget);
  if (t == 0) { Serial.println("[WHAM] Keine Teilnehmer mehr."); gameMode=GAME_IDLE; return; }
  roundBarEndAt  = 0;
  whamTarget     = t;
  whamLastTarget = t;
  setLED(whamTarget, COL_YELLOW, ANIM_BLINK_FAST);
  whamLitAt     = millis();                 // Zeitmessung ab Aufleuchten des Ziels
  for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i!=whamTarget) setLED(i,COL_OFF,ANIM_SOLID);
  whamRoundDone = false;
  whamRound++;
  Serial.printf("[WHAM] Runde %u/%u - Node %u leuchtet!\n", whamRound, cfgWhamRounds, whamTarget);
}

bool whamStart() {
  if (activeCount() < minNodesFor(GAME_WHACKAMOLE)) { Serial.println("[WHAM] Mindestens 2 aktive Nodes."); return false; }
  gameMode      = GAME_WHACKAMOLE;
  whamRound     = 0;
  whamTarget    = 0;
  whamRoundDone = true;
  whamLastTarget = 0;
  for (uint8_t i=0;i<=MAX_NODES;i++) {
    whamScores[i]=0; players[i].points=0;
    if (inGame[i] && players[i].name[0]==0) snprintf(players[i].name,20,"Node %u",i);
  }
  partLED(COL_OFF,ANIM_SOLID);
  bool ok = true;
  for (uint8_t c=3;c>0 && ok;c--) {
    partLED(COL_WHITE,ANIM_SOLID); ok = yieldDelay(400);
    partLED(COL_OFF,ANIM_SOLID);   if (ok) ok = yieldDelay(300);
  }
  whamNextAt = millis() + random(WHAM_DELAY_MIN_MS, WHAM_DELAY_MAX_MS);
  Serial.printf("[WHAM] Start! %u Runden\n", cfgWhamRounds);
  return ok;
}

void whamOnButton(uint8_t id) {
  if (whamRoundDone) return;
  if (id == whamTarget) {
    whamRoundDone = true;
    whamScores[id]++;
    players[id].points++;
    Serial.printf("[WHAM] %s trifft! Punkte: %u\n", players[id].name, whamScores[id]);
    setLED(id, COL_GREEN, ANIM_SOLID);
    for (uint8_t i=1;i<=MAX_NODES;i++) if (inGame[i] && i!=id) setLED(i,COL_OFF,ANIM_SOLID);
    uint32_t pause = roundPauseMs();
    startRoundBar(pause);
    whamNextAt = millis() + pause;
  } else {
    setLED(id, COL_RED, ANIM_BLINK_FAST);
  }
}

void whamUpdate() {
  uint32_t now = millis();
  updateRoundBar();
  if (whamRoundDone) {
    if ((long)(now - whamNextAt) >= 0) whamStartRound();
  } else if ((uint32_t)(now - whamLitAt) >= WHAM_TIMEOUT_MS) {
    whamRoundDone = true;
    setLED(whamTarget, COL_RED, ANIM_SOLID);
    Serial.printf("[WHAM] Timeout! Node %u nicht getroffen.\n", whamTarget);
    uint32_t pause = roundPauseMs();
    startRoundBar(pause);
    whamNextAt = now + pause;
  }
}

// ─────────────────────────────────────────────────────────────
// Spiel-Verteilung (Tasten / Update / Start / Stop)
// ─────────────────────────────────────────────────────────────
void gameOnButton(uint8_t id) {
  if (!isPart(id)) return;   // Nodes, die beim Start nicht da waren, spielen nicht mit
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
  else if (gameMode==GAME_WHACKAMOLE)  whamOnButton(id);
}

void gameUpdate() {
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
  else if (gameMode==GAME_WHACKAMOLE) whamUpdate();
}

void startGameNow(uint8_t m) {
  if (rolloutActive) { Serial.println("[GAME] Start abgelehnt: Firmware-Update laeuft."); return; }
  if (m < GAME_CTF || m > GAME_WHACKAMOLE) return;
  gameMode=GAME_IDLE; gameEndTime=0; idleBlinkUntil=0; roundBarEndAt=0;
  // Teilnehmer = beim Start aktive Nodes
  for (uint8_t i=0;i<=MAX_NODES;i++) inGame[i] = (i>=1 && nodes[i].known && nodes[i].active);
  allLED(COL_OFF,ANIM_SOLID);
  gameSeed();
  bool ok = false;
  switch (m) {
    case GAME_CTF:         ok = ctfStart();   break;
    case GAME_MEMORY:      ok = memStart();   break;
    case GAME_BOMB:        ok = bombStart();  break;
    case GAME_REACTION:    ok = reactStart(); break;
    case GAME_SIMON:       ok = simonStart(); break;
    case GAME_HOTPOTATO:   ok = potatoStart(); break;
    case GAME_KINGHILL:    ok = kingStart();  break;
    case GAME_TUGWAR:      ok = tugStart();   break;
    case GAME_MINESWEEPER: ok = mineStart();  break;
    case GAME_KNOCKOUT:    ok = knockStart(); break;
    case GAME_COLORHUNT:   ok = huntStart();  break;
    case GAME_WHACKAMOLE:  ok = whamStart();  break;
  }
  // ok == false: zu wenige Nodes (Spiel laeuft nicht) oder Start-Animation abgekuerzt
  if (!ok && gameMode != GAME_IDLE) Serial.println("[GAME] Start-Animation abgekuerzt (Web-Aktion wartet)");
}

void stopGameNow() {
  gameMode=GAME_IDLE; idleBlinkUntil=0; roundBarEndAt=0;
  allLED(COL_OFF,ANIM_SOLID);
  Serial.println("[GAME] Gestoppt.");
}

// Nach Spielende: Blinken nach 10s stoppen -> Nodes leuchten solid in ihrer Soll-Farbe
void idleBlinkService() {
  if (idleBlinkUntil == 0 || (long)(millis() - idleBlinkUntil) < 0) return;
  idleBlinkUntil = 0;
  for (uint8_t i=1;i<=nodeCount;i++) {
    NodeInfo& n = nodes[i];
    if (!n.known || n.ledSeq == 0 || n.ledType != PKT_SET_LED) continue;
    if (n.ledB == ANIM_BLINK_SLOW || n.ledB == ANIM_BLINK_FAST || n.ledB == ANIM_FLASH || n.ledB == ANIM_PULSE)
      setLED(i, n.ledA, ANIM_SOLID);
  }
  Serial.println("[IDLE] Blinken gestoppt - Nodes leuchten solid.");
}

// ─────────────────────────────────────────────────────────────
// Node-Verwaltung: Reconnect / Vergessen / Finden
// ─────────────────────────────────────────────────────────────
void resetBroadcastService() {
  if (rstBcastLeft == 0) return;
  if ((long)(millis() - rstBcastNext) < 0) return;
  sendPkt(bcastIP, PKT_RESET, 0xFF);
  rstBcastLeft--;
  rstBcastNext = millis() + 50;
}

// Alle Nodes melden sich neu an – IDs bleiben (Zuordnung per MAC), Soll-LEDs werden neu gesendet
void reconnectNodes() {
  rstBcastLeft = 3; rstBcastNext = millis();
  for (uint8_t i=1;i<=nodeCount;i++)
    if (nodes[i].known && nodes[i].active && nodes[i].ip) sendPkt(IPAddress(nodes[i].ip), PKT_RESET, 0xFF);
  Serial.println("[RESET] Reconnect gesendet – Nodes melden sich neu an (IDs bleiben).");
}

// Node-Liste komplett vergessen: Nodes bekommen neue Nummern (Namen bleiben je Nummer)
void forgetNodes() {
  gameMode = GAME_IDLE; idleBlinkUntil = 0; roundBarEndAt = 0;
  for (uint8_t i=1;i<=nodeCount;i++)
    if (nodes[i].known && nodes[i].active && nodes[i].ip) sendPkt(IPAddress(nodes[i].ip), PKT_RESET, 0xFF);
  for (uint8_t i=0;i<=MAX_NODES;i++) { memset(&nodes[i], 0, sizeof(NodeInfo)); inGame[i] = false; }
  nodeCount = 0;
  saveMacs();
  rstBcastLeft = 3; rstBcastNext = millis();
  Serial.println("[RESET] Node-Liste geloescht – Nodes melden sich neu an (neue Nummern).");
}

void identifyNode(uint8_t id) {
  if (!nodeValid(id) || !nodes[id].active || !nodes[id].ip) return;
  sendPkt(IPAddress(nodes[id].ip), PKT_IDENTIFY, id, 5);
  Serial.printf("[NODE] Node %u blinkt weiss (Finden)\n", id);
}

// ─────────────────────────────────────────────────────────────
// Node-Firmware speichern (LittleFS) und per Rollout verteilen (ESP32)
// ─────────────────────────────────────────────────────────────
uint32_t rdLE32(const uint8_t* b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Prueft den Kopf (IMG_HDR_LEN Bytes) einer hochgeladenen Firmware.
// Leerer String = ok, sonst deutsche Fehlermeldung. d = gelesene Kennung.
String imageCheck(const uint8_t* h, uint8_t wantRole, CtfFwDesc& d) {
  String use = (wantRole == CTF_ROLE_NODE) ? " – bitte node.ino.bin verwenden." : " – bitte master.ino.bin verwenden.";
  memset(&d, 0, sizeof(d));
  if (h[0] == 0xFF)                 return "Das ist ein komplettes Flash-Abbild (.merged.bin)" + use;
  if (h[0] == 0xAA && h[1] == 0x50) return "Das ist die Partitionstabelle (.partitions.bin)" + use;
  if (h[0] != 0xE9)                 return "Keine ESP32-Firmware-Datei" + use;
  if (h[0x20] == 0x50)              return "Das ist der Bootloader (.bootloader.bin)" + use;
  if (h[1] < 1 || h[1] > 16)        return "Ungueltiger Firmware-Kopf" + use;
  if ((h[12] | (h[13] << 8)) != 0)  return "Firmware ist fuer einen anderen Chip (nicht ESP32)" + use;
  if (rdLE32(h + 0x20) != 0xABCD5432UL) return "Keine gueltige ESP32-App" + use;
  memcpy(&d, h + CTF_DESC_OFFSET, sizeof(CtfFwDesc));
  d.version[sizeof(d.version) - 1] = 0;
  // Version nur aus druckbaren Zeichen ohne Leerzeichen (wird in /node.meta gespeichert)
  for (uint8_t k = 0; k < sizeof(d.version) && d.version[k]; k++)
    if ((uint8_t)d.version[k] <= 0x20 || (uint8_t)d.version[k] >= 0x7F) d.version[k] = '_';
  if (d.version[0] == 0) { d.version[0] = '?'; d.version[1] = 0; }
  if (d.magic != CTF_DESC_MAGIC)
    return "Firmware ohne CTF-Kennung (alte Version 1.x oder anderes Programm)" + use;
  if (d.role != wantRole)
    return (d.role == CTF_ROLE_MASTER) ? String("Das ist die MASTER-Firmware – hier wird die Node-Firmware (node.ino.bin) gebraucht.")
                                       : String("Das ist die NODE-Firmware – hier wird die Master-Firmware (master.ino.bin) gebraucht.");
  if (wantRole == CTF_ROLE_NODE && d.proto != PROTO_VERSION)
    return "Node-Firmware passt nicht zum Master (Protokoll " + String(d.proto) + ", erwartet " + String(PROTO_VERSION) + ")";
  return "";
}

String htmlEsc(const String& s) {
  String o; o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

// JSON-String mit Escaping anhaengen (" \ und Steuerzeichen)
void jsonStr(String& j, const char* s) {
  j += '"';
  for (; s && *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c == '"' || c == '\\') { j += '\\'; j += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); j += b; }
    else j += (char)c;
  }
  j += '"';
}

#ifdef ESP32
void fsRefresh() {
  if (!fsOk) { fsTotal = fsUsed = 0; return; }
  fsTotal = LittleFS.totalBytes();
  fsUsed  = LittleFS.usedBytes();
}

void loadNodeFwMeta() {
  memset(&nodeFw, 0, sizeof(nodeFw));
  if (!fsOk || !LittleFS.exists("/node.bin") || !LittleFS.exists("/node.meta")) return;
  File m = LittleFS.open("/node.meta", "r");
  if (!m) return;
  String s = m.readStringUntil('\n');
  m.close();
  unsigned a = 0, b = 0, c = 0, pr = 0; unsigned long sz = 0;
  char md5[33] = {0}, ver[16] = {0};
  if (sscanf(s.c_str(), "%u %u %u %u %lu %32s %15s", &a, &b, &c, &pr, &sz, md5, ver) != 7) return;
  File f = LittleFS.open("/node.bin", "r");
  if (!f) return;
  size_t real = f.size();
  f.close();
  if (real != sz || sz == 0 || strlen(md5) != 32) return;
  nodeFw.present = true;
  nodeFw.major = a; nodeFw.minor = b; nodeFw.patch = c; nodeFw.proto = pr;
  nodeFw.size = sz;
  memcpy(nodeFw.md5, md5, 33);
  memcpy(nodeFw.version, ver, 16); nodeFw.version[15] = 0;
}

bool writeNodeFwMeta() {
  File m = LittleFS.open("/node.meta", "w");
  if (!m) return false;
  char line[96];
  int len = snprintf(line, sizeof(line), "%u %u %u %u %lu %s %s\n",
    (unsigned)nodeFw.major, (unsigned)nodeFw.minor, (unsigned)nodeFw.patch, (unsigned)nodeFw.proto,
    (unsigned long)nodeFw.size, nodeFw.md5, nodeFw.version);
  bool ok = len > 0 && len < (int)sizeof(line) && m.write((const uint8_t*)line, len) == (size_t)len;
  m.close();
  return ok;
}

void nodeFwDelete() {
  if (rolloutActive) { fwMsg = "Loeschen nicht moeglich: Rollout laeuft."; return; }
  if (fsOk) { LittleFS.remove("/node.bin"); LittleFS.remove("/node.meta"); LittleFS.remove("/node.tmp"); }
  memset(&nodeFw, 0, sizeof(nodeFw));
  fsRefresh();
  fwMsg = "Gespeicherte Node-Firmware geloescht.";
  Serial.println("[FW] Node-Firmware geloescht");
}

bool fwEqualsStored(const NodeInfo& n) {
  return nodeFw.present && n.fw[0] == nodeFw.major && n.fw[1] == nodeFw.minor && n.fw[2] == nodeFw.patch;
}

// ── Upload-Helfer ──
// RAW-Modus-Schutz: Ein POST ohne multipart-Content-Type ruft den Upload-Callback
// im RAW-Modus auf – dann ist webServer.upload() ungueltig (Absturz). Nur bei multipart anfassen.
bool upMultipart() { return webServer.header("Content-Type").startsWith("multipart/"); }

void upBegin(uint8_t kind) {
  // Reste eines frueheren, unvollstaendigen Uploads aufraeumen
  if (Update.isRunning()) Update.abort();
  if (upFile) upFile.close();
  upKind = kind; upStarted = true; upFail = false; upHdrOk = false; upDone = false;
  upHdrLen = 0; upTotal = 0; upMsg = "";
  memset(&upDesc, 0, sizeof(upDesc));
}

// Fehler merken (erste Meldung zaehlt) und aufraeumen
void upError(const String& m) {
  if (!upFail) { upFail = true; upMsg = m; Serial.printf("[UPLOAD] Fehler: %s\n", m.c_str()); }
  if (upKind == UP_MASTER) {
    if (Update.isRunning()) Update.abort();
  } else if (upKind == UP_NODE) {
    if (upFile) upFile.close();
    if (fsOk) LittleFS.remove("/node.tmp");
  }
}

// Kopiert Bytes in den Kopfpuffer, bis IMG_HDR_LEN erreicht ist; liefert die Anzahl uebernommener Bytes
size_t upCollectHdr(const uint8_t* buf, size_t len) {
  if (upHdrLen >= IMG_HDR_LEN) return 0;
  size_t take = IMG_HDR_LEN - upHdrLen;
  if (take > len) take = len;
  memcpy(upHdr + upHdrLen, buf, take);
  upHdrLen += take;
  return take;
}

// POST /fw/node – Upload-Callback (laeuft komplett innerhalb EINES handleClient())
void webHandleNodeFwUpload() {
  wdtFeed();                       // auch bei abgelehnten Uploads: der Body wird trotzdem komplett gelesen
  if (!upMultipart()) return;
  HTTPUpload& up = webServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    upBegin(UP_NODE);
    Serial.printf("[FW] Node-Firmware-Upload: %s\n", up.filename.c_str());
    if (!fsOk) { upError("Dateisystem (LittleFS) nicht verfuegbar."); return; }
    if (gameMode != GAME_IDLE || rolloutActive) { upError("Nicht moeglich, waehrend ein Spiel oder Update laeuft."); return; }
    // Es passt nur EIN Firmware-Image in den Speicher -> altes vorher loeschen
    LittleFS.remove("/node.bin"); LittleFS.remove("/node.meta"); LittleFS.remove("/node.tmp");
    memset(&nodeFw, 0, sizeof(nodeFw));
    upFile = LittleFS.open("/node.tmp", "w");
    if (!upFile) { upError("Datei konnte nicht angelegt werden."); return; }
    upMd5.begin();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upStarted || upFail || upKind != UP_NODE) return;
    if (upTotal + up.currentSize > CTF_APP_MAX_SIZE) { upError("Datei zu gross (max. 1.310.720 Bytes)."); return; }
    if (!upHdrOk) {
      upCollectHdr(up.buf, up.currentSize);
      if (upHdrLen >= IMG_HDR_LEN) {
        String e = imageCheck(upHdr, CTF_ROLE_NODE, upDesc);
        if (e.length()) { upError(e); return; }
        upHdrOk = true;
      }
    }
    if (upFile.write(up.buf, up.currentSize) != up.currentSize) { upError("Speicher voll – Datei passt nicht in den Flash-Speicher."); return; }
    upMd5.add(up.buf, up.currentSize);
    upTotal += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (!upStarted || upFail || upKind != UP_NODE) return;
    if (!upHdrOk) { upError("Datei zu klein – keine gueltige Firmware."); return; }
    upFile.close();
    LittleFS.remove("/node.bin");
    if (!LittleFS.rename("/node.tmp", "/node.bin")) { upError("Speichern fehlgeschlagen."); return; }
    upMd5.calculate();
    nodeFw.major = upDesc.major; nodeFw.minor = upDesc.minor; nodeFw.patch = upDesc.patch;
    nodeFw.proto = upDesc.proto; nodeFw.size = upTotal;
    memcpy(nodeFw.version, upDesc.version, 16); nodeFw.version[15] = 0;
    String md5 = upMd5.toString();
    strncpy(nodeFw.md5, md5.c_str(), 32); nodeFw.md5[32] = 0;
    if (!writeNodeFwMeta()) {
      LittleFS.remove("/node.bin"); LittleFS.remove("/node.meta");
      memset(&nodeFw, 0, sizeof(nodeFw));
      upError("Speicher voll (Info-Datei).");
      return;
    }
    nodeFw.present = true;
    upDone = true;
    upMsg = "Node-Firmware " + String(nodeFw.version) + " gespeichert (" + String(nodeFw.size) + " Bytes).";
    Serial.printf("[FW] %s MD5 %s\n", upMsg.c_str(), nodeFw.md5);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    // Achtung: ABORTED kommt auch NACH einem erfolgreichen END, wenn der Browser
    // die Verbindung schon getrennt hat -> dann ist die Datei trotzdem gueltig gespeichert.
    if (upStarted && upKind == UP_NODE && !upDone) upError("Upload abgebrochen.");
    if (upStarted && upKind == UP_NODE && upDone) fwMsg = upMsg;
    upStarted = false;   // final handler wird nach ABORTED nicht aufgerufen
    fsRefresh();
  }
}

void webHandleNodeFwResult() {
  bool mine = upStarted && upKind == UP_NODE;
  bool ok   = mine && upDone && !upFail;
  String msg = mine ? upMsg : String("Keine Datei empfangen.");
  if (mine && !ok && msg.length() == 0) msg = "Upload fehlgeschlagen.";
  upStarted = false;
  fwMsg = msg;
  fsRefresh();
  String j = "{\"ok\":"; j += ok ? "true" : "false";
  j += ",\"msg\":"; jsonStr(j, msg.c_str());
  j += ",\"version\":"; jsonStr(j, ok ? nodeFw.version : "");
  j += "}";
  webServer.send(ok ? 200 : 400, "application/json", j);
}

// GET /fw/node.bin – von den Nodes beim OTA abgerufen
void webHandleFwGet() {
  if (!fsOk || !nodeFw.present) { webServer.send(404, "text/plain", "keine Node-Firmware gespeichert"); return; }
  File f = LittleFS.open("/node.bin", "r");
  if (!f) { webServer.send(404, "text/plain", "keine Node-Firmware gespeichert"); return; }
  size_t sz = f.size();
  webServer.sendHeader("x-MD5", nodeFw.md5);
  webServer.setContentLength(sz);
  webServer.send(200, "application/octet-stream", "");
  // Selbst senden (statt streamFile), damit der Watchdog gefuettert wird
  NetworkClient& c = webServer.client();
  static uint8_t buf[1436];
  size_t sent = 0;
  uint32_t t0 = millis();
  while (sent < sz) {
    size_t want = sz - sent; if (want > sizeof(buf)) want = sizeof(buf);
    size_t n = f.read(buf, want);
    if (n == 0) break;
    if (c.write(buf, n) != n) break;
    sent += n;
    wdtFeed();
    if ((uint32_t)(millis() - t0) > 120000UL) break;
  }
  f.close();
  Serial.printf("[FW] node.bin an %s: %u/%u Bytes\n", c.remoteIP().toString().c_str(), (unsigned)sent, (unsigned)sz);
}

// ── Rollout ──
void rolloutStart(bool force) {
  if (rolloutActive) return;
  if (gameMode != GAME_IDLE) { fwMsg = "Rollout nicht moeglich: Spiel laeuft – erst stoppen."; return; }
  if (!fsOk || !nodeFw.present) { fwMsg = "Keine Node-Firmware gespeichert."; return; }
  roQLen = 0; roQPos = 0;
  for (uint8_t i = 0; i <= MAX_NODES; i++) { roState[i] = 0; roErr[i] = 0; }
  for (uint8_t i = 1; i <= nodeCount; i++) {
    NodeInfo& n = nodes[i];
    if (!n.known || !n.active) continue;
    if (!force && fwEqualsStored(n)) continue;
    roQueue[roQLen++] = i;
    roState[i] = 1;
  }
  if (roQLen == 0) {
    fwMsg = force ? String("Keine aktiven Nodes.") : "Alle aktiven Nodes haben bereits Version " + String(nodeFw.version) + ".";
    return;
  }
  rolloutActive = true; roCancel = false; roPhase = RO_IDLE; roCur = 0;
  fwMsg = "Rollout gestartet: " + String(roQLen) + " Node(s) – bitte eingeschaltet lassen.";
  Serial.printf("[ROLLOUT] Start (%s): %u Node(s), Version %s\n", force ? "alle" : "veraltete", roQLen, nodeFw.version);
}

void rolloutFinish() {
  uint8_t ok = 0, bad = 0;
  for (uint8_t i = 1; i <= MAX_NODES; i++) {
    if (roState[i] == 3) ok++;
    else if (roState[i] == 4) bad++;
    else if (roState[i] == 1 || roState[i] == 2) roState[i] = 0;   // nicht mehr drangekommen
  }
  fwMsg = "Rollout beendet: " + String(ok) + " ok, " + String(bad) + " Fehler" + (roCancel ? " (abgebrochen)." : ".");
  Serial.printf("[ROLLOUT] %s\n", fwMsg.c_str());
  rolloutActive = false; roCancel = false; roPhase = RO_IDLE; roCur = 0;
}

void rolloutNodeDone(bool ok, uint8_t err) {
  uint8_t id = roCur;
  if (id >= 1 && id <= MAX_NODES) { roState[id] = ok ? 3 : 4; roErr[id] = err; }
  Serial.printf("[ROLLOUT] Node %u: %s (Code %u)\n", id, ok ? "fertig" : "FEHLER", err);
  roQPos++; roPhase = RO_IDLE; roCur = 0;
}

void rolloutCancelReq() {
  if (!rolloutActive) return;
  roCancel = true;
  fwMsg = "Abbruch angefordert – der aktuelle Node wird noch fertig.";
}

// Zustandsautomat: Nodes nacheinander flashen (laeuft in loop)
void rolloutService() {
  if (!rolloutActive) return;
  uint32_t now = millis();
  if (roPhase == RO_IDLE) {
    if (roCancel || roQPos >= roQLen || !fsOk || !nodeFw.present) { rolloutFinish(); return; }
    uint8_t id = roQueue[roQPos];
    NodeInfo& n = nodes[id];
    if (!n.known || !n.active || !n.ip) {
      roCur = id; rolloutNodeDone(false, RO_ERR_OFFLINE);
      return;
    }
    roCur = id; roState[id] = 2; roPhase = RO_SEND;
    roTries = 0; roLastSend = now; roPhaseAt = now; roRegAt = n.regCount;
    n.otaState = 0; n.otaPct = 0; n.otaErr = 0;
    Serial.printf("[ROLLOUT] Node %u (%u/%u)\n", id, roQPos + 1, roQLen);
    return;
  }
  uint8_t id = roCur;
  if (id < 1 || id > MAX_NODES) { roPhase = RO_IDLE; roQPos++; return; }
  NodeInfo& n = nodes[id];
  bool reReg = (n.regCount != roRegAt) && n.statusSeen;   // neu angemeldet + STATUS erhalten
  if (reReg && fwEqualsStored(n))   { rolloutNodeDone(true, 0); return; }
  if (n.otaState == OTA_ST_FAILED)  { rolloutNodeDone(false, n.otaErr ? n.otaErr : OTA_ERR_DOWNLOAD); return; }
  if (roPhase == RO_SEND) {
    if (n.otaState == OTA_ST_STARTED || n.otaState == OTA_ST_PROGRESS || n.otaState == OTA_ST_SUCCESS) {
      roPhase = RO_WAIT; roPhaseAt = now; roRegAt = n.regCount;   // ab jetzt auf Neustart warten
      return;
    }
    if (roTries == 0 || (uint32_t)(now - roLastSend) >= 1500UL) {
      if (roTries >= 4) { rolloutNodeDone(false, RO_ERR_NOANSWER); return; }
      uint32_t sz = nodeFw.size;
      sendPkt(IPAddress(n.ip), PKT_OTA, id, sz & 0xFF, (sz >> 8) & 0xFF, (sz >> 16) & 0xFF, (sz >> 24) & 0xFF);
      roTries++; roLastSend = now;
    }
  } else {   // RO_WAIT
    if (reReg) { rolloutNodeDone(false, RO_ERR_VERSION); return; }   // neu gestartet, aber alte Version
    if ((uint32_t)(now - roPhaseAt) >= 120000UL) rolloutNodeDone(false, RO_ERR_TIMEOUT);
  }
}

// ── Master-Selbst-Update (POST /update) ──
void webHandleUpdateUpload() {
  wdtFeed();                       // auch bei abgelehnten Uploads: der Body wird trotzdem komplett gelesen
  if (!upMultipart()) return;
  HTTPUpload& up = webServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    upBegin(UP_MASTER);
    Serial.printf("[OTA] Master-Update: %s\n", up.filename.c_str());
    if (gameMode != GAME_IDLE || rolloutActive) { upError("Nicht moeglich, waehrend ein Spiel oder Update laeuft."); return; }
    if (restartAt) { upError("Neustart laeuft bereits."); return; }
    markAppValid("Update gestartet");   // laufende Firmware funktioniert offensichtlich
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upStarted || upFail || upKind != UP_MASTER) return;
    size_t off = 0;
    if (!upHdrOk) {
      off = upCollectHdr(up.buf, up.currentSize);
      if (upHdrLen < IMG_HDR_LEN) return;            // Kopf noch nicht komplett
      String e = imageCheck(upHdr, CTF_ROLE_MASTER, upDesc);
      if (e.length()) { upError(e); return; }
      upHdrOk = true;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { upError(String("Update-Start fehlgeschlagen: ") + Update.errorString()); return; }
      if (Update.write(upHdr, IMG_HDR_LEN) != IMG_HDR_LEN) { upError(String("Schreibfehler: ") + Update.errorString()); return; }
      upTotal = IMG_HDR_LEN;
    }
    size_t rest = up.currentSize - off;
    if (rest > 0) {
      if (Update.write(up.buf + off, rest) != rest) { upError(String("Schreibfehler: ") + Update.errorString()); return; }
      upTotal += rest;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!upStarted || upFail || upKind != UP_MASTER) return;
    if (!upHdrOk) { upError("Datei zu klein – keine gueltige Firmware."); return; }
    if (!Update.end(true)) { upError(String("Pruefung fehlgeschlagen: ") + Update.errorString()); return; }
    upDone = true;
    upMsg = "Master-Firmware " + String(upDesc.version) + " installiert (" + String(upTotal) + " Bytes) – Neustart...";
    if (upDesc.proto != PROTO_VERSION)
      upMsg += " Achtung: anderes Protokoll (" + String(upDesc.proto) + " statt " + String(PROTO_VERSION) + ") – Nodes danach ebenfalls aktualisieren!";
    Serial.printf("[OTA] %s\n", upMsg.c_str());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    // ABORTED kann auch NACH einem erfolgreichen END kommen (Browser schon weg):
    // dann ist die neue Firmware bereits aktiviert -> trotzdem neu starten.
    if (upStarted && upKind == UP_MASTER) {
      if (upDone && !upFail) { fwMsg = upMsg; restartAt = (millis() + 800UL) | 1UL; }
      else upError("Upload abgebrochen.");
    }
    upStarted = false;   // final handler wird nach ABORTED nicht aufgerufen
  }
}

void webHandleUpdateResult() {
  bool mine = upStarted && upKind == UP_MASTER;
  bool ok   = mine && upDone && !upFail;
  String msg = mine ? upMsg : String("Keine Datei empfangen.");
  if (mine && !ok && msg.length() == 0) msg = "Update fehlgeschlagen.";
  upStarted = false;
  fwMsg = msg;
  if (webServer.hasArg("json")) {
    String j = "{\"ok\":"; j += ok ? "true" : "false";
    j += ",\"msg\":"; jsonStr(j, msg.c_str());
    j += ",\"version\":"; jsonStr(j, ok ? upDesc.version : "");
    j += "}";
    webServer.send(ok ? 200 : 400, "application/json", j);
  } else {
    webServer.send(200, "text/html",
      String("<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>")
      + (ok ? "<meta http-equiv='refresh' content='10;url=/'>" : "")
      + "<title>OTA</title><style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;"
      "display:flex;flex-direction:column;align-items:center;padding:40px}a{color:#a8dadc}</style></head><body>"
      + (ok ? "<h2>&#9989; Update erfolgreich! Neustart...</h2>" : "<h2>&#10060; Update fehlgeschlagen!</h2>")
      + "<p>" + htmlEsc(msg) + "</p><a href='/'>&#8592; Zurueck</a></body></html>");
  }
  // Neustart erst NACH der Antwort, in loop() (nie im Handler)
  if (ok) restartAt = (millis() + 800UL) | 1UL;
}

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
    "<p>Datei: <b>master.ino.bin</b> (Arduino IDE &rarr; Sketch &rarr; Kompilierte Bin&auml;rdatei exportieren)</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin' required>"
    "<button type='submit'>Firmware hochladen</button>"
    "</form><br><a href='/'>&#8592; Zurueck</a></body></html>");
}
#endif  // ESP32

// Markiert die laufende Firmware als gut (kein automatischer Rollback mehr)
void markAppValid(const char* why) {
#ifdef ESP32
  const esp_partition_t* p = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  bool pending = p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  esp_ota_mark_app_valid_cancel_rollback();
  appValidMarked = true;
  if (pending) Serial.printf("[OTA] Neue Firmware bestaetigt (%s) – kein Rollback mehr.\n", why);
#else
  (void)why;
#endif
}

void rollbackService() {
  if (appValidMarked) return;
  if (firstLoopAt == 0) { firstLoopAt = millis() | 1UL; return; }
  if ((uint32_t)(millis() - firstLoopAt) >= 20000UL) markAppValid("20 s stabil");
}

// ─────────────────────────────────────────────────────────────
// Pending-Aktionen ausfuehren (nur aus loop()!)
// ─────────────────────────────────────────────────────────────
void processPending() {
  if (pendingAction == ACT_NONE) return;
  uint8_t a = pendingAction, arg = pendingArg;
  pendingAction = ACT_NONE; pendingArg = 0;
  switch (a) {
    case ACT_START:     startGameNow(arg); break;
    case ACT_STOP:      stopGameNow();     break;
    case ACT_RECONNECT: reconnectNodes();  break;
    case ACT_FORGET:    forgetNodes();     break;
    case ACT_IDENTIFY:  identifyNode(arg); break;
#ifdef ESP32
    case ACT_ROLLOUT:   rolloutStart(arg != 0); break;
    case ACT_CANCEL:    rolloutCancelReq();     break;
    case ACT_FWDELETE:  nodeFwDelete();         break;
#endif
    default: break;
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
.row{display:flex;justify-content:space-between;align-items:center;padding:3px 0;gap:8px}
.badge{padding:3px 10px;border-radius:20px;font-size:.82rem;font-weight:600}
.idle{background:#21262d;color:#8b949e}.running{background:#0f5132;color:#3fb950}
label{display:block;color:#8b949e;font-size:.83rem;margin:9px 0 3px}
select,input[type=number],input[type=text],input[type=file]{width:100%;padding:8px 11px;background:#21262d;border:1px solid #30363d;border-radius:7px;color:#e6edf3;font-size:.93rem}
.hidden{display:none}
.btn{width:100%;padding:12px;border:none;border-radius:8px;font-size:.97rem;font-weight:700;cursor:pointer;margin-top:7px}
.btn:disabled{opacity:.4;cursor:not-allowed}
.btn-start{background:#238636;color:#fff}.btn-stop{background:#b91c1c;color:#fff}.btn-save{background:#0f3460;color:#a8dadc}.btn-reset{background:#6e40c9;color:#fff}
.btn-small{padding:8px;font-size:.82rem}.btn-grey{background:#21262d;color:#c9d1d9}
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
.pl{display:flex;align-items:center;gap:7px;margin:5px 0;font-size:.88rem}
.pl .pdot{width:10px;height:10px;border-radius:50%;flex-shrink:0;box-shadow:0 0 5px currentColor}
.pl .pdot.on{background:#3fb950;color:#3fb950}
.pl .pdot.off{background:#f85149;color:#f85149}
.pl .pname{flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pl .pstat{font-size:.74rem;color:#8b949e}
.pl.dead .pname{color:#6e7681}
.psig{display:flex;align-items:center;gap:3px;flex-shrink:0}
.sig{display:inline-flex;align-items:flex-end;gap:1px;height:12px}
.sig i{display:block;width:3px;background:#30363d;border-radius:1px}
.sig i.f{background:#3fb950}
.pfw{font-size:.72rem;color:#8b949e;min-width:34px;text-align:right;flex-shrink:0}
.pfw.bad{color:#f85149;font-weight:700}
.fbtn{background:#21262d;color:#a8dadc;border:1px solid #30363d;border-radius:6px;padding:3px 7px;font-size:.72rem;cursor:pointer;flex-shrink:0}
.fbtn:disabled{opacity:.35}
.pnote{font-size:.75rem;color:#e3b341;margin:-2px 0 5px 17px}
.pnote.red{color:#f85149}
.warn{background:#3d2c00;border:1px solid #e3b341;color:#e3b341;border-radius:10px;padding:10px 12px;margin-bottom:10px;font-size:.86rem;font-weight:600}
.warn.red{background:#3d1518;border-color:#f85149;color:#f85149}
.prog{height:10px;background:#21262d;border-radius:5px;overflow:hidden;margin-top:8px}
.prog div{height:100%;width:0;background:#3fb950;transition:width .2s}
.fwmsg{font-size:.84rem;margin-top:6px;color:#a8dadc;word-break:break-word}
.fwmsg.err{color:#f85149}
.bad{color:#f85149;font-weight:700}
.rorow{display:flex;justify-content:space-between;gap:8px;font-size:.84rem;padding:4px 0;border-bottom:1px solid #21262d}
.sub{font-size:.78rem;color:#8b949e;margin-top:4px}
hr{border:none;border-top:1px solid #30363d;margin:14px 0}
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

<div id="banners"></div>

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
  <button class="btn btn-save" id="namesBtn" onclick="saveNames()">&#128190; Speichern</button>
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
  <div id="roundDelayOpts" class="hidden">
    <label id="roundDelayLabel">&#9203; Rundenabstand (Sekunden)</label>
    <input type="number" id="roundDelay" value="10" min="0" max="30">
    <div id="roundDelayHint" style="font-size:.78rem;color:#8b949e;margin-top:3px"></div>
  </div>
  <div id="generalOpts">
    <label>Spielverlauf speichern (Anzahl)</label>
    <input type="number" id="historySize" value="20" min="3" max="20">
  </div>
  <button class="btn btn-start" id="startBtn" onclick="startGame()">&#9654; STARTEN</button>
  <button class="btn btn-stop" onclick="stopGame()">&#9632; STOPPEN</button>
  <button class="btn btn-reset" id="resetBtn" onclick="resetNodes()">&#128260; NODES RECONNECT</button>
  <button class="btn btn-grey btn-small" id="forgetBtn" onclick="forgetNodes()">&#128465; Node-Nummern neu vergeben</button>
  <a href="/update" class="btn" style="display:block;background:#0f3460;color:#a8dadc;text-decoration:none;text-align:center">&#128640; OTA Update</a>
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

<div class="card" id="fwCard">
  <h2>&#128190; Firmware &amp; OTA</h2>
  <div class="row"><span>Master-Firmware</span><strong id="mFw">-</strong></div>
  <div class="row"><span>Letzter Neustart</span><span id="mRst">-</span></div>
  <div id="mUpSec" class="hidden">
    <label>Master aktualisieren (master.ino.bin)</label>
    <input type="file" id="mFile" accept=".bin">
    <button class="btn btn-save" id="mUpBtn" onclick="upMaster()">&#11014; Master-Firmware hochladen</button>
    <div class="prog hidden"><div id="mBar"></div></div>
    <div class="fwmsg" id="mMsg"></div>
  </div>
  <div id="fwNodeSec" class="hidden">
    <hr>
    <div class="row"><span>Gespeicherte Node-Firmware</span><strong id="nFw">keine</strong></div>
    <div class="sub" id="fsInfo"></div>
    <label>Node-Firmware speichern (node.ino.bin)</label>
    <input type="file" id="nFile" accept=".bin">
    <button class="btn btn-save" id="nUpBtn" onclick="upNode()">&#11014; Node-Firmware hochladen</button>
    <div class="prog hidden"><div id="nBar"></div></div>
    <div class="fwmsg" id="nMsg"></div>
    <button class="btn btn-start" id="roBtn1" onclick="rollout(0)">&#128260; Veraltete Nodes aktualisieren</button>
    <button class="btn btn-reset" id="roBtn2" onclick="rollout(1)">&#9889; Alle Nodes neu flashen</button>
    <button class="btn btn-stop" id="roCancel" onclick="roStop()">&#9632; Abbrechen</button>
    <button class="btn btn-grey btn-small" id="fwDel" onclick="fwDelete()">&#128465; Gespeicherte Firmware l&ouml;schen</button>
    <div class="fwmsg" id="roState"></div>
    <div class="fwmsg" id="fwMsg"></div>
    <div id="roList"></div>
  </div>
  <details>
    <summary>&#8505; Welche Datei?</summary>
    <div class="instr">Arduino IDE &rarr; <b>Sketch &rarr; Kompilierte Bin&auml;rdatei exportieren</b>. Danach liegt im Sketch-Ordner unter <b>build/esp32.esp32.esp32/</b> die Datei <b>node.ino.bin</b> bzw. <b>master.ino.bin</b> &ndash; genau diese hochladen.<br>NICHT verwenden: <i>.merged.bin</i>, <i>.bootloader.bin</i>, <i>.partitions.bin</i>.<br>Ablauf Nodes: Node-Firmware hochladen &rarr; &bdquo;Veraltete Nodes aktualisieren&ldquo;. Die Nodes laden die Datei nacheinander vom Master und starten neu. Nodes mit alter Firmware 1.x m&uuml;ssen einmal per USB geflasht werden.</div>
  </details>
</div>

<script>
var modeNames=['Idle','CTF','Memory','Bomb','Reaktion','Simon Says','Heisse Kartoffel','King of the Hill','Tauziehen','Minesweeper','Knockout','Farbjagd','Whack-a-Mole'];
var tc=['#8b949e','#f85149','#388bfd','#3fb950','#e3b341'];
var knownNodes=-1;
var RST=['?','Einschalten','Reset-Taste','Software-Neustart','Absturz','Interrupt-Watchdog','Task-Watchdog','Watchdog','Deep-Sleep','Brownout','SDIO','USB','JTAG','eFuse','Spannungseinbruch','CPU-Lockup'];
var OERR={1:'keine Verbindung zum Master',2:'HTTP-Fehler',3:'Dateigroesse ungueltig',4:'Flash-Start fehlgeschlagen',5:'Download abgebrochen',6:'Schreibfehler',7:'Pruefung fehlgeschlagen',8:'Node nicht bereit',9:'Firmware passt nicht',20:'keine Antwort',21:'Zeitueberschreitung',22:'offline',23:'nach Neustart alte Version'};

function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function rstPower(r){return r==9||r==14;}
function rstCrash(r){return r==4||r==5||r==6||r==7||r==15;}
function rstWarn(r){if(rstPower(r))return (RST[r]||r)+' – Stromversorgung pruefen!';if(rstCrash(r))return 'Absturz/Watchdog ('+(RST[r]||r)+')';return '';}
function sig(r){var l=r>=-55?4:r>=-65?3:r>=-75?2:r>=-85?1:0,h='<span class="sig" title="'+r+' dBm">';for(var i=1;i<=4;i++)h+='<i'+(i<=l?' class="f"':'')+' style="height:'+(i*3)+'px"></i>';return h+'</span><span class="pstat">'+r+'</span>';}
function dis(id,v){var e=document.getElementById(id);if(e)e.disabled=!!v;}

var instrs={
  1:"Nodes durch Druecken fuer dein Team beanspruchen. Button wechselt Farbe (neutral&rarr;rot&rarr;blau). Nach der Zeit gewinnt das Team mit den meisten Nodes.",
  2:"Nodes leuchten 2s auf &ndash; merke dir die Farben. Druecke zwei gleich-farbige Nodes nacheinander. Kein Treffer? Beide gehen wieder aus.",
  3:"Ein Node blinkt ROT = Bombe! Die Sequenz der anderen Nodes zeigt die Reihenfolge zum Entschaerfen. Bombe druecken = Sequenz nochmal zeigen.",
  4:"Ein zufaelliger Node leuchtet GELB. Wer zuerst drueckt, bekommt einen Punkt. Reaktionszeit wird gemessen.",
  5:"Jeder Node hat eine feste Farbe (Node 1=ROT, 2=BLAU …). Beim Start kurz alle Farben merken! Simon zeigt eine Sequenz &ndash; jeder Node leuchtet in seiner Farbe. Reihenfolge nachdr&uuml;cken. Jede Runde wird die Sequenz um einen Schritt l&auml;nger. Falscher Druck = Aus. W&auml;hrend Eingabe zeigen Nodes ihre Farbe gedimmt als Erinnerung.",
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
  // Rundenabstand / Sperrdauer je nach Spielmodus
  var rdShow=(m===1||m===4||m===10||m===12);
  document.getElementById('roundDelayOpts').classList.toggle('hidden',!rdShow);
  if(rdShow){
    if(m===1){
      document.getElementById('roundDelayLabel').innerHTML='&#128274; Sperrdauer nach Einnahme (Sekunden)';
      document.getElementById('roundDelayHint').innerHTML='Node ist nach dem Druecken fuer diese Zeit gesperrt &ndash; schrumpfender Balken zeigt Restzeit. Gegner muss warten bevor er zurueckdruecken kann. 0 = keine Sperre.';
    } else {
      document.getElementById('roundDelayLabel').innerHTML='&#9203; Rundenabstand (Sekunden)';
      document.getElementById('roundDelayHint').innerHTML='Pflichtpause nach jedem Treffer &ndash; Gegner kann nicht einfach nachmachen. Schrumpfender Balken zeigt Restzeit. 0 = sofort.';
    }
  }
  var instrEl=document.getElementById('instrText');
  if(instrs[m]){instrEl.innerHTML=instrs[m];document.getElementById('instrDetails').classList.remove('hidden');}
  else{document.getElementById('instrDetails').classList.add('hidden');}
}

function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});}
// POST mit Fehlermeldung (z.B. "Firmware-Update laeuft")
function act(u,b){return post(u,b).then(function(r){if(!r.ok)r.text().then(function(t){alert(t);});return r;}).catch(function(){alert('Keine Verbindung zum Master.');});}

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
  if(m==='1'||m==='4'||m==='10'||m==='12') q+='&rounddelay='+document.getElementById('roundDelay').value;
  q+='&history='+document.getElementById('historySize').value;
  act('/start',q);
}
function stopGame(){act('/stop','');}
function resetNodes(){act('/reset','').then(function(){var b=document.getElementById('resetBtn');var orig=b.innerHTML;b.textContent='Gesendet...';setTimeout(function(){b.innerHTML=orig;},1500);});}
function forgetNodes(){if(!confirm('Alle Node-Nummern vergessen?\nDie Nodes melden sich neu an und bekommen neue Nummern (Namen bleiben je Nummer). Ein laufendes Spiel wird beendet.'))return;act('/forget','');}
function ident(id){act('/identify','id='+id);}

function saveNames(){
  var q='';
  for(var i=1;i<=16;i++){var el=document.getElementById('n'+i);if(el)q+='&n'+i+'='+encodeURIComponent(el.value||'');}
  post('/names',q.slice(1)).then(function(){var b=document.getElementById('namesBtn');b.textContent='✓ Gespeichert';setTimeout(function(){b.innerHTML='&#128190; Speichern';},1500);});
}

function clearScores(){post('/clearscores','').then(function(){document.getElementById('hsTbody').innerHTML='';document.getElementById('hsCard').classList.add('hidden');});}

function updateNameInputs(n,nl){
  var c=document.getElementById('nameInputs');
  while(c.children.length<n){
    var idx=c.children.length+1;
    var d=document.createElement('div');d.className='name-row';
    d.innerHTML='<span class="node-lbl">Node '+idx+'</span><input type="text" id="n'+idx+'" placeholder="Spieler '+idx+'" maxlength="18">';
    c.appendChild(d);
    var nm='';nl.forEach(function(x){if(x.id===idx)nm=x.name;});
    if(nm&&nm!=='Spieler '+idx)document.getElementById('n'+idx).value=nm;
  }
  while(c.children.length>n)c.removeChild(c.lastChild);
  document.getElementById('namesCard').classList.toggle('hidden',n===0);
}

// Node-Liste: Zeilen bleiben bestehen (nur Inhalt wird aktualisiert), damit "Finden" klickbar bleibt
var rows={};
function renderNodes(nl,mfw){
  var nn=document.getElementById('nodeNames'),seen={};
  nl.forEach(function(n){
    var r=rows[n.id];
    if(!r){
      r=document.createElement('div');
      r.innerHTML='<div class="pl"><span class="pdot"></span><span class="pname"></span><span class="psig"></span><span class="pfw"></span><button class="fbtn" onclick="ident('+n.id+')">Finden</button></div><div class="pnote hidden"></div>';
      rows[n.id]=r;
    }
    seen[n.id]=1;
    var pl=r.firstChild,c=pl.children;
    pl.className='pl'+(n.on?'':' dead');
    c[0].className='pdot '+(n.on?'on':'off');
    c[1].textContent=n.id+'. '+n.name;
    var sh=n.on?sig(n.rssi):'<span class="pstat">offline</span>';
    if(c[2]._h!==sh){c[2].innerHTML=sh;c[2]._h=sh;}
    var bad=!!(n.fw&&n.fw!==mfw);
    c[3].className='pfw'+(bad?' bad':'');c[3].textContent=n.fw||'';c[3].title=bad?'andere Firmware als der Master ('+mfw+')':'Firmware';
    c[4].disabled=!n.on;
    var w=rstWarn(n.rst),pn=r.lastChild;
    pn.className='pnote'+(w?'':' hidden')+(rstPower(n.rst)?' red':'');
    pn.textContent=w?('⚠ Letzter Neustart: '+w):'';
    if(r.parentNode!==nn)nn.appendChild(r);
  });
  for(var k in rows){if(!seen[k]){if(rows[k].parentNode)rows[k].parentNode.removeChild(rows[k]);delete rows[k];}}
}

function renderBanners(d,nl){
  var b=[];
  if(d.legacy&&d.legacy.length)b.push(['','Node(s) mit alter Firmware 1.x gefunden ('+d.legacy.map(esc).join(', ')+') &ndash; einmal per USB mit der neuen Node-Firmware flashen.']);
  if(d.apMax&&d.stations>=d.apMax)b.push(['red','WLAN voll: max. '+(d.apMax-1)+' Nodes + 1 Handy. Weitere Ger&auml;te k&ouml;nnen sich nicht verbinden.']);
  var pw=[];nl.forEach(function(n){if(rstPower(n.rst))pw.push(n.id);});
  if(pw.length)b.push(['red','Brownout bei Node '+pw.join(', ')+' &ndash; Stromversorgung pr&uuml;fen (Netzteil, Kabel, Akku).']);
  if(rstPower(d.masterRst))b.push(['red','Master wurde wegen Unterspannung neu gestartet &ndash; Stromversorgung pr&uuml;fen!']);
  var od=[];nl.forEach(function(n){if(n.on&&n.fw&&n.fw!==d.masterFw)od.push(n.id);});
  if(od.length)b.push(['','Node '+od.join(', ')+' mit anderer Firmware als der Master ('+esc(d.masterFw)+') &ndash; unten unter &bdquo;Firmware &amp; OTA&ldquo; aktualisieren.']);
  if(d.rollout&&d.rollout.active)b.push(['','Firmware-Update der Nodes l&auml;uft &ndash; Spiele sind solange gesperrt.']);
  var h=b.map(function(x){return '<div class="warn '+x[0]+'">&#9888; '+x[1]+'</div>';}).join('');
  var el=document.getElementById('banners');if(el._h!==h){el.innerHTML=h;el._h=h;}
}

var upBusy=false;
function upFile(url,fileId,barId,msgId,cb){
  var inp=document.getElementById(fileId),f=inp.files&&inp.files[0],m=document.getElementById(msgId);
  if(!f){m.className='fwmsg err';m.textContent='Bitte zuerst eine .bin-Datei auswaehlen.';return;}
  if(/merged|bootloader|partitions/i.test(f.name)){m.className='fwmsg err';m.textContent='Falsche Datei ('+f.name+'): bitte die <sketch>.ino.bin verwenden, nicht .merged/.bootloader/.partitions.bin.';return;}
  var bar=document.getElementById(barId);bar.parentNode.classList.remove('hidden');bar.style.width='0%';
  m.className='fwmsg';m.textContent='Lade hoch...';upBusy=true;
  var fd=new FormData();fd.append('firmware',f,f.name);
  var x=new XMLHttpRequest();x.open('POST',url,true);
  x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);bar.style.width=p+'%';m.textContent=p<100?('Lade hoch... '+p+' %'):'Wird geprueft...';}};
  x.onload=function(){upBusy=false;var r;try{r=JSON.parse(x.responseText);}catch(e){r={ok:false,msg:'Unerwartete Antwort (HTTP '+x.status+')'};}
    m.className='fwmsg'+(r.ok?'':' err');m.textContent=r.msg||'';if(!r.ok)bar.style.width='0%';if(cb)cb(r);};
  x.onerror=function(){upBusy=false;m.className='fwmsg err';m.textContent='Verbindung abgebrochen.';bar.style.width='0%';};
  x.send(fd);
}
function upMaster(){upFile('/update?json=1','mFile','mBar','mMsg',function(r){if(r.ok){upBusy=true;document.getElementById('mMsg').textContent=r.msg+' Seite wird in 10 s neu geladen.';setTimeout(function(){location.reload();},10000);}});}
function upNode(){upFile('/fw/node','nFile','nBar','nMsg',null);}
function rollout(f){if(f&&!confirm('Alle aktiven Nodes neu flashen (auch aktuelle)?'))return;act('/fw/rollout','force='+(f?1:0));}
function roStop(){act('/fw/cancel','');}
function fwDelete(){if(!confirm('Gespeicherte Node-Firmware loeschen?'))return;act('/fw/delete','');}

function renderFw(d,nl){
  document.getElementById('mFw').textContent=d.masterFw||'?';
  var r=d.masterRst||0,w=rstWarn(r),mr=document.getElementById('mRst');
  mr.textContent=w?w:(RST[r]||r);mr.className=w?'bad':'';
  var hasFs=!!d.fs;
  document.getElementById('mUpSec').classList.toggle('hidden',!hasFs);
  document.getElementById('fwNodeSec').classList.toggle('hidden',!hasFs);
  var ro=d.rollout||{},roAct=!!ro.active;
  dis('startBtn',roAct);dis('forgetBtn',roAct);
  if(!hasFs)return;
  var nf=d.nodeFw||{},run=!!d.running;
  document.getElementById('nFw').textContent=nf.present?(nf.version+' ('+Math.round(nf.size/1024)+' KB)'):'keine';
  document.getElementById('fsInfo').textContent=d.fs.ok?('Speicher: '+Math.round(d.fs.used/1024)+' von '+Math.round(d.fs.total/1024)+' KB belegt'):'Dateisystem nicht verfuegbar!';
  dis('mUpBtn',roAct||upBusy||run);dis('nUpBtn',roAct||upBusy||run||!d.fs.ok);
  dis('roBtn1',roAct||upBusy||run||!nf.present);dis('roBtn2',roAct||upBusy||run||!nf.present);
  dis('fwDel',roAct||upBusy||!nf.present);dis('roCancel',!roAct||ro.cancel);
  document.getElementById('fwMsg').textContent=d.fwMsg||'';
  document.getElementById('roState').textContent=roAct?(ro.cancel?'Wird nach dem aktuellen Node beendet...':'Update laeuft – Nodes bitte eingeschaltet lassen!'):(run?'Waehrend eines Spiels gesperrt.':'');
  var byId={};nl.forEach(function(n){byId[n.id]=n;});
  function nm(id){return 'Node '+id+(byId[id]?' ('+esc(byId[id].name)+')':'');}
  function row(a,b,c){return '<div class="rorow"><span>'+a+'</span><span style="color:'+c+'">'+b+'</span></div>';}
  var h='';
  if(roAct&&ro.cur){var n=byId[ro.cur]||{},o=n.ota||{};
    var s=o.st==2?('l&auml;dt '+o.pct+' %'):o.st==1?'Download startet...':o.st==4?'geflasht, startet neu...':(ro.phase==2?'warte auf Neustart...':'wird angesprochen...');
    h+=row(nm(ro.cur),s,'#e3b341');}
  (ro.queue||[]).forEach(function(id){h+=row(nm(id),'wartet','#8b949e');});
  (ro.done||[]).forEach(function(id){h+=row(nm(id),'&#10003; fertig','#3fb950');});
  (ro.failed||[]).forEach(function(f){h+=row(nm(f.id),'&#10007; '+esc(OERR[f.err]||('Fehler '+f.err)),'#f85149');});
  var rl=document.getElementById('roList');if(rl._h!==h){rl.innerHTML=h;rl._h=h;}
}

function fmtTime(s){if(s<=0)return'0:00';return Math.floor(s/60)+':'+(s%60<10?'0':'')+s%60;}

var stBusy=0;
function updateStatus(){
  if(upBusy)return;                                   // waehrend Upload antwortet der Master nicht
  if(stBusy&&Date.now()-stBusy<5000)return;           // vorige Abfrage laeuft noch
  stBusy=Date.now();
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    stBusy=0;
    var nl=d.nodeList||[];
    var online=0;nl.forEach(function(n){if(n.on)online++;});
    document.getElementById('nodeCount').textContent=online+' / '+nl.length;
    if(d.nodes!==knownNodes){knownNodes=d.nodes;updateNameInputs(d.nodes,nl);}

    // Node-Chips: gruen=verbunden, rot=offline
    var g=document.getElementById('nodesGrid');g.innerHTML='';
    if(nl.length===0){var ph=document.createElement('div');ph.className='nd';ph.textContent='–';g.appendChild(ph);}
    nl.forEach(function(n){var dot=document.createElement('div');dot.className='nd'+(n.on?' on':' off');dot.textContent=n.id;dot.title=n.name+(n.on?' (verbunden)':' (offline)');g.appendChild(dot);});

    // Spielerliste mit Verbindungsstatus, Signal, Firmware, Finden
    renderNodes(nl,d.masterFw);
    renderBanners(d,nl);
    renderFw(d,nl);

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
        var locked=n.lock&&n.lock>0;
        var chip=document.createElement('div');chip.className='cnchip'+(n.on?'':' off');
        var lockBadge=locked?'<div style="font-size:.65rem;color:#e3b341;margin-top:2px">&#128274;'+n.lock+'s</div>':'';
        chip.innerHTML='<div class="dot" style="background:'+tc[t]+';'+(locked?'opacity:.45':'')+'"></div>'
          +'<div class="lbl">'+n.id+'. '+esc(n.name)+'</div>'+lockBadge;
        chip.title=n.name+' → '+(['Neutral','ROT','BLAU','GRUEN','GELB'][t]||'?')+(locked?' (gesperrt '+n.lock+'s)':'');
        nc.appendChild(chip);});
    } else cs.classList.add('hidden');

    // Generischer Live-Spielstand (Memory, Kartoffel, King, Knockout, Farbjagd)
    var lc=document.getElementById('liveScoreCard');
    if(d.running&&d.scoreLabel&&d.scoreLabel.length>0&&nl.length>0){
      lc.classList.remove('hidden');
      document.getElementById('liveScoreTitle').innerHTML='&#127919; Spielstand &ndash; '+esc(d.scoreLabel);
      var lv=document.getElementById('liveScores');lv.innerHTML='';
      var arr=nl.slice().sort(function(a,b){return b.v-a.v;});
      var mxv=1;arr.forEach(function(n){if(n.v>mxv)mxv=n.v;});
      arr.forEach(function(n,i){
        var row=document.createElement('div');row.className='srow';
        var pct=mxv>0?Math.round(n.v/mxv*100):0;
        var col=!n.on?'#6e7681':i==0?'#3fb950':i==1?'#388bfd':'#8b949e';
        row.innerHTML='<div class="rank">'+(i+1)+'</div>'
          +'<div class="sname"'+(n.on?'':' style="color:#6e7681"')+'>'+esc(n.name)+(n.on?'':' &#9888;')+'</div>'
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
          +'<div class="sname">'+esc(p.name)+'</div>'
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
        tr2.innerHTML='<td'+cls+'>'+(i+1)+'</td><td>'+esc(h.name)+'</td><td style="font-weight:700">'+h.points+'</td><td>'+h.bestMs+'ms</td>';
        tb.appendChild(tr2);});
    }
    var hc=document.getElementById('histCard');
    if(d.history&&d.history.length>0){
      hc.classList.remove('hidden');
      var hb=document.getElementById('histTbody');hb.innerHTML='';
      d.history.forEach(function(h,i){
        var tr3=document.createElement('tr');
        tr3.innerHTML='<td>'+(d.totalGames-i)+'</td>'
          +'<td><span class="hist-mode">'+esc(modeNames[h.mode]||h.mode)+'</span></td>'
          +'<td style="font-weight:600">'+esc(h.winner)+'</td>'
          +'<td>'+h.score+'</td>';
        hb.appendChild(tr3);});
    } else hc.classList.add('hidden');
  }).catch(function(){stBusy=0;});
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

// Setzt eine Web-Aktion (Ausfuehrung spaeter in loop). Antwortet sofort.
bool webSetPending(uint8_t a, uint8_t arg) {
  if (pendingAction != ACT_NONE) { webServer.send(503, "text/plain", "Bitte kurz warten und nochmal versuchen."); return false; }
  pendingAction = a; pendingArg = arg;
  webServer.send(200, "text/plain", "OK");
  return true;
}

void webHandleStatus() {
  uint32_t now = millis();
  uint32_t timeLeft=0;
  if (gameMode!=GAME_IDLE && gameEndTime!=0) {
    long r=(long)(gameEndTime-now);
    timeLeft=(r>0)?(uint32_t)r/1000:0;
  }

  String j; j.reserve(6144);
  j += "{\"nodes\":";   j += nodeCount;
  j += ",\"mode\":";    j += gameMode;
  j += ",\"running\":"; j += (gameMode!=GAME_IDLE ? "true" : "false");
  j += ",\"timeLeft\":"; j += timeLeft;

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
  j += ",\"scoreLabel\":"; jsonStr(j, scoreLabel);
  j += ",\"nodeList\":[";
  bool first = true;
  for (uint8_t i=1;i<=nodeCount;i++) {
    NodeInfo& n = nodes[i];
    if (!n.known) continue;
    uint16_t v=0;
    switch (gameMode) {
      case GAME_MEMORY:    v=memMatched[i]?1:0; break;
      case GAME_HOTPOTATO: v=potatoLives[i];    break;
      case GAME_KINGHILL: {
        uint32_t hold=kingHoldTime[i];
        if (kingHolder==i && kingLastCapture!=0) hold+=now-kingLastCapture;
        v=hold/1000; break;
      }
      case GAME_KNOCKOUT:  v=knockLives[i];  break;
      case GAME_COLORHUNT:  v=huntScores[i];  break;
      case GAME_WHACKAMOLE: v=whamScores[i];  break;
    }
    uint32_t lockSec=0;
    if (gameMode==GAME_CTF && ctfLockUntil[i]!=0 && (long)(ctfLockUntil[i]-now)>0) lockSec=(ctfLockUntil[i]-now+999)/1000;
    if (!first) j += ',';
    first = false;
    j += "{\"id\":"; j += i;
    j += ",\"name\":"; jsonStr(j, players[i].name);
    j += ",\"on\":"; j += (n.active ? "true" : "false");
    j += ",\"team\":"; j += (gameMode==GAME_CTF ? ctfTeam[i] : 0);
    j += ",\"lock\":"; j += lockSec;
    j += ",\"v\":"; j += v;
    j += ",\"rssi\":"; j += (int)n.rssi;
    j += ",\"fw\":\"";
    if (n.fw[0] || n.fw[1] || n.fw[2]) { j += n.fw[0]; j += '.'; j += n.fw[1]; j += '.'; j += n.fw[2]; }
    j += "\",\"rst\":"; j += n.resetReason;
    j += ",\"flags\":"; j += n.flags;
    j += ",\"ota\":{\"st\":"; j += n.otaState; j += ",\"pct\":"; j += n.otaPct; j += ",\"err\":"; j += n.otaErr; j += '}';
    j += ",\"inGame\":"; j += (inGame[i] ? "true" : "false");
    j += ",\"ack\":"; j += (n.ledAcked == n.ledSeq ? "true" : "false");
    j += '}';
  }
  j += ']';

  // Info-Zeile fuer Spiele mit gemeinsamem Zustand (kein Pro-Node-Score)
  String info="";
  if (gameMode==GAME_BOMB) {
    info="Bombe bei Node "+String(bombNode)+" - Schritt "+String(bombStep)+"/"+String(bombSeqLen);
  } else if (gameMode==GAME_SIMON) {
    info="Simon-Sequenz: Laenge "+String(simonLen);
  } else if (gameMode==GAME_TUGWAR) {
    if      (tugScore>50) info="Tauziehen: ROT fuehrt";
    else if (tugScore<50) info="Tauziehen: BLAU fuehrt";
    else                  info="Tauziehen: Gleichstand";
  } else if (gameMode==GAME_MINESWEEPER) {
    info="Sicher gedeckt: "+String(mineScore)+"/"+String(mineSafeCount)+" - Leben: "+String(mineLives);
  }
  j += ",\"info\":"; jsonStr(j, info.c_str());

  if (gameMode==GAME_CTF) {
    j += ",\"scores\":{";
    for (uint8_t t=1;t<=cfgTeams;t++) {
      uint8_t s=0; for(uint8_t i=1;i<=nodeCount;i++) if(nodes[i].known && ctfTeam[i]==t) s++;
      j += '"'; j += t; j += "\":"; j += s;
      if (t<cfgTeams) j += ',';
    }
    j += '}';
    uint8_t neu=0; for(uint8_t i=1;i<=nodeCount;i++) if(nodes[i].known && ctfTeam[i]==0) neu++;
    j += ",\"neutral\":"; j += neu;
  }
  if (gameMode==GAME_REACTION) {
    j += ",\"round\":"; j += reactRound;
    j += ",\"totalRounds\":"; j += cfgReactRounds;
    j += ",\"reactScores\":[";
    bool f2 = true;
    for (uint8_t i=1;i<=nodeCount;i++) {
      if (!nodes[i].known) continue;
      if (!f2) j += ',';
      f2 = false;
      j += "{\"id\":"; j += i;
      j += ",\"name\":"; jsonStr(j, players[i].name);
      j += ",\"points\":"; j += players[i].points;
      j += ",\"bestMs\":"; j += players[i].bestMs;
      j += '}';
    }
    j += ']';
  }

  j += ",\"highscores\":[";
  for (uint8_t i=0;i<highScoreCount;i++) {
    if (i) j += ',';
    j += "{\"name\":"; jsonStr(j, highScores[i].name);
    j += ",\"points\":"; j += highScores[i].points;
    j += ",\"bestMs\":"; j += highScores[i].bestMs;
    j += '}';
  }
  j += ']';

  j += ",\"history\":[";
  uint8_t cap2 = (cfgMaxHistory < MAX_HISTORY) ? cfgMaxHistory : MAX_HISTORY;
  uint8_t hc = (historyCount < cap2) ? historyCount : cap2;
  for (int8_t i=(int8_t)hc-1; i>=0; i--) {
    j += "{\"mode\":"; j += gameHistory[i].mode;
    j += ",\"winner\":"; jsonStr(j, gameHistory[i].winner);
    j += ",\"score\":"; j += gameHistory[i].score;
    j += '}';
    if (i>0) j += ',';
  }
  j += ']';
  j += ",\"totalGames\":"; j += totalGames;

  // System / Firmware
  j += ",\"masterFw\":"; jsonStr(j, MASTER_FW_STR);
  j += ",\"masterRst\":"; j += masterResetReason;
  j += ",\"uptime\":"; j += now / 1000;
  j += ",\"stations\":"; j += WiFi.softAPgetStationNum();
  j += ",\"apMax\":"; j += apMaxConn;
  j += ",\"channel\":"; j += apChannel;
  j += ",\"rxDrops\":"; j += (uint32_t)rxDrops;
  j += ",\"legacy\":[";
  first = true;
  for (uint8_t k = 0; k < 8; k++) {
    if (!legacyNodes[k].ip || (uint32_t)(now - legacyNodes[k].t) >= 60000UL) continue;
    if (!first) j += ',';
    first = false;
    jsonStr(j, IPAddress(legacyNodes[k].ip).toString().c_str());
  }
  j += ']';
#ifdef ESP32
  j += ",\"fs\":{\"ok\":"; j += (fsOk ? "true" : "false");
  j += ",\"total\":"; j += fsTotal; j += ",\"used\":"; j += fsUsed; j += '}';
  j += ",\"nodeFw\":{\"present\":"; j += (nodeFw.present ? "true" : "false");
  j += ",\"version\":"; jsonStr(j, nodeFw.present ? nodeFw.version : "");
  j += ",\"size\":"; j += nodeFw.size; j += '}';
  j += ",\"rollout\":{\"active\":"; j += (rolloutActive ? "true" : "false");
  j += ",\"cancel\":"; j += (roCancel ? "true" : "false");
  j += ",\"cur\":"; j += roCur;
  j += ",\"phase\":"; j += roPhase;
  const char* keys[3] = {"queue", "done", "failed"};
  const uint8_t states[3] = {1, 3, 4};
  for (uint8_t s = 0; s < 3; s++) {
    j += ",\""; j += keys[s]; j += "\":[";
    first = true;
    for (uint8_t i = 1; i <= MAX_NODES; i++) {
      if (roState[i] != states[s]) continue;
      if (!first) j += ',';
      first = false;
      if (states[s] == 4) { j += "{\"id\":"; j += i; j += ",\"err\":"; j += roErr[i]; j += '}'; }
      else j += i;
    }
    j += ']';
  }
  j += '}';
  j += ",\"fwMsg\":"; jsonStr(j, fwMsg.c_str());
#endif
  j += '}';

  webServer.sendHeader("Access-Control-Allow-Origin","*");
  webServer.sendHeader("Cache-Control","no-store");
  webServer.send(200,"application/json",j);
}

void webHandleStart() {
  if (!webServer.hasArg("mode")) { webServer.send(400,"text/plain","missing mode"); return; }
  int m=webServer.arg("mode").toInt();
  if (m < GAME_CTF || m > GAME_WHACKAMOLE) { webServer.send(400,"text/plain","Unbekannter Spielmodus"); return; }
  if (rolloutActive) { webServer.send(409,"text/plain","Firmware-Update der Nodes laeuft – Spielstart erst danach moeglich."); return; }
  if (pendingAction != ACT_NONE) { webServer.send(503,"text/plain","Bitte kurz warten und nochmal versuchen."); return; }
  if (webServer.hasArg("teams"))    cfgTeams      = constrain(webServer.arg("teams").toInt(),2,4);
  if (webServer.hasArg("duration")) cfgDuration   = constrain(webServer.arg("duration").toInt(),30,600);
  if (webServer.hasArg("seqLen"))   cfgSeqLen     = constrain(webServer.arg("seqLen").toInt(),3,12);
  if (webServer.hasArg("duration")&&m==3) cfgBombDur = constrain(webServer.arg("duration").toInt(),30,300);
  if (webServer.hasArg("rounds"))   cfgReactRounds = constrain(webServer.arg("rounds").toInt(),3,20);
  if (webServer.hasArg("mines"))    cfgMines       = constrain(webServer.arg("mines").toInt(),1,nodeCount>1?nodeCount-1:1);
  if (webServer.hasArg("klives"))   cfgKnockLives  = constrain(webServer.arg("klives").toInt(),1,5);
  if (webServer.hasArg("hrounds"))  cfgHuntRounds  = constrain(webServer.arg("hrounds").toInt(),3,20);
  if (webServer.hasArg("whamrounds"))  cfgWhamRounds  = constrain(webServer.arg("whamrounds").toInt(),5,30);
  if (webServer.hasArg("history"))     cfgMaxHistory  = constrain(webServer.arg("history").toInt(),3,20);
  if (webServer.hasArg("rounddelay")) cfgRoundDelay  = constrain(webServer.arg("rounddelay").toInt(),0,30);
  // duration used by king and tug too
  if (webServer.hasArg("duration")&&m==7) cfgDuration = constrain(webServer.arg("duration").toInt(),30,600);
  if (webServer.hasArg("duration")&&m==8) cfgDuration = constrain(webServer.arg("duration").toInt(),30,600);
  webSetPending(ACT_START, (uint8_t)m);
}

void webHandleStop()  { webSetPending(ACT_STOP, 0); }
void webHandleReset() { webSetPending(ACT_RECONNECT, 0); }

void webHandleForget() {
  if (rolloutActive) { webServer.send(409,"text/plain","Nicht moeglich, waehrend die Nodes aktualisiert werden."); return; }
  webSetPending(ACT_FORGET, 0);
}

void webHandleIdentify() {
  int id = webServer.arg("id").toInt();
  if (id < 1 || id > MAX_NODES || !nodes[id].known) { webServer.send(400,"text/plain","Unbekannte Node"); return; }
  if (!nodes[id].active) { webServer.send(409,"text/plain","Node ist offline"); return; }
  webSetPending(ACT_IDENTIFY, (uint8_t)id);
}

// Namen: hoechstens 19 Byte, keine Steuerzeichen, keine halben UTF-8-Zeichen
void setPlayerName(uint8_t i, const String& v) {
  char buf[20]; size_t o = 0;
  for (size_t k = 0; k < v.length() && o < 19; k++) {
    uint8_t c = (uint8_t)v[k];
    if (c < 0x20 || c == 0x7F) continue;
    buf[o++] = (char)c;
  }
  // abgeschnittenes Mehrbyte-Zeichen am Ende entfernen
  if (o > 0) {
    size_t s = o - 1;
    while (s > 0 && ((uint8_t)buf[s] & 0xC0) == 0x80) s--;
    uint8_t lead = (uint8_t)buf[s];
    size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
    if (o - s < need) o = s;
  }
  buf[o] = 0;
  if (o == 0) snprintf(players[i].name, 20, "Spieler %u", i);
  else        memcpy(players[i].name, buf, o + 1);
}

void webHandleNames() {
  for (uint8_t i=1;i<=MAX_NODES;i++) {
    String key="n"+String(i);
    if (webServer.hasArg(key)) {
      String v=webServer.arg(key);
      v.trim();
      setPlayerName(i, v);
    }
  }
  saveNames();
  webServer.send(200,"text/plain","OK");
}

void webHandleClearScores() {
  highScoreCount=0;
  memset(highScores,0,sizeof(highScores));
  historyCount=0; totalGames=0;
  webServer.send(200,"text/plain","OK");
}

#ifdef ESP32
void webHandleRollout() {
  if (rolloutActive)        { webServer.send(409,"text/plain","Rollout laeuft bereits."); return; }
  if (gameMode!=GAME_IDLE)  { webServer.send(409,"text/plain","Spiel laeuft – bitte erst stoppen."); return; }
  if (!nodeFw.present)      { webServer.send(409,"text/plain","Keine Node-Firmware gespeichert – bitte zuerst hochladen."); return; }
  webSetPending(ACT_ROLLOUT, webServer.arg("force").toInt() == 1 ? 1 : 0);
}
void webHandleFwCancel() { webSetPending(ACT_CANCEL, 0); }
void webHandleFwDelete() {
  if (rolloutActive) { webServer.send(409,"text/plain","Nicht moeglich, waehrend die Nodes aktualisiert werden."); return; }
  webSetPending(ACT_FWDELETE, 0);
}
#endif

// ─────────────────────────────────────────────────────────────
// Serielles Menü (Aktionen laufen wie im Web ueber processPending)
// ─────────────────────────────────────────────────────────────
const char* resetReasonText(uint8_t r) {
  static const char* T[] = {"unbekannt","Einschalten","Reset-Taste","Software","ABSTURZ","Int-Watchdog","Task-Watchdog",
                            "Watchdog","Deep-Sleep","BROWNOUT (Stromversorgung!)","SDIO","USB","JTAG","eFuse","Spannungseinbruch","CPU-Lockup"};
  return (r < 16) ? T[r] : "?";
}

void handleSerial() {
  if (!Serial.available()) return;
  char c=Serial.read();
  uint8_t m=0;
  switch (c) {
    case '1': m=GAME_CTF; break;        case '2': m=GAME_MEMORY; break;
    case '3': m=GAME_BOMB; break;       case '4': m=GAME_REACTION; break;
    case '5': m=GAME_SIMON; break;      case '6': m=GAME_HOTPOTATO; break;
    case '7': m=GAME_KINGHILL; break;   case '8': m=GAME_TUGWAR; break;
    case '9': m=GAME_MINESWEEPER; break; case 'k': m=GAME_KNOCKOUT; break;
    case 'h': m=GAME_COLORHUNT; break;  case 'w': m=GAME_WHACKAMOLE; break;
  }
  if (m) { if (pendingAction==ACT_NONE) { pendingAction=ACT_START; pendingArg=m; } return; }
  if      (c=='0') { if (pendingAction==ACT_NONE) pendingAction=ACT_STOP; }
  else if (c=='r') { if (pendingAction==ACT_NONE) pendingAction=ACT_RECONNECT; }
  else if (c=='s') {
    Serial.printf("Firmware %s  Modus:%u  Nodes(max ID):%u  WLAN-Geraete:%u  Kanal:%u  RX-Verluste:%u\n",
      CTF_FW_VERSION, gameMode, nodeCount, WiFi.softAPgetStationNum(), apChannel, (unsigned)rxDrops);
    for (uint8_t i=1;i<=nodeCount;i++) {
      NodeInfo& n = nodes[i];
      if (!n.known) continue;
      Serial.printf("  Node%u '%s': %s  %s  RSSI %d  FW %u.%u.%u  Reset:%s  LED %s\n", i, players[i].name,
        n.active?"OK":"offline", IPAddress(n.ip).toString().c_str(), n.rssi, n.fw[0], n.fw[1], n.fw[2],
        resetReasonText(n.resetReason), (n.ledAcked==n.ledSeq)?"ok":"ausstehend");
    }
    if (highScoreCount>0) {
      Serial.println("Highscores:");
      for (uint8_t i=0;i<highScoreCount;i++)
        Serial.printf("  %u. %s - %u Pkt, %lums\n",i+1,highScores[i].name,highScores[i].points,(unsigned long)highScores[i].bestMs);
    }
  }
}

// ─────────────────────────────────────────────────────────────
// WLAN: Kanal automatisch waehlen (1/6/11 mit der geringsten Belegung)
// ─────────────────────────────────────────────────────────────
uint8_t pickChannel() {
  Serial.println("[WLAN] Suche freien Kanal...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int n = WiFi.scanNetworks();
  if (n <= 0) { WiFi.scanDelete(); Serial.println("[WLAN] Keine fremden Netze -> Kanal 1"); return 1; }
  static const uint8_t CH[3] = {1, 6, 11};
  long score[3] = {0, 0, 0};
  for (int i = 0; i < n; i++) {
    int c = WiFi.channel(i);
    int w = WiFi.RSSI(i) + 100;              // -40 dBm -> 60, -90 dBm -> 10
    if (w < 1) w = 1;
    for (uint8_t k = 0; k < 3; k++) {
      int d = abs(c - (int)CH[k]);
      if (d <= 4) score[k] += (long)w * (5 - d);   // ueberlappende Kanaele, naeher = staerker
    }
  }
  WiFi.scanDelete();
  uint8_t best = 0;
  for (uint8_t k = 1; k < 3; k++) if (score[k] < score[best]) best = k;
  Serial.printf("[WLAN] %d Netze, Belegung K1:%ld K6:%ld K11:%ld -> Kanal %u\n", n, score[0], score[1], score[2], CH[best]);
  return CH[best];
}

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
#ifdef ESP32
  uint8_t rr = (uint8_t)esp_reset_reason();
  masterResetReason = (rr > 15) ? 15 : rr;
#endif
  Serial.printf("\n=== ESP CTF Game – Master  Firmware %s (Protokoll %u) ===\n", MASTER_FW_STR, PROTO_VERSION);
  Serial.printf("Letzter Neustart: %s\n", resetReasonText(masterResetReason));
  if (masterResetReason == 9) Serial.println("!!! BROWNOUT: Stromversorgung des Masters pruefen !!!");

  // Gespeicherte Node-Zuordnung (MAC -> ID) und Spielernamen laden
  loadMacs();
  loadNames();
  for (uint8_t i=1;i<=MAX_NODES;i++)
    if (nodes[i].known && players[i].name[0]==0) snprintf(players[i].name,20,"Spieler %u",i);
  Serial.printf("Bekannte Nodes: %u\n", nodeCount);

#ifdef ESP32
  // Speicher fuer Node-Firmware (formatiert sich beim ersten Start selbst)
  fsOk = LittleFS.begin(true);
  if (fsOk) {
    if (LittleFS.exists("/node.tmp")) LittleFS.remove("/node.tmp");
    loadNodeFwMeta();
    fsRefresh();
    Serial.printf("[FS] LittleFS ok (%u/%u Bytes belegt), Node-Firmware: %s\n",
      (unsigned)fsUsed, (unsigned)fsTotal, nodeFw.present ? nodeFw.version : "keine");
  } else {
    Serial.println("[FS] LittleFS nicht verfuegbar – Node-Firmware kann nicht gespeichert werden.");
  }
#endif

  // WLAN Access Point
  apChannel = WIFI_CHANNEL;
  if (apChannel < 1 || apChannel > 13) apChannel = pickChannel();
  int maxConn = AP_MAX_CONN;
  if (maxConn < 1) maxConn = 1;
  if (maxConn > 10) maxConn = 10;    // ESP32: effektiv max. 10 Stationen
  apMaxConn = (uint8_t)maxConn;
  const char* pass = strlen(WIFI_PASS) > 0 ? WIFI_PASS : nullptr;
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(WIFI_SSID, pass, apChannel, 0, maxConn);
  if (!apOk && apChannel != 1) { apChannel = 1; apOk = WiFi.softAP(WIFI_SSID, pass, apChannel, 0, maxConn); }
  if (!apOk) Serial.println("[WLAN] FEHLER: Access Point startet nicht! (WIFI_PASS muss leer oder mind. 8 Zeichen sein)");
#ifdef ESP32
  WiFi.setSleep(false);   // AP-Stromsparen aus -> keine verpassten Pakete
  IPAddress b = WiFi.softAPBroadcastIP();
  if ((uint32_t)b != 0) bcastIP = b;
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  Serial.printf("AP: %s  IP: %s  Kanal:%u  maxConn:%d\n", WIFI_SSID, WiFi.softAPIP().toString().c_str(), apChannel, maxConn);

#ifdef ESP32
  rxQueue = xQueueCreate(64, sizeof(RxItem));
  audp.onPacket(onUdpPacket);
  if (!rxQueue || !audp.listen(UDP_PORT)) Serial.println("[UDP] FEHLER: Empfang konnte nicht gestartet werden!");
#else
  udp.begin(UDP_PORT);
#endif

  webServer.on("/",            webHandleRoot);
  webServer.on("/status",      webHandleStatus);
  webServer.on("/start",  HTTP_POST, webHandleStart);
  webServer.on("/stop",   HTTP_POST, webHandleStop);
  webServer.on("/names",  HTTP_POST, webHandleNames);
  webServer.on("/clearscores", HTTP_POST, webHandleClearScores);
  webServer.on("/reset",       HTTP_POST, webHandleReset);
  webServer.on("/forget",      HTTP_POST, webHandleForget);
  webServer.on("/identify",    HTTP_POST, webHandleIdentify);
#ifdef ESP32
  webServer.on("/update", HTTP_GET,  webHandleUpdateForm);
  webServer.on("/update", HTTP_POST, webHandleUpdateResult, webHandleUpdateUpload);
  webServer.on("/fw/node",     HTTP_POST, webHandleNodeFwResult, webHandleNodeFwUpload);
  webServer.on("/fw/node.bin", HTTP_GET,  webHandleFwGet);
  webServer.on("/fw/rollout",  HTTP_POST, webHandleRollout);
  webServer.on("/fw/cancel",   HTTP_POST, webHandleFwCancel);
  webServer.on("/fw/delete",   HTTP_POST, webHandleFwDelete);
  // Content-Type wird fuer den RAW-Modus-Schutz der Upload-Routen gebraucht
  const char* hdrs[] = {"Content-Type"};
  webServer.collectHeaders(hdrs, 1);
#else
  httpUpdater.setup(&webServer, "/update");
#endif
  webServer.begin();

  // ArduinoOTA (fuer Flashen via Arduino IDE Netzwerk-Port)
  ArduinoOTA.setHostname("ctf-master");
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]()  { Serial.println("[OTA] Start"); markAppValid("OTA gestartet"); });
  ArduinoOTA.onEnd([]()    { Serial.println("[OTA] Fertig"); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { wdtFeed(); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Fehler %u\n", e); });
  ArduinoOTA.begin();

  Serial.println("Web: http://192.168.4.1");
  Serial.println("OTA: Web-UI -> 'Firmware & OTA' (master.ino.bin / node.ino.bin)");
  Serial.println("Seriell: 1=CTF 2=Memory 3=Bomb 4=Reaktion 5=Simon 6=HotPotato");
  Serial.println("         7=KingHill 8=TugWar 9=Minesweeper k=Knockout h=ColorHunt w=WhackaMole");
  Serial.println("         0=Stop s=Status r=Reconnect");

#if defined(ESP32) && USE_WATCHDOG
  // Watchdog erst am Ende (nach evtl. LittleFS-Formatierung und Kanalsuche)
  esp_task_wdt_config_t wc = { .timeout_ms = (uint32_t)WDT_TIMEOUT_S * 1000UL, .idle_core_mask = 1, .trigger_panic = true };
  esp_err_t we = esp_task_wdt_reconfigure(&wc);
  if (we == ESP_ERR_INVALID_STATE) we = esp_task_wdt_init(&wc);
  if (we == ESP_OK) { enableLoopWDT(); wdtOn = true; Serial.printf("[WDT] Watchdog aktiv (%u s)\n", (unsigned)WDT_TIMEOUT_S); }
  else Serial.printf("[WDT] Watchdog nicht aktiv (Fehler %d)\n", (int)we);
#endif
}

void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();
  netPoll(false);
  ledService();
  handleSerial();
  processPending();
  nodeTimeoutCheck();
  idleBlinkService();
#ifdef ESP32
  rolloutService();
#endif
  resetBroadcastService();

  // Geplanter Neustart nach Master-Update (nie direkt im Web-Handler)
  if (restartAt != 0 && (long)(millis() - restartAt) >= 0) {
    Serial.println("[OTA] Neustart...");
    Serial.flush();
    ESP.restart();
  }
  rollbackService();

  if (pendingAction == ACT_NONE && !rolloutActive) gameUpdate();
}
