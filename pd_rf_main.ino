/*
  PD RF — ESP32-C3 Super Mini
  CC1101 + SSD1306 0.96" OLED + SD card + 3 buttons
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <SD.h>

#define PIN_MOSI    7
#define PIN_MISO    2
#define PIN_SCK     6
#define PIN_CC_CS   5
#define PIN_CC_GDO0 4
#define PIN_CC_GDO2 3
#define PIN_SD_CS   8
#define PIN_SDA     9
#define PIN_SCL     10
#define PIN_BTN_UP  0
#define PIN_BTN_DN  1
#define PIN_BTN_OK  21

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

enum Tab { TAB_SPECTRUM = 0, TAB_RECORD, TAB_PLAY, TAB_SETTINGS, TAB_COUNT };
const char* tabNames[] = {"SPEC","REC","PLAY","SET"};
int activeTab = TAB_SPECTRUM;

struct Settings {
  float   freq     = 433.920f;
  uint8_t bwIndex  = 2;
  uint8_t modIndex = 0;
  bool    highGain = true;
};
Settings cfg;

const float bwTable[]    = {58.0f, 101.5f, 203.1f, 406.2f, 812.5f};
const char* bwLabels[]   = {"58kHz","101kHz","203kHz","406kHz","812kHz"};
const char* modLabels[]  = {"OOK","2FSK"};
#define BW_COUNT  5
#define MOD_COUNT 2

struct Button {
  uint8_t  pin;
  bool     lastState;
  uint32_t lastTime;
  bool     pressed;
};
Button btnUp = {PIN_BTN_UP, HIGH, 0, false};
Button btnDn = {PIN_BTN_DN, HIGH, 0, false};
Button btnOk = {PIN_BTN_OK, HIGH, 0, false};

void updateBtn(Button& b) {
  bool cur = digitalRead(b.pin);
  if (cur == LOW && b.lastState == HIGH && millis() - b.lastTime > 40) {
    b.pressed  = true;
    b.lastTime = millis();
  } else {
    b.pressed = false;
  }
  b.lastState = cur;
}

// ── Spectrum ──────────────────────────────────────────────────────
#define SPEC_BINS 32
int8_t specBars[SPEC_BINS];

void specUpdate() {
  for (int i = 0; i < SPEC_BINS; i++) {
    ELECHOUSE_cc1101.setChannel(i);
    delayMicroseconds(200);
    int rssi = ELECHOUSE_cc1101.getRssi();
    int h = constrain(map(rssi, -110, -10, 0, 50), 0, 50);
    specBars[i] = (specBars[i] * 3 + h) / 4;
  }
  ELECHOUSE_cc1101.setChannel(0);
}

void drawSpectrum() {
  u8g2.setFont(u8g2_font_5x7_mr);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.3fMHz", cfg.freq);
  u8g2.drawStr(0, 20, buf);
  for (int i = 0; i < SPEC_BINS; i++) {
    int h = specBars[i], x = i * 4, y = 63 - h;
    u8g2.drawBox(x, y, 3, h);
  }
  u8g2.drawHLine(0, 63, 128);
  int maxH = 0, maxI = 0;
  for (int i = 0; i < SPEC_BINS; i++) if (specBars[i] > maxH) { maxH = specBars[i]; maxI = i; }
  u8g2.drawVLine(maxI * 4 + 1, 12, 51);
}

// ── Record ────────────────────────────────────────────────────────
#define REC_BUF_SIZE 4096
uint8_t  recBuf[REC_BUF_SIZE];
uint16_t recLen       = 0;
bool     isRecording  = false;
bool     hasRecording = false;
uint32_t recStartMs   = 0;
uint32_t recDurationMs= 0;
uint8_t  wavePreview[128];
uint8_t  waveLen = 0;

void recStart() {
  recLen = 0; waveLen = 0; isRecording = true; recStartMs = millis();
  ELECHOUSE_cc1101.SetRx();
}

void recStop() {
  isRecording   = false;
  hasRecording  = recLen > 0;
  recDurationMs = millis() - recStartMs;
  waveLen = min((int)recLen, 128);
  for (int i = 0; i < waveLen; i++)
    wavePreview[i] = (recBuf[(int)i * recLen / waveLen] > 127) ? 1 : 0;
  ELECHOUSE_cc1101.setSidle();
}

void recSave() {
  if (!hasRecording) return;
  File f = SD.open("/signal.bin", FILE_WRITE);
  if (!f) return;
  f.write(recBuf, recLen);
  f.close();
}

void recLoad() {
  File f = SD.open("/signal.bin");
  if (!f) return;
  recLen = f.read(recBuf, REC_BUF_SIZE);
  f.close();
  hasRecording  = recLen > 0;
  recDurationMs = recLen * 10;
  waveLen = min((int)recLen, 128);
  for (int i = 0; i < waveLen; i++)
    wavePreview[i] = (recBuf[(int)i * recLen / waveLen] > 127) ? 1 : 0;
}

void recUpdate() {
  if (!isRecording) return;
  if (ELECHOUSE_cc1101.CheckReceiveFlag()) {
    int n = ELECHOUSE_cc1101.ReceiveData(recBuf + recLen);
    recLen = min((int)(recLen + n), REC_BUF_SIZE);
  }
  if (recLen >= REC_BUF_SIZE) recStop();
}

void drawWaveform(uint8_t* wave, uint8_t len, int yHigh, int yLow, bool showRec) {
  for (int i = 0; i < (int)len - 1; i++) {
    int x = i + (128 - len) / 2;
    int y = wave[i] ? yHigh : yLow;
    int yn = wave[i+1] ? yHigh : yLow;
    u8g2.drawHLine(x, y, 1);
    if (y != yn) u8g2.drawVLine(x, min(y, yn), abs(y - yn) + 1);
  }
  if (showRec) {
    if ((millis() / 500) % 2 == 0) { u8g2.drawCircle(120, 18, 3); u8g2.drawDisc(120, 18, 2); }
    u8g2.setFont(u8g2_font_5x7_mr);
    char t[16];
    uint32_t sec = (millis() - recStartMs) / 1000;
    uint32_t ms  = ((millis() - recStartMs) % 1000) / 100;
    snprintf(t, sizeof(t), "%02lu:%02lu.%lu", sec/60, sec%60, ms);
    u8g2.drawStr(0, 62, t);
  }
}

void drawRecord() {
  u8g2.setFont(u8g2_font_5x7_mr);
  char buf[24];
  snprintf(buf, sizeof(buf), "%.3fMHz", cfg.freq);
  u8g2.drawStr(0, 20, buf);
  u8g2.drawHLine(0, 22, 128);
  if (isRecording) {
    uint8_t liveWave[64]; int lLen = min((int)recLen, 64);
    for (int i = 0; i < lLen; i++) liveWave[i] = (recBuf[recLen - lLen + i] > 127) ? 1 : 0;
    drawWaveform(liveWave, lLen, 28, 42, true);
    u8g2.drawStr(80, 62, "REC");
  } else if (hasRecording) {
    drawWaveform(wavePreview, waveLen, 28, 42, false);
    snprintf(buf, sizeof(buf), "LEN:%u", recLen);
    u8g2.drawStr(0, 62, buf);
    u8g2.drawStr(60, 62, "[OK=SAVE]");
  } else {
    u8g2.drawStr(20, 38, "No signal");
    u8g2.drawStr(10, 50, "[OK] to record");
  }
}

// ── Play ──────────────────────────────────────────────────────────
bool     isPlaying   = false;
uint32_t playStartMs = 0;
uint16_t playPos     = 0;

void playStart() {
  if (!hasRecording) return;
  isPlaying = true; playPos = 0; playStartMs = millis();
  ELECHOUSE_cc1101.SetTx();
}

void playStop() {
  isPlaying = false;
  ELECHOUSE_cc1101.setSidle();
}

void playUpdate() {
  if (!isPlaying) return;
  if (playPos >= recLen) { playStop(); return; }
  int chunk = min(64, (int)(recLen - playPos));
  ELECHOUSE_cc1101.SendData(recBuf + playPos, chunk);
  playPos += chunk;
}

void drawPlay() {
  u8g2.setFont(u8g2_font_5x7_mr);
  char buf[24];
  if (!hasRecording) {
    u8g2.drawStr(10, 38, "No recording");
    u8g2.drawStr(10, 50, "Go to REC tab");
    return;
  }
  snprintf(buf, sizeof(buf), "%.3fMHz", cfg.freq);
  u8g2.drawStr(0, 20, buf);
  u8g2.drawHLine(0, 22, 128);
  drawWaveform(wavePreview, waveLen, 28, 42, false);
  uint32_t elapsed = isPlaying ? (millis() - playStartMs) : 0;
  uint32_t total   = recDurationMs ? recDurationMs : 1;
  int prog = constrain((int)(124 * (long)elapsed / total), 0, 124);
  u8g2.drawFrame(2, 50, 124, 4);
  u8g2.drawBox(2, 50, prog, 4);
  uint32_t sec = elapsed/1000, tsec = recDurationMs/1000;
  snprintf(buf, sizeof(buf), "%02lu:%02lu/%02lu:%02lu", sec/60, sec%60, tsec/60, tsec%60);
  u8g2.drawStr(0, 62, buf);
  u8g2.drawStr(108, 62, isPlaying ? ">" : "||");
}

// ── Settings ──────────────────────────────────────────────────────
int  setItem    = 0;
bool setEditing = false;
#define SET_ITEMS 5

void applySettings() {
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(cfg.freq);
  ELECHOUSE_cc1101.setRxBW(bwTable[cfg.bwIndex]);
  ELECHOUSE_cc1101.setModulation(cfg.modIndex == 0 ? 2 : 0);
  // High gain: set AGC via register directly (works on all versions)
  byte agcVal = cfg.highGain ? 0x91 : 0x40;
  ELECHOUSE_cc1101.SpiWriteReg(0x1B, agcVal);
  ELECHOUSE_cc1101.SetRx();
}

void drawSettings() {
  u8g2.setFont(u8g2_font_5x7_mr);
  const char* labels[] = {"FREQ","BW","MOD","GAIN","SAVE"};
  char vals[SET_ITEMS][16];
  snprintf(vals[0],16,"%.3fMHz",cfg.freq);
  snprintf(vals[1],16,"%s",bwLabels[cfg.bwIndex]);
  snprintf(vals[2],16,"%s",modLabels[cfg.modIndex]);
  snprintf(vals[3],16,"%s",cfg.highGain?"HIGH":"LOW");
  snprintf(vals[4],16,"[ SAVE ]");
  for (int i = 0; i < SET_ITEMS; i++) {
    int y = 13 + i * 10;
    if (i == setItem) { u8g2.drawBox(0, y-8, 128, 10); u8g2.setDrawColor(0); }
    char line[32];
    snprintf(line, sizeof(line), "%-5s%s", labels[i], vals[i]);
    u8g2.drawStr(2, y, line);
    u8g2.setDrawColor(1);
  }
}

// ── Header ────────────────────────────────────────────────────────
void drawHeader() {
  u8g2.drawBox(0, 0, 128, 11);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x12_mr);
  u8g2.drawStr(1, 9, "PD RF");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x7_mr);
  for (int i = 0; i < TAB_COUNT; i++) {
    int x = 45 + i * 21;
    if (i == activeTab) { u8g2.drawBox(x-1, 0, 21, 11); u8g2.setDrawColor(0); }
    u8g2.drawStr(x, 9, tabNames[i]);
    u8g2.setDrawColor(1);
  }
  u8g2.drawHLine(0, 11, 128);
}

// ── Buttons ───────────────────────────────────────────────────────
void handleButtons() {
  updateBtn(btnUp); updateBtn(btnDn); updateBtn(btnOk);
  if (btnUp.pressed && !(activeTab == TAB_SETTINGS && setEditing)) {
    activeTab = (activeTab - 1 + TAB_COUNT) % TAB_COUNT;
    if (activeTab == TAB_SPECTRUM) applySettings();
  }
  if (btnDn.pressed && !(activeTab == TAB_SETTINGS && setEditing)) {
    activeTab = (activeTab + 1) % TAB_COUNT;
    if (activeTab == TAB_SPECTRUM) applySettings();
  }
  switch (activeTab) {
    case TAB_RECORD:
      if (btnOk.pressed) { if (!isRecording) recStart(); else { recStop(); recSave(); } }
      break;
    case TAB_PLAY:
      if (btnOk.pressed) { if (!isPlaying) playStart(); else playStop(); }
      break;
    case TAB_SETTINGS:
      if (!setEditing) {
        if (btnUp.pressed) setItem = (setItem - 1 + SET_ITEMS) % SET_ITEMS;
        if (btnDn.pressed) setItem = (setItem + 1) % SET_ITEMS;
        if (btnOk.pressed) { if (setItem == 4) applySettings(); else setEditing = true; }
      } else {
        switch (setItem) {
          case 0:
            if (btnUp.pressed) cfg.freq = constrain(cfg.freq + 0.005f, 300.0f, 928.0f);
            if (btnDn.pressed) cfg.freq = constrain(cfg.freq - 0.005f, 300.0f, 928.0f);
            break;
          case 1:
            if (btnUp.pressed) cfg.bwIndex = (cfg.bwIndex + 1) % BW_COUNT;
            if (btnDn.pressed) cfg.bwIndex = (cfg.bwIndex - 1 + BW_COUNT) % BW_COUNT;
            break;
          case 2:
            if (btnUp.pressed || btnDn.pressed) cfg.modIndex = (cfg.modIndex + 1) % MOD_COUNT;
            break;
          case 3:
            if (btnUp.pressed || btnDn.pressed) cfg.highGain = !cfg.highGain;
            break;
        }
        if (btnOk.pressed) setEditing = false;
      }
      break;
  }
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setContrast(200);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_9x18B_mr);
  u8g2.drawStr(20, 30, "PD RF");
  u8g2.setFont(u8g2_font_5x7_mr);
  u8g2.drawStr(30, 45, "ESP32-C3");
  u8g2.sendBuffer();
  delay(1500);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  ELECHOUSE_cc1101.setSpiPin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CC_CS);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(cfg.freq);
  ELECHOUSE_cc1101.setRxBW(bwTable[cfg.bwIndex]);
  ELECHOUSE_cc1101.setModulation(2); // OOK
  ELECHOUSE_cc1101.SpiWriteReg(0x1B, 0x91); // high gain AGC
  ELECHOUSE_cc1101.SetRx();
  if (!SD.begin(PIN_SD_CS)) Serial.println("SD init failed");
  else recLoad();
  memset(specBars, 0, sizeof(specBars));
}

// ── Loop ──────────────────────────────────────────────────────────
uint32_t lastSpecUpdate = 0;
uint32_t lastDraw       = 0;

void loop() {
  handleButtons();
  recUpdate();
  playUpdate();
  if (activeTab == TAB_SPECTRUM && millis() - lastSpecUpdate > 50) {
    specUpdate();
    lastSpecUpdate = millis();
  }
  if (millis() - lastDraw > 50) {
    lastDraw = millis();
    u8g2.clearBuffer();
    drawHeader();
    switch (activeTab) {
      case TAB_SPECTRUM: drawSpectrum(); break;
      case TAB_RECORD:   drawRecord();   break;
      case TAB_PLAY:     drawPlay();     break;
      case TAB_SETTINGS: drawSettings(); break;
    }
    u8g2.sendBuffer();
  }
}
