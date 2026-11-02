#pragma once
#include <stdint.h>

// ================================================================
// ESP CTF Game – gemeinsames Protokoll Master <-> Node  (v3: ESP-NOW)
// Diese Datei existiert ZWEIMAL (master/protocol.h und node/protocol.h)
// und muss in beiden Ordnern IDENTISCH sein.
// ================================================================

// Firmware-Version (Master und Nodes werden gemeinsam versioniert).
#define CTF_FW_MAJOR   3
#define CTF_FW_MINOR   0
#define CTF_FW_PATCH   0
#define CTF_FW_VERSION "3.0.0"

// Spiel-Protokoll: steht bei PKT_REGISTER im Feld nodeId und bei PKT_ACK in data[1].
// Unterschiedliche Protokolle -> Node laeuft im "Update-Modus" (nur Firmware-Update).
#define PROTO_VERSION  3

// ================================================================
// KOMPATIBILITAETSVERTRAG (ab v3 EINGEFROREN – nie mehr aendern!)
// Damit JEDER kuenftige Master JEDE Node ab v3 per Funk aktualisieren kann:
//  - Funk:      ESP-NOW auf dem Kanal des Master-WLANs, Nutzlast = CtfFrame (10 Byte)
//  - Pakete:    PKT_REGISTER, PKT_ACK, PKT_STATUS, PKT_PONG, PKT_RESET,
//               PKT_IDENTIFY, PKT_OTA, PKT_OTA_STATUS behalten Nummer und Feld-Layout
//  - Update:    Node verbindet sich mit dem Master-WLAN (WIFI_SSID/WIFI_PASS) und laedt
//               http://192.168.4.1/fw/node.bin  (HTTP/1.0, Content-Length, optional x-MD5)
//  - Datei:     CtfFwDesc bei .bin-Offset 0x120 mit otaCompat == OTA_COMPAT
// Neue Spiel-Funktionen duerfen NEUE Pakettypen bekommen (>= 0x10) oder das
// Spiel-Protokoll (PROTO_VERSION) erhoehen – OTA_COMPAT bleibt 1.
// ================================================================
#define OTA_COMPAT     1

#define UDP_PORT 4210          // nur noch zum Erkennen alter v1/v2-Nodes (die per WLAN/UDP funken)

// ----------------------------------------------------------------
// Firmware-Kennung im .bin (fuer OTA-Pruefung)
// Jede Firmware legt eine CtfFwDesc in die Section ".rodata_custom_desc".
// Bei Arduino-ESP32 3.3.x liegt sie im .bin bei Offset 0x120. WICHTIG: der Linker
// behaelt sie nur, wenn der Code sie benutzt (z.B. Version beim Start ausgeben).
// ----------------------------------------------------------------
#define CTF_DESC_MAGIC    0x44465443UL   // Bytes 'C','T','F','D'
#define CTF_DESC_OFFSET   0x120
#define CTF_ROLE_MASTER   1
#define CTF_ROLE_NODE     2
#define CTF_APP_MAX_SIZE  0x140000UL     // App-Partition (Default-Schema), 1.310.720 Byte

struct CtfFwDesc {
  uint32_t magic;        // CTF_DESC_MAGIC
  uint8_t  role;         // CTF_ROLE_*
  uint8_t  proto;        // PROTO_VERSION (Spiel-Protokoll)
  uint8_t  major, minor, patch;
  uint8_t  otaCompat;    // OTA_COMPAT (v2: war reserved[0] = 0)
  uint8_t  reserved[2];
  char     version[16];  // CTF_FW_VERSION, 0-terminiert
};

// ----------------------------------------------------------------
// Funkrahmen (ESP-NOW-Nutzlast): IMMER genau 10 Byte = 2 Byte Kennung + Packet
// Rahmen anderer Laenge oder mit falscher Kennung werden verworfen
// (fremde ESP-NOW-Geraete in der Naehe).
// ----------------------------------------------------------------
#define CTF_FRAME_MAGIC0  0x43   // 'C'
#define CTF_FRAME_MAGIC1  0x54   // 'T'

struct Packet {
  uint8_t type;
  uint8_t nodeId;
  uint8_t data[6];
};

struct CtfFrame {
  uint8_t magic[2];      // CTF_FRAME_MAGIC0, CTF_FRAME_MAGIC1
  Packet  p;
};

// ----------------------------------------------------------------
// Pakete   (N->M = Node an Master, M->N = Master an Node)
// Adressierung: Master sendet an die STA-MAC der Node (= ihre Kennung),
// Node sendet an die AP-MAC des Masters (BSSID aus dem WLAN-Scan).
// ----------------------------------------------------------------
#define PKT_REGISTER   0x01  // N->M  nodeId=PROTO_VERSION der Node, data[0..5]=STA-MAC der Node        [EINGEFROREN]
#define PKT_ACK        0x02  // M->N  nodeId=ID, data[0]=ID, data[1]=PROTO_VERSION des Masters,
                              //        data[2..5]=MAC[2..5] der Node                                      [EINGEFROREN]
                              //        Protokolle gleich -> Spielbetrieb; verschieden -> Update-Modus.
#define PKT_BUTTON     0x03  // N->M  nodeId=ID, data[0]=Druck-Seq (1..255), data[1]=Sitzungs-Nonce (1..255)
#define PKT_SET_LED    0x04  // M->N  data[0]=Farbe, data[1]=Animation,                 data[5]=LED-Seq
#define PKT_STATUS     0x07  // N->M  nodeId=ID, data[0]=angewendete LED-Seq, data[1]=RSSI des Masters (int8),
                              //        data[2..4]=FW major/minor/patch, data[5]: Bit0..3 NODE_FLAG_*,
                              //        Bit4..7 Reset-Grund. Alle 2 s + nach jedem LED-Befehl.            [EINGEFROREN]
#define PKT_SET_BAR    0x08  // M->N  data[0]=Farbe, data[1]=Anzahl(0-8), data[2]=Hintergrund, data[5]=LED-Seq
#define PKT_SET_SPLIT  0x09  // M->N  data[0]=FarbeA, data[1]=AnzahlA(0-8), data[2]=FarbeB,   data[5]=LED-Seq
#define PKT_RESET      0x0A  // M->N  nodeId=0xFF, data[0..5]=Ziel-MAC (alles 0 = alle Nodes):
                              //        ID verwerfen, nach Zufallspause neu registrieren                  [EINGEFROREN]
#define PKT_BTN_ACK    0x0B  // M->N  nodeId=ID, data[0]=bestaetigte Druck-Seq, data[1]=Nonce
#define PKT_IDENTIFY   0x0C  // M->N  nodeId=ID, data[0]=Sekunden (0 -> 3, max 10): Node blinkt weiss    [EINGEFROREN]
#define PKT_OTA        0x0D  // M->N  nodeId=ID, data[0..3]=Firmware-Groesse (uint32 LE), data[4]=WLAN-Kanal:
                              //        Node verbindet sich mit dem Master-WLAN und laedt
                              //        http://192.168.4.1/fw/node.bin                                     [EINGEFROREN]
#define PKT_OTA_STATUS 0x0E  // N->M  nodeId=ID, data[0]=OTA_ST_*, data[1]=Fortschritt %, data[2]=OTA_ERR_* [EINGEFROREN]
#define PKT_PONG       0x0F  // M->N  Antwort auf PKT_STATUS: nodeId=ID, data[0]=aktuelle Soll-LED-Seq      [EINGEFROREN]
// Neue Pakettypen ab 0x10.

// LED-Sequenznummer (data[5] bei SET_LED/SET_BAR/SET_SPLIT): wie v2
//  - Master zaehlt je Node 1..255 (0 wird uebersprungen) bei jeder Aenderung des Soll-Zustands.
//  - Node wendet nur an, wenn seq != zuletzt angewendete seq, und quittiert per PKT_STATUS.
//  - Master wiederholt den Soll-Zustand, bis die Node ihn quittiert.
//  - Nach (Neu-)Registrierung: angewendete seq = 0 -> Master sendet den Soll-Zustand erneut.
// Tastendruck: wie v2 (Seq 1..255, Nonce neu je Registrierung und immer != vorige,
//  senden + Wiederholung bis PKT_BTN_ACK; Master quittiert JEDES Button-Paket).

// NODE_FLAG_* (PKT_STATUS data[5], Bit 0..3)
#define NODE_FLAG_OTA_BUSY    0x01  // Node laedt gerade Firmware
#define NODE_FLAG_OTA_FAILED  0x02  // letzter OTA-Versuch fehlgeschlagen
#define NODE_FLAG_IDENTIFY    0x04  // Node blinkt gerade zur Identifikation
#define NODE_FLAG_UPDATE_ONLY 0x08  // Node ist im Update-Modus (Protokoll passt nicht zum Master)
#define NODE_FLAG_MASK        0x0F
// Reset-Grund (data[5] >> 4) = esp_reset_reason_t: 1=Einschalten 3=Software 4=Absturz
//  5/6/7=Watchdog 9=Brownout (Stromversorgung zu schwach!)

// OTA_ST_* (PKT_OTA_STATUS data[0])
#define OTA_ST_STARTED   1
#define OTA_ST_PROGRESS  2
#define OTA_ST_FAILED    3
#define OTA_ST_SUCCESS   4   // geflasht, Node startet gleich neu
#define OTA_ST_WIFI      5   // verbindet sich gerade mit dem Master-WLAN

// OTA-Fehlercodes (PKT_OTA_STATUS data[2])
#define OTA_ERR_NONE       0
#define OTA_ERR_CONNECT    1   // keine TCP-Verbindung zum Master
#define OTA_ERR_HTTP       2   // keine/ungueltige HTTP-Antwort (kein 200)
#define OTA_ERR_SIZE       3   // Content-Length fehlt/ungueltig/zu gross
#define OTA_ERR_BEGIN      4   // Update.begin() fehlgeschlagen (Partition zu klein?)
#define OTA_ERR_DOWNLOAD   5   // Download abgebrochen / Timeout
#define OTA_ERR_WRITE      6   // Schreiben in den Flash fehlgeschlagen
#define OTA_ERR_VERIFY     7   // Update.end(): MD5/Image-Pruefung fehlgeschlagen
#define OTA_ERR_BUSY       8   // Node ist nicht bereit
#define OTA_ERR_IMAGE      9   // Datei ist keine passende Node-Firmware
#define OTA_ERR_WIFI      10   // Master-WLAN nicht erreichbar (voll? Passwort falsch?)

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
#define COL_COUNT   9

// Animations
#define ANIM_SOLID      0
#define ANIM_BLINK_SLOW 1   // 500 ms period (startet in der AN-Phase)
#define ANIM_BLINK_FAST 2   // 125 ms period (startet in der AN-Phase)
#define ANIM_PULSE      3   // breathing
#define ANIM_FLASH      4   // one quick flash, then solid
#define ANIM_BAR        5   // N LEDs solid color, rest bgColor (used with PKT_SET_BAR)
#define ANIM_SPLIT      6   // left N=colorA, rest=colorB (used with PKT_SET_SPLIT)
#define ANIM_COUNT      7

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
