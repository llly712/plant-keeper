/*
  植物养护从机 ESP32-C3
  地址改下面这个，第一块是2，最多到33
*/
#include <Arduino.h>

#define LIGHT_PIN   3
#define SOIL_PIN    4
#define PUMP1_PIN   10
#define PUMP2_PIN   5
#define PUMP3_PIN   7
#define LIGHT_PIN2  1
#define BUZZER_PIN  0
#define RS485_DIR   6
#define BTN_PIN     9

// PCB上的RXD0/TXD0就是C3的GPIO20/21，调试走原生USB
#define RS485_RX    20
#define RS485_TX    21

const byte myAddr = 3;
const int pumpMaxSec = 60;

HardwareSerial bus(0);

uint16_t regv[10];
unsigned long pumpStart[3];
uint16_t pumpSec[3];
const int pumpPin[3] = {PUMP1_PIN, PUMP2_PIN, PUMP3_PIN};

unsigned long lastRead = 0;
unsigned long lastCmd = 0;
unsigned long buzzStart = 0;
int buzzMs = 0;

uint8_t rxbuf[64];
int rxlen = 0;
unsigned long lastByte = 0;
bool rxDrop = false;

uint16_t crc16(const uint8_t* b, int n) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < n; i++) {
    c ^= b[i];
    for (int j = 0; j < 8; j++) {
      if (c & 1) c = (c >> 1) ^ 0xA001;
      else c >>= 1;
    }
  }
  return c;
}

void sendFrame(const uint8_t* data, int n) {
  uint8_t out[40];
  memcpy(out, data, n);
  uint16_t c = crc16(data, n);
  out[n] = c & 0xFF;
  out[n + 1] = c >> 8;

  digitalWrite(RS485_DIR, HIGH);
  delayMicroseconds(30);
  bus.write(out, n + 2);
  bus.flush();
  digitalWrite(RS485_DIR, LOW);
}

void sendError(byte fn, byte code) {
  uint8_t out[3] = {myAddr, (uint8_t)(fn | 0x80), code};
  sendFrame(out, 3);
}

void readAdc() {
  if (millis() - lastRead < 300) return;
  lastRead = millis();

  long a = 0, b = 0;
  for (int i = 0; i < 5; i++) {
    a += analogRead(LIGHT_PIN);
    b += analogRead(SOIL_PIN);
    delay(2);
  }
  regv[0] = a / 5;
  regv[1] = b / 5;
}

void stopPump(int n) {
  ledcWrite(pumpPin[n], 0);
  pumpSec[n] = 0;
  regv[2 + n] = 0;
  regv[7 + n] = 0;
}

void runPump(int n, uint16_t sec) {
  if (sec == 0) {
    stopPump(n);
    return;
  }
  sec = min(sec, (uint16_t)pumpMaxSec);
  pumpSec[n] = sec;
  pumpStart[n] = millis();
  regv[2 + n] = 1;
  ledcWrite(pumpPin[n], 255);
}

void checkOutput() {
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (pumpSec[i] && now - pumpStart[i] >= pumpSec[i] * 1000UL) stopPump(i);
  }

  if (buzzMs && now - buzzStart >= (unsigned long)buzzMs) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzMs = 0;
  }

  // 主机掉线就停泵，灯不用关
  if (now - lastCmd > 120000UL) {
    for (int i = 0; i < 3; i++) stopPump(i);
    ledcWrite(LIGHT_PIN2, 0);
    regv[5] = 0;
  }
}

void doRead(const uint8_t* q) {
  uint16_t start = ((uint16_t)q[2] << 8) | q[3];
  uint16_t count = ((uint16_t)q[4] << 8) | q[5];
  if (count == 0 || start + count > 10) {
    sendError(0x03, 0x02);
    return;
  }

  readAdc();
  for (int i = 0; i < 3; i++) regv[2 + i] = pumpSec[i] ? 1 : 0;

  uint8_t out[32];
  out[0] = myAddr;
  out[1] = 0x03;
  out[2] = count * 2;
  for (int i = 0; i < count; i++) {
    uint16_t v = regv[start + i];
    out[3 + i * 2] = v >> 8;
    out[4 + i * 2] = v & 0xFF;
  }
  sendFrame(out, 3 + count * 2);
}

void doWrite(const uint8_t* q) {
  uint16_t addr = ((uint16_t)q[2] << 8) | q[3];
  uint16_t val = ((uint16_t)q[4] << 8) | q[5];
  if (addr < 5 || addr > 9) {
    sendError(0x06, 0x02);
    return;
  }

  if (addr == 5) {
    regv[5] = min(val, (uint16_t)255);
    ledcWrite(LIGHT_PIN2, regv[5]);
  } else if (addr == 6) {
    regv[6] = val;
    if (val) {
      digitalWrite(BUZZER_PIN, HIGH);
      buzzStart = millis();
      buzzMs = val == 1 ? 100 : 500;
    }
  } else {
    if (val > pumpMaxSec) {
      sendError(0x06, 0x03);
      return;
    }
    runPump(addr - 7, val);
    regv[addr] = val;
  }

  sendFrame(q, 6);
}

void handleFrame() {
  if (rxlen != 8 || rxbuf[0] != myAddr) return;
  uint16_t got = ((uint16_t)rxbuf[7] << 8) | rxbuf[6];
  if (crc16(rxbuf, 6) != got) return;

  if (rxbuf[1] == 0x03) {
    uint16_t start = ((uint16_t)rxbuf[2] << 8) | rxbuf[3];
    uint16_t count = ((uint16_t)rxbuf[4] << 8) | rxbuf[5];
    if (count && start + count <= 10) lastCmd = millis();
    doRead(rxbuf);
  } else if (rxbuf[1] == 0x06) {
    uint16_t addr = ((uint16_t)rxbuf[2] << 8) | rxbuf[3];
    uint16_t val = ((uint16_t)rxbuf[4] << 8) | rxbuf[5];
    if (addr >= 5 && addr <= 9 && (addr < 7 || val <= pumpMaxSec)) lastCmd = millis();
    doWrite(rxbuf);
  } else {
    sendError(rxbuf[1], 0x01);
  }
}

void readBus() {
  while (bus.available()) {
    uint8_t v = bus.read();
    lastByte = micros();
    if (!rxDrop && rxlen < (int)sizeof(rxbuf)) rxbuf[rxlen++] = v;
    else rxDrop = true;
  }

  if ((rxlen || rxDrop) && micros() - lastByte >= 4500) {
    if (!rxDrop && rxlen == 8) handleFrame();
    rxlen = 0;
    rxDrop = false;
  }
}

void setup() {
  pinMode(RS485_DIR, OUTPUT);
  digitalWrite(RS485_DIR, LOW);
  delay(300);
  bus.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);

  analogReadResolution(12);
  pinMode(LIGHT_PIN, INPUT);
  pinMode(SOIL_PIN, INPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < 3; i++) {
    ledcAttach(pumpPin[i], 5000, 8);
    stopPump(i);
  }
  ledcAttach(LIGHT_PIN2, 5000, 8);
  ledcWrite(LIGHT_PIN2, 0);

  readAdc();
  lastCmd = millis();

  digitalWrite(BUZZER_PIN, HIGH);
  delay(90);
  digitalWrite(BUZZER_PIN, LOW);
}

void loop() {
  readBus();
  checkOutput();
  readAdc();
  delay(2);
}
