// Lightning Warning Station - detector with built-in web dashboard
//
// TWO MODES (switch on the web page):
//   NORMAL     - only real AS3935 sensor detections are used. Wand messages are ignored.
//   SIMULATION - the emulator wand is tracked live: its distance is estimated from its radio
//                signal strength (RSSI), and pressing its button sends a SIMULATED strike.
//                Strike strength = how long the button is held (tap = weak, 2 s hold = 100%).
//                Simulated strikes are labeled and counted separately from real detections.
//
// The ESP32 creates its own Wi-Fi network (no venue Wi-Fi needed):
//   join "LightningStation" (password lightning123), open http://192.168.4.1
//
// NO-INT VERSION: events are read over SPI (polling), so the sensor's INT wire is not needed.
//
// Wiring:
//   AS3935: SCK 18, MISO 19, MOSI 23, CS 5, 3V3, GND   (INT left disconnected)
//   LED:    GPIO 26 -> 220 ohm -> LED long leg; short leg -> GND
//   Sound:  GPIO 25 -> buzzer + (buzzer - to GND)   OR   GPIO 25 -> 220 ohm -> speaker red, speaker black -> GND
//
// Library: "SparkFun AS3935 Lightning Detector Arduino Library"

#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include "SparkFun_AS3935.h"

// ---------- settings ----------
#define TUNE_CAP      0       // factory tuning (SparkFun boards ship within 1% of 500 kHz)
const char* AP_SSID = "LightningStation";
const char* AP_PASS = "lightning123";
int   alarmKm      = 20;      // alarm if a strike is this close or closer (changeable on the page)
const unsigned long ALARM_MS = 10000;   // LED flashes for 10 seconds
const unsigned long CLEAR_MS = 15000;   // all-clear countdown after the last strike
                                        // (15 s for the demo; the real safety rule is 30 minutes)

// Wand signal strength -> simulated distance
const int RSSI_NEAR = -35;    // dBm with the wand right at the station  -> 1 km
const int RSSI_FAR  = -85;    // dBm with the wand across the room       -> 40 km

#define CS_PIN    5
#define LED_PIN   26
#define SPK_PIN   25
// ------------------------------

SparkFun_AS3935 lightning;
WebServer server(80);

#define SIM_EVENT 0x80   // our own code for a simulated strike (not a sensor event)
struct Event { unsigned long ms; uint8_t type; int km; long strength; };
const int LOG_SIZE = 20;
Event logBuf[LOG_SIZE];
int logCount = 0;
unsigned long strikes = 0, disturbers = 0, noises = 0, simStrikes = 0;
int lastKm = -1;
unsigned long lastStrikeMs = 0;

bool simMode = false;

// sensor health
bool sensorOk = false;
unsigned long lastHealthMs = 0;
const uint8_t NOISE_LEVEL = 2;

// wand tracking (written in the radio callback, processed in loop)
volatile bool beaconPending = false, strikePending = false;
volatile int  beaconRssi = -90, strikeRssi = -90;
volatile uint8_t strikeStrength = 0;
volatile uint8_t chargePct = 0;
volatile unsigned long chargeMs = 0;
float wandRssi = -90;            // smoothed signal strength
unsigned long lastBeaconMs = 0;

// alarm
bool alarmOn = false;
int alarmStrength = 100;   // 10-100: sets siren loudness/speed and LED blink rate
unsigned long alarmStart = 0;

int rssiToKm(float rssi) {
  float r = constrain(rssi, (float)RSSI_FAR, (float)RSSI_NEAR);
  int km = 1 + (int)((RSSI_NEAR - r) * 39.0f / (RSSI_NEAR - RSSI_FAR) + 0.5f);
  return constrain(km, 1, 40);
}
bool wandOnline() { return lastBeaconMs && millis() - lastBeaconMs < 1500; }

void addEvent(uint8_t type, int km, long strength) {
  for (int i = LOG_SIZE - 1; i > 0; i--) logBuf[i] = logBuf[i - 1];
  logBuf[0] = { millis(), type, km, strength };
  if (logCount < LOG_SIZE) logCount++;
}

// ---- sound (square-wave tone on GPIO 25; works on ESP32 Arduino core 2.x and 3.x) ----
bool spkAttached = false;

// Idle: the pin is a plain output held LOW, so no stray PWM/noise reaches the buzzer.
void spkOff() {
  if (spkAttached) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcDetach(SPK_PIN);
#else
    ledcDetachPin(SPK_PIN);
#endif
    spkAttached = false;
  }
  pinMode(SPK_PIN, OUTPUT);
  digitalWrite(SPK_PIN, LOW);
}

void spkTone(int hz) {
  if (hz <= 0) { spkOff(); return; }
  if (!spkAttached) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(SPK_PIN, hz, 8);
#else
    ledcSetup(0, hz, 8);
    ledcAttachPin(SPK_PIN, 0);
#endif
    spkAttached = true;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(SPK_PIN, hz);
#else
  ledcWriteTone(0, hz);
#endif
}

void spkBegin() { spkOff(); }

// all-clear countdown + counters the web page uses to trigger phone alerts
bool dangerActive = false;
unsigned long dangerMs = 0;
unsigned long dangerSeq = 0, clearSeq = 0;
bool chimeOn = false;
unsigned long chimeStart = 0;

void startAlarm(int strength = 100) {
  alarmOn = true; alarmStart = millis(); alarmStrength = constrain(strength, 10, 100);
  dangerActive = true; dangerMs = millis();      // every strike restarts the all-clear countdown
  dangerSeq++;
  chimeOn = false;
}
void stopAlarm()  { alarmOn = false; digitalWrite(LED_PIN, LOW); spkTone(0); }

// Countdown finished -> "all clear" two-tone chime (rising = safe)
void updateClear() {
  if (dangerActive && !alarmOn && millis() - dangerMs >= CLEAR_MS) {
    dangerActive = false;
    clearSeq++;
    chimeOn = true; chimeStart = millis();
    Serial.println("ALL CLEAR");
  }
  if (chimeOn) {
    unsigned long t = millis() - chimeStart;
    if (t < 200) spkTone(880);
    else if (t < 450) spkTone(1320);
    else { spkTone(0); chimeOn = false; }
  }
}

void updateAlarm() {
  if (!alarmOn) return;
  unsigned long t = millis() - alarmStart;
  if (t >= ALARM_MS) { stopAlarm(); return; }
  // Stronger strike = faster blinking, faster siren sweep, louder buzzer
  unsigned long blink = map(alarmStrength, 10, 100, 1200, 300);     // ms per blink cycle
  digitalWrite(LED_PIN, (t % blink) < blink * 6 / 10 ? HIGH : LOW);
  static unsigned long lastToneMs = 0;                  // siren: 1 s sweep 600 -> 1600 -> 600 Hz
  if (millis() - lastToneMs < 10) return;
  lastToneMs = millis();
  unsigned long sweep = map(alarmStrength, 10, 100, 2000, 500);    // ms per siren sweep
  unsigned long ph = t % sweep;
  unsigned long half = sweep / 2;
  int hz = ph < half ? 600 + ph * 1000 / half : 1600 - (ph - half) * 1000 / half;
  spkTone(hz);
  // loudness: PWM duty from ~4% (weak) to 50% (strong); a square wave is loudest at 50%
  int duty = map(alarmStrength, 10, 100, 10, 128);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(SPK_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// ---------- radio messages from the wand ----------
void handleMsg(const uint8_t* data, int len, int rssi) {
  if (len >= 4 && memcmp(data, "LBCN", 4) == 0) {           // position beacon (5 per second)
    beaconRssi = rssi; beaconPending = true;
  } else if (len >= 5 && memcmp(data, "LCHG", 4) == 0) {    // button being held: live strength
    chargePct = data[4]; chargeMs = millis();
  } else if (len >= 5 && memcmp(data, "LSIM", 4) == 0) {    // simulated strike + strength
    strikeRssi = rssi; strikeStrength = data[4]; strikePending = true;
  }
}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onWandRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  handleMsg(data, len, info->rx_ctrl->rssi);
}
#else
void onWandRecv(const uint8_t* mac, const uint8_t* data, int len) {   // core 2.x: no RSSI
  handleMsg(data, len, -60);
}
#endif

void handleWand() {
  if (beaconPending) {
    beaconPending = false;
    int r = beaconRssi;
    wandRssi = (lastBeaconMs && millis() - lastBeaconMs < 2000) ? 0.7f * wandRssi + 0.3f * r : r;  // smooth jitter
    lastBeaconMs = millis();
  }
  if (strikePending) {
    strikePending = false;
    if (!simMode) return;                       // NORMAL mode ignores the wand
    float r = wandOnline() ? wandRssi : strikeRssi;
    int km = rssiToKm(r);
    long strength = strikeStrength;
    simStrikes++;
    addEvent(SIM_EVENT, km, strength);
    Serial.printf("SIMULATED strike  %d km  strength %ld%%\n", km, strength);
    if (km <= alarmKm) startAlarm(strength);
  }
}

// ---------- real sensor ----------
void checkSensor() {
  if (millis() - lastHealthMs < 3000) return;
  lastHealthMs = millis();
  sensorOk = (lightning.readNoiseLevel() == NOISE_LEVEL);
}

void handleSensor() {
  static unsigned long lastPoll = 0;            // no INT wire: poll over SPI every 20 ms
  if (millis() - lastPoll < 20) return;
  lastPoll = millis();
  uint8_t ev = lightning.readInterruptReg();    // 0 = nothing new (reading clears the event)
  if (ev == 0) return;

  if (ev == LIGHTNING) {
    int km = lightning.distanceToStorm();       // 1 = overhead, 63 = out of range
    long energy = lightning.lightningEnergy();  // relative strength (no units)
    strikes++; lastKm = km; lastStrikeMs = millis();
    addEvent(LIGHTNING, km, energy);
    Serial.printf("LIGHTNING  %d km  energy %ld\n", km, energy);
    if (km != 63 && km <= alarmKm) startAlarm();
  } else if (ev == DISTURBER_DETECT) {
    disturbers++; addEvent(DISTURBER_DETECT, -1, 0);
  } else if (ev == NOISE_TO_HIGH) {
    noises++; addEvent(NOISE_TO_HIGH, -1, 0);
  }
}

// ---------------- web page ----------------
const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lightning Station</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0}
main{max-width:520px;margin:auto;padding:16px}
h1{font-size:1.3rem;margin:.2rem 0 1rem}
.card{background:#1e293b;border-radius:12px;padding:14px;margin-bottom:12px}
.big{font-size:2rem;font-weight:700}
.ok{color:#4ade80}.warn{color:#facc15}.danger{color:#f87171}.sim{color:#c084fc}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;text-align:center}
.row{display:flex;justify-content:space-between;gap:8px}
.lbl{font-size:.75rem;color:#94a3b8}
table{width:100%;border-collapse:collapse;font-size:.9rem}
td,th{padding:4px;text-align:left;border-bottom:1px solid #334155}
button,input{font-size:1rem;padding:8px 12px;border-radius:8px;border:0}
button{background:#3b82f6;color:#fff;margin:4px 4px 0 0}
button.off{background:#334155;color:#94a3b8}
input{width:70px}
.note{font-size:.8rem;color:#94a3b8}
.bar{height:14px;background:#334155;border-radius:7px;overflow:hidden;margin-top:6px}
.fill{height:100%;width:0;background:#c084fc;transition:width .3s}
#simcard{display:none;border:1px solid #c084fc}
#overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;flex-direction:column;
 background:rgba(220,38,38,.9);color:#fff;font-size:2rem;font-weight:800;text-align:center;z-index:9;animation:pulse 1s infinite}
#overlay.clear{background:rgba(22,163,74,.92);animation:none}
@keyframes pulse{50%{opacity:.6}}
#cd{font-size:3rem;font-weight:800;color:#facc15}
</style></head><body><main>
<div id="overlay" onclick="this.style.display='none'"><div id="ovText">LIGHTNING NEARBY</div>
 <div style="font-size:1rem;font-weight:400;margin-top:8px" id="ovSub">Get out of the water and take shelter</div></div>
<h1>&#9889; Lightning Warning Station</h1>
<div class="card" id="alertCard"><div class="lbl">PHONE ALERTS</div>
 <button id="bAlert" onclick="enableAlerts()">Enable alerts on this phone</button>
 <div class="note" id="alertNote">Your phone will vibrate and sound an alarm when lightning is detected. Keep this page open.</div>
</div>
<div class="card row">
 <div><div class="lbl">STATION</div><div id="link" class="danger">&#9679; Connecting...</div></div>
 <div><div class="lbl">SENSOR</div><div id="sens" class="warn">&#9679; Checking...</div></div>
 <div><div class="lbl">UPTIME</div><div id="up">--</div></div>
</div>
<div class="card"><div class="lbl">MODE</div>
 <button id="bNorm" onclick="setMode(0)">Normal</button>
 <button id="bSim" onclick="setMode(1)">Simulation</button>
 <div id="modeNote" class="note"></div>
</div>
<div class="card" id="simcard"><div class="lbl">WAND (SIMULATION)</div>
 <div class="row"><div id="wand" class="warn">&#9679; Searching...</div><div id="wsig" class="note"></div></div>
 <div class="big sim" id="wkm">--</div>
 <div class="note">Simulated storm distance (closer wand = closer storm)</div>
 <div class="bar"><div class="fill" id="wbar"></div></div>
 <div class="note" style="margin-top:10px">Last strike strength</div>
 <div class="big" id="chg" style="color:#facc15">--</div>
 <div class="bar"><div class="fill" id="cbar" style="background:#facc15"></div></div>
</div>
<div class="card"><div class="lbl">STATUS</div><div id="status" class="big ok">All clear</div>
<div id="last" class="note"></div>
<div id="cdBox" style="display:none;margin-top:8px"><div class="lbl">STAY SHELTERED - ALL CLEAR IN</div>
 <div id="cd">--</div><div class="bar"><div class="fill" id="cdbar" style="background:#facc15"></div></div>
 <div class="note">Every new strike restarts the countdown (demo: <span id="cdt"></span> s; real rule: 30 minutes).</div></div>
</div>
<div class="card grid">
 <div><div class="lbl">Last distance</div><div id="km" class="big">--</div></div>
 <div><div class="lbl">Last strength</div><div id="str" class="big">--</div></div>
 <div><div class="lbl">Strikes</div><div id="n" class="big">0</div></div>
</div>
<div class="card"><div class="lbl">ALARM</div>
 Flash warning light within <input id="th" type="number" min="1" max="40"> km
 <button onclick="setTh()">Save</button><br>
 <button onclick="fetch('/test')">Test alarm</button>
 <button onclick="fetch('/stop')" style="background:#64748b">Stop alarm</button>
</div>
<div class="card"><div class="lbl">RECENT EVENTS</div>
<table><thead><tr><th>Time ago</th><th>Type</th><th>Distance</th><th>Strength</th></tr></thead><tbody id="log"></tbody></table>
<div id="counts" class="note"></div></div>
<p class="note">Real detections come from the AS3935 sensor. Its strength value is a relative energy reading
(no physical units). Simulated strikes come from the emulator wand and are labeled in purple.</p>
<p class="note">Direction: a single-antenna sensor (AS3935) can measure distance but not direction.</p>
</main><script>
let thLoaded=false, fails=0, seenD=null, seenC=null, alertsOn=false, ac=null;
function enableAlerts(){
 try{ac=ac||new (window.AudioContext||window.webkitAudioContext)();ac.resume();}catch(e){}
 alertsOn=true;beep(880,.15);if(navigator.vibrate)navigator.vibrate(100);
 $('bAlert').textContent='Alerts enabled';$('bAlert').className='off';
 $('alertNote').textContent='Alerts on. Keep this page open and the screen on'+(navigator.vibrate?'.':' (this phone does not support vibration; sound only).');
}
function beep(f,dur,when){if(!ac)return;const t=ac.currentTime+(when||0);const o=ac.createOscillator(),g=ac.createGain();
 o.type='square';o.frequency.value=f;g.gain.setValueAtTime(.15,t);g.gain.setValueAtTime(0,t+dur);o.connect(g);g.connect(ac.destination);o.start(t);o.stop(t+dur+.05);}
function sirenSound(){if(!ac)return;const o=ac.createOscillator(),g=ac.createGain(),t=ac.currentTime;o.type='sawtooth';g.gain.value=.15;
 for(let i=0;i<6;i++){o.frequency.setValueAtTime(600,t+i*.5);o.frequency.linearRampToValueAtTime(1400,t+i*.5+.25);o.frequency.linearRampToValueAtTime(600,t+i*.5+.5);}
 o.connect(g);g.connect(ac.destination);o.start(t);o.stop(t+3);}
function showOverlay(clear,text,sub){const o=$('overlay');o.className=clear?'clear':'';$('ovText').textContent=text;$('ovSub').textContent=sub;o.style.display='flex';}
function phoneAlerts(d){
 if(seenD===null){seenD=d.dseq;seenC=d.cseq;return;}      // don't alert for events from before the page opened
 if(d.dseq!==seenD){seenD=d.dseq;
  const ev=d.events.find(e=>e.t=='L'||e.t=='S');
  showOverlay(false,(ev&&ev.t=='S')?'SIMULATED STRIKE':'LIGHTNING NEARBY',
   (ev?kmTxt(ev.km)+' away. ':'')+'Get out of the water and take shelter.');
  if(alertsOn){if(navigator.vibrate)navigator.vibrate([600,200,600,200,600,200,600]);sirenSound();}
 }
 if(d.cseq!==seenC){seenC=d.cseq;
  showOverlay(true,'ALL CLEAR','No strikes for '+d.clearTotal+' s. Safe to return.');
  setTimeout(()=>{if($('overlay').className=='clear')$('overlay').style.display='none'},4000);
  if(alertsOn){if(navigator.vibrate)navigator.vibrate(200);beep(880,.2);beep(1320,.25,.22);}
 }
 if(!d.alarm&&d.clear>0&&$('overlay').style.display=='flex'&&$('overlay').className=='')$('overlay').style.display='none';
}
function $(i){return document.getElementById(i)}
function setLink(ok){$('link').innerHTML=ok?'&#9679; Online':'&#9679; Offline';$('link').className=ok?'ok':'danger';}
function ago(s){return s<60?s+'s':s<3600?Math.floor(s/60)+'m':Math.floor(s/3600)+'h'}
function kmTxt(k){return k==1?'Overhead':k==63?'Out of range':k+' km'}
function strTxt(e){return e.t=='S'?e.s+'%':e.t=='L'?String(e.s):''}
async function tick(){
 try{
  const d=await (await fetch('/data',{cache:'no-store'})).json();
  fails=0;setLink(true);
  $('sens').innerHTML=d.sensorOk?'&#9679; Active':'&#9679; Not responding';$('sens').className=d.sensorOk?'ok':'danger';
  $('up').textContent=ago(d.uptime);
  if(!thLoaded){$('th').value=d.alarmKm;thLoaded=true}
  $('bNorm').className=d.sim?'off':'';$('bSim').className=d.sim?'':'off';
  $('modeNote').textContent=d.sim?'Simulation: the wand is tracked and its button triggers labeled simulated strikes.'
                                 :'Normal: only real sensor detections. Wand is ignored.';
  $('simcard').style.display=d.sim?'block':'none';
  if(d.sim){
   $('wand').innerHTML=d.wand?'&#9679; Wand connected':'&#9679; Wand not found';$('wand').className=d.wand?'ok':'warn';
   $('wsig').textContent=d.wand?('Signal '+d.wrssi+' dBm'):'';
   $('wkm').textContent=d.wand?d.wkm+' km':'--';
   $('wbar').style.width=d.wand?Math.round((41-d.wkm)/40*100)+'%':'0%';
   const lastSim=d.events.find(e=>e.t=='S');
   if(lastSim){$('chg').textContent=lastSim.s+'%';$('cbar').style.width=lastSim.s+'%';}
   else{$('chg').textContent='--';$('cbar').style.width='0%';}
  }
  const ev0=d.events.find(e=>e.t=='L'||e.t=='S');
  phoneAlerts(d);
  $('cdt').textContent=d.clearTotal;
  if(d.clear>0){$('cdBox').style.display='block';$('cd').textContent=d.clear+' s';
   $('cdbar').style.width=Math.round(d.clear/d.clearTotal*100)+'%';} else $('cdBox').style.display='none';
  const st=$('status');
  if(d.alarm){st.textContent=(ev0&&ev0.t=='S')?'SIMULATED STRIKE NEARBY':'LIGHTNING NEARBY';st.className='big danger'}
  else if(d.clear>0){st.textContent='Take shelter';st.className='big warn'}
  else{st.textContent='All clear';st.className='big ok'}
  $('km').textContent=ev0?kmTxt(ev0.km):'--';
  $('str').textContent=ev0?strTxt(ev0):'--';
  $('n').textContent=d.strikes+(d.sims?' + '+d.sims+' sim':'');
  $('last').textContent=ev0?('Last '+(ev0.t=='S'?'simulated strike ':'strike ')+ago(ev0.ago)+' ago'):'No lightning detected yet';
  $('counts').textContent='Disturbers (man-made noise) rejected: '+d.disturbers+' | Noise warnings: '+d.noise;
  $('log').innerHTML=d.events.map(e=>
   '<tr><td>'+ago(e.ago)+'</td><td>'+(e.t=='L'?'<b class="danger">Lightning</b>':e.t=='S'?'<b class="sim">Simulated strike</b>':e.t=='D'?'Disturber':'Noise')+
   '</td><td>'+(e.t=='L'||e.t=='S'?kmTxt(e.km):'')+'</td><td>'+strTxt(e)+'</td></tr>').join('');
 }catch(e){if(++fails>=3){setLink(false);$('sens').innerHTML='&#9679; Unknown';$('sens').className='warn';}}
}
async function setTh(){await fetch('/set?km='+$('th').value);}
async function setMode(m){await fetch('/mode?sim='+m);tick();}
setInterval(tick,300);tick();
</script></body></html>
)HTML";

void handleData() {
  unsigned long now = millis();
  bool wOn = wandOnline();
  String j = "{\"alarm\":" + String(alarmOn ? "true" : "false");
  j += ",\"sim\":" + String(simMode ? "true" : "false");
  j += ",\"sensorOk\":" + String(sensorOk ? "true" : "false");
  j += ",\"uptime\":" + String(now / 1000);
  j += ",\"alarmKm\":" + String(alarmKm);
  j += ",\"wand\":" + String(wOn ? "true" : "false");
  j += ",\"wrssi\":" + String((int)wandRssi);
  j += ",\"wkm\":" + String(rssiToKm(wandRssi));
  j += ",\"charge\":" + String((chargeMs && now - chargeMs < 400) ? chargePct : 0);
  j += ",\"astr\":" + String(alarmStrength);
  unsigned long left = (dangerActive && now - dangerMs < CLEAR_MS) ? (CLEAR_MS - (now - dangerMs) + 999) / 1000 : 0;
  j += ",\"clear\":" + String(left);
  j += ",\"clearTotal\":" + String(CLEAR_MS / 1000);
  j += ",\"dseq\":" + String(dangerSeq);
  j += ",\"cseq\":" + String(clearSeq);
  j += ",\"strikes\":" + String(strikes);
  j += ",\"sims\":" + String(simStrikes);
  j += ",\"disturbers\":" + String(disturbers);
  j += ",\"noise\":" + String(noises);
  j += ",\"events\":[";
  for (int i = 0; i < logCount; i++) {
    const Event& e = logBuf[i];
    char t = e.type == LIGHTNING ? 'L' : e.type == DISTURBER_DETECT ? 'D' : e.type == SIM_EVENT ? 'S' : 'N';
    if (i) j += ",";
    j += "{\"ago\":" + String((now - e.ms) / 1000) + ",\"t\":\"" + t + "\",\"km\":" + String(e.km) +
         ",\"s\":" + String(e.strength) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  spkBegin();

  SPI.begin(18, 19, 23, CS_PIN);
  if (!lightning.beginSPI(CS_PIN, 500000)) Serial.println("Sensor not found - check wiring.");
  lightning.tuneCap(TUNE_CAP);
  lightning.calibrateOsc();
  lightning.setIndoorOutdoor(INDOOR);   // change to OUTDOOR when used outside
  lightning.setNoiseLevel(NOISE_LEVEL);
  lightning.maskDisturber(false);
  sensorOk = (lightning.readNoiseLevel() == NOISE_LEVEL);
  Serial.println(sensorOk ? "Sensor OK" : "Sensor NOT responding - check wiring");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1);      // channel 1 (the wand transmits on channel 1)
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onWandRecv);
  else Serial.println("ESP-NOW init failed (simulation mode unavailable)");
  Serial.print("Join Wi-Fi '"); Serial.print(AP_SSID);
  Serial.print("' and open http://"); Serial.println(WiFi.softAPIP());

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.on("/test", []() { startAlarm(); server.send(200, "text/plain", "ok"); });
  server.on("/stop", []() { stopAlarm();  server.send(200, "text/plain", "ok"); });
  server.on("/set", []() {
    if (server.hasArg("km")) alarmKm = constrain(server.arg("km").toInt(), 1, 40);
    server.send(200, "text/plain", "ok");
  });
  server.on("/mode", []() {
    if (server.hasArg("sim")) simMode = server.arg("sim").toInt() == 1;
    server.send(200, "text/plain", "ok");
  });
  server.begin();
}

void loop() {
  checkSensor();
  handleSensor();
  handleWand();
  updateAlarm();
  updateClear();
  server.handleClient();
}
