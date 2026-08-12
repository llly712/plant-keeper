/*
  智能植物养护系统 - 国赛版
  主机 ESP32-S3
  负责：OV2640拍照 + TFLite病虫害识别 + RS485主控 + OLED + 舵机旋转
  从机通过RS485听令干活（浇水施肥补光）
*/

// ==================== 头文件 ====================
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <DHT.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// AI库 - 用TensorFlow Lite Micro在芯片上跑模型
#include <TensorFlowLite_ESP32.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "plant_model.h"  // 你的AI模型（训练好后替换这个文件）

// ==================== 摄像头引脚 (OV2640) ====================
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_VSYNC 6
#define CAM_HREF  9
#define CAM_XCLK  10
#define CAM_PCLK  13
#define CAM_D2    11
#define CAM_D3    12
#define CAM_D4    14
#define CAM_D5    15
#define CAM_D6    1
#define CAM_D7    39
#define CAM_D8    40
#define CAM_D9    8

// ==================== 主机外设引脚 ====================
#define DHTPIN     48
#define OLED_SDA   7
#define OLED_SCL   18
#define BUZZER     21
#define LED_BUILT  19
#define SERVO_PIN  20    // SG90 360°舵机 控制摄像头转
#define BTN_PIN    2     // 按键切换OLED页面
#define RELAY1     41    // 继电器K1
#define RELAY2     42    // 继电器K2

// RS485
#define RXD2       16
#define TXD2       17
#define RS485_CTRL 38    // MAX485方向脚 DE+RE接一起

// ==================== WiFi & 服务器 ====================
const char* ssid = "你的WiFi";
const char* pass = "你的密码";
const char* upUrl = "http://192.168.1.100:3000/api/upload";
const char* cfgUrl = "http://192.168.1.100:3000/api/config";

bool wifiOk = false;
bool timeOk = false;
struct tm tnow;

// ==================== 全局对象 ====================
DHT dht(DHTPIN, DHT21);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
Servo myServo;

// ==================== 传感器数据 ====================
float airTemp = 0, airHum = 0;
float soilTemp = 0, soilHum = 0;  // 来自RS485三合一传感器
int soilEC = 0;
int lightVal = 0;     // 从机光照
int soilAnalog = 0;   // 从机模拟土壤湿度

// ==================== AI结果 ====================
struct {
  float probs[5];     // [健康,白粉病,叶斑病,锈病,虫害] 概率
  int bestClass;      // 概率最高的类别
  float bestConf;     // 最高概率值
  int healthScore;    // 健康分 0-100
  bool aiOk;          // AI初始化是否成功
} ai;

// ==================== 植物状态 ====================
int plantStage = 1;   // 0幼苗 1生长 2开花 3结果
int plantAgeDays = 30;
const char* stageName[] = {"幼苗期","生长期","开花期","结果期"};

// ==================== 远程配置(可下发修改) ====================
int cfgSoilMin = 1800, cfgSoilMax = 2500;
int cfgEcMin = 150, cfgEcMax = 300;
int cfgWaterCd = 5, cfgFertCd = 60;

// ==================== Modbus地址 ====================
const byte SLAVE1 = 0x02;       // 从机1号地址
const byte SOIL_SENSOR = 0x01;  // 土壤三合一传感器地址

// ==================== 从机状态缓存 ====================
bool sPump1 = false, sPump2 = false, sPump3 = false;
int sLightPwm = 0;
bool sBuzzing = false;

// ==================== 舵机位置 ====================
int servoPos = 90;  // 90=中间

// ==================== 定时 ====================
unsigned long tSensor = 0, tPhoto = 0, tUpload = 0;
unsigned long tCfg = 0, tDisp = 0, tServo = 0, tBeep = 0;
// 正式版时间间隔，调试时改小
int sensorGap = 900000;   // 15分钟
int photoGap = 86400000;  // 24小时（调试改成10000 就是10秒）
int cfgGap = 3600000;     // 1小时拉配置
int uploadGap = 3600000;  // 1小时上传

// ==================== 摄像头 ====================
bool camInitOk = false;

// ==================== OLED页面 ====================
int oledPage = 0;  // 0=主界面 1=从机状态 2=AI详情

// ==================== 历史 ====================
float healthHist[30];
int healthIdx = 0;
int imgCnt = 0;  // 拍照计数

// ========== 调试开关 ==========
// 正式比赛改成false
bool debugMode = true;

// ==================== Modbus CRC16 ====================
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

// ==================== RS485 发送Modbus请求 ====================
// 返回响应数据长度，0=超时或失败
int rs485Ask(byte addr, byte func, uint16_t reg, uint16_t cnt,
             uint8_t* respBuf, int maxLen, int timeoutMs) {
  uint8_t req[8];
  req[0] = addr;
  req[1] = func;
  req[2] = (reg >> 8) & 0xFF;
  req[3] = reg & 0xFF;
  req[4] = (cnt >> 8) & 0xFF;
  req[5] = cnt & 0xFF;
  uint16_t crc = crc16(req, 6);
  req[6] = crc & 0xFF;
  req[7] = (crc >> 8) & 0xFF;

  // 切换到发送模式
  digitalWrite(RS485_CTRL, HIGH);
  delayMicroseconds(200);
  Serial2.write(req, 8);
  Serial2.flush();
  delayMicroseconds(500);
  digitalWrite(RS485_CTRL, LOW);

  // 等回复
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs && idx < maxLen) {
    if (Serial2.available()) {
      respBuf[idx++] = Serial2.read();
      t0 = millis(); // 每收到一个字节刷新超时
    }
  }
  if (idx < 4) return 0;

  // 验一下CRC
  uint16_t gotCrc = respBuf[idx-1] << 8 | respBuf[idx-2];
  if (crc16(respBuf, idx-2) != gotCrc) return 0;

  return idx;
}

// ==================== 写单个寄存器 (功能码06 用于控制从机) ====================
bool rs485Write(byte addr, uint16_t reg, uint16_t val) {
  uint8_t req[8];
  req[0] = addr;
  req[1] = 0x06;
  req[2] = (reg >> 8) & 0xFF;
  req[3] = reg & 0xFF;
  req[4] = (val >> 8) & 0xFF;
  req[5] = val & 0xFF;
  uint16_t crc = crc16(req, 6);
  req[6] = crc & 0xFF;
  req[7] = (crc >> 8) & 0xFF;

  digitalWrite(RS485_CTRL, HIGH);
  delayMicroseconds(200);
  Serial2.write(req, 8);
  Serial2.flush();
  delayMicroseconds(500);
  digitalWrite(RS485_CTRL, LOW);

  // 简单等个响应确认下（正常会回一样的）
  uint8_t resp[8];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 100 && idx < 8) {
    if (Serial2.available()) resp[idx++] = Serial2.read();
  }
  return idx >= 6;
}

// ==================== 读取从机传感器 ====================
bool readSlave() {
  uint8_t buf[32];
  // 读10个寄存器 (0x0000-0x0009)
  int len = rs485Ask(SLAVE1, 0x03, 0x0000, 10, buf, 32, 500);
  if (len < 5) return false;

  int byteCnt = buf[2];  // 数据字节数
  if (byteCnt < 20) return false;

  // 解析 (从buf[3]开始)
  lightVal = (buf[3] << 8) | buf[4];
  soilAnalog = (buf[5] << 8) | buf[6];
  sPump1 = buf[7] << 8 | buf[8];
  sPump2 = buf[9] << 8 | buf[10];
  sPump3 = buf[11] << 8 | buf[12];
  sLightPwm = buf[13] << 8 | buf[14];
  // buf[15..19] 其他状态

  return true;
}

// ==================== 读取土壤三合一传感器 ====================
bool readSoilSensor() {
  uint8_t buf[16];
  // 读3个寄存器: 0004湿度 0005温度 0006电导率
  int len = rs485Ask(SOIL_SENSOR, 0x03, 0x0004, 3, buf, 16, 500);
  if (len < 7) return false;

  int rawHum = (buf[3] << 8) | buf[4];
  int rawTemp = (buf[5] << 8) | buf[6];
  int rawEC = (buf[7] << 8) | buf[8];

  soilHum = rawHum / 10.0;   // 文档说÷10得%
  soilTemp = rawTemp / 10.0;
  soilEC = rawEC;            // μS/cm 读数即是

  return true;
}

// ==================== 控制从机水泵 ====================
void slavePump(int pumpNum, int seconds) {
  uint16_t reg;
  if (pumpNum == 1) reg = 0x0007;
  else if (pumpNum == 2) reg = 0x0008;
  else reg = 0x0009;

  rs485Write(SLAVE1, reg, seconds);
}

// ==================== 设置从机补光灯 ====================
void slaveLight(int brightness) {
  brightness = constrain(brightness, 0, 255);
  rs485Write(SLAVE1, 0x0005, brightness);
}

// ==================== 从机蜂鸣器 ====================
void slaveBeep(int mode) {
  rs485Write(SLAVE1, 0x0006, mode);
}

// ==================== 本地蜂鸣器 ====================
void beep(int ms, int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(ms);
    digitalWrite(BUZZER, LOW);
    if (i < times - 1) delay(80);
  }
}

// ==================== WiFi ====================
void connWiFi() {
  WiFi.begin(ssid, pass);
  int n = 0;
  while (WiFi.status() != WL_CONNECTED && n < 40) {
    delay(500);
    Serial.print(".");
    n++;
  }
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    Serial.println("\nWiFi OK");
    digitalWrite(LED_BUILT, HIGH);
    beep(80, 2);
  } else {
    Serial.println("\nWiFi fail");
  }
}

// ==================== NTP对时 ====================
void syncTime() {
  configTime(8 * 3600, 0, "pool.ntp.org", "ntp.aliyun.com");
  int retry = 0;
  while (!getLocalTime(&tnow) && retry < 20) {
    delay(500);
    retry++;
  }
  timeOk = (retry < 20);
  if (timeOk) Serial.println("Time OK");
}

// ==================== 获取时间字符串 ====================
String timeStr() {
  if (!timeOk) return "----";
  getLocalTime(&tnow);
  char s[20];
  sprintf(s, "%02d:%02d:%02d", tnow.tm_hour, tnow.tm_min, tnow.tm_sec);
  return String(s);
}

String dateStr() {
  if (!timeOk) return "----";
  getLocalTime(&tnow);
  char s[30];
  sprintf(s, "%04d-%02d-%02d %02d:%02d:%02d",
          tnow.tm_year + 1900, tnow.tm_mon + 1, tnow.tm_mday,
          tnow.tm_hour, tnow.tm_min, tnow.tm_sec);
  return String(s);
}

// ==================== 读AM2301 ====================
void readAM2301() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    airHum = h;
    airTemp = t;
  }
}

// ==================== 摄像头初始化 ====================
void initCam() {
  camera_config_t cfg;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = CAM_D2;
  cfg.pin_d1 = CAM_D3;
  cfg.pin_d2 = CAM_D4;
  cfg.pin_d3 = CAM_D5;
  cfg.pin_d4 = CAM_D6;
  cfg.pin_d5 = CAM_D7;
  cfg.pin_d6 = CAM_D8;
  cfg.pin_d7 = CAM_D9;
  cfg.pin_xclk = CAM_XCLK;
  cfg.pin_pclk = CAM_PCLK;
  cfg.pin_vsync = CAM_VSYNC;
  cfg.pin_href = CAM_HREF;
  cfg.pin_sscb_sda = CAM_SIOD;
  cfg.pin_sscb_scl = CAM_SIOC;
  cfg.pin_pwdn = -1;
  cfg.pin_reset = -1;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = FRAMESIZE_QVGA;  // 320x240
  cfg.jpeg_quality = 8;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&cfg);
  camInitOk = (err == ESP_OK);
  if (camInitOk) {
    Serial.println("Cam OK");
  } else {
    Serial.printf("Cam fail: 0x%x\n", err);
  }
}

// ==================== 舵机控制 (360°舵机) ====================
// 360°舵机靠脉冲宽度控制转速和方向
// 1500us=停 700us=全速左 2300us=全速右
void servoStop() {
  myServo.writeMicroseconds(1500);
}

void servoLeft(int ms) {
  myServo.writeMicroseconds(700);
  delay(ms);
  myServo.writeMicroseconds(1500);
}

void servoRight(int ms) {
  myServo.writeMicroseconds(2300);
  delay(ms);
  myServo.writeMicroseconds(1500);
}

// 转到某个位置 (大概位置 360°舵机没法精确定位)
void servoTo(int pos) {
  int diff = pos - servoPos;
  if (diff > 10) {
    servoRight(map(diff, 10, 90, 80, 600));
  } else if (diff < -10) {
    servoLeft(map(-diff, 10, 90, 80, 600));
  }
  servoPos = pos;
}

// ==================== 拍照 ====================
camera_fb_t* takePhoto() {
  if (!camInitOk) return NULL;

  // 拍照前先把补光灯打开保证画质
  slaveLight(255);
  delay(400);

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("拍照失败");
    slaveLight(sLightPwm > 0 ? sLightPwm : 0);
    return NULL;
  }

  imgCnt++;
  Serial.printf("照片 %d: %d bytes\n", imgCnt, fb->len);
  return fb;
}

// ==================== AI 初始化 (TensorFlow Lite) ====================
namespace {
  const tflite::Model* tfModel = nullptr;
  tflite::MicroInterpreter* tfInterp = nullptr;
  TfLiteTensor* tfInput = nullptr;
  TfLiteTensor* tfOutput = nullptr;
  uint8_t* tfArena = nullptr;
}

bool aiInit() {
  // 在PSRAM里分配tensor内存
  tfArena = (uint8_t*)ps_malloc(350 * 1024);
  if (!tfArena) {
    Serial.println("AI: PSRAM不够");
    return false;
  }

  tfModel = tflite::GetModel(plant_model);
  if (tfModel->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("AI: 模型版本不对");
    return false;
  }

  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter interp(
    tfModel, resolver, tfArena, 350 * 1024);

  tfInterp = &interp;
  if (tfInterp->AllocateTensors() != kTfLiteOk) {
    Serial.println("AI: 分配tensor失败");
    return false;
  }

  tfInput = tfInterp->input(0);
  tfOutput = tfInterp->output(0);

  Serial.println("AI: 模型加载OK");
  Serial.printf("  输入: %dx%dx%d\n", tfInput->dims->data[1],
                tfInput->dims->data[2], tfInput->dims->data[3]);
  Serial.printf("  输出: %d类\n", tfOutput->dims->data[1]);
  return true;
}

// ==================== AI推理 ====================
bool aiRun(camera_fb_t* fb) {
  if (!fb || !tfInterp) return false;
  int inW = tfInput->dims->data[1];   // 模型输入宽 比如96
  int inH = tfInput->dims->data[2];   // 模型输入高

  // JPEG转RGB888
  uint8_t* rgb = (uint8_t*)ps_malloc(320 * 240 * 3);
  if (!rgb) return false;
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb);
  if (!ok) { free(rgb); return false; }

  // 中心裁剪320x240 → min(W,H)×min(W,H) 然后缩放到模型大小
  int cropSz = 240;  // 用高作为正方形边长
  int cropX = (320 - cropSz) / 2;

  uint8_t* inputData = tfInput->data.uint8;
  for (int y = 0; y < inH; y++) {
    for (int x = 0; x < inW; x++) {
      int sx = cropX + (x * cropSz / inW);
      int sy = y * cropSz / inH;
      int si = (sy * 320 + sx) * 3;
      int di = (y * inW + x) * 3;
      inputData[di + 0] = rgb[si + 0];
      inputData[di + 1] = rgb[si + 1];
      inputData[di + 2] = rgb[si + 2];
    }
  }
  free(rgb);

  // 跑推理
  TfLiteStatus s = tfInterp->Invoke();
  if (s != kTfLiteOk) return false;

  float* outData = tfOutput->data.f;
  int nClass = tfOutput->dims->data[1];  // 类别数

  // 提取结果
  ai.bestClass = 0;
  ai.bestConf = outData[0];
  for (int i = 0; i < nClass && i < 5; i++) {
    ai.probs[i] = outData[i];
    if (outData[i] > ai.bestConf) {
      ai.bestConf = outData[i];
      ai.bestClass = i;
    }
  }

  // 健康分: [第0类是健康] 的概率×100
  ai.healthScore = (int)(ai.probs[0] * 100);
  if (ai.healthScore > 100) ai.healthScore = 100;
  if (ai.healthScore < 0) ai.healthScore = 0;

  // 记录历史
  healthHist[healthIdx % 30] = ai.healthScore;
  healthIdx++;

  return true;
}

// ==================== 无AI时的备用评估 (老版HSV方法) ====================
int backupHealth(camera_fb_t* fb) {
  if (!fb) return 50;

  uint16_t* buf16 = (uint16_t*)ps_malloc(320 * 240 * 2);
  if (!buf16) return 50;

  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, (uint8_t*)buf16);
  if (!ok) { free(buf16); return 50; }

  int green = 0, yellow = 0, red = 0, dark = 0;
  int total = 320 * 240;

  for (int i = 0; i < total; i++) {
    uint16_t p = buf16[i];
    int r = ((p >> 11) & 0x1F) << 3;
    int g = ((p >> 5) & 0x3F) << 2;
    int b = (p & 0x1F) << 3;

    int mn = min(r, min(g, b));
    int mx = max(r, max(g, b));
    int d = mx - mn;
    float s = mx == 0 ? 0 : (float)d / mx * 100;
    float v = (float)mx / 255 * 100;

    if (g > 130 && s > 30 && v > 30) {
      green += 3;
    } else if (v < 25 || s < 15) {
      dark += 2;
    } else if (r > g + 30 && r > b + 30) {
      red += 3;
    } else {
      yellow += 1;
    }
  }
  free(buf16);

  int sum = green + yellow + red + dark;
  if (sum == 0) return 50;
  float sc = (green * 1.0 + yellow * 0.5 + red * 0.2 + dark * 0.0) / sum * 100;
  return constrain((int)sc, 0, 100);
}

// ==================== 多角度拍照+AI分析 ====================
void multiAngleCheck() {
  if (!camInitOk) return;

  Serial.println("===== 多角度拍照开始 =====");

  // 记录初始位置
  int origPos = servoPos;

  // 位置1: 中间
  servoTo(90);
  delay(500);
  camera_fb_t* fb1 = takePhoto();
  if (fb1) {
    if (ai.aiOk) aiRun(fb1);
    else ai.healthScore = backupHealth(fb1);
    esp_camera_fb_return(fb1);
  }

  // 位置2: 右边
  servoTo(130);
  delay(500);
  camera_fb_t* fb2 = takePhoto();
  if (fb2) {
    int h2;
    if (ai.aiOk) {
      aiRun(fb2);
      h2 = ai.healthScore;
    } else {
      h2 = backupHealth(fb2);
    }
    // 取最差角度作为评估（保守策略）
    if (h2 < ai.healthScore) ai.healthScore = h2;
    esp_camera_fb_return(fb2);
  }

  // 位置3: 左边
  servoTo(50);
  delay(500);
  camera_fb_t* fb3 = takePhoto();
  if (fb3) {
    int h3;
    if (ai.aiOk) {
      aiRun(fb3);
      h3 = ai.healthScore;
    } else {
      h3 = backupHealth(fb3);
    }
    if (h3 < ai.healthScore) ai.healthScore = h3;
    esp_camera_fb_return(fb3);
    // 用最后一张上传（也可以用第一张）
  }

  // 回到原位
  servoTo(origPos);
  slaveLight(sLightPwm > 0 ? sLightPwm : 0);
  Serial.printf("多角度完成 健康分:%d\n", ai.healthScore);
}

// ==================== 单角度拍照(用于上传) ====================
camera_fb_t* photoForUpload() {
  if (!camInitOk) return NULL;
  slaveLight(255);
  delay(400);
  camera_fb_t* fb = esp_camera_fb_get();
  // 先不关灯，上传完再关
  return fb;
}

// ==================== 上传数据到服务器 ====================
void uploadData(camera_fb_t* fb) {
  if (!wifiOk) return;

  HTTPClient http;
  http.begin(upUrl);
  http.setTimeout(30000);

  String boundary = "----PlantKeeper" + String(millis());

  if (fb) {
    // 带图片上传 multipart
    String ct = "multipart/form-data; boundary=" + boundary;
    http.addHeader("Content-Type", ct);

    String p1 = "--" + boundary + "\r\n";
    p1 += "Content-Disposition: form-data; name=\"image\"; filename=\"plant.jpg\"\r\n";
    p1 += "Content-Type: image/jpeg\r\n\r\n";

    String p2 = "\r\n--" + boundary + "\r\n";
    p2 += "Content-Disposition: form-data; name=\"data\"\r\n\r\n";

    // JSON数据
    String json = "{";
    json += "\"temp\":" + String(airTemp) + ",";
    json += "\"humidity\":" + String(airHum) + ",";
    json += "\"light\":" + String(lightVal) + ",";
    json += "\"soil\":" + String(soilAnalog) + ",";
    json += "\"soil_temp\":" + String(soilTemp) + ",";
    json += "\"soil_hum\":" + String(soilHum) + ",";
    json += "\"ec\":" + String(soilEC) + ",";
    json += "\"health\":" + String(ai.healthScore) + ",";
    json += "\"ai_class\":" + String(ai.bestClass) + ",";
    json += "\"ai_conf\":" + String(ai.bestConf) + ",";
    json += "\"stage\":" + String(plantStage) + ",";
    json += "\"stage_name\":\"" + String(stageName[plantStage]) + "\",";
    json += "\"time\":\"" + dateStr() + "\"";
    json += "}";

    String p3 = "\r\n--" + boundary + "--\r\n";

    size_t total = p1.length() + fb->len + p2.length() + json.length() + p3.length();
    http.addHeader("Content-Length", String(total));

    WiFiClient* stream = http.getStreamPtr();
    stream->print(p1);
    // 分块传图片
    size_t left = fb->len;
    uint8_t* ptr = fb->buf;
    while (left > 0) {
      size_t chunk = left > 1024 ? 1024 : left;
      stream->write(ptr, chunk);
      ptr += chunk;
      left -= chunk;
      delay(5);
    }
    stream->print(p2);
    stream->print(json);
    stream->print(p3);

    int code = http.sendRequest("POST");
    Serial.printf("上传(带图):%d\n", code);
  } else {
    // 纯JSON上传
    http.addHeader("Content-Type", "application/json");
    String json = "{";
    json += "\"temp\":" + String(airTemp) + ",";
    json += "\"humidity\":" + String(airHum) + ",";
    json += "\"light\":" + String(lightVal) + ",";
    json += "\"soil\":" + String(soilAnalog) + ",";
    json += "\"soil_temp\":" + String(soilTemp) + ",";
    json += "\"soil_hum\":" + String(soilHum) + ",";
    json += "\"ec\":" + String(soilEC) + ",";
    json += "\"health\":" + String(ai.healthScore) + ",";
    json += "\"ai_class\":" + String(ai.bestClass) + ",";
    json += "\"stage\":" + String(plantStage) + ",";
    json += "\"stage_name\":\"" + String(stageName[plantStage]) + "\",";
    json += "\"time\":\"" + dateStr() + "\"";
    json += "}";
    int code = http.POST(json);
    Serial.printf("上传(纯数据):%d\n", code);
  }
  http.end();
  slaveLight(sLightPwm > 0 ? sLightPwm : 0);
}

// ==================== 拉远程配置 ====================
void fetchConfig() {
  if (!wifiOk) return;

  HTTPClient http;
  http.begin(cfgUrl);
  http.setTimeout(5000);
  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String body = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      plantAgeDays = doc["plant_age_days"] | plantAgeDays;
      cfgSoilMin = doc["soil_min"] | cfgSoilMin;
      cfgSoilMax = doc["soil_max"] | cfgSoilMax;
      cfgEcMin = doc["ec_min"] | cfgEcMin;
      cfgEcMax = doc["ec_max"] | cfgEcMax;
      cfgWaterCd = doc["water_cd"] | cfgWaterCd;
      cfgFertCd = doc["fert_cd"] | cfgFertCd;
      Serial.println("配置更新OK");
    }
  }
  http.end();
}

// ==================== 决策：判断要不要浇水施肥 ====================
void makeDecision() {
  // 获取当前阶段的EC目标
  int ecMinNow = cfgEcMin;
  int ecMaxNow = cfgEcMax;
  switch (plantStage) {
    case 0: ecMinNow *= 0.6; ecMaxNow *= 0.6; break;
    case 2: ecMinNow *= 1.2; ecMaxNow *= 1.2; break;
    case 3: ecMinNow *= 1.0; ecMaxNow *= 1.0; break;
    default: break;
  }

  // === 浇水判断 ===
  // 结合土壤湿度(从机模拟)和土壤湿度(RS485传感器)
  bool needWater = false;
  if (soilAnalog > 0 && soilAnalog < cfgSoilMin) needWater = true;

  // AI辅助调整：如果检测到病害(真菌类) 土壤太湿会加重
  // 白粉病(1)、锈病(3)在潮湿环境易发 适当降低浇水频率
  bool dampDisease = (ai.bestClass == 1 || ai.bestClass == 3) && ai.bestConf > 0.6;
  if (dampDisease && soilAnalog > cfgSoilMin * 0.8) {
    needWater = false;  // 有真菌病且土壤还不算太干 先不浇
  }

  if (needWater && !sPump1) {
    int dur = 5;  // 浇水5秒
    // 如果植物健康度低 说明可能根系有问题 少浇点
    if (ai.healthScore < 40) dur = 2;
    slavePump(1, dur);
    Serial.printf("浇水 %ds (健康分:%d)\n", dur, ai.healthScore);
    beep(100, 2);
  }

  // === 施肥判断 ===
  if (soilEC > 0 && soilEC < ecMinNow && !sPump2 && !sPump3) {
    int diff = ecMinNow - soilEC;

    // AI辅助: 如果健康分很低，先别施肥（可能是病害不是缺肥）
    if (ai.healthScore < 30 && diff < 100) {
      Serial.println("健康分低 暂不施肥");
    } else {
      int dur = 3;
      if (ai.healthScore < 50) dur = 1;  // 减量

      // 幼苗/生长期用泵2(氮肥为主)  开花结果用泵3(磷钾为主)
      if (plantStage <= 1) {
        slavePump(2, dur);
        Serial.printf("施肥A %ds\n", dur);
      } else {
        slavePump(3, dur);
        Serial.printf("施肥B %ds\n", dur);
      }
      beep(100, 2);
    }
  }

  // EC过高紧急处理
  if (soilEC > 600 && !sPump1) {
    slavePump(1, 8);  // 大量浇水稀释
    Serial.println("EC过高! 稀释浇水");
    beep(200, 4);
  }
}

// ==================== 判断生长阶段 (AI+规则混合) ====================
void updateStage() {
  // AI辅助: 如果健康>80 且拍照看叶片茂盛 可能是生长或开花期
  float score[4] = {0, 0, 0, 0};

  // 日龄
  if (plantAgeDays < 15) {
    score[0] += 25; score[1] += 5;
  } else if (plantAgeDays < 30) {
    score[1] += 25; score[2] += 8;
  } else if (plantAgeDays < 50) {
    score[2] += 25; score[3] += 10;
  } else {
    score[3] += 25; score[2] += 10;
  }

  // AI健康分
  if (ai.healthScore > 70) {
    score[2] += 10; score[3] += 8;  // 健康的可能是后期
  } else if (ai.healthScore > 40) {
    score[1] += 10;
  } else {
    score[0] += 8;  // 不太健康可能是幼苗需要多照顾
  }

  // EC值
  if (soilEC < cfgEcMin * 0.5) score[0] += 10;
  else if (soilEC >= cfgEcMin && soilEC <= cfgEcMax) score[1] += 12;
  else if (soilEC > cfgEcMax && soilEC <= cfgEcMax * 1.4) score[2] += 10;
  else if (soilEC > cfgEcMax * 0.8) score[3] += 8;

  // 找最高分
  int best = 0;
  for (int i = 1; i < 4; i++) {
    if (score[i] > score[best]) best = i;
  }
  plantStage = best;
}

// ==================== OLED显示 ====================
void drawOLED() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);

  if (oledPage == 0) {
    // ----- 主页 -----
    // 时间
    u8g2.setCursor(0, 10);
    if (timeOk) {
      getLocalTime(&tnow);
      u8g2.printf("%02d:%02d", tnow.tm_hour, tnow.tm_min);
    } else {
      u8g2.print("--:--");
    }

    // 生长阶段
    u8g2.setCursor(50, 10);
    u8g2.print(stageName[plantStage]);

    // WiFi状态
    u8g2.setCursor(110, 10);
    u8g2.print(wifiOk ? "W" : "x");

    // 健康分 + AI标识
    u8g2.setCursor(0, 25);
    u8g2.print("AI健康:");
    u8g2.print(ai.healthScore);
    u8g2.print("%");

    // 病害提示
    if (ai.bestClass > 0 && ai.bestConf > 0.5) {
      const char* disease[] = {"", "白粉病","叶斑病","锈病","虫害"};
      u8g2.setCursor(0, 38);
      u8g2.print("⚠");
      u8g2.print(disease[ai.bestClass]);
      u8g2.print(" ");
      u8g2.print((int)(ai.bestConf * 100));
      u8g2.print("%");
    }

    // 环境数据
    u8g2.setCursor(0, 52);
    u8g2.printf("T:%.1f H:%.0f%%", airTemp, airHum);

    u8g2.setCursor(0, 63);
    u8g2.printf("S:%d EC:%d L:%d", soilAnalog, soilEC, lightVal);
  } else if (oledPage == 1) {
    // ----- 从机状态页 -----
    u8g2.setCursor(0, 10);
    u8g2.print("-- 从机状态 --");

    u8g2.setCursor(0, 25);
    u8g2.printf("泵1:%s 泵2:%s", sPump1 ? "ON " : "OFF", sPump2 ? "ON " : "OFF");

    u8g2.setCursor(0, 38);
    u8g2.printf("泵3:%s 灯:%d", sPump3 ? "ON " : "OFF", sLightPwm);

    u8g2.setCursor(0, 52);
    u8g2.printf("土壤(从):%d", soilAnalog);

    u8g2.setCursor(0, 63);
    u8g2.printf("soilT:%.1f EC:%d", soilTemp, soilEC);
  } else {
    // ----- AI详情页 -----
    u8g2.setCursor(0, 10);
    u8g2.print("== AI 详情 ==");

    const char* clsName[] = {"健康","白粉病","叶斑病","锈病","虫害"};
    for (int i = 0; i < 5; i++) {
      int y = 22 + i * 8;
      u8g2.setCursor(0, y);
      u8g2.printf("%s:%.0f%%", clsName[i], ai.probs[i] * 100);
    }

    u8g2.setCursor(0, 63);
    u8g2.printf("拍照:%d张", imgCnt);
  }

  u8g2.sendBuffer();
}

// ==================== 按键处理 ====================
void checkBtn() {
  static unsigned long lastPress = 0;
  if (digitalRead(BTN_PIN) == LOW) {
    delay(30);  // 消抖
    if (digitalRead(BTN_PIN) == LOW && millis() - lastPress > 300) {
      oledPage = (oledPage + 1) % 3;
      lastPress = millis();
      beep(30, 1);
    }
  }
}

// ==================== 启动时自检 ====================
void selfTest() {
  Serial.println("=== 自检开始 ===");
  // 蜂鸣器
  digitalWrite(BUZZER, HIGH); delay(200); digitalWrite(BUZZER, LOW);
  // LED
  digitalWrite(LED_BUILT, LOW); delay(200); digitalWrite(LED_BUILT, HIGH);
  // 舵机微动
  servoLeft(100); delay(200); servoRight(100); delay(200); servoStop();
  // 继电器咔哒一下
  digitalWrite(RELAY1, HIGH); delay(200); digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, HIGH); delay(200); digitalWrite(RELAY2, LOW);
  Serial.println("=== 自检完成 ===");
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== 植物养护主机 国赛版 =====");
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // 引脚初始化
  pinMode(BUZZER, OUTPUT);
  pinMode(LED_BUILT, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(LED_BUILT, LOW);
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);

  // RS485
  pinMode(RS485_CTRL, OUTPUT);
  digitalWrite(RS485_CTRL, LOW);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  // AM2301
  dht.begin();

  // OLED
  u8g2.begin();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.clearBuffer();
  u8g2.setCursor(0, 30);
  u8g2.print("系统启动中...");
  u8g2.sendBuffer();

  // 舵机
  myServo.attach(SERVO_PIN);
  servoStop();

  // 摄像头
  initCam();

  // AI模型
  ai.aiOk = aiInit();

  // WiFi
  connWiFi();
  if (wifiOk) {
    syncTime();
    fetchConfig();
  }

  // 自检
  selfTest();

  // 第一次读传感器
  readAM2301();
  readSlave();
  readSoilSensor();

  // 第一次拍照+AI
  if (camInitOk) {
    camera_fb_t* fb = takePhoto();
    if (fb) {
      if (ai.aiOk) aiRun(fb);
      else ai.healthScore = backupHealth(fb);
      esp_camera_fb_return(fb);
    }
    slaveLight(0);
  }

  // 定时器初始化
  tSensor = tPhoto = tUpload = tCfg = millis();

  beep(300, 2);
  Serial.println("初始化完成!\n");
}

// ==================== loop ====================
void loop() {
  unsigned long now = millis();

  // --- 读传感器+控制决策 ---
  if (now - tSensor >= sensorGap) {
    tSensor = now;
    readAM2301();
    readSlave();
    readSoilSensor();
    updateStage();
    makeDecision();
  }

  // --- 拍照+AI ---
  if (now - tPhoto >= photoGap) {
    tPhoto = now;
    if (camInitOk) {
      multiAngleCheck();           // 多角度拍照+AI
      camera_fb_t* fb = photoForUpload();
      if (wifiOk) {
        uploadData(fb);
      }
      if (fb) {
        if (ai.aiOk) aiRun(fb);    // 上传那张也可以跑下AI
        esp_camera_fb_return(fb);
      }
      slaveLight(sLightPwm > 0 ? sLightPwm : 0);
    }
  }

  // --- 定期上传(没有新照片时) ---
  if (now - tUpload >= uploadGap && wifiOk) {
    tUpload = now;
    uploadData(NULL);  // 纯数据上传
  }

  // --- 拉配置 ---
  if (now - tCfg >= cfgGap && wifiOk) {
    tCfg = now;
    fetchConfig();
  }

  // --- 按键 ---
  checkBtn();

  // --- OLED刷新 ---
  if (now - tDisp >= 2000) {
    tDisp = now;
    drawOLED();
  }

  // --- 调试模式下快速循环 ---
  if (debugMode) {
    photoGap = 15000;    // 15秒拍一次
    sensorGap = 30000;   // 30秒读一次
    uploadGap = 60000;   // 1分钟传一次
    cfgGap = 60000;
  }

  delay(10);
}
