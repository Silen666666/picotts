#pragma once
// Web-Oberflaeche des Masters (HTML + JavaScript).
// Liegt bewusst in einer .h-Datei: die Arduino IDE sucht in .ino-Dateien automatisch nach
// Funktionen (ctags) und haelt sonst die JavaScript-"function"s fuer C++ -> Compilerfehler
// "'function' does not name a type". Wird in master.ino NACH config.h eingebunden
// (MASTER_IS_NODE steuert Teile der Seite).
#include <Arduino.h>

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
.pupd{font-size:.68rem;font-weight:700;background:#2a1a4a;color:#d2a8ff;border:1px solid #a371f7;border-radius:10px;padding:1px 6px;flex-shrink:0}
.nd.upd{background:#2a1a4a;border-color:#a371f7;color:#d2a8ff}
.box{border:1px solid #30363d;border-radius:10px;padding:10px 12px;margin:10px 0;background:#0d1117}
.box b{color:#a8dadc}
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
  <div class="row"><span>WLAN-Kanal</span><span id="chInfo">-</span></div>
  <button class="btn btn-grey btn-small" id="chBtn" onclick="rechannel()">&#128246; WLAN-Kanal neu w&auml;hlen</button>
  <div class="fwmsg" id="chMsg"></div>
  <div id="aBox" class="box hidden">
    <b>&#9889; Alles aktualisieren</b>
    <label>Master-Firmware (master.ino.bin) &ndash; leer lassen = Master bleibt</label>
    <input type="file" id="aMFile" accept=".bin">
    <label>Node-Firmware (node.ino.bin) &ndash; leer lassen = gespeicherte verwenden</label>
    <input type="file" id="aNFile" accept=".bin">
    <button class="btn btn-start" id="aUpBtn" onclick="oneClick()">&#11014; Alles aktualisieren</button>
    <div class="prog hidden"><div id="aBar"></div></div>
    <div class="fwmsg" id="aMsg"></div>
    <div class="sub">Ablauf: Node-Firmware speichern &rarr; Master aktualisieren und neu starten &rarr; danach werden alle veralteten Nodes automatisch nacheinander aktualisiert. Nodes bitte eingeschaltet lassen.</div>
  </div>
  <details id="advSec" class="hidden">
    <summary>Einzeln hochladen (f&uuml;r Fortgeschrittene)</summary>
    <label>Master aktualisieren (master.ino.bin)</label>
    <input type="file" id="mFile" accept=".bin">
    <button class="btn btn-save" id="mUpBtn" onclick="upMaster()">&#11014; Master-Firmware hochladen</button>
    <div class="prog hidden"><div id="mBar"></div></div>
    <div class="fwmsg" id="mMsg"></div>
    <label>Node-Firmware speichern (node.ino.bin)</label>
    <input type="file" id="nFile" accept=".bin">
    <button class="btn btn-save" id="nUpBtn" onclick="upNode()">&#11014; Node-Firmware hochladen</button>
    <div class="prog hidden"><div id="nBar"></div></div>
    <div class="fwmsg" id="nMsg"></div>
  </details>
  <div id="fwNodeSec" class="hidden">
    <hr>
    <div class="row"><span>Gespeicherte Node-Firmware</span><strong id="nFw">keine</strong></div>
    <div class="sub" id="fsInfo"></div>
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
    <div class="instr">Arduino IDE &rarr; <b>Sketch &rarr; Kompilierte Bin&auml;rdatei exportieren</b>. Danach liegt im Sketch-Ordner unter <b>build/esp32.esp32.esp32/</b> die Datei <b>node.ino.bin</b> bzw. <b>master.ino.bin</b> &ndash; genau diese hochladen.<br>NICHT verwenden: <i>.merged.bin</i>, <i>.bootloader.bin</i>, <i>.partitions.bin</i>.<br>Am einfachsten: beide Dateien oben bei &bdquo;Alles aktualisieren&ldquo; ausw&auml;hlen und einmal klicken. Die Nodes verbinden sich zum Laden kurz mit dem Master-WLAN, laden die Datei nacheinander und starten neu. Nodes im <b>Update-Modus</b> (lila Blinken) passen nicht zum Master und werden dabei mit aktualisiert. Nodes mit alter Firmware v1/v2 m&uuml;ssen einmal per USB geflasht werden.</div>
  </details>
</div>

<script>
var modeNames=['Idle','CTF','Memory','Bomb','Reaktion','Simon Says','Heisse Kartoffel','King of the Hill','Tauziehen','Minesweeper','Knockout','Farbjagd','Whack-a-Mole'];
var tc=['#8b949e','#f85149','#388bfd','#3fb950','#e3b341'];
var knownNodes=-1;
var RST=['?','Einschalten','Reset-Taste','Software-Neustart','Absturz','Interrupt-Watchdog','Task-Watchdog','Watchdog','Deep-Sleep','Brownout','SDIO','USB','JTAG','eFuse','Spannungseinbruch','CPU-Lockup'];
var OERR={1:'keine Verbindung zum Master',2:'HTTP-Fehler',3:'Dateigroesse ungueltig',4:'Flash-Start fehlgeschlagen',5:'Download abgebrochen',6:'Schreibfehler',7:'Pruefung fehlgeschlagen',8:'Node nicht bereit',9:'Firmware passt nicht',10:'Master-WLAN nicht erreichbar',20:'keine Antwort',21:'Zeitueberschreitung',22:'offline',23:'nach Neustart alte Version',24:'WLAN voll'};

function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function rstPower(r){return r==9||r==14;}
function rstCrash(r){return r==4||r==5||r==6||r==7||r==15;}
function rstWarn(r){if(rstPower(r))return (RST[r]||r)+' – Stromversorgung pruefen!';if(rstCrash(r))return 'Absturz/Watchdog ('+(RST[r]||r)+')';return '';}
// r = Empfang beim Master, rn = Empfang laut Node (dBm)
function sig(r,rn){if(!r)return '<span class="pstat">?</span>';var l=r>=-55?4:r>=-65?3:r>=-75?2:r>=-85?1:0,h='<span class="sig" title="Master empf&auml;ngt '+r+' dBm'+(rn?', Node empf&auml;ngt '+rn+' dBm':'')+'">';for(var i=1;i<=4;i++)h+='<i'+(i<=l?' class="f"':'')+' style="height:'+(i*3)+'px"></i>';return h+'</span><span class="pstat">'+r+'</span>';}
function dis(id,v){var e=document.getElementById(id);if(e)e.disabled=!!v;}

var instrs={
  1:"Nodes durch Druecken fuer dein Team beanspruchen. Button wechselt Farbe (neutral&rarr;rot&rarr;blau). Nach der Zeit gewinnt das Team mit den meisten Nodes.",
  2:"Nodes leuchten 2s auf &ndash; merke dir die Farben. Druecke zwei gleich-farbige Nodes nacheinander. Kein Treffer? Beide gehen wieder aus.",
  3:"Ein Node blinkt ROT = Bombe! Die Sequenz der anderen Nodes zeigt die Reihenfolge zum Entschaerfen. Bombe druecken = Sequenz nochmal zeigen.",
  4:"Ein zufaelliger Node leuchtet GELB. Wer zuerst drueckt, bekommt einen Punkt. Reaktionszeit wird gemessen.",
  5:"Jeder mitspielende Node hat eine feste Farbe (nach Nummer: ROT, BLAU, GRUEN …). Beim Start kurz alle Farben merken! Simon zeigt eine Sequenz &ndash; jeder Node leuchtet in seiner Farbe. Reihenfolge nachdr&uuml;cken. Jede Runde wird die Sequenz um einen Schritt l&auml;nger. Falscher Druck = Aus. W&auml;hrend Eingabe zeigen Nodes ihre Farbe gedimmt als Erinnerung.",
  6:"Ein Node haelt die Heisse Kartoffel (orange blinkend). Druecken gibt sie weiter. Wer sie beim Alarm haelt, verliert ein Leben. 3 Leben = 3 Balken.",
  7:"Der GOLDENE Node ist der Thron. Druecken = du haeltst ihn. Haltezeit = Punkte. Der Thron wandert alle 30s zu einem anderen Node weiter!",
  8:"Die erste Haelfte der Nodes (kleinere Nummern) ist ROT, der Rest BLAU &ndash; beim Start leuchten die Teamfarben kurz auf. Jeder Druck verschiebt den Balken. Erste Farbe, die alle 8 LEDs fuellt, gewinnt (sonst wer bei Zeitende vorne liegt)!",
  9:"Manche Nodes sind Minen. Druecke Nodes frei: Gruen = sicher (+Punkt), Rot = Mine (-Leben). 3 Leben insgesamt. Alle sicheren Nodes finden = Sieg!",
  10:"Wie Reaktion, aber mit Elimination. Ein Node leuchtet gelb &ndash; 2s Gnadenfrist nach dem ersten Treffer. Wer nicht drueckt, verliert ein Leben. Letzter gewinnt!",
  11:"Nodes zeigen kurz ihre zugewiesene Farbe. Dann blinkt der erste mitspielende Node (kleinste Nummer) die ZIELFARBE. Wer zuerst den passenden Node drueckt, bekommt einen Punkt. Meiste Punkte nach allen Runden gewinnt.",
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
  for(var i=1;i<=32;i++){var el=document.getElementById('n'+i);if(el)q+='&n'+i+'='+encodeURIComponent(el.value||'');}
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
      r.innerHTML='<div class="pl"><span class="pdot"></span><span class="pname"></span><span class="pupd hidden" title="Firmware passt nicht zum Master (Update-Modus) &ndash; unten aktualisieren">Update n&ouml;tig</span><span class="psig"></span><span class="pfw"></span><button class="fbtn" onclick="ident('+n.id+')">Finden</button></div><div class="pnote hidden"></div>';
      rows[n.id]=r;
    }
    seen[n.id]=1;
    var pl=r.firstChild,c=pl.children;
    pl.className='pl'+(n.on?'':' dead');
    c[0].className='pdot '+(n.on?'on':'off');
    c[1].textContent=n.id+'. '+n.name;
    c[2].className='pupd'+(n.upd?'':' hidden');
)rawliteral"
#if MASTER_IS_NODE
R"rawliteral(    var sh=n.on?(n.local?'<span class="pstat" title="Taster und LEDs direkt am Master (lokal)">Master</span>':sig(n.rssi,n.rssiN)):'<span class="pstat">offline</span>';
)rawliteral"
#else
R"rawliteral(    var sh=n.on?sig(n.rssi,n.rssiN):'<span class="pstat">offline</span>';
)rawliteral"
#endif
R"rawliteral(    if(c[3]._h!==sh){c[3].innerHTML=sh;c[3]._h=sh;}
    var bad=!!(n.fw&&n.fw!==mfw);
    c[4].className='pfw'+(bad?' bad':'');c[4].textContent=n.fw||'';c[4].title=bad?'andere Firmware als der Master ('+mfw+')':'Firmware';
    c[5].disabled=!n.on;
    var w=rstWarn(n.rst),pn=r.lastChild;
    pn.className='pnote'+(w?'':' hidden')+(rstPower(n.rst)?' red':'');
    pn.textContent=w?('⚠ Letzter Neustart: '+w):'';
    if(r.parentNode!==nn)nn.appendChild(r);
  });
  for(var k in rows){if(!seen[k]){if(rows[k].parentNode)rows[k].parentNode.removeChild(rows[k]);delete rows[k];}}
}

function renderBanners(d,nl){
  var b=[];
  if(d.espnow===false)b.push(['red','Funk (ESP-NOW) konnte nicht gestartet werden &ndash; Master bitte neu starten (Strom kurz trennen).']);
  if(d.legacy&&d.legacy.length)b.push(['','Node(s) mit alter Firmware v1/v2 im WLAN ('+d.legacy.map(esc).join(', ')+') &ndash; diese funken nicht per ESP-NOW und belegen WLAN-Pl&auml;tze. Einmal per USB mit der neuen node.ino flashen.']);
  if(d.apMax&&d.stations>=d.apMax)b.push(['red','WLAN voll ('+d.stations+' Ger&auml;te) &ndash; Nodes k&ouml;nnen sich zum Aktualisieren nicht verbinden. Alte v1/v2-Nodes oder &uuml;berz&auml;hlige Handys trennen.']);
  var pw=[];nl.forEach(function(n){if(rstPower(n.rst))pw.push(n.id);});
  if(pw.length)b.push(['red','Brownout bei Node '+pw.join(', ')+' &ndash; Stromversorgung pr&uuml;fen (Netzteil, Kabel, Akku).']);
  if(rstPower(d.masterRst))b.push(['red','Master wurde wegen Unterspannung neu gestartet &ndash; Stromversorgung pr&uuml;fen!']);
  var up=[];nl.forEach(function(n){if(n.on&&n.upd)up.push(n.id);});
  if(up.length)b.push(['','Node '+up.join(', ')+' im Update-Modus (Firmware passt nicht zum Master, spielt nicht mit) &ndash; unten &bdquo;Veraltete Nodes aktualisieren&ldquo;.']);
  var od=[];nl.forEach(function(n){if(n.on&&!n.upd&&n.fw&&n.fw!==d.masterFw)od.push(n.id);});
  if(od.length)b.push(['','Node '+od.join(', ')+' mit anderer Firmware als der Master ('+esc(d.masterFw)+') &ndash; unten unter &bdquo;Firmware &amp; OTA&ldquo; aktualisieren.']);
  if(d.rollout&&d.rollout.active)b.push(['','Firmware-Update der Nodes l&auml;uft &ndash; Spiele sind solange gesperrt.']);
  else if(d.rollout&&d.rollout.autoIn>0)b.push(['','Nodes werden in '+d.rollout.autoIn+' s automatisch aktualisiert &ndash; bitte noch kein Spiel starten.']);
  var h=b.map(function(x){return '<div class="warn '+x[0]+'">&#9888; '+x[1]+'</div>';}).join('');
  var el=document.getElementById('banners');if(el._h!==h){el.innerHTML=h;el._h=h;}
}

var upBusy=false;
function upLock(v){['mUpBtn','nUpBtn','aUpBtn','roBtn1','roBtn2','fwDel','chBtn'].forEach(function(i){dis(i,v);});}
function upDone(){upBusy=false;stBusy=0;updateStatus();}   // Knoepfe sofort wieder richtig setzen
function binBad(f,m){
  if(!/merged|bootloader|partitions/i.test(f.name))return false;
  m.className='fwmsg err';m.textContent='Falsche Datei ('+f.name+'): bitte die <sketch>.ino.bin verwenden, nicht .merged/.bootloader/.partitions.bin.';return true;}
// Datei per XHR hochladen (mit Fortschritt); cb(r), r={ok,msg,version}
function xhrUp(url,f,bar,m,pre,cb){
  bar.parentNode.classList.remove('hidden');bar.style.width='0%';
  m.className='fwmsg';m.textContent=pre+'Lade hoch...';
  var fd=new FormData();fd.append('firmware',f,f.name);
  var x=new XMLHttpRequest();x.open('POST',url,true);
  x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);bar.style.width=p+'%';m.textContent=pre+(p<100?('Lade hoch... '+p+' %'):'Wird geprueft...');}};
  x.onload=function(){var r;try{r=JSON.parse(x.responseText);}catch(e){r={ok:false,msg:'Unerwartete Antwort (HTTP '+x.status+')'};}
    m.className='fwmsg'+(r.ok?'':' err');m.textContent=pre+(r.msg||'');if(!r.ok)bar.style.width='0%';cb(r);};
  x.onerror=function(){m.className='fwmsg err';m.textContent=pre+'Verbindung abgebrochen.';bar.style.width='0%';cb({ok:false});};
  x.send(fd);
}
// Einzel-Upload. cb(r) liefert true, wenn die Sperre bleiben soll (Master startet neu)
function upFile(url,fileId,barId,msgId,cb){
  if(upBusy)return;                                   // kein zweiter Upload parallel (Doppelklick)
  var inp=document.getElementById(fileId),f=inp.files&&inp.files[0],m=document.getElementById(msgId);
  if(!f){m.className='fwmsg err';m.textContent='Bitte zuerst eine .bin-Datei auswaehlen.';return;}
  if(binBad(f,m))return;
  upBusy=true;upLock(true);
  xhrUp(url,f,document.getElementById(barId),m,'',function(r){if(!(cb&&cb(r)))upDone();});
}
// Nach Master-Update / Kanalwechsel: warten, bis der NEU GESTARTETE Master antwortet (max. ~60 s), dann neu laden
function waitMaster(ver,m,pre){
  var t0=Date.now();
  m.className='fwmsg';m.textContent=pre+'Warte auf den Master...';
  function next(){
    if(Date.now()-t0>60000){m.className='fwmsg err';m.textContent=pre+'Master antwortet nicht. Handy wieder mit dem Spiel-WLAN verbinden und die Seite neu laden.';upDone();return;}
    setTimeout(poll,2000);}
  function poll(){
    var done=false;
    var to=setTimeout(function(){if(!done){done=true;next();}},3000);
    fetch('/status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
      if(done)return;done=true;clearTimeout(to);
      if(d.uptime*1000>Date.now()-t0+1000){next();return;}   // noch der alte Lauf (Neustart folgt gleich)
      if(ver&&d.masterFw!==ver){m.className='fwmsg err';m.textContent=pre+'Master laeuft mit '+d.masterFw+' statt '+ver+' (Rollback?) - Nodes werden nicht automatisch aktualisiert.';setTimeout(function(){location.reload();},6000);return;}
      m.textContent=pre+'Master ist wieder da - Seite wird neu geladen...';
      setTimeout(function(){location.reload();},1500);
    }).catch(function(){if(done)return;done=true;clearTimeout(to);next();});
  }
  setTimeout(poll,3000);
}
function upMaster(){upFile('/update?json=1','mFile','mBar','mMsg',function(r){if(!r.ok)return false;waitMaster(r.version,document.getElementById('mMsg'),r.msg+' ');return true;});}
function upNode(){upFile('/fw/node','nFile','nBar','nMsg',null);}
// Ein-Klick: (1) Node-Datei speichern, (2) Master aktualisieren (?auto=1: nach dem Neustart startet
// der Master das Node-Update selbst) oder (3) nur Node-Datei: Node-Update sofort starten
function oneClick(){
  if(upBusy)return;
  var mi=document.getElementById('aMFile'),ni=document.getElementById('aNFile');
  var mf=mi.files&&mi.files[0],nf=ni.files&&ni.files[0];
  var m=document.getElementById('aMsg'),bar=document.getElementById('aBar');
  if(!mf&&!nf){m.className='fwmsg err';m.textContent='Bitte mindestens eine Datei auswaehlen (master.ino.bin und/oder node.ino.bin).';return;}
  if((mf&&binBad(mf,m))||(nf&&binBad(nf,m)))return;
  upBusy=true;upLock(true);
  function master(){
    xhrUp('/update?json=1&auto=1',mf,bar,m,'Master: ',function(r){
      if(!r.ok){upDone();return;}
      waitMaster(r.version,m,'Master startet neu ... Nodes werden danach automatisch aktualisiert. ');
    });
  }
  function nodesNow(){
    m.className='fwmsg';m.textContent='Node-Firmware gespeichert - starte das Update der Nodes...';
    post('/fw/rollout','force=0').then(function(r){return r.text().then(function(t){
      if(r.ok)m.textContent='Node-Firmware gespeichert - Update der Nodes gestartet (Fortschritt unten).';
      else{m.className='fwmsg err';m.textContent='Node-Firmware gespeichert, Update nicht gestartet: '+t;}
      upDone();});}).catch(function(){m.className='fwmsg err';m.textContent='Keine Verbindung zum Master.';upDone();});
  }
  if(nf)xhrUp('/fw/node',nf,bar,m,'Node: ',function(r){if(!r.ok){upDone();return;}if(mf)master();else nodesNow();});
  else master();
}
function rechannel(){
  if(upBusy)return;
  if(!confirm('WLAN-Kanal neu waehlen?\nDer Master startet neu und sucht den freiesten Kanal. Das Handy muss sich evtl. neu verbinden, die Nodes finden den Master automatisch wieder.'))return;
  var m=document.getElementById('chMsg');
  post('/channel','').then(function(r){return r.text().then(function(t){
    if(!r.ok){m.className='fwmsg err';m.textContent=t;return;}
    upBusy=true;upLock(true);waitMaster(null,m,'Neuer Kanal wird gesucht. ');});})
  .catch(function(){m.className='fwmsg err';m.textContent='Keine Verbindung zum Master.';});
}
function rollout(f){if(f&&!confirm('Alle aktiven Nodes neu flashen (auch aktuelle)?'))return;act('/fw/rollout','force='+(f?1:0));}
function roStop(){act('/fw/cancel','');}
function fwDelete(){if(!confirm('Gespeicherte Node-Firmware loeschen?'))return;act('/fw/delete','');}

function renderFw(d,nl){
  document.getElementById('mFw').textContent=d.masterFw||'?';
  var r=d.masterRst||0,w=rstWarn(r),mr=document.getElementById('mRst');
  mr.textContent=w?w:(RST[r]||r);mr.className=w?'bad':'';
  document.getElementById('chInfo').textContent=(d.channel||'?')+(d.chAuto?' (automatisch gewaehlt)':' (fest in config.h)');
  var hasFs=!!d.fs;
  document.getElementById('aBox').classList.toggle('hidden',!hasFs);
  document.getElementById('advSec').classList.toggle('hidden',!hasFs);
  document.getElementById('fwNodeSec').classList.toggle('hidden',!hasFs);
  var ro=d.rollout||{},roAct=!!ro.active,auto=ro.autoIn>0,run=!!d.running;
  dis('startBtn',roAct);dis('forgetBtn',roAct);dis('resetBtn',roAct);
  dis('chBtn',!d.chAuto||roAct||auto||upBusy||run);
  if(!hasFs)return;
  var nf=d.nodeFw||{};
  document.getElementById('nFw').textContent=nf.present?(nf.version+' ('+Math.round(nf.size/1024)+' KB)'):'keine';
  document.getElementById('fsInfo').textContent=d.fs.ok?('Speicher: '+Math.round(d.fs.used/1024)+' von '+Math.round(d.fs.total/1024)+' KB belegt'):'Dateisystem nicht verfuegbar!';
  dis('mUpBtn',roAct||upBusy||run);dis('nUpBtn',roAct||upBusy||run||!d.fs.ok);dis('aUpBtn',roAct||upBusy||run);
  dis('roBtn1',roAct||upBusy||run||!nf.present);dis('roBtn2',roAct||upBusy||run||!nf.present);
  dis('fwDel',roAct||upBusy||auto||!nf.present);dis('roCancel',!((roAct&&!ro.cancel)||auto));
  document.getElementById('fwMsg').textContent=d.fwMsg||'';
  document.getElementById('roState').textContent=roAct?(ro.cancel?'Wird nach dem aktuellen Node beendet...':'Update laeuft – Nodes bitte eingeschaltet lassen!'):
    (auto?('Automatisches Node-Update startet in '+ro.autoIn+' s ("Abbrechen" verhindert es).'):(run?'Waehrend eines Spiels gesperrt.':''));
  var byId={};nl.forEach(function(n){byId[n.id]=n;});
  function nm(id){return 'Node '+id+(byId[id]?' ('+esc(byId[id].name)+')':'');}
  function row(a,b,c){return '<div class="rorow"><span>'+a+'</span><span style="color:'+c+'">'+b+'</span></div>';}
  var h='';
  if(roAct&&ro.cur){var n=byId[ro.cur]||{},o=n.ota||{};
    var s=o.st==2?('l&auml;dt '+o.pct+' %'):o.st==1?'Download startet...':o.st==5?'verbindet sich mit dem WLAN...':o.st==4?'geflasht, startet neu...':
      (ro.phase==3?'wartet auf freien WLAN-Platz...':ro.phase==2?'warte auf Neustart...':'wird angesprochen...');
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

    // Node-Chips: gruen=verbunden, lila=Update-Modus, rot=offline
    var g=document.getElementById('nodesGrid');g.innerHTML='';
    if(nl.length===0){var ph=document.createElement('div');ph.className='nd';ph.textContent='–';g.appendChild(ph);}
    nl.forEach(function(n){var dot=document.createElement('div');dot.className='nd'+(n.on?(n.upd?' upd':' on'):' off');dot.textContent=n.id;dot.title=n.name+(n.on?(n.upd?' (Update-Modus)':' (verbunden)'):' (offline)');g.appendChild(dot);});

    // Spielerliste mit Verbindungsstatus, Signal, Firmware, Finden
    renderNodes(nl,d.masterFw);
    renderBanners(d,nl);
    renderFw(d,nl);

    // Info-Zeile (Bombe/Simon/Tauziehen/Minesweeper)
    var il=document.getElementById('infoLine');
    if(d.running&&d.info&&d.info.length>0){il.classList.remove('hidden');il.textContent=d.info;}
    else if(!d.running&&d.gameMsg){il.classList.remove('hidden');il.textContent=d.gameMsg;}
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
