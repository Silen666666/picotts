/*
 * ESP CTF Game – Master Controller   (Firmware-Version: siehe protocol.h)
 * Nur ESP32. Funk zu den Nodes: ESP-NOW auf dem Kanal des Master-WLANs.
 * Das WLAN (Access Point) dient dem Handy (Web-UI) und den Nodes nur zum Firmware-Download.
 *
 * Handy: WLAN "ESP-CTF-Game" → Browser http://192.168.4.1
 * Seriell (115200):
 *   1=CTF  2=Memory  3=Bomb  4=Reaktion  5=Simon  6=HotPotato
 *   7=KingHill  8=TugWar  9=Minesweeper  k=Knockout  h=ColorHunt  w=WhackaMole
 *   0=Stop  s=Status  r=Reconnect
 */

#ifndef ESP32
  #error "Diese Master-Firmware laeuft nur auf dem ESP32 (Board: ESP32 Dev Module)."
#endif

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <MD5Builder.h>
#include <ArduinoOTA.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
WebServer webServer(80);

#include "config.h"

// ── Fallbacks: aeltere config.h-Kopien kennen manche Optionen noch nicht ──
#ifndef WIFI_SSID
  #define WIFI_SSID "ESP-CTF-Game"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif
#ifndef WIFI_CHANNEL
  #define WIFI_CHANNEL 0          // 0 = automatisch (beim ersten Start, danach gespeichert)
#endif
#ifndef AP_MAX_CONN
  #define AP_MAX_CONN 10
#endif
#ifndef MAX_NODES
  #define MAX_NODES 16
#endif
#ifndef NODE_TIMEOUT_MS
  #define NODE_TIMEOUT_MS 15000UL
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
#ifndef CTF_TEAMS
  #define CTF_TEAMS 2
#endif
#ifndef CTF_DURATION_S
  #define CTF_DURATION_S 180
#endif
#ifndef BOMB_SEQ_LEN
  #define BOMB_SEQ_LEN 5
#endif
#ifndef BOMB_DURATION_S
  #define BOMB_DURATION_S 120
#endif
#ifndef BOMB_STEP_MS
  #define BOMB_STEP_MS 1200
#endif
#ifndef REACT_ROUNDS_DEFAULT
  #define REACT_ROUNDS_DEFAULT 10
#endif
#ifndef REACT_TIMEOUT_MS
  #define REACT_TIMEOUT_MS 5000
#endif
#ifndef REACT_DELAY_MIN_MS
  #define REACT_DELAY_MIN_MS 600
#endif
#ifndef REACT_DELAY_MAX_MS
  #define REACT_DELAY_MAX_MS 3000
#endif
#ifndef MAX_HIGHSCORES
  #define MAX_HIGHSCORES 10
#endif
#ifndef KING_THRONE_MOVE_S
  #define KING_THRONE_MOVE_S 30
#endif
#ifndef MINE_COUNT_DEFAULT
  #define MINE_COUNT_DEFAULT 2
#endif
#ifndef KNOCK_LIVES_DEFAULT
  #define KNOCK_LIVES_DEFAULT 3
#endif
#ifndef COLORHUNT_ROUNDS
  #define COLORHUNT_ROUNDS 8
#endif
#ifndef WHAM_ROUNDS_DEFAULT
  #define WHAM_ROUNDS_DEFAULT 15
#endif
#ifndef WHAM_TIMEOUT_MS
  #define WHAM_TIMEOUT_MS 3000UL
#endif
#ifndef WHAM_DELAY_MIN_MS
  #define WHAM_DELAY_MIN_MS 400
#endif
#ifndef WHAM_DELAY_MAX_MS
  #define WHAM_DELAY_MAX_MS 1800
#endif
#ifndef ROUND_DELAY_S
  #define ROUND_DELAY_S 10
#endif
#ifndef MAX_HISTORY
  #define MAX_HISTORY 20
#endif
#ifndef MASTER_IS_NODE
  #define MASTER_IS_NODE 0        // 1 = Master hat selbst LED-Balken + Taster (lokale Spieler-Station)
#endif
#if MASTER_IS_NODE
  #ifndef MASTER_NEO_PIN
    #define MASTER_NEO_PIN 13
  #endif
  #ifndef MASTER_BUTTON_PIN
    #define MASTER_BUTTON_PIN 18
  #endif
  #ifndef MASTER_NEO_COUNT
    #define MASTER_NEO_COUNT 8
  #endif
  #ifndef MASTER_NEO_BRIGHTNESS
    #define MASTER_NEO_BRIGHTNESS 45
  #endif
  #ifndef MASTER_LAT_MS
    #define MASTER_LAT_MS 6
  #endif
  #ifndef MASTER_BTN_DEBOUNCE_MS
    #define MASTER_BTN_DEBOUNCE_MS 50   // wie BUTTON_DEBOUNCE_MS der Node
  #endif
  #include <Adafruit_NeoPixel.h>
  #include "esp_mac.h"
#endif

#include "protocol.h"

// C-Linkage VOR den automatisch erzeugten Prototypen festlegen (Hook des Cores, siehe unten)
extern "C" bool verifyRollbackLater();

// ESP-NOW: hoechstens 20 Partner (Peers) insgesamt, einer davon ist der Broadcast-Peer
#if MAX_NODES < 2 || MAX_NODES > 19
  #error "MAX_NODES in config.h muss zwischen 2 und 19 liegen (ESP-NOW: max. 19 Nodes)"
#endif
#if MAX_NODES > ESP_NOW_MAX_TOTAL_PEER_NUM - 1
  #error "MAX_NODES ist groesser als die ESP-NOW-Peer-Tabelle"
#endif

#define BAR_LEDS     8       // LEDs je Node (NEO_COUNT der Node-Firmware)
#define IMG_HDR_LEN  0x140   // so viele Bytes eines .bin werden vorab geprueft (inkl. CtfFwDesc)
static_assert(CTF_DESC_OFFSET + sizeof(CtfFwDesc) <= IMG_HDR_LEN, "CtfFwDesc liegt ausserhalb des Pruefbereichs");

// ─────────────────────────────────────────────────────────────
// Node-Registry: feste ID je MAC-Adresse (im NVS gespeichert)
// ─────────────────────────────────────────────────────────────
struct NodeInfo {
  uint8_t  mac[6];       // STA-MAC der Node = ihre Kennung und ESP-NOW-Zieladresse
  bool     known;        // Slot belegt (MAC bekannt)
  bool     reg;          // seit dem Start des Masters registriert (sonst -> PKT_RESET)
  bool     updateOnly;   // Update-Modus: Protokoll passt nicht -> nur Firmware-Update, spielt nie mit
  uint8_t  proto;        // bei der Registrierung gemeldetes Spiel-Protokoll
  bool     active;       // in den letzten NODE_TIMEOUT_MS etwas gehoert
  uint32_t lastSeen;
  int8_t   rssi;         // Empfangsstaerke beim Master (dBm, geglaettet, 0 = unbekannt)
  int8_t   rssiNode;     // Empfangsstaerke laut Node (dBm)
  uint8_t  fw[3];        // Firmware-Version laut STATUS (0.0.0 = unbekannt)
  uint8_t  flags;        // NODE_FLAG_*
  uint8_t  resetReason;  // esp_reset_reason() der Node (letzter Start)
  bool     statusSeen;   // STATUS seit der letzten Registrierung erhalten
  uint8_t  regCount;     // zaehlt Registrierungen (Rollout erkennt so den Neustart)
  uint32_t regAt;        // Zeitpunkt der letzten Registrierung
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

// ESP-NOW-Empfang: der Callback (WLAN-Task!) kopiert nur in diese Queue, loop() verarbeitet
struct RxItem { uint8_t mac[6]; int8_t rssi; Packet p; };
#define RX_QUEUE_LEN 64
QueueHandle_t     rxQueue   = NULL;
volatile uint32_t rxDrops   = 0;     // verworfen, weil die Queue voll war
volatile uint32_t rxForeign = 0;     // fremde ESP-NOW-Rahmen (falsche Laenge/Kennung)
volatile uint32_t txFails   = 0;     // Unicast ohne Empfangsbestaetigung (Node ausser Reichweite?)
bool              espNowOk  = false;
static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Nur noch zum Erkennen alter v1/v2-Nodes (funken per WLAN/UDP): keine Antworten
WiFiUDP legacyUdp;
bool    legacyUdpOk = false;

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

// Nur diese Aktionen brechen laufende Spiel-Animationen ab (yieldDelay -> false) und
// verwerfen Tastendruecke. Harmlose Aktionen (Rollout-Start/-Abbruch, Firmware loeschen,
// Reconnect) warten einfach, bis die Animation fertig ist.
bool actAborts(uint8_t a) { return a == ACT_START || a == ACT_STOP || a == ACT_FORGET; }

// PKT_RESET an alle (Broadcast, Ziel-MAC 0): 3x im Abstand von ~50 ms (nicht blockierend)
uint8_t  rstBcastLeft = 0;
uint32_t rstBcastNext = 0;

// Reset-Begrenzung: hoechstens 1x pro Sekunde je unbekannter MAC
struct MacNote { uint8_t mac[6]; bool used; uint32_t t; };
MacNote  resetNotes[8];
// Nodes mit alter Firmware v1/v2 (funken per WLAN/UDP) – nur fuer die Warnung im Web-UI
struct IpNote { uint32_t ip; uint32_t t; uint8_t info; };
IpNote   legacyNodes[8];     // info = Generation (1 oder 2)

uint32_t restartAt         = 0;   // geplanter Neustart (nach Master-Update / Kanalwechsel), 0 = keiner
uint8_t  masterResetReason = 0;
bool     wdtOn             = false;
uint8_t  apChannel         = 1;
bool     apChAuto          = (WIFI_CHANNEL < 1 || WIFI_CHANNEL > 13);   // Kanal automatisch (gespeichert)
uint8_t  apMaxConn         = AP_MAX_CONN;   // tatsaechlich verwendetes Limit (1..10)
bool     appValidMarked    = false;
uint32_t firstLoopAt       = 0;
bool     rolloutActive     = false;   // Node-Firmware wird gerade verteilt
uint32_t autoRolloutAt     = 0;       // Ein-Klick-Update: Rollout startet automatisch (0 = nein)
String   fwMsg;                       // letzte Meldung Upload/Rollout fuer das Web-UI
String   gameMsg;                     // Hinweis, warum ein Spiel nicht gestartet ist (wird ~15 s angezeigt)
uint32_t gameMsgAt = 0;

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
bool       upAuto    = false;   // Master-Update per Ein-Klick: danach Nodes automatisch aktualisieren
uint16_t   upHdrLen  = 0;
uint32_t   upTotal   = 0;
uint8_t    upHdr[IMG_HDR_LEN];
CtfFwDesc  upDesc;
String     upMsg;
File       upFile;
MD5Builder upMd5;

// Rollout (Node-Firmware nacheinander an die Nodes verteilen)
#define RO_IDLE 0     // naechsten Node waehlen
#define RO_SEND 1     // PKT_OTA senden, auf OTA_STATUS WIFI/STARTED warten
#define RO_WAIT 2     // Node laedt/flasht, auf Neustart mit neuer Version warten
#define RO_SLOT 3     // auf einen freien WLAN-Platz warten (Node braucht ihn fuer den Download)
#define RO_ERR_NOANSWER 20
#define RO_ERR_TIMEOUT  21
#define RO_ERR_OFFLINE  22
#define RO_ERR_VERSION  23
#define RO_ERR_APFULL   24
#define RO_ACK_MS       10000UL    // so lange auf OTA_STATUS WIFI/STARTED warten ...
#define RO_SEND_GAP_MS  2500UL     // ... und PKT_OTA in diesem Abstand wiederholen (max. 4x)
#define RO_SLOT_MS      30000UL    // so lange auf einen freien WLAN-Platz warten
#define RO_NODE_MS      180000UL   // Gesamtlimit je Node
#define AUTO_ROLLOUT_DELAY_MS 25000UL   // Ein-Klick-Update: Nodes erst wieder anmelden lassen
bool     roCancel = false;
bool     roForce  = false;   // "Alle neu flashen": Erfolg erst nach tatsaechlichem Update zaehlen
uint8_t  roQueue[MAX_NODES];
uint8_t  roQLen = 0, roQPos = 0;
uint8_t  roCur = 0, roPhase = RO_IDLE, roTries = 0, roRegAt = 0;
uint32_t roLastSend = 0, roPhaseAt = 0, roNodeAt = 0;
uint8_t  roState[MAX_NODES + 1];   // 0=-, 1=wartet, 2=laeuft, 3=fertig, 4=Fehler
uint8_t  roErr[MAX_NODES + 1];     // OTA_ERR_* bzw. RO_ERR_*

// Firmware-Kennung (wird beim Hochladen geprueft) + automatischer Rollback:
// eine neue Firmware gilt erst nach 20 s stabilem Lauf als gut.
extern "C" const CtfFwDesc ctf_fw_desc __attribute__((section(".rodata_custom_desc"), used)) =
  { CTF_DESC_MAGIC, CTF_ROLE_MASTER, PROTO_VERSION, CTF_FW_MAJOR, CTF_FW_MINOR, CTF_FW_PATCH, OTA_COMPAT, {0, 0}, CTF_FW_VERSION };
extern "C" bool verifyRollbackLater() { return true; }
// WICHTIG: ctf_fw_desc muss im Code BENUTZT werden (MASTER_FW_STR), sonst entfernt
// der Linker (--gc-sections) die Kennung und Updates koennen sie nicht pruefen.
#define MASTER_FW_STR ctf_fw_desc.version

#if MASTER_IS_NODE
// ─────────────────────────────────────────────────────────────
// Lokale Spieler-Station (MASTER_IS_NODE 1): der Master hat selbst LED-Balken + Taster.
//  - steht wie jede Node in der Registry: feste ID ueber die eigene STA-MAC (im NVS wie die
//    anderen), immer aktiv, nie Timeout, nie Update-Modus, nie im Rollout (= Master-Firmware)
//  - KEIN ESP-NOW-Peer, empfaengt nie Funk-Rahmen: sendPkt() verweigert ihre MAC, reg bleibt
//    false (Funk-Pakete mit ihrer ID scheitern an senderOk()), REGISTER mit ihrer MAC wird ignoriert
//  - ledSet() wendet den Soll-Zustand sofort lokal an und gilt als quittiert
//  - Taster: Entprellung wie node.ino; ein Flanken-Interrupt merkt sich zusaetzlich kurze Drucke,
//    waehrend loop() blockiert (Seite senden, NVS). Gleiche Sperren wie ein Funk-Druck, aber
//    erst MASTER_LAT_MS spaeter gewertet (Funk-Nodes haben einige ms Laufzeit -> fair)
//  - Anzeige wie node.ino (Farben, Animationen, show() nur bei Aenderung); FLASH blitzt in der
//    Zielfarbe statt weiss (schont das Netzteil des Masters)
// ─────────────────────────────────────────────────────────────
Adafruit_NeoPixel localStrip(MASTER_NEO_COUNT, MASTER_NEO_PIN, NEO_GRB + NEO_KHZ800);
uint8_t  localId = 0;            // ID der Station (0 = nicht angemeldet, z.B. Tabelle voll)
uint8_t  localMac[6];            // eigene STA-MAC (der Master funkt ueber die AP-MAC)

static const uint32_t LCOLORS[] = {   // 24-bit RGB je COL_* (wie node.ino)
  0x000000, 0xFF0000, 0x0000FF, 0x00FF00, 0xFFFF00, 0xFFFFFF, 0x800080, 0x00FFFF, 0xFF8000
};
static_assert(sizeof(LCOLORS) / sizeof(LCOLORS[0]) == COL_COUNT, "LCOLORS[] passt nicht zu COL_COUNT");

struct LocalLed {
  uint8_t  colorIdx, anim;
  uint32_t lastChange;
  uint8_t  phase;         // Blinken: 0 = AN
  bool     flashDone;
  uint8_t  barCount, barBg;
  bool     dirty;         // neu zeichnen noetig
};
LocalLed lled = {COL_OFF, ANIM_SOLID, 0, 0, false, 0, COL_OFF, true};
uint32_t lIdentUntil = 0, lIdentSince = 0;   // "Finden": weiss blinken bis (0 = aus)
int8_t   lIdentOn    = -1;
bool     lBtnRaw = HIGH, lBtnDeb = HIGH;     // Taster-Entprellung wie node.ino
uint32_t lBtnTs  = 0, lSvcAt = 0;            // letzte Pegelaenderung / letzter Aufruf von localNodeService()
bool     lPressDue = false;                  // erkannter Druck, wird ab lPressAt gewertet
uint32_t lPressAt  = 0;
volatile bool     lIsrHit = false;           // Interrupt: fallende Flanke seit dem letzten Aufruf ...
volatile uint32_t lIsrAt  = 0;               // ... und ihr Zeitpunkt

// Fallende Flanke am Taster: NUR merken (gewertet wird in localNodeService)
void IRAM_ATTR localBtnIsr() {
  if (!lIsrHit) { lIsrAt = millis(); lIsrHit = true; }
}

bool isLocal(uint8_t id) { return localId != 0 && id == localId; }

uint32_t lColor(uint8_t idx) { return LCOLORS[(idx < COL_COUNT) ? idx : COL_OFF]; }

void localFill(uint32_t rgb) {
  for (uint8_t i = 0; i < MASTER_NEO_COUNT; i++) localStrip.setPixelColor(i, rgb);
  localStrip.show();   // RMT, ~0,3 ms bei 8 LEDs, Interrupts bleiben an
}

void localSetLed(uint8_t colorIdx, uint8_t anim) {
  if (colorIdx >= COL_COUNT) colorIdx = COL_OFF;
  if (anim >= ANIM_COUNT)    anim = ANIM_SOLID;
  if (colorIdx == lled.colorIdx && anim == lled.anim && anim != ANIM_FLASH) return;   // unveraendert
  lled.colorIdx = colorIdx; lled.anim = anim;
  lled.lastChange = millis(); lled.phase = 0; lled.flashDone = false; lled.dirty = true;
}

void localSetBar(uint8_t anim, uint8_t colorA, uint8_t count, uint8_t colorB) {
  lled.anim     = anim;
  lled.colorIdx = (colorA < COL_COUNT) ? colorA : COL_OFF;
  lled.barCount = (count <= MASTER_NEO_COUNT) ? count : MASTER_NEO_COUNT;
  lled.barBg    = (colorB < COL_COUNT) ? colorB : COL_OFF;
  lled.lastChange = millis(); lled.phase = 0; lled.flashDone = false; lled.dirty = true;
}

// Soll-Zustand (wie PKT_SET_LED / SET_BAR / SET_SPLIT) uebernehmen – gezeichnet wird in localRender()
void localApply(uint8_t type, uint8_t a, uint8_t b, uint8_t c) {
  if (type == PKT_SET_LED) localSetLed(a, (b >= ANIM_BAR) ? ANIM_SOLID : b);
  else localSetBar(type == PKT_SET_BAR ? ANIM_BAR : ANIM_SPLIT, a, b, c);
}

// Normale Anzeige (Port von updateLed() aus node.ino)
void localUpdateLed() {
  uint32_t now = millis(), color = lColor(lled.colorIdx);
  switch (lled.anim) {
    case ANIM_BLINK_SLOW:
    case ANIM_BLINK_FAST:
      if (now - lled.lastChange >= (lled.anim == ANIM_BLINK_SLOW ? 500UL : 125UL)) {
        lled.phase ^= 1; lled.lastChange = now; lled.dirty = true;
      }
      if (!lled.dirty) return;
      localFill(lled.phase ? 0 : color);   // Phase 0 = AN -> sofort sichtbar
      lled.dirty = false;
      break;
    case ANIM_PULSE: {
      if (now - lled.lastChange < 33) return;   // ~30 fps
      lled.lastChange = now; lled.dirty = false;
      uint32_t t = now % 2000;
      uint32_t v = (t < 1000) ? (t / 4) : (255 - ((t - 1000) / 4));
      localFill(((((color >> 16) & 0xFF) * v / 255) << 16) | ((((color >> 8) & 0xFF) * v / 255) << 8) | ((color & 0xFF) * v / 255));
      break;
    }
    case ANIM_FLASH:   // Zielfarbe 80 ms, aus 60 ms, dann Zielfarbe solid (Node: weiss – hier Netzteil schonen)
      if (!lled.flashDone) {
        if (lled.phase == 0) {
          if (lled.dirty) { localFill(color); lled.dirty = false; }
          if (now - lled.lastChange >= 80) { lled.phase = 1; lled.lastChange = now; lled.dirty = true; }
        } else if (lled.phase == 1) {
          if (lled.dirty) { localFill(0); lled.dirty = false; }
          if (now - lled.lastChange >= 60) { lled.phase = 2; lled.lastChange = now; lled.dirty = true; }
        } else {
          lled.flashDone = true; lled.dirty = true;
        }
      } else if (lled.dirty) { localFill(color); lled.dirty = false; }
      break;
    case ANIM_BAR:
    case ANIM_SPLIT:
      if (!lled.dirty) return;
      for (uint8_t i = 0; i < MASTER_NEO_COUNT; i++)
        localStrip.setPixelColor(i, (i < lled.barCount) ? color : lColor(lled.barBg));
      localStrip.show();
      lled.dirty = false;
      break;
    default:   // ANIM_SOLID
      if (!lled.dirty) return;
      localFill(color);
      lled.dirty = false;
      break;
  }
}

// Overlay "Finden" (weiss schnell blinken), danach normale Anzeige wiederherstellen
void localRender() {
  if (lIdentUntil != 0) {
    uint32_t now = millis();
    if ((int32_t)(now - lIdentUntil) >= 0) {
      lIdentUntil = 0; lled.dirty = true;
      if (localId) nodes[localId].flags &= ~NODE_FLAG_IDENTIFY;
    } else {
      int8_t on = (((now - lIdentSince) / 125) % 2 == 0) ? 1 : 0;
      if (on != lIdentOn) { lIdentOn = on; localFill(on ? LCOLORS[COL_WHITE] : 0); }
      return;
    }
  }
  localUpdateLed();
}

void localIdentify(uint8_t s) {
  lIdentSince = millis();
  lIdentUntil = (lIdentSince + (uint32_t)s * 1000UL) | 1UL;
  lIdentOn    = -1;
  if (localId) nodes[localId].flags |= NODE_FLAG_IDENTIFY;
}

// Druck der Station werten: gleiche Regeln wie PKT_BUTTON in processPacket()
void localPress(bool inAnim) {
  nodes[localId].lastSeen = millis();
  if (inAnim || rolloutActive || actAborts(pendingAction)) {
    Serial.printf("[BTN] Node %u lokal (ignoriert: %s)\n", localId,
      inAnim ? "Animation" : (rolloutActive ? "Firmware-Update" : "Aktion laeuft"));
    return;
  }
  Serial.printf("[BTN] Node %u (lokal)\n", localId);
  gameOnButton(localId);   // darf yieldDelay() aufrufen -> dort localNodeService(true)
}

// Taster abfragen, Druck werten, LEDs zeichnen. Aus loop() (inAnim=false), yieldDelay() und
// langen Upload-/Download-Schleifen (inAnim=true: Druck wird erkannt, aber verworfen).
void localNodeService(bool inAnim) {
  if (!localId) return;
  uint32_t now = millis();
  bool raw = digitalRead(MASTER_BUTTON_PIN);
  bool blind = (uint32_t)(now - lSvcAt) >= MASTER_BTN_DEBOUNCE_MS;   // so lange nicht abgefragt (loop blockiert)
  lSvcAt = now;
  bool hit = lIsrHit;
  uint32_t edge = now;
  if (hit) { lIsrHit = false; edge = lIsrAt; }   // erst loeschen, dann lesen -> keine Flanke geht verloren
  if (raw != lBtnRaw) {
    lBtnRaw = raw;
    lBtnTs  = (hit && blind && raw == LOW) ? edge : now;   // Druck begann waehrend der Blockade -> ab der Flanke
  }
  bool press = false;
  if ((uint32_t)(now - lBtnTs) >= MASTER_BTN_DEBOUNCE_MS && raw != lBtnDeb) {
    lBtnDeb = raw;
    press = (raw == LOW);   // fallende Flanke (entprellt) = Druck
  } else if (hit && blind && raw == HIGH && lBtnDeb == HIGH) {
    press = true;           // kurzer Druck komplett waehrend der Blockade: nur der Interrupt hat ihn gesehen
  }
  if (press && !lPressDue) { lPressDue = true; lPressAt = now + MASTER_LAT_MS; }
  if (lPressDue && (int32_t)(now - lPressAt) >= 0) {
    lPressDue = false;      // vor localPress(): gameOnButton kann hierher zurueckkehren (yieldDelay)
    localPress(inAnim);
  }
  localRender();
}

// Station in der Registry anmelden (Start und nach "Vergessen"). Eigene STA-MAC wie eine Node:
// bekannte MAC behaelt ihre ID, sonst kleinste freie ID; ist alles belegt, wird der am laengsten
// inaktive Slot uebernommen (nie ein Teilnehmer des laufenden Spiels).
void localRegister(bool save) {
  uint32_t now = millis();
  uint8_t id = findMac(localMac);
  bool fresh = (id == 0);
  if (fresh) {
    for (uint8_t i = 1; i <= MAX_NODES; i++) if (!nodes[i].known) { id = i; break; }
    if (id == 0) {
      uint32_t oldest = 0;
      for (uint8_t i = 1; i <= MAX_NODES; i++) {
        if (!nodes[i].known || nodes[i].active || (gameMode != GAME_IDLE && inGame[i])) continue;
        uint32_t age = (uint32_t)(now - nodes[i].lastSeen);
        if (id == 0 || age > oldest) { id = i; oldest = age; }
      }
    }
    if (id == 0) { localId = 0; Serial.println("[LOKAL] Kein freier Platz – Master-Station deaktiviert"); return; }
    if (nodes[id].known) Serial.printf("[LOKAL] ID %u wird neu vergeben (alte Node lange offline)\n", id);
    memset(&nodes[id], 0, sizeof(NodeInfo));
    memcpy(nodes[id].mac, localMac, 6);
    nodes[id].known = true;
    inGame[id] = false;
    if (id > nodeCount) nodeCount = id;
  }
  char dflt[20];
  snprintf(dflt, sizeof(dflt), "Spieler %u", id);
  if (players[id].name[0] == 0 || strcmp(players[id].name, dflt) == 0) strcpy(players[id].name, "Master");
  NodeInfo& n = nodes[id];
  n.active = true; n.lastSeen = now; n.reg = false; n.updateOnly = false; n.proto = PROTO_VERSION;
  n.rssi = 0; n.rssiNode = 0; n.statusSeen = true;
  n.fw[0] = CTF_FW_MAJOR; n.fw[1] = CTF_FW_MINOR; n.fw[2] = CTF_FW_PATCH;
  n.flags = 0; n.resetReason = masterResetReason;
  n.otaState = 0; n.otaPct = 0; n.otaErr = 0;
  localId = id;
  if (n.ledSeq) { localApply(n.ledType, n.ledA, n.ledB, n.ledC); n.ledAcked = n.ledSeq; }
  else localSetLed(COL_GREEN, ANIM_FLASH);   // wie eine Node nach der Registrierung
  if (fresh && save) saveMacs();
  Serial.printf("[LOKAL] Master-Station = Node %u %s (MAC ..%02X:%02X, Taster D%u, LEDs D%u)\n", id,
    fresh ? "neu" : "angemeldet", localMac[4], localMac[5], MASTER_BUTTON_PIN, MASTER_NEO_PIN);
}
#endif

// ─────────────────────────────────────────────────────────────
// Funk (ESP-NOW): Peers verwalten, senden, empfangen
// ─────────────────────────────────────────────────────────────
static_assert(sizeof(Packet) == 8 && sizeof(CtfFrame) == 10, "Funkrahmen muss 10 Byte haben");

bool macEq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }

// Eine Unicast-MAC (nicht 00:00:.., nicht Broadcast/Multicast)?
bool macUsable(const uint8_t* m) {
  bool zero = true;
  for (uint8_t k = 0; k < 6; k++) if (m[k]) zero = false;
  return !zero && (m[0] & 0x01) == 0;
}

// Peer-Tabelle voll: einen Platz freimachen. Zuerst verwaiste Peers (MAC gehoert keiner
// bekannten Node mehr), sonst den der am laengsten nicht gehoerten INAKTIVEN Node.
// Der Broadcast-Peer und 'keep' werden nie entfernt.
bool peerEvictOne(const uint8_t* keep) {
  esp_now_peer_info_t pi;
  bool head = true;
  while (esp_now_fetch_peer(head, &pi) == ESP_OK) {   // liefert nur Unicast-Peers
    head = false;
    if (macEq(pi.peer_addr, keep) || macEq(pi.peer_addr, BCAST_MAC)) continue;
    if (findMac(pi.peer_addr) == 0) return esp_now_del_peer(pi.peer_addr) == ESP_OK;
  }
  uint32_t now = millis(), bestAge = 0;
  uint8_t  best = 0;
  for (uint8_t i = 1; i <= MAX_NODES; i++) {
    NodeInfo& n = nodes[i];
    if (!n.known || n.active || macEq(n.mac, keep) || !esp_now_is_peer_exist(n.mac)) continue;
    uint32_t age = (uint32_t)(now - n.lastSeen);
    if (best == 0 || age > bestAge) { best = i; bestAge = age; }
  }
  if (best == 0) return false;
  Serial.printf("[FUNK] Peer-Tabelle voll – Node %u (inaktiv) vorlaeufig entfernt\n", best);
  return esp_now_del_peer(nodes[best].mac) == ESP_OK;
}

// Sorgt dafuer, dass 'mac' in der ESP-NOW-Peer-Liste steht (Senden geht nur an Peers).
bool peerEnsure(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_num_t pn;
  if (esp_now_get_peer_num(&pn) == ESP_OK && pn.total_num >= ESP_NOW_MAX_TOTAL_PEER_NUM) peerEvictOne(mac);
  esp_now_peer_info_t pi;
  memset(&pi, 0, sizeof(pi));
  memcpy(pi.peer_addr, mac, 6);
  pi.channel = 0;              // 0 = aktueller Kanal des Access Points
  pi.ifidx   = WIFI_IF_AP;     // der Master funkt ueber seine AP-Schnittstelle
  pi.encrypt = false;
  esp_err_t e = esp_now_add_peer(&pi);
  if (e == ESP_OK || e == ESP_ERR_ESPNOW_EXIST) return true;
  static uint32_t lastLog = 0;
  if ((uint32_t)(millis() - lastLog) > 5000UL) {
    lastLog = millis();
    Serial.printf("[FUNK] Peer %02X:%02X:%02X:%02X:%02X:%02X nicht angelegt (Fehler %d)\n",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)e);
  }
  return false;
}

// Sendet einen CtfFrame an 'mac' (Node-MAC oder BCAST_MAC).
// Liefert false, wenn der Rahmen nicht abgeschickt werden konnte (z.B. Sendepuffer voll)
// -> die vorhandenen Wiederholungen (LED-Dienst, Node-Wiederholungen) gleichen das aus.
bool sendPkt(const uint8_t* mac, uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
#if MASTER_IS_NODE
  if (mac && localId && macEq(mac, localMac)) return false;   // Master-Station: nie per Funk (kein Peer)
#endif
  if (!espNowOk || !mac || !peerEnsure(mac)) return false;
  CtfFrame f;
  f.magic[0] = CTF_FRAME_MAGIC0; f.magic[1] = CTF_FRAME_MAGIC1;
  f.p.type = type; f.p.nodeId = nid;
  f.p.data[0]=d0; f.p.data[1]=d1; f.p.data[2]=d2;
  f.p.data[3]=d3; f.p.data[4]=d4; f.p.data[5]=d5;
  return esp_now_send(mac, (const uint8_t*)&f, sizeof(f)) == ESP_OK;
}

void wdtFeed() {
  if (wdtOn) esp_task_wdt_reset();
}

bool nodeValid(uint8_t id) { return id >= 1 && id <= MAX_NODES && nodes[id].known; }

// ESP-NOW-Empfang. Laeuft im WLAN-Task: NUR pruefen und in die Queue kopieren
// (keine Ausgaben, keine Spiel-Logik, kein esp_now_send).
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !info->src_addr || !data || !rxQueue) return;
  if (len != (int)sizeof(CtfFrame) || data[0] != CTF_FRAME_MAGIC0 || data[1] != CTF_FRAME_MAGIC1) {
    rxForeign = rxForeign + 1;
    return;
  }
  RxItem it;
  memcpy(it.mac, info->src_addr, 6);
  it.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
  memcpy(&it.p, data + 2, sizeof(Packet));
  if (xQueueSend(rxQueue, &it, 0) != pdTRUE) rxDrops = rxDrops + 1;   // nur dieser Task schreibt
}

// Sendeergebnis (WLAN-Task): nur zaehlen
void onEspNowSent(const esp_now_send_info_t* info, esp_now_send_status_t st) {
  (void)info;
  if (st != ESP_NOW_SEND_SUCCESS) txFails = txFails + 1;
}

// ESP-NOW starten (nach softAP). Bei Fehler laeuft der Master ohne Funk weiter (Web-UI zeigt es).
void espNowStart() {
  if (!rxQueue) rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(RxItem));
  esp_err_t e = esp_now_init();
  if (e != ESP_OK) { esp_now_deinit(); delay(200); e = esp_now_init(); }   // ein zweiter Versuch
  if (e != ESP_OK || !rxQueue) {
    Serial.printf("[FUNK] FEHLER: ESP-NOW startet nicht (Fehler %d) – Master bitte neu starten!\n", (int)e);
    espNowOk = false;
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);
  espNowOk = true;
  if (!peerEnsure(BCAST_MAC)) Serial.println("[FUNK] FEHLER: Broadcast-Peer konnte nicht angelegt werden");
  uint8_t m[6];
  esp_wifi_get_mac(WIFI_IF_AP, m);
  Serial.printf("[FUNK] ESP-NOW aktiv auf Kanal %u, Master-MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
    apChannel, m[0], m[1], m[2], m[3], m[4], m[5]);
}

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
  bool ok = sendPkt(n.mac, n.ledType, id, n.ledA, n.ledB, n.ledC, 0, 0, n.ledSeq);
  n.ledSentAt = millis();
  if (!ok)                 n.ledTries = 1;    // Senden fehlgeschlagen -> bald (40 ms) erneut
  else if (n.ledTries < 250) n.ledTries++;
}

// Neuen Soll-Zustand setzen (fuer alle BEKANNTEN Nodes, auch offline –
// er wird dann nach dem Wiederverbinden gesendet). Nodes im Update-Modus
// ignorieren Spiel-Pakete -> an sie wird nichts gesendet.
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
#if MASTER_IS_NODE
  // Master-Station: sofort anwenden (gezeichnet wird in localNodeService), gilt als quittiert
  if (isLocal(id)) { localApply(type, a, b, c); n.ledAcked = n.ledSeq; return; }
#endif
  if (n.active && !n.updateOnly) ledSend(id);
}

// Wiederholt nicht quittierte Soll-Zustaende (loop + yieldDelay)
void ledService() {
  uint32_t now = millis();
  for (uint8_t i = 1; i <= nodeCount; i++) {
    NodeInfo& n = nodes[i];
    if (!n.known || !n.active || n.updateOnly || n.ledSeq == 0 || n.ledAcked == n.ledSeq) continue;
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
// Empfang: ESP-NOW-Queue leeren, jedes Paket mit processPacket()
// ─────────────────────────────────────────────────────────────
void netPoll(bool inAnim) {
  if (!rxQueue) return;
  RxItem it;
  for (uint8_t k = 0; k < RX_QUEUE_LEN && xQueueReceive(rxQueue, &it, 0) == pdTRUE; k++)
    processPacket(it.p, it.mac, it.rssi, inAnim);
}

// Alte v1/v2-Nodes (WLAN/UDP) erkennen: nur fuer die Warnung im Web-UI, keine Antwort.
// Robust leeren: parsePacket() liefert 0, solange ein Paket nicht ganz gelesen ist.
void legacyPoll() {
  if (!legacyUdpOk) return;
  int sz;
  for (uint8_t k = 0; k < 8 && (sz = legacyUdp.parsePacket()) > 0; k++) {
    if (sz != (int)sizeof(Packet)) { legacyUdp.clear(); continue; }
    Packet p;
    if (legacyUdp.read((uint8_t*)&p, sizeof(p)) != (int)sizeof(p)) { legacyUdp.clear(); continue; }
    // v2: REGISTER nodeId = 2, STATUS data[2] = Firmware-Major 2; alles andere: v1
    uint8_t gen = 0;
    if (p.type == PKT_REGISTER)    gen = (p.nodeId  == 2) ? 2 : 1;
    else if (p.type == PKT_STATUS) gen = (p.data[2] == 2) ? 2 : 1;   // v1: PKT_PING (gleiche Nummer)
    if (gen) legacyNote((uint32_t)legacyUdp.remoteIP(), gen);
  }
}

// Nicht-blockierendes Warten: Web, OTA, Empfang und LED-Wiederholungen laufen weiter.
// Liefert false, sobald Start/Stop/Vergessen ansteht -> Aufrufer soll abbrechen.
bool yieldDelay(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    if (actAborts(pendingAction)) return false;
    ArduinoOTA.handle();
    webServer.handleClient();
    netPoll(true);
#if MASTER_IS_NODE
    localNodeService(true);   // Anzeige laeuft weiter, Druck wird (wie per Funk) verworfen
#endif
    ledService();
    wdtFeed();
    if (actAborts(pendingAction)) return false;
    yield();
  }
  return true;
}

// ─────────────────────────────────────────────────────────────
// Registry: Speichern (NVS), Registrierung, Pakete
// ─────────────────────────────────────────────────────────────
void saveMacs() {
  uint8_t buf[MAX_NODES * 6];
  memset(buf, 0, sizeof(buf));
  for (uint8_t i = 1; i <= MAX_NODES; i++) if (nodes[i].known) memcpy(buf + (i - 1) * 6, nodes[i].mac, 6);
  if (prefs.begin("ctf", false)) { prefs.putBytes("macs", buf, sizeof(buf)); prefs.end(); }
}

void loadMacs() {
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
}

void saveNames() {
  uint8_t buf[MAX_NODES * 20];
  for (uint8_t i = 1; i <= MAX_NODES; i++) {
    memcpy(buf + (i - 1) * 20, players[i].name, 20);
    buf[(i - 1) * 20 + 19] = 0;
  }
  if (prefs.begin("ctf", false)) { prefs.putBytes("names", buf, sizeof(buf)); prefs.end(); }
}

void loadNames() {
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
}

uint8_t findMac(const uint8_t* mac) {
  for (uint8_t i = 1; i <= MAX_NODES; i++)
    if (nodes[i].known && memcmp(nodes[i].mac, mac, 6) == 0) return i;
  return 0;
}

// Fordert eine unbekannte/verwirrte Node auf, sich neu anzumelden (max. 1x pro Sekunde je MAC).
// Unbekannte MACs sind keine Peers -> Broadcast mit der Ziel-MAC in data[0..5] (nur diese Node reagiert).
void maybeSendReset(const uint8_t* mac) {
  if (!macUsable(mac)) return;
  uint32_t now = millis();
  uint8_t slot = 0; uint32_t bestAge = 0; bool found = false;
  for (uint8_t k = 0; k < 8; k++) {
    if (resetNotes[k].used && macEq(resetNotes[k].mac, mac)) {
      if ((uint32_t)(now - resetNotes[k].t) < 1000UL) return;
      slot = k; found = true; break;
    }
  }
  if (!found) {   // freien bzw. aeltesten Eintrag nehmen
    for (uint8_t k = 0; k < 8; k++) {
      uint32_t age = resetNotes[k].used ? (uint32_t)(now - resetNotes[k].t) : 0xFFFFFFFFUL;
      if (age >= bestAge) { bestAge = age; slot = k; }
    }
  }
  memcpy(resetNotes[slot].mac, mac, 6);
  resetNotes[slot].used = true;
  resetNotes[slot].t    = now;
  sendPkt(BCAST_MAC, PKT_RESET, 0xFF, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Node mit alter Firmware v1/v2 (WLAN/UDP) meldet sich -> nur fuer die Warnung im Web-UI merken
void legacyNote(uint32_t ip, uint8_t gen) {
  if (!ip) return;
  uint32_t now = millis();
  for (uint8_t k = 0; k < 8; k++)
    if (legacyNodes[k].ip == ip) { legacyNodes[k].t = now; legacyNodes[k].info = gen; return; }
  uint8_t slot = 0; uint32_t bestAge = 0;
  for (uint8_t k = 0; k < 8; k++) {
    uint32_t age = legacyNodes[k].ip ? (uint32_t)(now - legacyNodes[k].t) : 0xFFFFFFFFUL;
    if (age >= bestAge) { bestAge = age; slot = k; }
  }
  legacyNodes[slot].ip = ip; legacyNodes[slot].t = now; legacyNodes[slot].info = gen;
  Serial.printf("[REG] Node mit alter Firmware v%u (WLAN/UDP) von %s – bitte einmal per USB neu flashen!\n",
    gen, IPAddress(ip).toString().c_str());
}

// Absender pruefen: ID bekannt, seit dem Master-Start registriert und dieselbe MAC
bool senderOk(uint8_t id, const uint8_t* mac) {
  return nodeValid(id) && nodes[id].reg && macEq(nodes[id].mac, mac);
}

// Empfangsstaerke beim Master glaetten (dBm, negativ; 0 = noch kein Wert)
void rssiAdd(NodeInfo& n, int8_t rssi) {
  if (rssi >= 0) return;
  if (n.rssi >= 0) { n.rssi = rssi; return; }
  int s = (int)n.rssi * 3 + (int)rssi;            // < 0
  n.rssi = (int8_t)(-((-s + 2) / 4));             // gerundeter Mittelwert (3:1)
}

// Gueltiges Paket einer registrierten Node: aktiv, Zeitstempel, Signal
void nodeHeard(uint8_t id, int8_t rssi) {
  NodeInfo& n = nodes[id];
  if (!n.active) Serial.printf("[NODE] Node %u wieder online\n", id);
  n.active   = true;
  n.lastSeen = millis();
  rssiAdd(n, rssi);
}

// PKT_REGISTER: mac = Absender (Kennung), proto = Spiel-Protokoll der Node,
// echo = data[] der Node (ihre MAC; data[2..5] kommen im ACK zurueck)
void handleRegister(const uint8_t* mac, uint8_t proto, const uint8_t* echo, int8_t rssi) {
  if (!macUsable(mac)) return;
  if (proto < 3) return;             // vor v3 gab es kein ESP-NOW -> kein Rahmen unserer Firmware
  uint32_t now = millis();
  uint8_t id = findMac(mac);
#if MASTER_IS_NODE
  if (isLocal(id)) return;   // fremdes Geraet mit der MAC der Master-Station – nie ueber Funk anmelden
#endif
  bool newSlot = false;
  if (id == 0) {
    // Neue Node: kleinste freie ID ...
    for (uint8_t i = 1; i <= MAX_NODES; i++) if (!nodes[i].known) { id = i; break; }
    // ... sonst den am laengsten inaktiven Slot wiederverwenden
    if (id == 0) {
      uint32_t oldest = 0;
      for (uint8_t i = 1; i <= MAX_NODES; i++) {
        // nie einen Teilnehmer des laufenden Spiels ersetzen (Spielzustand verweist auf die ID),
        // auch nicht die Node, die gerade Firmware laedt (ist dabei oft kurz "offline")
        if (!nodes[i].known || nodes[i].active || (gameMode != GAME_IDLE && inGame[i]) ||
            (rolloutActive && i == roCur)) continue;
        uint32_t age = (uint32_t)(now - nodes[i].lastSeen);
        if (id == 0 || age > oldest) { id = i; oldest = age; }
      }
    }
    if (id == 0) {
      static uint32_t lastFullLog = 0;
      if ((uint32_t)(now - lastFullLog) > 5000UL) {
        lastFullLog = now;
        Serial.printf("[REG] Kein freier Platz (MAX_NODES=%u) – Node ..%02X:%02X ignoriert\n", MAX_NODES, mac[4], mac[5]);
      }
      return;
    }
    // (Der Peer der alten MAC bleibt stehen und wird bei Platzmangel als verwaist entfernt.)
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
  // Die Node schickt REGISTER mehrfach kurz hintereinander -> nur einmal protokollieren
  bool quiet = !newSlot && n.reg && n.active && (uint32_t)(now - n.regAt) < 3000UL;
  n.reg        = true;
  n.regAt      = now;
  n.proto      = proto;
  n.updateOnly = (proto != PROTO_VERSION);   // anderes Protokoll -> nur Firmware-Update
  n.active     = true;
  n.lastSeen   = now;
  rssiAdd(n, rssi);
  n.statusSeen = false;
  n.regCount++;
  n.ledAcked   = 0;      // Node hat nach der Registrierung nichts angewendet -> Soll-Zustand neu senden
  n.lastBtnSeq = 0;      // neue Tasten-Sitzung (Node beginnt wieder bei Seq 1, die nie 0 ist)
  n.ledTries   = 0;
  n.ledSentAt  = 0;
  if (n.updateOnly) inGame[id] = false;   // Update-Modus: spielt nie mit
  // ACK doppelt gegen Paketverlust; data[1] = unser Protokoll (Node erkennt so den Update-Modus)
  bool sent = false;
  for (uint8_t k = 0; k < 2; k++)
    if (sendPkt(mac, PKT_ACK, id, id, PROTO_VERSION, echo[2], echo[3], echo[4], echo[5])) sent = true;
  if (!quiet) {
    Serial.printf("[REG] Node %u %s (MAC ..%02X:%02X, %d dBm)\n", id, newSlot ? "neu" : "angemeldet", mac[4], mac[5], rssi);
    if (n.updateOnly)
      Serial.printf("[REG] Node %u im UPDATE-MODUS (Protokoll %u, Master %u) – bitte Nodes aktualisieren\n", id, proto, PROTO_VERSION);
  }
  if (!sent) Serial.printf("[REG] Node %u: ACK konnte nicht gesendet werden\n", id);
  // Spiel laeuft und die Node spielt nicht mit (war beim Start nicht da) -> neutraler
  // Zustand. Teilnehmer bekommen ihren Soll-Zustand automatisch neu (ledAcked = 0).
  if (gameMode != GAME_IDLE && !inGame[id] && !n.updateOnly)
    ledSet(id, PKT_SET_LED, (gameMode == GAME_CTF) ? COL_WHITE : COL_OFF, ANIM_SOLID, 0);
}

// EINE Verarbeitungsfunktion fuer alle Pakete (aus loop und yieldDelay).
// mac = Absender (ESP-NOW), rssi = Empfangsstaerke beim Master.
// inAnim = true: aufgerufen waehrend einer Animation (yieldDelay) -> Tasten
// werden quittiert, aber nicht an das Spiel weitergegeben.
void processPacket(const Packet& p, const uint8_t* mac, int8_t rssi, bool inAnim) {
  if (!macUsable(mac)) return;
  uint8_t id = p.nodeId;
  switch (p.type) {
    case PKT_REGISTER:
      handleRegister(mac, p.nodeId, p.data, rssi);   // nodeId = Protokoll der Node
      break;

    case PKT_STATUS: {
      // Unbekannt / falsche MAC / seit dem Master-Start nicht registriert / keine gueltige
      // Version -> Node soll sich neu anmelden (dabei wird auch das Protokoll geprueft)
      if (!senderOk(id, mac) || p.data[2] == 0) { maybeSendReset(mac); break; }
      NodeInfo& n = nodes[id];
      nodeHeard(id, rssi);
      n.rssiNode    = (int8_t)p.data[1];
      n.fw[0] = p.data[2]; n.fw[1] = p.data[3]; n.fw[2] = p.data[4];
      n.flags       = p.data[5] & NODE_FLAG_MASK;
      n.resetReason = p.data[5] >> 4;
      if ((n.flags & NODE_FLAG_UPDATE_ONLY) && !n.updateOnly) {
        n.updateOnly = true; inGame[id] = false;
        Serial.printf("[NODE] Node %u meldet Update-Modus\n", id);
      }
      n.statusSeen  = true;
      n.ledAcked    = p.data[0];
      sendPkt(mac, PKT_PONG, id, n.ledSeq);
      break;
    }

    case PKT_BUTTON: {
      if (!senderOk(id, mac)) { maybeSendReset(mac); break; }
      NodeInfo& n = nodes[id];
      nodeHeard(id, rssi);
      uint8_t seq = p.data[0], nonce = p.data[1];
      sendPkt(mac, PKT_BTN_ACK, id, seq, nonce);     // IMMER quittieren (auch Wiederholungen)
      if (nonce != n.btnNonce) n.btnNonce = nonce;   // neue Sitzung der Node -> verarbeiten
      else if (seq == n.lastBtnSeq) break;            // Wiederholung desselben Drucks
      n.lastBtnSeq = seq;
      if (n.updateOnly) break;                        // Update-Modus: spielt nicht mit
      if (inAnim || rolloutActive || actAborts(pendingAction)) {
        Serial.printf("[BTN] Node %u (ignoriert: %s)\n", id,
          inAnim ? "Animation" : (rolloutActive ? "Firmware-Update" : "Aktion laeuft"));
        break;
      }
      Serial.printf("[BTN] Node %u\n", id);
      gameOnButton(id);
      break;
    }

    case PKT_OTA_STATUS: {
      if (!senderOk(id, mac)) break;
      uint8_t st = p.data[0];
      if (st < OTA_ST_STARTED || st > OTA_ST_WIFI) break;
      NodeInfo& n = nodes[id];
      nodeHeard(id, rssi);
      if (st != n.otaState || p.data[1] != n.otaPct) {
        if      (st == OTA_ST_WIFI)     Serial.printf("[OTA] Node %u: verbindet sich mit dem WLAN\n", id);
        else if (st == OTA_ST_STARTED)  Serial.printf("[OTA] Node %u: Download gestartet\n", id);
        else if (st == OTA_ST_PROGRESS) Serial.printf("[OTA] Node %u: %u %%\n", id, p.data[1]);
        else if (st == OTA_ST_SUCCESS)  Serial.printf("[OTA] Node %u: geflasht, startet neu\n", id);
        else                            Serial.printf("[OTA] Node %u: FEHLER %u\n", id, p.data[2]);
      }
      n.otaState = st; n.otaPct = p.data[1]; n.otaErr = p.data[2];
      break;
    }

    default: break;   // M->N-Pakete (z.B. eines anderen Masters) und Unbekanntes ignorieren
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
#if MASTER_IS_NODE
    if (isLocal(i)) continue;   // Master-Station ist immer da
#endif
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
#ifndef GAME_SAFETY_MS
  #define GAME_SAFETY_MS         900000UL  // Memory/Minesweeper: Notende nach 15 min (z.B. Node mit Paar/Feld ausgefallen)
#endif
uint32_t gameSafetyEnd = 0;   // Notende-Zeitpunkt fuer Spiele ohne eigenes Zeitlimit (0 = aus)

// Zeitpunkt "jetzt + ms", nie 0 (0 bedeutet bei ctfLockUntil, memHideAt, knockGraceAt, ... "aus")
uint32_t tAfter(uint32_t ms) { return (millis() + ms) | 1UL; }

// Mindestanzahl aktiver Nodes je Spiel
uint8_t minNodesFor(uint8_t m) { return (m == GAME_BOMB || m == GAME_MINESWEEPER) ? 3 : 2; }

// Zurzeit verbundene, spielfaehige Nodes (Pruefung vor dem Start; ohne Update-Modus)
uint8_t onlineCount() {
  uint8_t c = 0;
  for (uint8_t i=1;i<=MAX_NODES;i++) if (nodes[i].known && nodes[i].active && !nodes[i].updateOnly) c++;
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

// Zufall: random() nutzt auf dem ESP32 den Hardware-Zufallsgenerator (kein randomSeed() noetig –
// es wuerde auf den schwaecheren Pseudo-Zufall umschalten).

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

// Notende fuer Spiele ohne eigenes Zeitlimit pruefen: true = Spiel wurde beendet
bool gameSafetyExpired(uint8_t mode, uint16_t score) {
  if (gameSafetyEnd == 0 || (long)(millis() - gameSafetyEnd) < 0) return false;
  gameSafetyEnd = 0;
  Serial.println("[GAME] Zeitlimit erreicht (15 min) – Spiel beendet.");
  addHistory(mode, "Zeit abgelaufen", score);
  gameOverBlink(COL_RED, ANIM_BLINK_SLOW);
  gameMode = GAME_IDLE;
  return true;
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
  gameSafetyEnd=tAfter(GAME_SAFETY_MS);
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
  if (gameSafetyExpired(GAME_MEMORY, memFoundPairs)) return;
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
  gameSafetyEnd = tAfter(GAME_SAFETY_MS);
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
#if MASTER_IS_NODE
  if (pc > 2 && isLocal(huntDisplay)) huntDisplay = ids[1];   // nie die Master-Station (ausser bei nur 2 Teilnehmern)
#endif
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
  else if (gameMode==GAME_MINESWEEPER) gameSafetyExpired(GAME_MINESWEEPER, mineScore);  // sonst ereignisgesteuert
  else if (gameMode==GAME_KNOCKOUT)   knockUpdate();
  else if (gameMode==GAME_COLORHUNT)  huntUpdate();
  else if (gameMode==GAME_WHACKAMOLE) whamUpdate();
}

void startGameNow(uint8_t m) {
  if (rolloutActive) { Serial.println("[GAME] Start abgelehnt: Firmware-Update laeuft."); return; }
  if (m < GAME_CTF || m > GAME_WHACKAMOLE) return;
  gameMode=GAME_IDLE; gameEndTime=0; idleBlinkUntil=0; roundBarEndAt=0;
  // Teilnehmer = beim Start aktive Nodes (ohne Nodes im Update-Modus)
  for (uint8_t i=0;i<=MAX_NODES;i++)
    inGame[i] = (i>=1 && nodes[i].known && nodes[i].active && !nodes[i].updateOnly);
  allLED(COL_OFF,ANIM_SOLID);
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
  if (!ok && gameMode == GAME_IDLE && pendingAction == ACT_NONE) {
    gameMsg = "Spiel nicht gestartet: mindestens " + String(minNodesFor(m)) + " verbundene Nodes noetig (verbunden: " + String(onlineCount()) + ").";
    gameMsgAt = millis() | 1UL;
  } else gameMsgAt = 0;
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
// PKT_RESET an alle (Ziel-MAC 00:00:00:00:00:00), 3x per Broadcast (ohne Empfangsbestaetigung)
void resetBroadcastService() {
  if (rstBcastLeft == 0) return;
  if ((int32_t)(millis() - rstBcastNext) < 0) return;
  sendPkt(BCAST_MAC, PKT_RESET, 0xFF, 0, 0, 0, 0, 0, 0);
  rstBcastLeft--;
  rstBcastNext = millis() + 50;
}

// Aktive Nodes zusaetzlich direkt (Unicast mit Empfangsbestaetigung + Wiederholungen) zuruecksetzen
void resetActiveNodes() {
  for (uint8_t i=1;i<=nodeCount;i++) {
    NodeInfo& n = nodes[i];
    if (n.known && n.active) sendPkt(n.mac, PKT_RESET, 0xFF, n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
  }
}

// Alle Nodes melden sich neu an – IDs bleiben (Zuordnung per MAC), Soll-LEDs werden neu gesendet
void reconnectNodes() {
  rstBcastLeft = 3; rstBcastNext = millis();
  resetActiveNodes();
  Serial.println("[RESET] Reconnect gesendet – Nodes melden sich neu an (IDs bleiben).");
}

// Node-Liste komplett vergessen: Nodes bekommen neue Nummern (Namen bleiben je Nummer)
void forgetNodes() {
  gameMode = GAME_IDLE; idleBlinkUntil = 0; roundBarEndAt = 0;
  resetActiveNodes();
  // Peers bleiben vorerst stehen (Rahmen sind evtl. noch unterwegs); sie gelten ab jetzt
  // als verwaist und werden bei Platzmangel entfernt.
  for (uint8_t i=0;i<=MAX_NODES;i++) { memset(&nodes[i], 0, sizeof(NodeInfo)); inGame[i] = false; }
  nodeCount = 0;
#if MASTER_IS_NODE
  localId = 0;
  localRegister(false);   // Master-Station meldet sich sofort neu an (kleinste freie ID)
#endif
  saveMacs();
  rstBcastLeft = 3; rstBcastNext = millis();
  Serial.println("[RESET] Node-Liste geloescht – Nodes melden sich neu an (neue Nummern).");
}

void identifyNode(uint8_t id) {
#if MASTER_IS_NODE
  if (isLocal(id)) { localIdentify(5); Serial.printf("[NODE] Node %u (lokal) blinkt weiss (Finden)\n", id); return; }
#endif
  if (!nodeValid(id) || !nodes[id].active) return;
  sendPkt(nodes[id].mac, PKT_IDENTIFY, id, 5);
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
  // Update-Kennung (Kompatibilitaetsvertrag): nur Firmware ab v3 (ESP-NOW) kann spaeter
  // wieder per Funk aktualisiert werden. Ein anderes Spiel-Protokoll ist erlaubt (Update-Modus).
  if (d.otaCompat != OTA_COMPAT)
    return (wantRole == CTF_ROLE_NODE)
      ? "Node-Firmware " + String(d.version) + " ist zu alt (vor v3, ohne ESP-NOW) – bitte die aktuelle node.ino.bin verwenden."
      : "Master-Firmware " + String(d.version) + " ist zu alt (vor v3, ohne ESP-NOW) – damit waeren alle Nodes getrennt. Falls wirklich gewollt: per USB flashen.";
  return "";
}

// Hinweis, wenn das Spiel-Protokoll einer hochgeladenen Firmware nicht zum Master passt
String protoNote(const CtfFwDesc& d, uint8_t role) {
  if (d.proto == PROTO_VERSION) return "";
  String s = " Achtung: Spiel-Protokoll " + String(d.proto) + " statt " + String(PROTO_VERSION) + " – ";
  if (role == CTF_ROLE_NODE)
    s += (d.proto > PROTO_VERSION) ? "Nodes laufen danach im Update-Modus, bis auch der Master aktualisiert ist."
                                   : "Nodes laufen danach im Update-Modus, bis eine passende Node-Firmware verteilt ist.";
  else
    s += "Nodes laufen danach im Update-Modus, bis auch sie aktualisiert sind.";
  return s;
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
  // Kopf der gespeicherten Datei pruefen: eine von v1/v2 uebrig gebliebene Node-Firmware
  // (ohne v3-Update-Kennung) wuerde jede Node beim Update ablehnen -> verwerfen.
  {
    uint8_t h[IMG_HDR_LEN];
    File g = LittleFS.open("/node.bin", "r");
    size_t got = g ? g.read(h, IMG_HDR_LEN) : 0;
    if (g) g.close();
    CtfFwDesc d;
    if (got != IMG_HDR_LEN || imageCheck(h, CTF_ROLE_NODE, d).length()) {
      LittleFS.remove("/node.bin"); LittleFS.remove("/node.meta");
      fwMsg = "Gespeicherte Node-Firmware ist ungueltig oder zu alt (vor v3) und wurde verworfen – bitte node.ino.bin (v3) neu hochladen.";
      Serial.printf("[FW] %s\n", fwMsg.c_str());
      return;
    }
  }
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
  upKind = kind; upStarted = true; upFail = false; upHdrOk = false; upDone = false; upAuto = false;
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
    // Die gespeicherte Firmware bleibt erhalten, bis der Kopf der neuen Datei geprueft ist
    // (falsche Datei -> alte Node-Firmware ist danach noch da).
    upMd5.begin();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upStarted || upFail || upKind != UP_NODE) return;
    netPoll(true); ledService();     // Nodes waehrend des Uploads weiter bedienen (PONG), Tasten nicht ans Spiel
#if MASTER_IS_NODE
    localNodeService(true);          // nur Taster/LEDs, kein WebServer-Aufruf -> im Upload-Callback erlaubt
#endif
    size_t off = 0;
    if (!upHdrOk) {
      off = upCollectHdr(up.buf, up.currentSize);
      if (upHdrLen < IMG_HDR_LEN) return;            // Kopf noch unvollstaendig (Bytes liegen in upHdr)
      String e = imageCheck(upHdr, CTF_ROLE_NODE, upDesc);
      if (e.length()) { upError(e); return; }
      upHdrOk = true;
      // Kopf ist gueltig. Es passt nur EIN Firmware-Image in den Speicher -> erst jetzt das alte loeschen
      LittleFS.remove("/node.bin"); LittleFS.remove("/node.meta"); LittleFS.remove("/node.tmp");
      memset(&nodeFw, 0, sizeof(nodeFw));
      upFile = LittleFS.open("/node.tmp", "w");
      if (!upFile) { upError("Datei konnte nicht angelegt werden."); return; }
      if (upFile.write(upHdr, IMG_HDR_LEN) != IMG_HDR_LEN) { upError("Speicher voll – Datei passt nicht in den Flash-Speicher."); return; }
      upMd5.add(upHdr, IMG_HDR_LEN);
      upTotal = IMG_HDR_LEN;
    }
    size_t len = up.currentSize - off;
    if (len == 0) return;
    if (upTotal + len > CTF_APP_MAX_SIZE) { upError("Datei zu gross (max. 1.310.720 Bytes)."); return; }
    if (upFile.write(up.buf + off, len) != len) { upError("Speicher voll – Datei passt nicht in den Flash-Speicher."); return; }
    upMd5.add(up.buf + off, len);
    upTotal += len;
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
    upMsg = "Node-Firmware " + String(nodeFw.version) + " gespeichert (" + String(nodeFw.size) + " Bytes)."
          + protoNote(upDesc, CTF_ROLE_NODE);
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
    netPoll(true); ledService();   // andere Nodes weiter bedienen (PONG) -> sie melden sich nicht ab
#if MASTER_IS_NODE
    localNodeService(true);
#endif
    if ((uint32_t)(millis() - t0) > 120000UL) break;
  }
  f.close();
  Serial.printf("[FW] node.bin an %s: %u/%u Bytes\n", c.remoteIP().toString().c_str(), (unsigned)sent, (unsigned)sz);
}

// ── Rollout ──
// Gehoert die Node zu "Veraltete Nodes aktualisieren"? (andere Version oder Update-Modus)
// Veraltet = andere Version als die gespeicherte Node-Firmware. Eine Node im Update-Modus,
// die diese Version schon hat, kann durch erneutes Flashen nicht besser werden
// (dann passt die gespeicherte Firmware nicht zum Master -> passende node.ino.bin hochladen).
bool nodeOutdated(const NodeInfo& n) { return !fwEqualsStored(n); }

void rolloutStart(bool force) {
  if (rolloutActive) return;
  autoRolloutAt = 0;   // ein evtl. geplanter automatischer Start ist damit erledigt
  if (gameMode != GAME_IDLE) { fwMsg = "Rollout nicht moeglich: Spiel laeuft – erst stoppen."; return; }
  if (!fsOk || !nodeFw.present) { fwMsg = "Keine Node-Firmware gespeichert."; return; }
  if (!espNowOk) { fwMsg = "Rollout nicht moeglich: Funk (ESP-NOW) laeuft nicht – Master neu starten."; return; }
  roQLen = 0; roQPos = 0;
  for (uint8_t i = 0; i <= MAX_NODES; i++) { roState[i] = 0; roErr[i] = 0; }
  for (uint8_t i = 1; i <= nodeCount; i++) {   // nodeCount <= MAX_NODES -> roQueue reicht
    NodeInfo& n = nodes[i];
    if (!n.known || !n.active) continue;
#if MASTER_IS_NODE
    if (isLocal(i)) continue;   // Master-Station = Master-Firmware, wird nur mit dem Master aktualisiert
#endif
    if (!force && !nodeOutdated(n)) continue;
    roQueue[roQLen++] = i;
    roState[i] = 1;
  }
  if (roQLen == 0) {
    fwMsg = force ? String("Keine aktiven Nodes.") : "Alle aktiven Nodes haben bereits Version " + String(nodeFw.version) + ".";
    return;
  }
  rolloutActive = true; roCancel = false; roForce = force; roPhase = RO_IDLE; roCur = 0;
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
  if (!rolloutActive) {
    if (autoRolloutAt) {   // geplanten automatischen Start verwerfen
      autoRolloutAt = 0;
      fwMsg = "Automatisches Node-Update abgebrochen.";
      Serial.println("[ROLLOUT] Automatischer Start abgebrochen");
    }
    return;
  }
  roCancel = true;
  fwMsg = "Abbruch angefordert – der aktuelle Node wird noch fertig.";
}

// Zustandsautomat: Nodes nacheinander flashen (laeuft in loop)
//  RO_SLOT: freier WLAN-Platz fuer den Download? (max. 30 s warten)
//  RO_SEND: PKT_OTA (mit WLAN-Kanal) alle 2,5 s, max. 4x, bis OTA_STATUS WIFI/STARTED (max. 10 s)
//  RO_WAIT: Node laedt + flasht + startet neu; fertig, wenn sie sich mit der neuen Version meldet
//  Gesamtlimit je Node 180 s.
void rolloutService() {
  if (!rolloutActive) return;
  uint32_t now = millis();
  if (roPhase == RO_IDLE) {
    if (roCancel || roQPos >= roQLen || !fsOk || !nodeFw.present) { rolloutFinish(); return; }
    uint8_t id = roQueue[roQPos];
    roCur = id;
    if (!nodeValid(id) || !nodes[id].active) { rolloutNodeDone(false, RO_ERR_OFFLINE); return; }
    NodeInfo& n = nodes[id];
    roState[id] = 2; roPhase = RO_SLOT;
    roNodeAt = now; roPhaseAt = now; roLastSend = now - 1000UL; roTries = 0;
    n.otaState = 0; n.otaPct = 0; n.otaErr = 0;
    Serial.printf("[ROLLOUT] Node %u (%u/%u)\n", id, roQPos + 1, roQLen);
    return;
  }
  uint8_t id = roCur;
  if (id < 1 || id > MAX_NODES) { roPhase = RO_IDLE; roQPos++; roCur = 0; return; }
  if (!nodes[id].known) { rolloutNodeDone(false, RO_ERR_OFFLINE); return; }
  NodeInfo& n = nodes[id];
  if ((uint32_t)(now - roNodeAt) >= RO_NODE_MS) { rolloutNodeDone(false, RO_ERR_TIMEOUT); return; }

  if (roPhase == RO_SLOT) {
    if (roCancel) { roPhase = RO_IDLE; roCur = 0; return; }        // noch nicht begonnen -> sofort beenden
    if ((uint32_t)(now - roLastSend) < 500UL) return;               // 2x pro Sekunde pruefen
    roLastSend = now;
    if (WiFi.softAPgetStationNum() >= apMaxConn) {
      if ((uint32_t)(now - roPhaseAt) >= RO_SLOT_MS) { rolloutNodeDone(false, RO_ERR_APFULL); return; }
      static uint32_t lastLog = 0;
      if ((uint32_t)(now - lastLog) >= 10000UL) { lastLog = now; Serial.println("[ROLLOUT] WLAN voll – warte auf einen freien Platz..."); }
      return;
    }
    roPhase = RO_SEND; roPhaseAt = now; roTries = 0; roRegAt = n.regCount;   // Platz frei -> gleich PKT_OTA senden
  }

  bool reReg = (n.regCount != roRegAt) && n.statusSeen;   // neu angemeldet + STATUS erhalten
  // Erfolg = neu gestartet mit der gespeicherten Version. Beim Erzwingen ("alle neu flashen")
  // haben die Nodes diese Version schon vorher -> dort nur nach angenommenem OTA (RO_WAIT) zaehlen.
  if (reReg && fwEqualsStored(n) && (roPhase == RO_WAIT || !roForce)) { rolloutNodeDone(true, 0); return; }
  if (reReg && roPhase == RO_SEND) {   // Node hat sich vor dem OTA neu angemeldet -> PKT_OTA erneut senden
    roRegAt = n.regCount; roTries = 0; roPhaseAt = now;
  }
  if (n.otaState == OTA_ST_FAILED) { rolloutNodeDone(false, n.otaErr ? n.otaErr : OTA_ERR_DOWNLOAD); return; }
  if (roPhase == RO_SEND) {
    if (n.otaState == OTA_ST_WIFI || n.otaState == OTA_ST_STARTED ||
        n.otaState == OTA_ST_PROGRESS || n.otaState == OTA_ST_SUCCESS) {
      roPhase = RO_WAIT; roPhaseAt = now; roRegAt = n.regCount;   // ab jetzt auf Neustart warten
      return;
    }
    if ((uint32_t)(now - roPhaseAt) >= RO_ACK_MS) { rolloutNodeDone(false, RO_ERR_NOANSWER); return; }
    if (roTries < 4 && (roTries == 0 || (uint32_t)(now - roLastSend) >= RO_SEND_GAP_MS)) {
      uint32_t sz = nodeFw.size;
      sendPkt(n.mac, PKT_OTA, id, sz & 0xFF, (sz >> 8) & 0xFF, (sz >> 16) & 0xFF, (sz >> 24) & 0xFF, apChannel);
      roTries++; roLastSend = now;
    }
  } else if (reReg) {   // RO_WAIT: neu gestartet, aber nicht mit der gespeicherten Version
    rolloutNodeDone(false, RO_ERR_VERSION);
  }
}

// ── Ein-Klick-Update: nach dem Master-Neustart automatisch die Nodes aktualisieren ──
// /update?auto=1 merkt sich {autoRo=1, autoVer=<neue Version>} erst nach erfolgreichem Update.end().
// Beim Start: Merker loeschen; laeuft wirklich die neue Version (kein Rollback) und ist eine
// Node-Firmware gespeichert -> Rollout (nur veraltete Nodes) 25 s nach dem Start.
void autoRolloutBoot() {
  if (!prefs.begin("ctf", false)) return;
  bool flag = prefs.isKey("autoRo") && prefs.getUChar("autoRo", 0) == 1;
  String expect = prefs.isKey("autoVer") ? prefs.getString("autoVer", "") : String("");
  if (prefs.isKey("autoRo"))  prefs.remove("autoRo");
  if (prefs.isKey("autoVer")) prefs.remove("autoVer");
  prefs.end();
  if (!flag) return;
  if (expect != MASTER_FW_STR) {
    fwMsg = "Master laeuft mit " + String(MASTER_FW_STR) + " statt " + expect + " (Rollback?) – Nodes werden nicht automatisch aktualisiert.";
  } else if (!fsOk || !nodeFw.present) {
    fwMsg = "Master aktualisiert auf " + String(MASTER_FW_STR) + " – keine Node-Firmware gespeichert, Nodes werden nicht automatisch aktualisiert.";
  } else if (nodeFw.proto != PROTO_VERSION) {
    fwMsg = "Master aktualisiert auf " + String(MASTER_FW_STR) + " – die gespeicherte Node-Firmware " + String(nodeFw.version)
          + " passt nicht zum Master-Protokoll. Bitte passende node.ino.bin hochladen.";
  } else {
    autoRolloutAt = (millis() + AUTO_ROLLOUT_DELAY_MS) | 1UL;
    fwMsg = "Master aktualisiert auf " + String(MASTER_FW_STR) + " – Nodes werden in 25 s automatisch aktualisiert (Node-Firmware "
          + String(nodeFw.version) + ").";
  }
  Serial.printf("[ROLLOUT] %s\n", fwMsg.c_str());
}

void autoRolloutService() {
  if (autoRolloutAt == 0 || (int32_t)(millis() - autoRolloutAt) < 0) return;
  autoRolloutAt = 0;
  Serial.println("[ROLLOUT] Ein-Klick-Update: Nodes werden jetzt aktualisiert");
  rolloutStart(false);
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
    // URL-Parameter (?auto=1) sind hier schon gelesen (vor dem Formular-Inhalt)
    upAuto = webServer.hasArg("auto") && webServer.arg("auto") == "1";
    markAppValid("Update gestartet");   // laufende Firmware funktioniert offensichtlich
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upStarted || upFail || upKind != UP_MASTER) return;
    netPoll(true); ledService();     // Nodes waehrend des Uploads weiter bedienen (PONG)
#if MASTER_IS_NODE
    localNodeService(true);
#endif
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
    upMsg += protoNote(upDesc, CTF_ROLE_MASTER);
    if (upAuto) {   // Ein-Klick-Update: nach dem Neustart die Nodes automatisch aktualisieren
      bool saved = false;
      if (prefs.begin("ctf", false)) {
        saved = prefs.putUChar("autoRo", 1) == 1 && prefs.putString("autoVer", upDesc.version) > 0;
        prefs.end();
      }
      upMsg += saved ? " Danach werden die Nodes automatisch aktualisiert."
                     : " (Automatisches Node-Update konnte nicht vorgemerkt werden – bitte danach von Hand starten.)";
    }
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

// Markiert die laufende Firmware als gut (kein automatischer Rollback mehr)
void markAppValid(const char* why) {
  const esp_partition_t* p = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  bool pending = p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  esp_ota_mark_app_valid_cancel_rollback();
  appValidMarked = true;
  if (pending) Serial.printf("[OTA] Neue Firmware bestaetigt (%s) – kein Rollback mehr.\n", why);
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
    case ACT_ROLLOUT:   rolloutStart(arg != 0); break;
    case ACT_CANCEL:    rolloutCancelReq();     break;
    case ACT_FWDELETE:  nodeFwDelete();         break;
    default: break;
  }
}

// ─────────────────────────────────────────────────────────────
// Web-UI HTML
// ─────────────────────────────────────────────────────────────
#include "webui.h"   // INDEX_HTML (Web-Oberflaeche) – eigene Datei, siehe dort

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

  String j; j.reserve(8192);
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
    j += ",\"rssi\":"; j += (int)n.rssi;          // beim Master gemessen
    j += ",\"rssiN\":"; j += (int)n.rssiNode;     // laut Node
    j += ",\"upd\":"; j += (n.updateOnly ? "true" : "false");
    j += ",\"fw\":\"";
    if (n.fw[0] || n.fw[1] || n.fw[2]) { j += n.fw[0]; j += '.'; j += n.fw[1]; j += '.'; j += n.fw[2]; }
    j += "\",\"rst\":"; j += n.resetReason;
    j += ",\"flags\":"; j += n.flags;
    j += ",\"ota\":{\"st\":"; j += n.otaState; j += ",\"pct\":"; j += n.otaPct; j += ",\"err\":"; j += n.otaErr; j += '}';
    j += ",\"inGame\":"; j += (inGame[i] ? "true" : "false");
    j += ",\"ack\":"; j += (n.ledAcked == n.ledSeq ? "true" : "false");
#if MASTER_IS_NODE
    if (isLocal(i)) j += ",\"local\":true";   // Master-Station: UI zeigt "Master" statt Signalstaerke
#endif
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
  j += ",\"chAuto\":"; j += (apChAuto ? "true" : "false");
  j += ",\"espnow\":"; j += (espNowOk ? "true" : "false");
  j += ",\"rxDrops\":"; j += (uint32_t)rxDrops;
  j += ",\"rxForeign\":"; j += (uint32_t)rxForeign;
  j += ",\"txFails\":"; j += (uint32_t)txFails;
  j += ",\"legacy\":[";
  first = true;
  for (uint8_t k = 0; k < 8; k++) {
    if (!legacyNodes[k].ip || (uint32_t)(now - legacyNodes[k].t) >= 60000UL) continue;
    if (!first) j += ',';
    first = false;
    String li = IPAddress(legacyNodes[k].ip).toString() + " (v" + String(legacyNodes[k].info) + ")";
    jsonStr(j, li.c_str());
  }
  j += ']';
  j += ",\"fs\":{\"ok\":"; j += (fsOk ? "true" : "false");
  j += ",\"total\":"; j += fsTotal; j += ",\"used\":"; j += fsUsed; j += '}';
  j += ",\"nodeFw\":{\"present\":"; j += (nodeFw.present ? "true" : "false");
  j += ",\"version\":"; jsonStr(j, nodeFw.present ? nodeFw.version : "");
  j += ",\"size\":"; j += nodeFw.size; j += '}';
  j += ",\"rollout\":{\"active\":"; j += (rolloutActive ? "true" : "false");
  j += ",\"cancel\":"; j += (roCancel ? "true" : "false");
  j += ",\"cur\":"; j += roCur;
  j += ",\"phase\":"; j += roPhase;
  uint32_t autoIn = 0;
  if (autoRolloutAt && (int32_t)(autoRolloutAt - now) > 0) autoIn = ((uint32_t)(autoRolloutAt - now) + 999UL) / 1000UL;
  j += ",\"autoIn\":"; j += autoIn;
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
  if (gameMsgAt && (uint32_t)(millis() - gameMsgAt) > 15000UL) gameMsgAt = 0;
  j += ",\"gameMsg\":"; jsonStr(j, gameMsgAt ? gameMsg.c_str() : "");
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
void webHandleReset() {
  if (rolloutActive) { webServer.send(409,"text/plain","Nicht moeglich, waehrend die Nodes aktualisiert werden."); return; }
  webSetPending(ACT_RECONNECT, 0);
}

void webHandleForget() {
  if (rolloutActive) { webServer.send(409,"text/plain","Nicht moeglich, waehrend die Nodes aktualisiert werden."); return; }
  webSetPending(ACT_FORGET, 0);
}

void webHandleIdentify() {
  int id = webServer.arg("id").toInt();
  if (id < 1 || id > MAX_NODES || !nodes[id].known) { webServer.send(400,"text/plain","Unbekannte Node"); return; }
  if (!nodes[id].active) { webServer.send(409,"text/plain","Node ist offline"); return; }
  identifyNode((uint8_t)id);   // nur ein Funk-Paket, aendert keinen Spielzustand -> direkt, ohne Spiel-Animation abzubrechen
  webServer.send(200, "text/plain", "OK");
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

void webHandleRollout() {
  if (rolloutActive)        { webServer.send(409,"text/plain","Rollout laeuft bereits."); return; }
  if (gameMode!=GAME_IDLE)  { webServer.send(409,"text/plain","Spiel laeuft – bitte erst stoppen."); return; }
  if (!nodeFw.present)      { webServer.send(409,"text/plain","Keine Node-Firmware gespeichert – bitte zuerst hochladen."); return; }
  webSetPending(ACT_ROLLOUT, webServer.arg("force").toInt() == 1 ? 1 : 0);
}
void webHandleFwCancel() { webSetPending(ACT_CANCEL, 0); }
void webHandleFwDelete() {
  if (rolloutActive) { webServer.send(409,"text/plain","Nicht moeglich, waehrend die Nodes aktualisiert werden."); return; }
  if (autoRolloutAt) { webServer.send(409,"text/plain","Automatisches Node-Update ist geplant – erst abbrechen."); return; }
  webSetPending(ACT_FWDELETE, 0);
}

// WLAN-Kanal neu waehlen: gespeicherten Kanal loeschen und neu starten (beim Start wird gesucht)
void webHandleChannel() {
  if (!apChAuto)                  { webServer.send(409,"text/plain","Der Kanal ist in config.h fest eingestellt (WIFI_CHANNEL)."); return; }
  if (gameMode != GAME_IDLE)      { webServer.send(409,"text/plain","Spiel laeuft – bitte erst stoppen."); return; }
  if (rolloutActive || autoRolloutAt) { webServer.send(409,"text/plain","Nicht moeglich, waehrend die Nodes aktualisiert werden."); return; }
  if (restartAt)                  { webServer.send(409,"text/plain","Neustart laeuft bereits."); return; }
  if (prefs.begin("ctf", false)) {
    if (prefs.isKey("apch")) prefs.remove("apch");
    prefs.end();
  }
  Serial.println("[WLAN] Kanal wird beim Neustart neu gewaehlt");
  webServer.send(200, "text/plain", "OK");
  restartAt = (millis() + 800UL) | 1UL;   // Neustart erst nach der Antwort (in loop)
}

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
    Serial.printf("Firmware %s  Modus:%u  Nodes(max ID):%u  WLAN-Geraete:%u  Kanal:%u  ESP-NOW:%s  RX-Verluste:%u  TX-Fehler:%u  Fremd:%u\n",
      MASTER_FW_STR, gameMode, nodeCount, WiFi.softAPgetStationNum(), apChannel, espNowOk ? "ok" : "FEHLER",
      (unsigned)rxDrops, (unsigned)txFails, (unsigned)rxForeign);
    for (uint8_t i=1;i<=nodeCount;i++) {
      NodeInfo& n = nodes[i];
      if (!n.known) continue;
      Serial.printf("  Node%u '%s': %s  %02X:%02X:%02X:%02X:%02X:%02X  RSSI %d/%d  FW %u.%u.%u%s  Reset:%s  LED %s\n", i, players[i].name,
        n.active?"OK":"offline", n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5], n.rssi, n.rssiNode,
        n.fw[0], n.fw[1], n.fw[2], n.updateOnly ? " UPDATE-MODUS" : "",
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

// Kanal: fest aus config.h, sonst der beim ersten Start gewaehlte (gespeichert -> die Nodes
// finden den Master nach einem Neustart sofort wieder). "WLAN-Kanal neu waehlen" loescht ihn.
uint8_t chooseChannel() {
  if (!apChAuto) return (uint8_t)WIFI_CHANNEL;
  uint8_t ch = 0;
  if (prefs.begin("ctf", true)) { ch = prefs.getUChar("apch", 0); prefs.end(); }
  if (ch >= 1 && ch <= 13) { Serial.printf("[WLAN] Gespeicherter Kanal %u\n", ch); return ch; }
  return pickChannel();
}

void saveChannel(uint8_t ch) {
  if (!apChAuto || !prefs.begin("ctf", false)) return;
  if (prefs.getUChar("apch", 0) != ch) prefs.putUChar("apch", ch);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────
void setup() {
#if MASTER_IS_NODE
  // Master-Station: ZUERST den LED-Balken loeschen (vor Kanalsuche und AP-Start);
  // nach einem Brownout nur halbe Helligkeit (Netzteil schonen)
  localStrip.begin();
  localStrip.setBrightness(esp_reset_reason() == ESP_RST_BROWNOUT ? MASTER_NEO_BRIGHTNESS / 2 : MASTER_NEO_BRIGHTNESS);
  localStrip.show();
#endif
  Serial.begin(115200);
  delay(300);
  uint8_t rr = (uint8_t)esp_reset_reason();
  masterResetReason = (rr > 15) ? 15 : rr;
  Serial.printf("\n=== ESP CTF Game – Master  Firmware %s (Protokoll %u) ===\n", MASTER_FW_STR, PROTO_VERSION);
  Serial.printf("Letzter Neustart: %s\n", resetReasonText(masterResetReason));
  if (masterResetReason == 9) Serial.println("!!! BROWNOUT: Stromversorgung des Masters pruefen !!!");
#if MASTER_IS_NODE
  if (masterResetReason == 9) Serial.println("[LOKAL] LED-Balken der Master-Station laeuft mit halber Helligkeit");
#endif

  // Gespeicherte Node-Zuordnung (MAC -> ID) und Spielernamen laden
  loadMacs();
  loadNames();
  for (uint8_t i=1;i<=MAX_NODES;i++)
    if (nodes[i].known && players[i].name[0]==0) snprintf(players[i].name,20,"Spieler %u",i);
  Serial.printf("Bekannte Nodes: %u\n", nodeCount);
#if MASTER_IS_NODE
  // Master-Station: Taster wie bei einer Node (+ Flanken-Interrupt), Anmeldung mit der eigenen STA-MAC
  pinMode(MASTER_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MASTER_BUTTON_PIN), localBtnIsr, FALLING);
  lIsrHit = false;
  esp_read_mac(localMac, ESP_MAC_WIFI_STA);    // eindeutig, aendert sich nie (AP-MAC = STA-MAC + 1)
  localRegister(true);
#endif

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

  // WLAN Access Point (Handy + Node-Updates); ESP-NOW funkt auf demselben Kanal
  apChannel = chooseChannel();
  int maxConn = AP_MAX_CONN;
  if (maxConn < 1) maxConn = 1;
  if (maxConn > 10) maxConn = 10;    // ESP32: effektiv max. 10 Stationen
  apMaxConn = (uint8_t)maxConn;
  const char* pass = strlen(WIFI_PASS) > 0 ? WIFI_PASS : nullptr;
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(WIFI_SSID, pass, apChannel, 0, maxConn);
  if (!apOk && apChannel != 1) { apChannel = 1; apOk = WiFi.softAP(WIFI_SSID, pass, apChannel, 0, maxConn); }
  if (!apOk) Serial.println("[WLAN] FEHLER: Access Point startet nicht! (WIFI_PASS muss leer oder mind. 8 Zeichen sein)");
  else saveChannel(apChannel);
  WiFi.setSleep(false);   // Stromsparen aus -> keine verpassten Pakete
  Serial.printf("AP: %s  IP: %s  Kanal:%u%s  maxConn:%d\n", WIFI_SSID, WiFi.softAPIP().toString().c_str(), apChannel,
    apChAuto ? " (automatisch)" : " (config.h)", maxConn);

  // Funk zu den Nodes (ESP-NOW) – erst nach dem Start des Access Points
  espNowStart();
  // Alte v1/v2-Nodes (WLAN/UDP) nur erkennen, um im Web-UI zu warnen
  legacyUdpOk = legacyUdp.begin(UDP_PORT) == 1;

  webServer.on("/",            webHandleRoot);
  webServer.on("/status",      webHandleStatus);
  webServer.on("/start",  HTTP_POST, webHandleStart);
  webServer.on("/stop",   HTTP_POST, webHandleStop);
  webServer.on("/names",  HTTP_POST, webHandleNames);
  webServer.on("/clearscores", HTTP_POST, webHandleClearScores);
  webServer.on("/reset",       HTTP_POST, webHandleReset);
  webServer.on("/forget",      HTTP_POST, webHandleForget);
  webServer.on("/identify",    HTTP_POST, webHandleIdentify);
  webServer.on("/channel",     HTTP_POST, webHandleChannel);
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
  webServer.begin();

  // ArduinoOTA (fuer Flashen via Arduino IDE Netzwerk-Port)
  ArduinoOTA.setHostname("ctf-master");
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]()  { Serial.println("[OTA] Start"); markAppValid("OTA gestartet"); });
  ArduinoOTA.onEnd([]()    { Serial.println("[OTA] Fertig"); });
#if MASTER_IS_NODE
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { wdtFeed(); localNodeService(true); });
#else
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { wdtFeed(); });
#endif
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Fehler %u\n", e); });
  ArduinoOTA.begin();

  Serial.println("Web: http://192.168.4.1");
  Serial.println("OTA: Web-UI -> 'Firmware & OTA' (master.ino.bin / node.ino.bin)");
  Serial.println("Seriell: 1=CTF 2=Memory 3=Bomb 4=Reaktion 5=Simon 6=HotPotato");
  Serial.println("         7=KingHill 8=TugWar 9=Minesweeper k=Knockout h=ColorHunt w=WhackaMole");
  Serial.println("         0=Stop s=Status r=Reconnect");

  // Ein-Klick-Update: Nodes nach dem Master-Update automatisch aktualisieren?
  autoRolloutBoot();

#if USE_WATCHDOG
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
#if MASTER_IS_NODE
  localNodeService(false);   // Taster der Master-Station (wie ein Funk-Druck) + LEDs zeichnen
#endif
  ledService();
  handleSerial();
  processPending();
  nodeTimeoutCheck();
  idleBlinkService();
  rolloutService();
  autoRolloutService();
  resetBroadcastService();
  legacyPoll();

  // Geplanter Neustart nach Master-Update / Kanalwechsel (nie direkt im Web-Handler)
  if (restartAt != 0 && (int32_t)(millis() - restartAt) >= 0) {
    Serial.println("[OTA] Neustart...");
    Serial.flush();
    ESP.restart();
  }
  rollbackService();

  if (pendingAction == ACT_NONE && !rolloutActive) gameUpdate();
}
