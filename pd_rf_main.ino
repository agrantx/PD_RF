/*
  PD RF — ESP32-C3 Super Mini
  CC1101 + SSD1306 0.96" OLED + SD card + 3 buttons

  Controls:
  - UP/DN              — switch tabs (if not in submenu)
  - SET tab: 2xOK      — enter settings; 2xOK again — exit
  - In settings:
      UP/DN            — scroll items
      OK (hold 1s)     — edit (highlighted white)
      UP/DN            — change value
      OK (hold 1s)     — confirm
      SAVE / EXIT      — select with OK
  - REC tab:
      OK (click)       — start/pause/resume recording
      OK (hold 1s)     — save to SD (only when paused)
  - PLAY tab:
      2xOK             — enter file list
      UP/DN            — navigate list
      OK (hold 1s)     — open file
      2xOK             — exit list / exit player
      OK (in player)   — pause / resume
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
  float   freq        = 433.920f;
  uint8_t bwIndex     = 2;
  uint8_t modIndex    = 0;
  bool    highGain    = true;
  bool    invertTheme = false;
};
Settings cfg;
Settings cfgBackup;

const float bwTable[]   = {58.0f, 101.5f, 203.1f, 406.2f, 812.5f};
const char* bwLabels[]  = {"58kHz","101kHz","203kHz","406kHz","812kHz"};
const char* modLabels[] = {"OOK","2FSK"};
#define BW_COUNT  5
#define MOD_COUNT 2

// ── Кнопки ────────────────────────────────────────────────────────
#define DEBOUNCE_MS   40
#define LONG_PRESS_MS 1000   // 1 секунда
#define DBLCLICK_MS   350

struct Button {
  uint8_t  pin;
  bool     lastRaw;
  uint32_t lastChangeMs;
  bool     pressed;       // одиночный клик
  bool     longPressed;   // удержание 1с
  bool     doubleClicked;
  uint32_t downMs;
  bool     isDown;
  bool     longFired;
  uint32_t lastClickMs;
  uint8_t  clickCount;
};

Button btnUp = {};
Button btnDn = {};
Button btnOk = {};

void initBtn(Button& b, uint8_t pin) {
  b.pin = pin;
  b.lastRaw = HIGH;
}

void updateBtn(Button& b) {
  b.pressed = false;
  b.longPressed = false;
  b.doubleClicked = false;

  bool raw = digitalRead(b.pin);
  if (raw != b.lastRaw) {
    if (millis() - b.lastChangeMs < DEBOUNCE_MS) return;
    b.lastChangeMs = millis();
    b.lastRaw = raw;
    if (raw == LOW) {
      b.isDown   = true;
      b.downMs   = millis();
      b.longFired = false;
    } else {
      if (b.isDown && !b.longFired) {
        b.clickCount++;
        if (b.clickCount >= 2 && (millis() - b.lastClickMs) < DBLCLICK_MS) {
          b.doubleClicked = true;
          b.clickCount = 0;
        } else {
          b.lastClickMs = millis();
        }
      }
      b.isDown = false;
    }
  }

  if (b.isDown && !b.longFired && (millis() - b.downMs >= LONG_PRESS_MS)) {
    b.longFired  = true;
    b.longPressed = true;
  }

  if (!b.isDown && b.clickCount == 1 && (millis() - b.lastClickMs) >= DBLCLICK_MS) {
    b.pressed    = true;
    b.clickCount = 0;
  }
}

// ── Настройки ─────────────────────────────────────────────────────
// setMode: 0=выкл, 1=листаем пункты, 2=редактируем значение
uint8_t setMode = 0;
int     setItem = 0;
#define SET_PARAM_COUNT 5
#define SET_TOTAL 7
int setScrollOffset = 0;
#define SET_VISIBLE 3

void applySettings() {
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(cfg.freq);
  ELECHOUSE_cc1101.setRxBW(bwTable[cfg.bwIndex]);
  ELECHOUSE_cc1101.setModulation(cfg.modIndex == 0 ? 2 : 0);
  byte agcVal = cfg.highGain ? 0x91 : 0x40;
  ELECHOUSE_cc1101.SpiWriteReg(0x1B, agcVal);
  ELECHOUSE_cc1101.SetRx();
}

void applyTheme() {
  u8g2.sendF("c", cfg.invertTheme ? 0xA7 : 0xA6);
}

// ── Спектр ────────────────────────────────────────────────────────
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
  // Растянуть на всю ширину: 128px / 32 бина = 4px на бин, 3px бар + 1px зазор
  for (int i = 0; i < SPEC_BINS; i++) {
    int h = specBars[i], x = i * 4, y = 63 - h;
    u8g2.drawBox(x, y, 3, h);
  }
  u8g2.drawHLine(0, 63, 128);
}

// ── Запись ────────────────────────────────────────────────────────
#define REC_BUF_SIZE 4096
uint8_t  recBuf[REC_BUF_SIZE];
uint16_t recLen        = 0;
bool     isRecording   = false;
bool     recPaused     = false;   // запись на паузе
bool     hasRecording  = false;
uint32_t recStartMs    = 0;
uint32_t recDurationMs = 0;
uint8_t  wavePreview[128];
uint8_t  waveLen = 0;

// RSSI-история для индикатора активности во время записи
#define RSSI_HIST 128
int8_t   rssiHist[RSSI_HIST];
uint8_t  rssiHead = 0;

void recStart() {
  recLen = 0; waveLen = 0; isRecording = true; recPaused = false;
  recStartMs = millis();
  memset(rssiHist, 0, sizeof(rssiHist));
  rssiHead = 0;
  ELECHOUSE_cc1101.SetRx();
}

void recStop() {
  isRecording = false; recPaused = false;
  hasRecording = recLen > 0;
  recDurationMs = millis() - recStartMs;
  waveLen = min((int)recLen, 128);
  for (int i = 0; i < waveLen; i++)
    wavePreview[i] = (recBuf[(int)i * recLen / waveLen] > 127) ? 1 : 0;
  ELECHOUSE_cc1101.setSidle();
}

void recSave() {
  if (!hasRecording) return;
  // Имя файла: /sigXXXX.bin, где XXXX — millis/100
  char fname[20];
  snprintf(fname, sizeof(fname), "/sig%04lu.sub", (millis() / 100) % 10000);
  File f = SD.open(fname, FILE_WRITE);
  if (!f) return;
  f.write(recBuf, recLen);
  f.close();
}

// Загрузка первого .bin файла (совместимость)
void recLoad() {
  File f = SD.open("/signal.sub");
  if (!f) return;
  recLen = f.read(recBuf, REC_BUF_SIZE); f.close();
  hasRecording = recLen > 0; recDurationMs = recLen * 10;
  waveLen = min((int)recLen, 128);
  for (int i = 0; i < waveLen; i++)
    wavePreview[i] = (recBuf[(int)i * recLen / waveLen] > 127) ? 1 : 0;
}

void recUpdate() {
  if (!isRecording || recPaused) return;
  // Читаем RSSI для индикатора активности
  int rssi = ELECHOUSE_cc1101.getRssi();
  rssiHist[rssiHead] = (int8_t)constrain(map(rssi, -110, -10, -63, 63), -63, 63);
  rssiHead = (rssiHead + 1) % RSSI_HIST;

  if (ELECHOUSE_cc1101.CheckReceiveFlag()) {
    int n = ELECHOUSE_cc1101.ReceiveData(recBuf + recLen);
    recLen = min((int)(recLen + n), REC_BUF_SIZE);
  }
  if (recLen >= REC_BUF_SIZE) recStop();
}

// Рисуем осциллограф из RSSI-истории (растянут на всю ширину)
void drawRssiScope(int yCenter, int amplitude) {
  for (int x = 0; x < 128; x++) {
    int idx = (rssiHead + x) % RSSI_HIST;
    int dy = (int)rssiHist[idx] * amplitude / 63;
    u8g2.drawPixel(x, yCenter - dy);
    if (x > 0) {
      int idxPrev = (rssiHead + x - 1) % RSSI_HIST;
      int dyPrev = (int)rssiHist[idxPrev] * amplitude / 63;
      int y0 = yCenter - dyPrev, y1 = yCenter - dy;
      if (y0 != y1) u8g2.drawVLine(x, min(y0,y1), abs(y0-y1)+1);
    }
  }
}

void drawWaveform(uint8_t* wave, uint8_t len, int yHigh, int yLow, bool showRec) {
  // Растянуть на всю ширину 128px
  if (len == 0) return;
  for (int x = 0; x < 128; x++) {
    int i = x * (len - 1) / 127;
    int inext = min(i + 1, (int)len - 1);
    int y   = wave[i]    ? yHigh : yLow;
    int yn  = wave[inext] ? yHigh : yLow;
    u8g2.drawPixel(x, y);
    if (y != yn) u8g2.drawVLine(x, min(y, yn), abs(y - yn) + 1);
  }
  if (showRec) {
    // Мигающий кружок записи
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawCircle(123, 15, 3);
      u8g2.drawDisc(123, 15, 2);
    }
    u8g2.setFont(u8g2_font_5x7_mr);
    char t[16];
    uint32_t sec = (millis() - recStartMs) / 1000;
    uint32_t ms  = ((millis() - recStartMs) % 1000) / 100;
    snprintf(t, sizeof(t), "%02lu:%02lu.%lu", sec / 60, sec % 60, ms);
    u8g2.drawStr(0, 62, t);
  }
}

void drawRecord() {
  u8g2.setFont(u8g2_font_5x7_mr);
  char buf[24];
  snprintf(buf, sizeof(buf), "%.3fMHz", cfg.freq);
  u8g2.drawStr(0, 20, buf);
  u8g2.drawHLine(0, 21, 128);

  if (isRecording) {
    // Показываем реальный RSSI-осциллограф
    u8g2.drawHLine(0, 35, 128); // центральная линия
    drawRssiScope(35, 12);
    drawWaveform(nullptr, 0, 0, 0, true); // только таймер + кружок
    u8g2.drawStr(80, 62, "REC");
    if (recPaused)
      u8g2.drawStr(0, 62, "1s=SAVE");
    else
      u8g2.drawStr(0, 62, "OK=PAUSE");
  } else if (hasRecording) {
    drawWaveform(wavePreview, waveLen, 28, 42, false);
    snprintf(buf, sizeof(buf), "%u bytes", recLen);
    u8g2.drawStr(0, 62, buf);
    if (recPaused)
      u8g2.drawStr(72, 62, "1s=SAVE");
  } else {
    u8g2.drawStr(10, 38, "NO RECORDING");
    u8g2.drawStr(5, 50, "[OK] = start");
  }
}

// ── Список файлов (PLAY) ──────────────────────────────────────────
#define MAX_FILES 16
char     fileList[MAX_FILES][24];
uint8_t  fileCount    = 0;
uint8_t  fileCursor   = 0;
int      fileScroll   = 0;
bool     inFileList   = false;   // true = показываем список
bool     inFilePlayer = false;   // true = файл открыт, показываем плеер
char     openedFile[24] = "";

#define FILE_VISIBLE 3

void scanFiles() {
  fileCount = 0;
  File root = SD.open("/");
  if (!root) return;
  File entry;
  while ((entry = root.openNextFile()) && fileCount < MAX_FILES) {
    String name = entry.name();
    if (name.endsWith(".sub") || name.endsWith(".SUB")) {
      name.toCharArray(fileList[fileCount], 24);
      fileCount++;
    }
    entry.close();
  }
  root.close();
}

void loadFile(const char* fname) {
  char fullPath[28];
  snprintf(fullPath, sizeof(fullPath), "/%s", fname);
  File f = SD.open(fullPath);
  if (!f) return;
  recLen = f.read(recBuf, REC_BUF_SIZE); f.close();
  hasRecording = recLen > 0;
  recDurationMs = recLen * 10;
  waveLen = min((int)recLen, 128);
  for (int i = 0; i < waveLen; i++)
    wavePreview[i] = (recBuf[(int)i * recLen / waveLen] > 127) ? 1 : 0;
  strncpy(openedFile, fname, 23);
}

// ── Воспроизведение ───────────────────────────────────────────────
bool     isPlaying   = false;
bool     isPaused    = false;
uint32_t playStartMs = 0;
uint32_t pauseOffset = 0;  // накопленное время до паузы
uint16_t playPos     = 0;

void playStart() {
  if (!hasRecording) return;
  isPlaying = true; isPaused = false;
  playPos = 0; playStartMs = millis(); pauseOffset = 0;
  ELECHOUSE_cc1101.SetTx();
}

void playPause() {
  if (!isPlaying) return;
  if (!isPaused) {
    isPaused = true;
    pauseOffset += millis() - playStartMs;
    ELECHOUSE_cc1101.setSidle();
  } else {
    isPaused = false;
    playStartMs = millis();
    ELECHOUSE_cc1101.SetTx();
  }
}

void playStop() {
  isPlaying = false; isPaused = false;
  playPos = 0; pauseOffset = 0;
  ELECHOUSE_cc1101.setSidle();
}

void playUpdate() {
  if (!isPlaying || isPaused) return;
  if (playPos >= recLen) { playStop(); return; }
  // Обновляем RSSI-историю для осциллографа в плеере
  int rssiVal = ELECHOUSE_cc1101.getRssi();
  rssiHist[rssiHead] = (int8_t)constrain(map(rssiVal, -110, -10, -63, 63), -63, 63);
  rssiHead = (rssiHead + 1) % RSSI_HIST;
  int chunk = min(64, (int)(recLen - playPos));
  ELECHOUSE_cc1101.SendData(recBuf + playPos, chunk);
  playPos += chunk;
}

void drawFileList() {
  // Корректируем скролл
  if (fileCursor < (uint8_t)fileScroll) fileScroll = fileCursor;
  if (fileCursor >= fileScroll + FILE_VISIBLE) fileScroll = fileCursor - FILE_VISIBLE + 1;

  u8g2.setFont(u8g2_font_5x7_mr);

  if (fileCount == 0) {
    u8g2.drawStr(10, 35, "No files on SD");
    u8g2.drawStr(10, 48, "2xOK = exit");
    return;
  }

  for (int vi = 0; vi < FILE_VISIBLE; vi++) {
    int idx = fileScroll + vi;
    if (idx >= fileCount) break;
    int y = 22 + vi * 14;
    bool sel = (idx == (int)fileCursor);

    if (sel) {
      u8g2.drawBox(0, y - 10, 128, 12);
      u8g2.setDrawColor(0);
      u8g2.drawStr(4, y, fileList[idx]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(4, y, fileList[idx]);
    }
  }

  // Полоса прокрутки
  if (fileCount > FILE_VISIBLE) {
    int barH = 42 * FILE_VISIBLE / fileCount;
    int barY = 12 + 42 * fileScroll / fileCount;
    u8g2.drawFrame(126, 12, 2, 42);
    u8g2.drawBox(126, barY, 2, barH);
  }

  u8g2.drawStr(0, 63, "1s=open  2xOK=exit");
}

void drawFilePlayer() {
  u8g2.setFont(u8g2_font_5x7_mr);

  // Название файла в верхней строке (уже после хедера)
  char nameBuf[24];
  snprintf(nameBuf, sizeof(nameBuf), "> %s", openedFile);
  u8g2.drawStr(0, 21, nameBuf);
  u8g2.drawHLine(0, 23, 128);

  // Живой RSSI-осциллограф (анимированный как при записи)
  u8g2.drawHLine(0, 38, 128); // центральная линия
  drawRssiScope(38, 10);

  // Прогресс-бар (растянут на всю ширину)
  uint32_t elapsed = isPlaying && !isPaused
    ? (pauseOffset + millis() - playStartMs)
    : pauseOffset;
  uint32_t total = recDurationMs ? recDurationMs : 1;
  if (elapsed > total) elapsed = total;
  int prog = (int)(128L * elapsed / total);
  u8g2.drawFrame(0, 48, 128, 4);
  u8g2.drawBox(0, 48, prog, 4);

  // Время
  uint32_t sec  = elapsed / 1000, tsec = recDurationMs / 1000;
  char tbuf[24];
  snprintf(tbuf, sizeof(tbuf), "%02lu:%02lu/%02lu:%02lu",
    sec/60, sec%60, tsec/60, tsec%60);
  u8g2.drawStr(0, 62, tbuf);

  // Статус
  const char* st = isPlaying ? (isPaused ? "||" : ">") : "[]";
  u8g2.drawStr(110, 62, st);
}

void drawPlay() {
  if (inFileList) {
    drawFileList();
  } else if (inFilePlayer) {
    drawFilePlayer();
  } else {
    // Стартовый экран вкладки PLAY
    u8g2.setFont(u8g2_font_5x7_mr);
    u8g2.drawStr(5, 30, "2xOK = file list");
    u8g2.drawStr(5, 44, "Files on SD:");
    char buf[8]; snprintf(buf, sizeof(buf), "%u", fileCount);
    u8g2.drawStr(84, 44, buf);
  }
}

// ── Настройки draw ────────────────────────────────────────────────
void getSettingLabel(int idx, char* out, int sz) {
  switch (idx) {
    case 0: snprintf(out, sz, "FREQ  %.3fMHz", cfg.freq); break;
    case 1: snprintf(out, sz, "BW    %s",  bwLabels[cfg.bwIndex]); break;
    case 2: snprintf(out, sz, "MOD   %s",  modLabels[cfg.modIndex]); break;
    case 3: snprintf(out, sz, "GAIN  %s",  cfg.highGain ? "HIGH" : "LOW"); break;
    case 4: snprintf(out, sz, "THEME %s",  cfg.invertTheme ? "INV" : "NORM"); break;
    case 5: snprintf(out, sz, "  SAVE"); break;
    case 6: snprintf(out, sz, "  EXIT"); break;
  }
}

void drawSettings() {
  if (setItem < setScrollOffset) setScrollOffset = setItem;
  if (setItem >= setScrollOffset + SET_VISIBLE)
    setScrollOffset = setItem - SET_VISIBLE + 1;

  u8g2.setFont(u8g2_font_5x7_mr);
  char buf[32];

  // Первая строка начинается ниже хедера (y=12), шаг 14px
  for (int vi = 0; vi < SET_VISIBLE; vi++) {
    int idx = setScrollOffset + vi;
    if (idx >= SET_TOTAL) break;
    int y = 23 + vi * 14;   // сдвинуто вниз, не заходит на хедер

    bool isSel  = (idx == setItem) && (setMode >= 1);
    bool isEdit = (idx == setItem) && (setMode == 2);

    getSettingLabel(idx, buf, sizeof(buf));

    if (idx == 5 || idx == 6) {
      // Кнопки СОХРАНИТЬ / ВЫЙТИ
      if (isSel) {
        u8g2.drawRBox(1, y - 10, 126, 12, 3);
        u8g2.setDrawColor(0);
        u8g2.drawStr(4, y, buf);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawRFrame(1, y - 10, 126, 12, 3);
        u8g2.drawStr(4, y, buf);
      }
    } else {
      if (isEdit) {
        // Редактирование: белый фон, чёрный текст
        u8g2.drawBox(0, y - 10, 128, 12);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, buf);
        u8g2.setDrawColor(1);
      } else if (isSel) {
        // Выбранный: белый фон, чёрный текст (инверсия)
        u8g2.drawBox(0, y - 10, 128, 12);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, buf);
        u8g2.setDrawColor(1);
      } else {
        // Обычный: чёрный текст
        u8g2.drawStr(2, y, buf);
      }
    }
  }

  // Полоса прокрутки
  if (SET_TOTAL > SET_VISIBLE) {
    int barH = 42 * SET_VISIBLE / SET_TOTAL;
    int barY = 12 + 42 * setScrollOffset / SET_TOTAL;
    u8g2.drawFrame(126, 12, 2, 42);
    u8g2.drawBox(126, barY, 2, barH);
  }

  // Подсказка внизу
  u8g2.setFont(u8g2_font_5x7_mr);
  if (setMode == 0) {
    u8g2.drawStr(0, 63, "2xOK=enter  2xOK=exit");
  } else if (setMode == 1) {
    u8g2.drawStr(0, 63, "1sOK=edit  2xOK=exit");
  } else {
    u8g2.drawStr(0, 63, "UP/DN=val  1sOK=OK");
  }
}

// ── Хедер ─────────────────────────────────────────────────────────
void drawHeader() {
  u8g2.drawBox(0, 0, 128, 11);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x12_mr);
  u8g2.drawStr(1, 9, "PD RF");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x7_mr);
  for (int i = 0; i < TAB_COUNT; i++) {
    int x = 45 + i * 21;
    if (i == activeTab) {
      u8g2.setDrawColor(0);
      u8g2.drawBox(x - 1, 0, 21, 11);
      u8g2.setDrawColor(1);
      u8g2.drawFrame(x - 1, 0, 21, 11);
      u8g2.drawStr(x, 9, tabNames[i]);
    } else {
      u8g2.setDrawColor(0);
      u8g2.drawStr(x, 9, tabNames[i]);
      u8g2.setDrawColor(1);
    }
  }
  u8g2.drawHLine(0, 11, 128);
}

// ── Обработка кнопок ─────────────────────────────────────────────
void handleButtons() {
  updateBtn(btnUp);
  updateBtn(btnDn);
  updateBtn(btnOk);

  // ── Таб SET — в любом setMode перехватываем 2xOK ──────────────
  if (activeTab == TAB_SETTINGS) {
    if (btnOk.doubleClicked) {
      if (setMode == 0) {
        // Войти в настройки
        setMode = 1; setItem = 0; setScrollOffset = 0;
        cfgBackup = cfg;
        btnOk.longFired = true; // block longPress on this same press
      } else {
        // Выйти из настроек (двойной клик всегда выходит)
        cfg = cfgBackup;
        applyTheme();
        setMode = 0;
      }
      return;
    }
  }

  // ── Вне режима настроек (листаем табы) ─────────────────────────
  if (setMode == 0) {
    // Навигация по табам
    if (activeTab != TAB_SETTINGS) {
      if (btnUp.pressed) activeTab = (activeTab - 1 + TAB_COUNT) % TAB_COUNT;
      if (btnDn.pressed) activeTab = (activeTab + 1) % TAB_COUNT;
    }

    // Таб REC
    if (activeTab == TAB_RECORD) {
      if (btnOk.pressed) {
        if (!isRecording) {
          recStart();          // старт записи
        } else if (!recPaused) {
          recPaused = true;    // поставить на паузу
          ELECHOUSE_cc1101.setSidle();
        } else {
          recPaused = false;   // продолжить запись
          ELECHOUSE_cc1101.SetRx();
        }
      }
      if (btnOk.longPressed) {
        if (isRecording && recPaused) {
          recSave();
          isRecording = false; recPaused = false; // stop, keep data
        }
      }
    }

    // Таб PLAY
    if (activeTab == TAB_PLAY) {
      if (inFilePlayer) {
        // Внутри плеера
        if (btnOk.pressed) {
          if (!isPlaying) playStart();
          else playPause();
        }
        if (btnOk.doubleClicked) {
          // Выход из плеера в список
          playStop();
          inFilePlayer = false;
          inFileList   = true;
        }
        // UP/DN во время воспроизведения — не используются
      } else if (inFileList) {
        // В списке файлов
        if (btnUp.pressed)
          fileCursor = (fileCursor == 0) ? (fileCount - 1) : (fileCursor - 1);
        if (btnDn.pressed)
          fileCursor = (fileCursor + 1) % fileCount;
        if (btnOk.longPressed && fileCount > 0) {
          // Открыть файл
          loadFile(fileList[fileCursor]);
          inFileList   = false;
          inFilePlayer = true;
        }
        if (btnOk.doubleClicked) {
          // Выход из списка
          inFileList = false;
        }
      } else {
        // Стартовый экран PLAY
        if (btnOk.doubleClicked) {
          scanFiles();
          fileCursor = 0; fileScroll = 0;
          inFileList = true;
          btnOk.longFired = true; // block longPress on same press
        }
        if (btnUp.pressed) activeTab = (activeTab - 1 + TAB_COUNT) % TAB_COUNT;
        if (btnDn.pressed) activeTab = (activeTab + 1) % TAB_COUNT;
      }
    }
    return;
  }

  // ── В настройках: листаем пункты ──────────────────────────────
  if (setMode == 1) {
    if (btnUp.pressed) setItem = (setItem - 1 + SET_TOTAL) % SET_TOTAL;
    if (btnDn.pressed) setItem = (setItem + 1) % SET_TOTAL;

    if (btnOk.pressed) {
      if (setItem == 5) { applySettings(); applyTheme(); setMode = 0; }
      if (setItem == 6) { cfg = cfgBackup; applyTheme(); setMode = 0; }
    }
    if (btnOk.longPressed && setItem < SET_PARAM_COUNT) {
      setMode = 2;
      btnOk.longFired = true; // consumed — prevent immediate re-trigger
    }
    return;
  }

  // ── Редактируем значение ───────────────────────────────────────
  if (setMode == 2) {
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
        if (btnUp.pressed || btnDn.pressed)
          cfg.modIndex = (cfg.modIndex + 1) % MOD_COUNT;
        break;
      case 3:
        if (btnUp.pressed || btnDn.pressed) cfg.highGain = !cfg.highGain;
        break;
      case 4:
        if (btnUp.pressed || btnDn.pressed) {
          cfg.invertTheme = !cfg.invertTheme;
          applyTheme();
        }
        break;
    }
    if (btnOk.longPressed) {
      setMode = 1;
      btnOk.longFired = true; // consumed — next long won't immediately re-enter edit
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  initBtn(btnUp, PIN_BTN_UP);
  initBtn(btnDn, PIN_BTN_DN);
  initBtn(btnOk, PIN_BTN_OK);
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
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.SpiWriteReg(0x1B, 0x91);
  ELECHOUSE_cc1101.SetRx();

  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("SD: init error");
  } else {
    recLoad();
    scanFiles();
  }

  memset(specBars, 0, sizeof(specBars));
  memset(rssiHist, 0, sizeof(rssiHist));
  cfgBackup = cfg;
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
