#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHTesp.h>
#include <time.h>
#include <sys/time.h>

const char* AP_SSID     = "Idle Cut";
const char* AP_PASSWORD = "";
const char* MDNS_HOST   = "project";

#define DHTPIN     4
#define RADAR_PIN  5     // RCWL-0516 OUT
#define LED_PIN    23    // led
#define BUTTON_PIN 16    // enable/disable the radar sensor
#define BUTTON2_PIN 18   // manual relay toggle
#define RELAY_PIN  26    // relay

const bool RELAY_ACTIVE_LOW = true;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

const char* TZ_STRING = "IST-5:30";

DHTesp dht;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);

float humidity = 0;
float tempC = 0;
char dateStr[12] = "--/--/----";
char timeStr[12] = "--:-- --";
bool sensorOK = false;
bool timeSynced = false;

bool motionDetected = false;
unsigned long lastMotionTime = 0;
const unsigned long motionTimeout = 10000;

bool loggingEnabled = false;
#define MOTION_LOG_SIZE 15
time_t motionLog[MOTION_LOG_SIZE];
int motionLogIndex = 0;
int motionLogCount = 0;

void logMotionEvent() {
  time_t now;
  time(&now);
  motionLog[motionLogIndex] = now;
  motionLogIndex = (motionLogIndex + 1) % MOTION_LOG_SIZE;
  if (motionLogCount < MOTION_LOG_SIZE) motionLogCount++;
}

bool radarEnabled = false;
bool lastButtonReading = HIGH;
bool debouncedButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 40;

bool lastButton2Reading = HIGH;
bool debouncedButton2State = HIGH;
unsigned long lastDebounce2Time = 0;

unsigned long radarConfirmMs = 300;
const unsigned long RADAR_CONFIRM_MIN = 50;
const unsigned long RADAR_CONFIRM_MAX = 1000;
bool radarRawHigh = false;
unsigned long radarHighSince = 0;

bool relayOn = false;
unsigned long relayAutoOffMinutes = 3;
const unsigned long AUTO_OFF_MIN_MINUTES = 1;
const unsigned long AUTO_OFF_MAX_MINUTES = 60;
unsigned long lastConfirmedMotionMillis = 0;

unsigned long motionActiveSince = 0;

unsigned long oledFlashUntil = 0;

#define MOTION_RATE_WINDOW 8
unsigned long motionEventTimes[MOTION_RATE_WINDOW];
int motionEventIndex = 0;
int motionEventCount = 0;
const unsigned long MOTION_RATE_WINDOW_MS = 6000; 

unsigned long lastBlinkToggle = 0;
bool ledBlinkState = false;
const unsigned long BLINK_INTERVAL_MIN = 100; 
const unsigned long BLINK_INTERVAL_MAX = 600; 

void recordMotionEvent() {
  motionEventTimes[motionEventIndex] = millis();
  motionEventIndex = (motionEventIndex + 1) % MOTION_RATE_WINDOW;
  if (motionEventCount < MOTION_RATE_WINDOW) motionEventCount++;
}

unsigned long currentBlinkInterval() {
  unsigned long now = millis();
  int recentCount = 0;
  for (int i = 0; i < motionEventCount; i++) {
    if (now - motionEventTimes[i] <= MOTION_RATE_WINDOW_MS) recentCount++;
  }
  if (recentCount <= 1) return BLINK_INTERVAL_MAX;
  int capped = min(recentCount, MOTION_RATE_WINDOW);
  long interval = map(capped, 1, MOTION_RATE_WINDOW, BLINK_INTERVAL_MAX, BLINK_INTERVAL_MIN);
  return (unsigned long)constrain(interval, (long)BLINK_INTERVAL_MIN, (long)BLINK_INTERVAL_MAX);
}

void relayWrite(bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
  if (on != relayOn) {
    display.invertDisplay(true);
    oledFlashUntil = millis() + 120;
  }
  relayOn = on;
}

void updateDateTimeString() {
  if (!timeSynced) {
    strcpy(dateStr, "Not synced");
    strcpy(timeStr, "--:-- --");
    return;
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    strcpy(dateStr, "Not synced");
    strcpy(timeStr, "--:-- --");
    return;
  }
  strftime(dateStr, sizeof(dateStr), "%d-%m-%Y", &timeinfo);
  int hour12 = timeinfo.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ampm = (timeinfo.tm_hour >= 12) ? "PM" : "AM";
  snprintf(timeStr, sizeof(timeStr), "%d:%02d %s", hour12, timeinfo.tm_min, ampm);
}

void readSensor() {
  TempAndHumidity values = dht.getTempAndHumidity();
  if (isnan(values.temperature) || isnan(values.humidity)) {
    sensorOK = false;
    return;
  }
  tempC = values.temperature;
  humidity = values.humidity;
  sensorOK = true;
}

void drawMotionIndicator(int cx, int cy) {
  if (!radarEnabled) {
    display.drawRect(cx - 3, cy - 3, 6, 6, SSD1306_WHITE);
    return;
  }
  if (motionDetected) {
    const unsigned long period = 900;
    unsigned long phase1 = (millis() - motionActiveSince) % period;
    unsigned long phase2 = (phase1 + period / 2) % period;
    int r1 = 2 + (int)((phase1 * 8) / period);
    int r2 = 2 + (int)((phase2 * 8) / period);
    display.drawCircle(cx, cy, r1, SSD1306_WHITE);
    display.drawCircle(cx, cy, r2, SSD1306_WHITE);
    display.fillCircle(cx, cy, 2, SSD1306_WHITE);
  } else {
    unsigned long t = millis() % 1600;
    int amt = (t < 800) ? (int)t : (int)(1600 - t);
    int r = 1 + amt / 400;
    display.drawCircle(cx, cy, r, SSD1306_WHITE);
  }
}

void drawAutoOffBar(int y) {
  if (!(relayOn && radarEnabled)) {
    display.drawLine(0, y, SCREEN_WIDTH, y, SSD1306_WHITE);
    return;
  }
  unsigned long elapsed = (millis() - lastConfirmedMotionMillis) / 1000;
  unsigned long total = relayAutoOffMinutes * 60;
  long remaining = (long)total - (long)elapsed;
  if (remaining < 0) remaining = 0;
  float frac = (total > 0) ? (float)remaining / (float)total : 0;
  int w = (int)(SCREEN_WIDTH * frac);
  display.drawRect(0, y - 1, SCREEN_WIDTH, 3, SSD1306_WHITE);
  if (w > 0) display.fillRect(0, y - 1, w, 3, SSD1306_WHITE);
}

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(dateStr);
  display.setCursor(SCREEN_WIDTH - (strlen(timeStr) * 6), 0);
  display.print(timeStr);
  display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);

  display.setCursor(4, 14);
  display.print("TEMP");
  display.setCursor(72, 14);
  display.print("HUMIDITY");

  display.setTextSize(2);
  display.setCursor(2, 24);
  if (sensorOK) { display.print(tempC, 1); display.print("C"); }
  else { display.print("Err"); }

  display.setCursor(74, 24);
  if (sensorOK) { display.print(humidity, 0); display.print("%"); }
  else { display.print("Err"); }

  display.drawLine(64, 13, 64, 46, SSD1306_WHITE);
  drawAutoOffBar(48);

  display.setTextSize(1);
  drawMotionIndicator(5, 56);
  display.setCursor(13, 53);
  if (!radarEnabled) {
    display.print("RADAR DISABLED");
  } else if (motionDetected) {
    display.print("MOTION DETECTED");
  } else {
    display.print("No motion");
  }

  display.display();
}

const unsigned char catBitmap[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xC0, 0x00,
  0x00, 0x00, 0x00, 0x7F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, 0x00,
  0x00, 0x00, 0x01, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xE0, 0x00,
  0x00, 0x00, 0x03, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xF0, 0x00,
  0x00, 0x00, 0x03, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xF0, 0x00,
  0x00, 0x00, 0x00, 0x1F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xF0, 0x00,
  0x00, 0x00, 0x00, 0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xF0, 0x00,
  0x00, 0x00, 0x01, 0xFF, 0xF0, 0x00, 0x01, 0xE0, 0x03, 0xFF, 0xF0, 0x00,
  0x01, 0xF0, 0x07, 0xFF, 0xF8, 0x00, 0x01, 0xF0, 0x0F, 0xFF, 0xF8, 0x00,
  0x00, 0xF0, 0x0F, 0xFF, 0xF8, 0x00, 0x00, 0x70, 0x1F, 0xFF, 0xF8, 0x00,
  0x00, 0x70, 0x1F, 0xFF, 0xF8, 0x00, 0x00, 0x70, 0x3F, 0xFF, 0xF8, 0x00,
  0x00, 0x70, 0x3F, 0xFF, 0xF8, 0x00, 0x00, 0x70, 0x7F, 0xFF, 0xF8, 0x00,
  0x00, 0x60, 0x7F, 0xFF, 0xF8, 0x00, 0x00, 0xE0, 0xFF, 0xFF, 0xF8, 0x00,
  0x00, 0xE1, 0xFF, 0xFF, 0xF8, 0x00, 0x01, 0xC1, 0xFF, 0xFF, 0xF0, 0x00,
  0x01, 0xC1, 0xFF, 0xFF, 0xF0, 0x00, 0x03, 0x83, 0xFF, 0xFF, 0xE0, 0x00,
  0x03, 0x83, 0xFF, 0xFF, 0xE0, 0x00, 0x03, 0x83, 0xFF, 0xFF, 0xE0, 0x00,
  0x03, 0x87, 0xFF, 0xFF, 0xC0, 0x00, 0x07, 0x07, 0xFF, 0xFF, 0xC0, 0x00,
  0x07, 0x87, 0xFF, 0xFF, 0xC0, 0x00, 0x07, 0x87, 0xFF, 0xFF, 0xC0, 0x00,
  0x03, 0x87, 0xFF, 0xFF, 0x80, 0x00, 0x03, 0xC7, 0xFF, 0xFF, 0x80, 0x00,
  0x01, 0xE7, 0xFF, 0xFF, 0x80, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xE0, 0x00,
  0x00, 0x7F, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0xF8, 0x00,
  0x00, 0x07, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#define CAT_BITMAP_W 44
#define CAT_BITMAP_H 56

void startupAnimation() {
  int x = (SCREEN_WIDTH - CAT_BITMAP_W) / 2;
  int y = (SCREEN_HEIGHT - CAT_BITMAP_H) / 2;

  for (int reveal = 0; reveal <= CAT_BITMAP_W; reveal += 3) {
    display.clearDisplay();
    display.fillScreen(SSD1306_WHITE);
    display.drawBitmap(x, y, catBitmap, CAT_BITMAP_W, CAT_BITMAP_H, SSD1306_BLACK);
    if (reveal < CAT_BITMAP_W) {
      display.fillRect(x + reveal, y, CAT_BITMAP_W - reveal, CAT_BITMAP_H, SSD1306_WHITE);
    }
    display.display();
    delay(12);
  }

  delay(1600);
  display.clearDisplay();
  display.display();
}

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>IdleCut</title>
<style>
:root{
  --bg:#0a0a0a; --panel:#141414; --panel2:#1d1d1d; --line:#242424; --line-strong:#303030;
  --accent:#8c45ff; --accent2:#c636ff; --accent-dim:rgba(140,69,255,.14); --accent-ink:#c9a6ff;
  --saving:#cbff00; --saving-dim:rgba(203,255,0,.10);
  --ink:#ffffff; --muted:#ababab; --danger:#ff5c5c;
  --sans:'Inter',-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;
  --mono:'SFMono-Regular',Consolas,'Liberation Mono',Menlo,monospace;
}
*{box-sizing:border-box;}
body{margin:0;font-family:var(--sans);background:var(--bg);color:var(--ink);
display:flex;flex-direction:column;align-items:center;padding:22px 16px 40px;min-height:100vh;}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;}
@media (prefers-reduced-motion:reduce){*{transition:none!important;animation:none!important;}}

.wrap{width:100%;max-width:400px;}
.header{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px;gap:10px;}
.brand{display:flex;align-items:center;gap:10px;}
.logo{width:30px;height:30px;flex-shrink:0;border-radius:9px;
background:linear-gradient(135deg,var(--accent),var(--accent2));display:flex;align-items:center;justify-content:center;}
.logo svg{width:16px;height:16px;color:#fff;}
.title{font-size:1.05rem;font-weight:700;letter-spacing:.2px;}
.tagline{font-size:.76rem;color:var(--muted);margin-top:1px;}
.chip{font-size:.7rem;font-weight:600;letter-spacing:.4px;padding:6px 12px;border-radius:999px;
text-transform:uppercase;white-space:nowrap;border:1px solid transparent;}
.chip.chipOn{background:var(--accent-dim);color:var(--accent-ink);border-color:rgba(140,69,255,.35);}
.chip.chipOff{background:var(--saving-dim);color:var(--saving);border-color:rgba(203,255,0,.25);}

.card{background:var(--panel);border:1px solid var(--line);border-radius:20px;padding:22px;}

.gaugeWrap{position:relative;width:180px;height:180px;margin:4px auto 8px;}
.gauge{width:100%;height:100%;transform:rotate(-90deg);}
.gauge circle{fill:none;stroke-width:11;stroke-linecap:round;}
.track{stroke:var(--line);}
.progress{stroke:var(--accent);transition:stroke-dashoffset 1s linear,stroke .3s;}
.gaugeCenter{position:absolute;inset:0;display:flex;flex-direction:column;
align-items:center;justify-content:center;}
.gaugeValue{font-family:var(--mono);font-variant-numeric:tabular-nums;font-size:1.7rem;font-weight:700;}
.gaugeLabel{font-size:.72rem;color:var(--muted);letter-spacing:.6px;text-transform:uppercase;margin-top:2px;}

.radarLine{text-align:center;color:var(--muted);font-size:.85rem;margin:2px 0 18px;}

.tiles{display:flex;gap:10px;margin-bottom:18px;}
.tile{flex:1;background:var(--panel2);border:1px solid var(--line);border-radius:14px;padding:14px;text-align:center;}
.tile .label{font-size:.7rem;color:var(--muted);text-transform:uppercase;letter-spacing:1px;}
.tile .value{font-family:var(--mono);font-variant-numeric:tabular-nums;font-size:1.6rem;font-weight:700;margin-top:3px;}

.panel{background:var(--panel2);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:12px;}
.panelRow{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;}
.panelLabel{font-size:.92rem;font-weight:600;}
.rowLabel{display:flex;justify-content:space-between;font-size:.88rem;margin-bottom:8px;}
.rowLabel span:last-child{font-family:var(--mono);color:var(--accent-ink);}
.hint{font-size:.76rem;color:var(--muted);margin-top:6px;line-height:1.4;}

input[type=range]{width:100%;accent-color:var(--accent);}

.btnRow{display:flex;gap:10px;margin-top:12px;}
button{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff;border:none;border-radius:10px;
padding:11px;font-weight:600;font-size:.9rem;cursor:pointer;flex:1;}
button:active{opacity:.85;}
button.secondary{background:var(--panel);color:var(--ink);border:1px solid var(--line-strong);}

.switch{position:relative;display:inline-block;width:42px;height:24px;flex-shrink:0;}
.switch input{opacity:0;width:0;height:0;}
.switchTrack{position:absolute;inset:0;background:var(--line-strong);border-radius:999px;
cursor:pointer;transition:background .2s;}
.switchTrack:before{content:"";position:absolute;height:18px;width:18px;left:3px;top:3px;
background:var(--ink);border-radius:50%;transition:transform .2s;}
input:checked + .switchTrack{background:linear-gradient(135deg,var(--accent),var(--accent2));}
input:checked + .switchTrack:before{transform:translateX(18px);background:#fff;}

#logPanel{display:none;margin-top:10px;background:var(--panel);border:1px solid var(--line);border-radius:10px;
padding:10px 12px;max-height:200px;overflow-y:auto;}
#logPanel div{padding:6px 2px;font-family:var(--mono);font-size:.84rem;border-bottom:1px solid var(--line);}
#logPanel div:last-child{border-bottom:none;}
#logEmpty{color:var(--muted);font-size:.84rem;text-align:center;padding:6px;}

.footer{display:flex;align-items:center;justify-content:space-between;margin-top:8px;padding:0 4px;}
.footer span{font-size:.8rem;color:var(--muted);}
.linkBtn{background:none;border:none;color:var(--accent-ink);font-size:.8rem;font-weight:600;
padding:4px;flex:none;cursor:pointer;}
</style></head><body>
<div class='wrap'>

  <div class='header'>
    <div class='brand'>
      <div class='logo'>
        <svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5'
          stroke-linecap='round' stroke-linejoin='round'><polygon points='13 2 3 14 11 14 9 22 21 10 13 10 13 2'/></svg>
      </div>
      <div>
        <div class='title'>IdleCut</div>
        <div class='tagline'>cuts power when nobody's around</div>
      </div>
    </div>
    <div class='chip' id='statusChip'>--</div>
  </div>

  <div class='card'>
    <div class='gaugeWrap'>
      <svg class='gauge' viewBox='0 0 180 180'>
        <circle class='track' cx='90' cy='90' r='70'/>
        <circle class='progress' id='gaugeProgress' cx='90' cy='90' r='70'
          stroke-dasharray='439.8' stroke-dashoffset='439.8'/>
      </svg>
      <div class='gaugeCenter'>
        <div class='gaugeValue' id='gaugeValue'>--</div>
        <div class='gaugeLabel' id='gaugeLabel'>--</div>
      </div>
    </div>
    <div class='radarLine' id='radarLine'>--</div>

    <div class='tiles'>
      <div class='tile'><div class='label'>Temp</div><div class='value' id='temp'>--</div></div>
      <div class='tile'><div class='label'>Humidity</div><div class='value' id='hum'>--</div></div>
    </div>

    <div class='panel'>
      <div class='panelLabel'>Manual override</div>
      <div class='hint'>Force the relay regardless of motion. Automatic control resumes on the next detection, or after auto-off.</div>
      <div class='btnRow'>
        <button onclick="setRelay('on')">Turn on</button>
        <button class='secondary' onclick="setRelay('off')">Turn off</button>
      </div>
    </div>

    <div class='panel'>
      <div class='rowLabel'><span>Sensitivity</span><span id='sensVal'>--</span></div>
      <input type='range' id='sensSlider' min='50' max='1000' step='25'
        oninput='updateSensLabel(this.value)' onchange='setConfig()'>
      <div class='hint'>Lower = triggers faster · Higher = filters more false triggers (fans, EMI)</div>
    </div>

    <div class='panel'>
      <div class='rowLabel'><span>Auto-off after no motion</span><span id='autooffVal'>--</span></div>
      <input type='range' id='autooffSlider' min='1' max='60' step='1'
        oninput='updateAutooffLabel(this.value)' onchange='setConfig()'>
    </div>

    <div class='panel'>
      <div class='panelRow'>
        <div>
          <div class='panelLabel'>Motion log</div>
          <div class='hint'>Off by default. Records up to 15 timestamps in memory, cleared on reboot.</div>
        </div>
        <label class='switch'>
          <input type='checkbox' id='logToggle' onchange='setLogging(this.checked)'>
          <span class='switchTrack'></span>
        </label>
      </div>
      <button class='secondary' style='margin-top:10px;width:100%;' onclick='toggleLogPanel()'>View log</button>
      <div id='logPanel'></div>
    </div>
  </div>

  <div class='footer'>
    <span id='datetime'>--</span>
    <button class='linkBtn' onclick='syncTime()'>Resync time</button>
  </div>
</div>

<script>
const CIRC = 439.8;

async function syncTime(){
  try{ await fetch('/setTime?epoch=' + Math.floor(Date.now()/1000)); }catch(e){}
}
async function setRelay(state){
  try{ await fetch('/relay?state=' + state); }catch(e){}
  refresh();
}
async function setLogging(on){
  try{ await fetch('/log?state=' + (on ? 'on' : 'off')); }catch(e){}
}
function updateSensLabel(v){ document.getElementById('sensVal').textContent = v + ' ms'; }
function updateAutooffLabel(v){ document.getElementById('autooffVal').textContent = v + ' min'; }
async function setConfig(){
  const c = document.getElementById('sensSlider').value;
  const a = document.getElementById('autooffSlider').value;
  try{ await fetch('/config?confirm=' + c + '&autooff=' + a); }catch(e){}
}
async function loadConfig(){
  try{
    const r = await fetch('/data');
    const d = await r.json();
    document.getElementById('sensSlider').value = d.confirm;
    document.getElementById('autooffSlider').value = d.autooff;
    updateSensLabel(d.confirm);
    updateAutooffLabel(d.autooff);
  }catch(e){}
}
function pad(n){ return String(n).padStart(2,'0'); }
function tickClock(){
  const now = new Date();
  const d = pad(now.getDate())+'-'+pad(now.getMonth()+1)+'-'+now.getFullYear();
  let h = now.getHours();
  const ampm = h >= 12 ? 'PM' : 'AM';
  h = h % 12; if(h === 0) h = 12;
  document.getElementById('datetime').textContent = d + '  ' + h + ':' + pad(now.getMinutes()) + ' ' + ampm;
}
function setGauge(fraction, value, label, on){
  const off = CIRC * (1 - Math.max(0, Math.min(1, fraction)));
  document.getElementById('gaugeProgress').style.strokeDashoffset = off;
  document.getElementById('gaugeProgress').style.stroke = on ? 'var(--accent)' : 'var(--line)';
  document.getElementById('gaugeValue').textContent = value;
  document.getElementById('gaugeLabel').textContent = label;
}
async function refresh(){
  try{
    const r = await fetch('/data');
    const d = await r.json();

    document.getElementById('temp').textContent = d.ok ? d.temp.toFixed(1) + '\u00B0C' : 'Err';
    document.getElementById('hum').textContent = d.ok ? d.hum.toFixed(0) + '%' : 'Err';

    const chip = document.getElementById('statusChip');
    if(d.relay){ chip.textContent = 'Power on'; chip.className = 'chip chipOn'; }
    else { chip.textContent = 'Saving power'; chip.className = 'chip chipOff'; }

    if(!d.relay){
      setGauge(0, 'OFF', 'Saving power', false);
    } else if(d.radar && d.remaining >= 0){
      const m = Math.floor(d.remaining / 60), s = d.remaining % 60;
      setGauge(d.remaining / (d.autooff * 60), m + ':' + pad(s), 'Auto-off in', true);
    } else {
      setGauge(1, 'ON', 'Manual hold', true);
    }

    const radarLine = document.getElementById('radarLine');
    if(!d.radar) radarLine.textContent = 'Sensor disabled';
    else if(d.motion) radarLine.textContent = 'Motion detected';
    else radarLine.textContent = 'No motion';

    document.getElementById('logToggle').checked = d.logging;
  }catch(e){}
}

let logOpen = false;
async function loadLog(){
  const panel = document.getElementById('logPanel');
  panel.innerHTML = 'Loading...';
  try{
    const r = await fetch('/events');
    const events = await r.json();
    panel.innerHTML = events.length
      ? events.map(t => `<div>${t}</div>`).join('')
      : "<div id='logEmpty'>No motion recorded yet</div>";
  }catch(e){
    panel.innerHTML = "<div id='logEmpty'>Couldn't load log</div>";
  }
}
function toggleLogPanel(){
  logOpen = !logOpen;
  const panel = document.getElementById('logPanel');
  panel.style.display = logOpen ? 'block' : 'none';
  if(logOpen) loadLog();
}

syncTime();
loadConfig();
tickClock();
setInterval(tickClock, 1000);
refresh();
setInterval(refresh, 2000);
</script>
</body></html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

void handleData() {
  long remaining = -1;
  if (relayOn && radarEnabled) {
    long elapsed = (millis() - lastConfirmedMotionMillis) / 1000;
    long total = relayAutoOffMinutes * 60;
    remaining = total - elapsed;
    if (remaining < 0) remaining = 0;
  }

  char json[320];
  snprintf(json, sizeof(json),
    "{\"ok\":%s,\"temp\":%.1f,\"hum\":%.1f,\"motion\":%s,\"radar\":%s,\"relay\":%s,"
    "\"confirm\":%lu,\"autooff\":%lu,\"remaining\":%ld,\"logging\":%s,\"date\":\"%s\",\"time\":\"%s\"}",
    sensorOK ? "true" : "false",
    tempC, humidity,
    motionDetected ? "true" : "false",
    radarEnabled ? "true" : "false",
    relayOn ? "true" : "false",
    radarConfirmMs, relayAutoOffMinutes, remaining,
    loggingEnabled ? "true" : "false",
    dateStr, timeStr);
  server.send(200, "application/json", json);
}

void handleRelay() {
  if (server.hasArg("state")) {
    String s = server.arg("state");
    if (s == "on") {
      relayWrite(true);
      lastConfirmedMotionMillis = millis();
    } else if (s == "off") {
      relayWrite(false);
    }
  }
  server.send(200, "text/plain", relayOn ? "on" : "off");
}

void handleConfig() {
  if (server.hasArg("confirm")) {
    long v = constrain(server.arg("confirm").toInt(), (long)RADAR_CONFIRM_MIN, (long)RADAR_CONFIRM_MAX);
    radarConfirmMs = (unsigned long)v;
  }
  if (server.hasArg("autooff")) {
    long v = constrain(server.arg("autooff").toInt(), (long)AUTO_OFF_MIN_MINUTES, (long)AUTO_OFF_MAX_MINUTES);
    relayAutoOffMinutes = (unsigned long)v;
  }
  char json[80];
  snprintf(json, sizeof(json), "{\"confirm\":%lu,\"autooff\":%lu}", radarConfirmMs, relayAutoOffMinutes);
  server.send(200, "application/json", json);
}

void handleLog() {
  if (server.hasArg("state")) {
    loggingEnabled = (server.arg("state") == "on");
  }
  server.send(200, "text/plain", loggingEnabled ? "on" : "off");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void handleEvents() {
  char json[MOTION_LOG_SIZE * 11 + 4];
  int pos = 0;
  json[pos++] = '[';
  int idx = (motionLogIndex - 1 + MOTION_LOG_SIZE) % MOTION_LOG_SIZE;
  for (int i = 0; i < motionLogCount; i++) {
    time_t t = motionLog[idx];
    struct tm ti;
    localtime_r(&t, &ti);
    char buf[10];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    pos += snprintf(json + pos, sizeof(json) - pos, "\"%s\"%s", buf, (i < motionLogCount - 1) ? "," : "");
    idx = (idx - 1 + MOTION_LOG_SIZE) % MOTION_LOG_SIZE;
  }
  json[pos++] = ']';
  json[pos] = '\0';
  server.send(200, "application/json", json);
}

void handleSetTime() {
  if (!server.hasArg("epoch")) {
    server.send(400, "text/plain", "Missing epoch");
    return;
  }
  time_t epoch = (time_t) strtoul(server.arg("epoch").c_str(), nullptr, 10);
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  timeSynced = true;
  updateDateTimeString();
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  pinMode(RADAR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);
  lastConfirmedMotionMillis = millis();

  dht.setup(DHTPIN, DHTesp::DHT11);

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED not found!");
  } else {
    startupAnimation();
  }

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP Started: "); Serial.println(AP_SSID);
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());

  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS ready: https://");
    Serial.print(MDNS_HOST);
    Serial.println(".local");
  } else {
    Serial.println("mDNS failed to start - use the IP address instead.");
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi AP Ready");
  display.println(WiFi.softAPIP());
  display.display();
  delay(1500);

  setenv("TZ", TZ_STRING, 1);
  tzset();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/setTime", handleSetTime);
  server.on("/events", handleEvents);
  server.on("/relay", handleRelay);
  server.on("/config", handleConfig);
  server.on("/log", handleLog);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started.");
}

unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 2000;
unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 100;

void loop() {
  server.handleClient();
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) lastDebounceTime = millis();
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != debouncedButtonState) {
      debouncedButtonState = reading;
      if (debouncedButtonState == LOW) {
        radarEnabled = !radarEnabled;
        if (!radarEnabled) {
          motionDetected = false;
        } else {
          lastConfirmedMotionMillis = millis();
        }
      }
    }
  }
  lastButtonReading = reading;

  bool reading2 = digitalRead(BUTTON2_PIN);
  if (reading2 != lastButton2Reading) lastDebounce2Time = millis();
  if ((millis() - lastDebounce2Time) > debounceDelay) {
    if (reading2 != debouncedButton2State) {
      debouncedButton2State = reading2;
      if (debouncedButton2State == LOW && !radarEnabled) {
        relayWrite(!relayOn);
        if (relayOn) lastConfirmedMotionMillis = millis();
      }
    }
  }
  lastButton2Reading = reading2;

  bool rawHigh = digitalRead(RADAR_PIN) == HIGH;
  if (rawHigh) {
    if (!radarRawHigh) { radarRawHigh = true; radarHighSince = millis(); }
  } else {
    radarRawHigh = false;
  }
  bool confirmedMotion = radarEnabled && radarRawHigh &&
                          (millis() - radarHighSince >= radarConfirmMs);

  if (radarEnabled) {
    if (confirmedMotion) {
      if (!motionDetected) {
        motionDetected = true;
        motionActiveSince = millis();
        recordMotionEvent();
        if (loggingEnabled) logMotionEvent();
      }
      lastMotionTime = millis();
      lastConfirmedMotionMillis = millis();
      if (!relayOn) relayWrite(true);
    }

    if (motionDetected && (millis() - lastMotionTime > motionTimeout)) {
      motionDetected = false;
    }

    if (relayOn && (millis() - lastConfirmedMotionMillis > relayAutoOffMinutes * 60000UL)) {
      relayWrite(false);
    }
  } else {
    motionDetected = false;
  }

  if (motionDetected) {
    unsigned long interval = currentBlinkInterval();
    if (millis() - lastBlinkToggle >= interval) {
      lastBlinkToggle = millis();
      ledBlinkState = !ledBlinkState;
      digitalWrite(LED_PIN, ledBlinkState ? HIGH : LOW);
    }
  } else {
    ledBlinkState = false;
    digitalWrite(LED_PIN, LOW);
  }

  if (oledFlashUntil != 0 && millis() >= oledFlashUntil) {
    display.invertDisplay(false);
    oledFlashUntil = 0;
  }

  if (millis() - lastSensorRead >= sensorInterval) {
    lastSensorRead = millis();
    readSensor();
  }

  if (millis() - lastDisplayUpdate >= displayInterval) {
    lastDisplayUpdate = millis();
    updateDateTimeString();
    updateOLED();
  }
}
