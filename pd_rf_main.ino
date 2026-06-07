/*
  PD RF — ESP32-C3 Super Mini
  CC1101 + SSD1306 0.96" OLED + SD card + 3 buttons

  Управление:
  - UP/DN       — всегда листают табы (если не в режиме настроек)
  - В табе SET: двойной клик OK → войти в режим настроек (UP/DN = пункты)
  - В настройках: зажать OK 2с → начать редактировать пункт (UP/DN = значение)
  - Зажать OK 2с ещё раз → зафиксировать и вернуться к листанию пунктов
  - СОХРАНИТЬ / ВЫЙТИ — кнопки внизу экрана, короткий OK
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
#define LONG_PRESS_MS 2000
#define DBLCLICK_MS   400

struct Button {
  uint8_t  pin;
  bool     lastRaw;
  uint32_t lastChangeMs;
  // события за этот цикл
  bool     pressed;      // короткий клик
  bool     longPressed;  // удержание 2с
  bool     doubleClicked;
  // внутреннее
  uint32_t downMs;       // когда нажали
  bool     isDown;
  bool     longFired;
  uint32_t lastClickMs;  // для двойного клика
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

  // Дребезг
  if (raw != b.lastRaw) {
    if (millis() - b.lastChangeMs < DEBOUNCE_MS) { return; }
    b.lastChangeMs = millis();
    b.lastRaw = raw;

    if (raw == LOW) {
      // Нажали
      b.isDown   = true;
      b.downMs   = millis();
      b.longFired= false;
    } else {
      // Отпустили
      if (b.isDown && !b.longFired) {
        uint32_t dur = millis() - b.downMs;
        if (dur < LONG_PRESS_MS) {
          // Короткий клик — проверяем на двойной
          b.clickCount++;
          if (b.clickCount >= 2 && (millis() - b.lastClickMs) < DBLCLICK_MS) {
            b.doubleClicked = true;
            b.clickCount = 0;
          } else {
            b.lastClickMs = millis();
          }
        }
      }
      b.isDown = false;
    }
  }

  // Long press — срабатывает пока держим
  if (b.isDown && !b.longFired && (millis() - b.downMs >= LONG_PRESS_MS)) {
    b.longFired  = true;
    b.longPressed= true;
  }

  // Отложенный одиночный клик (выдаём после паузы, если не было двойного)
  if (!b.isDown && b.clickCount == 1 && (millis() - b.lastClickMs) >= DBLCLICK_MS) {
    b.pressed    = true;
    b.clickCount = 0;
  }
}

// ── Режим настроек ────────────────────────────────────────────────
// 0 = не активен (листаем табы)
// 1 = листаем пункты (UP/DN = пункты)
// 2 = редактируем значение (UP/DN = значение)
uint8_t setMode = 0;
int     setItem = 0;
#define SET_PARAM_COUNT 5   // FREQ BW MOD GAIN THEME — редактируемых
// всего строк: 5 параметров + кнопка СОХРАНИТЬ + кнопка ВЫЙТИ
#define SET_TOTAL 7
// скролл
int setScrollOffset = 0;
#define SET_VISIBLE 3  // сколько строк влезает на экран (с нормальным шрифтом)

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
}

// ── Record ────────────────────────────────────────────────────────
#define REC_BUF_SIZE 4096
uint8_t  recBuf[REC_BUF_SIZE];
uint16_t recLen        = 0;
bool     isRecording   = false;
bool     hasRecording  = false;
uint32_t recStartMs    = 0;
uint32_t recDurationMs = 0;
uint8_t  wavePreview[128];
uint8_t  waveLen = 0;

void recStart() { recLen=0; waveLen=0; isRecording=true; recStartMs=millis(); ELECHOUSE_cc1101.SetRx(); }
void recStop() {
  isRecording=false; hasRecording=recLen>0; recDurationMs=millis()-recStartMs;
  waveLen=min((int)recLen,128);
  for(int i=0;i<waveLen;i++) wavePreview[i]=(recBuf[(int)i*recLen/waveLen]>127)?1:0;
  ELECHOUSE_cc1101.setSidle();
}
void recSave() {
  if(!hasRecording) return;
  File f=SD.open("/signal.bin",FILE_WRITE); if(!f) return;
  f.write(recBuf,recLen); f.close();
}
void recLoad() {
  File f=SD.open("/signal.bin"); if(!f) return;
  recLen=f.read(recBuf,REC_BUF_SIZE); f.close();
  hasRecording=recLen>0; recDurationMs=recLen*10;
  waveLen=min((int)recLen,128);
  for(int i=0;i<waveLen;i++) wavePreview[i]=(recBuf[(int)i*recLen/waveLen]>127)?1:0;
}
void recUpdate() {
  if(!isRecording) return;
  if(ELECHOUSE_cc1101.CheckReceiveFlag()) {
    int n=ELECHOUSE_cc1101.ReceiveData(recBuf+recLen);
    recLen=min((int)(recLen+n),REC_BUF_SIZE);
  }
  if(recLen>=REC_BUF_SIZE) recStop();
}

void drawWaveform(uint8_t* wave, uint8_t len, int yHigh, int yLow, bool showRec) {
  for(int i=0;i<(int)len-1;i++){
    int x=i+(128-len)/2, y=wave[i]?yHigh:yLow, yn=wave[i+1]?yHigh:yLow;
    u8g2.drawHLine(x,y,1);
    if(y!=yn) u8g2.drawVLine(x,min(y,yn),abs(y-yn)+1);
  }
  if(showRec){
    if((millis()/500)%2==0){u8g2.drawCircle(120,18,3);u8g2.drawDisc(120,18,2);}
    u8g2.setFont(u8g2_font_5x7_mr);
    char t[16]; uint32_t sec=(millis()-recStartMs)/1000, ms=((millis()-recStartMs)%1000)/100;
    snprintf(t,sizeof(t),"%02lu:%02lu.%lu",sec/60,sec%60,ms);
    u8g2.drawStr(0,62,t);
  }
}

void drawRecord() {
  u8g2.setFont(u8g2_font_5x7_mr);
  char buf[24]; snprintf(buf,sizeof(buf),"%.3fMHz",cfg.freq);
  u8g2.drawStr(0,20,buf); u8g2.drawHLine(0,22,128);
  if(isRecording){
    uint8_t liveWave[64]; int lLen=min((int)recLen,64);
    for(int i=0;i<lLen;i++) liveWave[i]=(recBuf[recLen-lLen+i]>127)?1:0;
    drawWaveform(liveWave,lLen,28,42,true);
    u8g2.drawStr(80,62,"REC");
  } else if(hasRecording){
    drawWaveform(wavePreview,waveLen,28,42,false);
    snprintf(buf,sizeof(buf),"LEN:%u",recLen); u8g2.drawStr(0,62,buf);
    u8g2.drawStr(60,62,"[OK=SAVE]");
  } else {
    u8g2.drawStr(20,38,"No signal"); u8g2.drawStr(10,50,"[OK] to record");
  }
}

// ── Play ──────────────────────────────────────────────────────────
bool     isPlaying=false;
uint32_t playStartMs=0;
uint16_t playPos=0;

void playStart(){ if(!hasRecording)return; isPlaying=true; playPos=0; playStartMs=millis(); ELECHOUSE_cc1101.SetTx(); }
void playStop(){ isPlaying=false; ELECHOUSE_cc1101.setSidle(); }
void playUpdate(){
  if(!isPlaying) return;
  if(playPos>=recLen){playStop();return;}
  int chunk=min(64,(int)(recLen-playPos));
  ELECHOUSE_cc1101.SendData(recBuf+playPos,chunk); playPos+=chunk;
}

void drawPlay(){
  u8g2.setFont(u8g2_font_5x7_mr); char buf[24];
  if(!hasRecording){u8g2.drawStr(10,38,"No recording");u8g2.drawStr(10,50,"Go to REC tab");return;}
  snprintf(buf,sizeof(buf),"%.3fMHz",cfg.freq); u8g2.drawStr(0,20,buf); u8g2.drawHLine(0,22,128);
  drawWaveform(wavePreview,waveLen,28,42,false);
  uint32_t elapsed=isPlaying?(millis()-playStartMs):0, total=recDurationMs?recDurationMs:1;
  int prog=constrain((int)(124*(long)elapsed/total),0,124);
  u8g2.drawFrame(2,50,124,4); u8g2.drawBox(2,50,prog,4);
  uint32_t sec=elapsed/1000,tsec=recDurationMs/1000;
  snprintf(buf,sizeof(buf),"%02lu:%02lu/%02lu:%02lu",sec/60,sec%60,tsec/60,tsec%60);
  u8g2.drawStr(0,62,buf); u8g2.drawStr(108,62,isPlaying?">":"||");
}

// ── Settings draw ─────────────────────────────────────────────────
// Строки настроек: 0=FREQ 1=BW 2=MOD 3=GAIN 4=THEME | 5=СОХРАНИТЬ 6=ВЫЙТИ

void getSettingLabel(int idx, char* out, int sz) {
  switch(idx) {
    case 0: snprintf(out,sz,"FREQ  %.3fMHz",cfg.freq); break;
    case 1: snprintf(out,sz,"BW    %s",bwLabels[cfg.bwIndex]); break;
    case 2: snprintf(out,sz,"MOD   %s",modLabels[cfg.modIndex]); break;
    case 3: snprintf(out,sz,"GAIN  %s",cfg.highGain?"HIGH":"LOW"); break;
    case 4: snprintf(out,sz,"THEME %s",cfg.invertTheme?"INV":"NORM"); break;
    case 5: snprintf(out,sz,"  СОХРАНИТЬ"); break;
    case 6: snprintf(out,sz,"  ВЫЙТИ"); break;
  }
}

void drawSettings() {
  // Корректируем скролл
  if(setItem < setScrollOffset) setScrollOffset = setItem;
  if(setItem >= setScrollOffset + SET_VISIBLE) setScrollOffset = setItem - SET_VISIBLE + 1;

  u8g2.setFont(u8g2_font_6x12_mr); // нормальный шрифт
  char buf[32];

  for(int vi = 0; vi < SET_VISIBLE; vi++) {
    int idx = setScrollOffset + vi;
    if(idx >= SET_TOTAL) break;
    int y = 14 + vi * 15; // строки с нормальным шрифтом

    bool isSel  = (idx == setItem) && (setMode >= 1);
    bool isEdit = (idx == setItem) && (setMode == 2);

    getSettingLabel(idx, buf, sizeof(buf));

    if(idx == 5 || idx == 6) {
      // Большие кнопки СОХРАНИТЬ / ВЫЙТИ
      if(isSel) {
        u8g2.drawRBox(1, y-11, 126, 14, 3);
        u8g2.setDrawColor(0);
        u8g2.drawStr(4, y, buf);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawRFrame(1, y-11, 126, 14, 3);
        u8g2.drawStr(4, y, buf);
      }
    } else {
      // Параметр
      if(isEdit) {
        // Редактирование: заливка + инверсный текст
        u8g2.drawBox(0, y-11, 128, 14);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, buf);
        u8g2.setDrawColor(1);
      } else if(isSel) {
        // Выбран: рамка
        u8g2.drawFrame(0, y-11, 128, 14);
        u8g2.drawStr(2, y, buf);
      } else {
        u8g2.drawStr(2, y, buf);
      }
    }
  }

  // Полоса прокрутки справа
  if(SET_TOTAL > SET_VISIBLE) {
    int barH = 52 * SET_VISIBLE / SET_TOTAL;
    int barY = 12 + 52 * setScrollOffset / SET_TOTAL;
    u8g2.drawFrame(126, 12, 2, 52);
    u8g2.drawBox(126, barY, 2, barH);
  }

  // Подсказка внизу
  u8g2.setFont(u8g2_font_5x7_mr);
  if(setMode == 0) {
    u8g2.drawStr(0, 63, "2xOK=войти в настройки");
  } else if(setMode == 1) {
    u8g2.drawStr(0, 63, "OK 2s=ред.  OK=кнопки");
  } else {
    u8g2.drawStr(0, 63, "UP/DN=менять  OK2s=OK");
  }
}

// ── Header ────────────────────────────────────────────────────────
void drawHeader() {
  // Вся полоса белая
  u8g2.drawBox(0, 0, 128, 11);
  // "PD RF" — чёрный текст
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x12_mr);
  u8g2.drawStr(1, 9, "PD RF");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x7_mr);
  for(int i = 0; i < TAB_COUNT; i++) {
    int x = 45 + i * 21;
    if(i == activeTab) {
      // Активный: чёрная заливка + белый текст + белая рамка
      u8g2.setDrawColor(0);
      u8g2.drawBox(x-1, 0, 21, 11);
      u8g2.setDrawColor(1);
      u8g2.drawFrame(x-1, 0, 21, 11);
      u8g2.drawStr(x, 9, tabNames[i]);
    } else {
      // Неактивный: чёрный текст на белом
      u8g2.setDrawColor(0);
      u8g2.drawStr(x, 9, tabNames[i]);
      u8g2.setDrawColor(1);
    }
  }
  u8g2.drawHLine(0, 11, 128);
}

// ── Buttons ───────────────────────────────────────────────────────
void handleButtons() {
  updateBtn(btnUp); updateBtn(btnDn); updateBtn(btnOk);

  // ─ Вне настроек: листаем табы ──────────────────────────────────
  if(setMode == 0) {
    if(btnUp.pressed) activeTab = (activeTab - 1 + TAB_COUNT) % TAB_COUNT;
    if(btnDn.pressed) activeTab = (activeTab + 1) % TAB_COUNT;

    // Двойной клик OK на табе SET — войти в настройки
    if(activeTab == TAB_SETTINGS && btnOk.doubleClicked) {
      setMode = 1;
      setItem = 0;
      setScrollOffset = 0;
      cfgBackup = cfg; // бэкап для ВЫЙТИ
    }

    // Действия на других табах
    if(activeTab == TAB_RECORD && btnOk.pressed) {
      if(!isRecording) recStart(); else { recStop(); recSave(); }
    }
    if(activeTab == TAB_PLAY && btnOk.pressed) {
      if(!isPlaying) playStart(); else playStop();
    }
    return;
  }

  // ─ В настройках: листаем пункты ────────────────────────────────
  if(setMode == 1) {
    if(btnUp.pressed) setItem = (setItem - 1 + SET_TOTAL) % SET_TOTAL;
    if(btnDn.pressed) setItem = (setItem + 1) % SET_TOTAL;

    // Короткий OK на СОХРАНИТЬ (5) или ВЫЙТИ (6)
    if(btnOk.pressed) {
      if(setItem == 5) { applySettings(); applyTheme(); setMode = 0; }
      if(setItem == 6) { cfg = cfgBackup; applyTheme(); setMode = 0; }
    }

    // Зажать OK 2с → войти в редактирование параметра
    if(btnOk.longPressed && setItem < 5) {
      setMode = 2;
    }
    return;
  }

  // ─ Редактируем значение ─────────────────────────────────────────
  if(setMode == 2) {
    switch(setItem) {
      case 0:
        if(btnUp.pressed) cfg.freq = constrain(cfg.freq + 0.005f, 300.0f, 928.0f);
        if(btnDn.pressed) cfg.freq = constrain(cfg.freq - 0.005f, 300.0f, 928.0f);
        break;
      case 1:
        if(btnUp.pressed) cfg.bwIndex = (cfg.bwIndex + 1) % BW_COUNT;
        if(btnDn.pressed) cfg.bwIndex = (cfg.bwIndex - 1 + BW_COUNT) % BW_COUNT;
        break;
      case 2:
        if(btnUp.pressed || btnDn.pressed) cfg.modIndex = (cfg.modIndex + 1) % MOD_COUNT;
        break;
      case 3:
        if(btnUp.pressed || btnDn.pressed) cfg.highGain = !cfg.highGain;
        break;
      case 4:
        if(btnUp.pressed || btnDn.pressed) { cfg.invertTheme = !cfg.invertTheme; applyTheme(); }
        break;
    }
    // Зажать OK 2с → зафиксировать, вернуться к листанию пунктов
    if(btnOk.longPressed) {
      setMode = 1;
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
  if(!SD.begin(PIN_SD_CS)) Serial.println("SD init failed");
  else recLoad();
  memset(specBars, 0, sizeof(specBars));
  cfgBackup = cfg;
}

// ── Loop ──────────────────────────────────────────────────────────
uint32_t lastSpecUpdate = 0;
uint32_t lastDraw       = 0;

void loop() {
  handleButtons();
  recUpdate();
  playUpdate();
  if(activeTab == TAB_SPECTRUM && millis() - lastSpecUpdate > 50) {
    specUpdate(); lastSpecUpdate = millis();
  }
  if(millis() - lastDraw > 50) {
    lastDraw = millis();
    u8g2.clearBuffer();
    drawHeader();
    switch(activeTab) {
      case TAB_SPECTRUM: drawSpectrum(); break;
      case TAB_RECORD:   drawRecord();   break;
      case TAB_PLAY:     drawPlay();     break;
      case TAB_SETTINGS: drawSettings(); break;
    }
    u8g2.sendBuffer();
  }
}
