/*
  智能植物养护系统 - 国赛版
  从机 ESP32
  负责：光照/土壤传感器采集 + 3路蠕动泵 + 补光灯驱动
  通过RS485 Modbus听从主机命令干活
  最多支持32个从机，改地址就行
*/

// ==================== 头文件 ====================
#include <Arduino.h>

// ==================== 引脚定义 ====================
#define LIGHT_PIN   3     // 光照传感器 (模拟)
#define SOIL_PIN    4     // 土壤湿度 (模拟)
#define PUMP1_PIN   10    // 水泵1 浇水 (PWM)
#define PUMP2_PIN   5     // 水泵2 肥料A (PWM)
#define PUMP3_PIN   7     // 水泵3 肥料B (PWM)
#define LIGHT_LED   1     // 补光灯 (PWM)
#define BUZZER_PIN  0     // 蜂鸣器
#define RS485_CTRL  6     // MAX485方向脚 DE+RE
#define BTN_PIN     9     // 按键 (暂没用)

// RS485串口 用的Serial2
#define RS485_SERIAL Serial2
// Serial2默认脚: RX=16 TX=17

// ==================== Modbus地址 ====================
byte myAddr = 0x02;  // 从机地址 可改(0x01-0x20)

// ==================== PWM参数 ====================
#define PWM_FREQ     5000
#define PWM_RES      8   // 0-255

// ==================== 寄存器 (模拟Holding Registers) ====================
// reg[0] = 光照传感器值
// reg[1] = 土壤湿度模拟值
// reg[2] = 泵1状态 (0=停 1=运行)
// reg[3] = 泵2状态
// reg[4] = 泵3状态
// reg[5] = 补光灯亮度 (0-255)
// reg[6] = 蜂鸣器命令 (1=短鸣 2=长鸣 收到后执行)
// reg[7] = 泵1运行时长 (秒, 写触发启动)
// reg[8] = 泵2运行时长
// reg[9] = 泵3运行时长
uint16_t reg[10] = {0};

// ==================== 泵运行计时 ====================
unsigned long pump1Start = 0, pump2Start = 0, pump3Start = 0;
int pump1Dur = 0, pump2Dur = 0, pump3Dur = 0;

// ==================== 传感器值 ====================
int lightRaw = 0;
int soilRaw = 0;
unsigned long tSensor = 0;

// ==================== 蜂鸣器计时 ====================
unsigned long tBuzzer = 0;
int buzzerDur = 0;

// ==================== CRC16 Modbus ====================
uint16_t crc16(uint8_t* buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

// ==================== 发送Modbus回复 ====================
void replyModbus(uint8_t* data, int len) {
  uint16_t crc = crc16(data, len);
  uint8_t buf[64];
  memcpy(buf, data, len);
  buf[len] = crc & 0xFF;
  buf[len+1] = (crc >> 8) & 0xFF;

  digitalWrite(RS485_CTRL, HIGH);
  delayMicroseconds(200);
  RS485_SERIAL.write(buf, len + 2);
  RS485_SERIAL.flush();
  delayMicroseconds(500);
  digitalWrite(RS485_CTRL, LOW);
}

// ==================== 回复异常 ====================
// func|0x80 表示出错
void replyError(byte func, byte errCode) {
  uint8_t buf[3];
  buf[0] = myAddr;
  buf[1] = func | 0x80;
  buf[2] = errCode;
  replyModbus(buf, 3);
}

// ==================== 处理读寄存器 (功能码03) ====================
void handleRead(uint8_t* req, int len) {
  if (len < 8) return;

  uint16_t startAddr = (req[2] << 8) | req[3];
  uint16_t regCnt = (req[4] << 8) | req[5];

  // 只读0x0000-0x0009
  if (startAddr + regCnt > 10) {
    replyError(0x03, 0x02);  // 地址超出范围
    return;
  }

  uint8_t resp[64];
  resp[0] = myAddr;
  resp[1] = 0x03;
  resp[2] = regCnt * 2;  // 字节数

  // 读之前先刷新传感器值
  readSensors();

  // 更新寄存器
  reg[0] = lightRaw;
  reg[1] = soilRaw;
  reg[2] = (pump1Dur > 0 && millis() - pump1Start < pump1Dur * 1000UL) ? 1 : 0;
  reg[3] = (pump2Dur > 0 && millis() - pump2Start < pump2Dur * 1000UL) ? 1 : 0;
  reg[4] = (pump3Dur > 0 && millis() - pump3Start < pump3Dur * 1000UL) ? 1 : 0;

  for (int i = 0; i < regCnt; i++) {
    resp[3 + i*2] = (reg[startAddr + i] >> 8) & 0xFF;
    resp[3 + i*2 + 1] = reg[startAddr + i] & 0xFF;
  }

  replyModbus(resp, 3 + regCnt * 2);
}

// ==================== 处理写单寄存器 (功能码06) ====================
void handleWrite(uint8_t* req, int len) {
  if (len < 8) return;

  uint16_t addr = (req[2] << 8) | req[3];
  uint16_t val = (req[4] << 8) | req[5];

  if (addr >= 10) {
    replyError(0x06, 0x02);
    return;
  }

  reg[addr] = val;

  // 根据写入的寄存器执行动作
  switch (addr) {
    case 5:  // 补光灯
      ledcWrite(LIGHT_LED, val);
      break;

    case 6:  // 蜂鸣器
      if (val == 1) { digitalWrite(BUZZER_PIN, HIGH); buzzerDur = 100; tBuzzer = millis(); }
      else if (val == 2) { digitalWrite(BUZZER_PIN, HIGH); buzzerDur = 500; tBuzzer = millis(); }
      break;

    case 7:  // 启动泵1 val=运行秒数
      if (val > 0) {
        ledcWrite(PUMP1_PIN, 255);
        pump1Start = millis();
        pump1Dur = val;
        Serial.printf("泵1启动 %ds\n", val);
      }
      break;

    case 8:  // 启动泵2
      if (val > 0) {
        ledcWrite(PUMP2_PIN, 255);
        pump2Start = millis();
        pump2Dur = val;
        Serial.printf("泵2启动 %ds\n", val);
      }
      break;

    case 9:  // 启动泵3
      if (val > 0) {
        ledcWrite(PUMP3_PIN, 255);
        pump3Start = millis();
        pump3Dur = val;
        Serial.printf("泵3启动 %ds\n", val);
      }
      break;
  }

  // 原样回复确认
  replyModbus(req, 6);
}

// ==================== 读传感器 ====================
void readSensors() {
  if (millis() - tSensor < 200) return;  // 200ms内不重复读
  tSensor = millis();

  // 光照 (中值滤波 取5次)
  long sumL = 0;
  for (int i = 0; i < 5; i++) {
    sumL += analogRead(LIGHT_PIN);
    delay(5);
  }
  lightRaw = sumL / 5;

  // 土壤湿度
  long sumS = 0;
  for (int i = 0; i < 5; i++) {
    sumS += analogRead(SOIL_PIN);
    delay(5);
  }
  soilRaw = sumS / 5;
}

// ==================== 泵超时检查 ====================
void checkPumps() {
  unsigned long now = millis();

  if (pump1Dur > 0 && now - pump1Start >= pump1Dur * 1000UL) {
    ledcWrite(PUMP1_PIN, 0);
    pump1Dur = 0;
    Serial.println("泵1自动停止");
  }
  if (pump2Dur > 0 && now - pump2Start >= pump2Dur * 1000UL) {
    ledcWrite(PUMP2_PIN, 0);
    pump2Dur = 0;
    Serial.println("泵2自动停止");
  }
  if (pump3Dur > 0 && now - pump3Start >= pump3Dur * 1000UL) {
    ledcWrite(PUMP3_PIN, 0);
    pump3Dur = 0;
    Serial.println("泵3自动停止");
  }
}

// ==================== 蜂鸣器超时 ====================
void checkBuzzer() {
  if (buzzerDur > 0 && millis() - tBuzzer >= buzzerDur) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerDur = 0;
  }
}

// ==================== 接收Modbus命令 ====================
void modbusLoop() {
  if (!RS485_SERIAL.available()) return;

  // 至少收到完整一帧(最小4字节: 地址+功能码+2字节CRC)
  // 简单处理: 等数据稳定再读
  delay(10);  // 等数据收完

  uint8_t buf[64];
  int idx = 0;
  while (RS485_SERIAL.available() && idx < 64) {
    buf[idx++] = RS485_SERIAL.read();
  }

  if (idx < 4) return;

  // 不是发给我的地址 不理
  if (buf[0] != myAddr) return;

  // 验CRC
  uint16_t gotCrc = buf[idx-1] << 8 | buf[idx-2];
  if (crc16(buf, idx-2) != gotCrc) {
    Serial.println("CRC错误");
    return;
  }

  byte func = buf[1];

  switch (func) {
    case 0x03:  // 读寄存器
      handleRead(buf, idx);
      break;
    case 0x06:  // 写单寄存器
      handleWrite(buf, idx);
      break;
    default:
      replyError(func, 0x01);  // 不支持的功能码
      break;
  }
}

// ==================== 安全看门狗 ====================
// 如果超过一定时间没收到主机命令，关掉所有泵 (防止失控)
unsigned long tLastCmd = 0;
#define WATCHDOG_TIMEOUT 120000  // 2分钟没命令就停

void watchdogCheck() {
  if (millis() - tLastCmd > WATCHDOG_TIMEOUT) {
    if (pump1Dur > 0) { ledcWrite(PUMP1_PIN, 0); pump1Dur = 0; }
    if (pump2Dur > 0) { ledcWrite(PUMP2_PIN, 0); pump2Dur = 0; }
    if (pump3Dur > 0) { ledcWrite(PUMP3_PIN, 0); pump3Dur = 0; }
    // 灯保持当前值 不关
  }
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== 植物养护从机 地址:" + String(myAddr) + " =====");

  // RS485串口
  RS485_SERIAL.begin(9600, SERIAL_8N1, 16, 17);

  // RS485方向控制
  pinMode(RS485_CTRL, OUTPUT);
  digitalWrite(RS485_CTRL, LOW);  // 默认接收

  // 模拟输入
  analogReadResolution(12);  // ESP32是12位ADC
  pinMode(LIGHT_PIN, INPUT);
  pinMode(SOIL_PIN, INPUT);

  // PWM初始化
  ledcAttach(PUMP1_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(PUMP2_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(PUMP3_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(LIGHT_LED, PWM_FREQ, PWM_RES);

  // 确保泵初始是停的
  ledcWrite(PUMP1_PIN, 0);
  ledcWrite(PUMP2_PIN, 0);
  ledcWrite(PUMP3_PIN, 0);
  ledcWrite(LIGHT_LED, 0);

  // 蜂鸣器
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // 按键
  pinMode(BTN_PIN, INPUT_PULLUP);

  // 首次读传感器
  readSensors();

  // 开机蜂鸣一下
  digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);

  tLastCmd = millis();
  Serial.println("初始化完成\n");
}

// ==================== loop ====================
void loop() {
  modbusLoop();       // 处理RS485命令
  checkPumps();       // 泵超时停止
  checkBuzzer();      // 蜂鸣器超时
  watchdogCheck();    // 安全看门狗

  // 定期读取传感器 (每秒)
  static unsigned long tRead = 0;
  if (millis() - tRead >= 1000) {
    tRead = millis();
    readSensors();
  }

  // 收到命令刷新看门狗
  if (RS485_SERIAL.available()) {
    tLastCmd = millis();
  }

  delay(5);
}
