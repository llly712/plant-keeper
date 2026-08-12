/*
  植物养护主机 ESP32-S3
  OV2640 + 本地AI + 32从机RS485
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <DHT.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "img_converters.h"
#include <tflm_esp32.h>
#include "plant_model.h"

#define CAM_SIOD   4
#define CAM_SIOC   5
#define CAM_VSYNC  6
#define CAM_HREF   9
#define CAM_XCLK   -1
#define CAM_PCLK   13
#define CAM_D0     11
#define CAM_D1     12
#define CAM_D2     14
#define CAM_D3     15
#define CAM_D4     1
#define CAM_D5     39
#define CAM_D6     40
#define CAM_D7     8
#define CAM_PWDN   3

#define DHT_PIN    48
#define OLED_SDA   7
#define OLED_SCL   18
#define BUZZER     21
#define LED_PIN    19
#define SERVO_PIN  20
#define BTN_PIN    2
#define RELAY1     41
#define RELAY2     42
#define RS485_RX   16
#define RS485_TX   17
#define RS485_DIR  38

const char* ssid = "JFD";
const char* pass = "jfd888888";
const char* upUrl = "http://8.130.43.250:9527/api/upload";
const char* cfgUrl = "http://8.130.43.250:9527/api/config";
const char* cmdUrl = "http://8.130.43.250:9527/api/device/command";

const byte firstSlave = 2;
const byte slaveTotal = 32;
struct Node {
  byte addr;
  byte soilAddr;
  bool online;
  bool soilOnline;
  uint16_t light;
  uint16_t soil;
  float soilTemp;
  float soilHum;
  uint16_t ec;
  bool pump[3];
  uint8_t lamp;
  uint8_t fail;
  uint8_t soilFail;
  unsigned long seen;
  unsigned long tried;
  unsigned long soilTried;
  unsigned long waterAt;
  unsigned long fertAt;
};

Node node[slaveTotal];
int onlineCount = 0;
int showNode = 0;

DHT dht(DHT_PIN, DHT21);
// 实物有132列显存偏移，用SH1106驱动后画面才会落在中间128列
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

float airTemp = 0, airHum = 0;
int soilMin = 1800, soilMax = 2500;
int ecMin = 150, ecMax = 300;
int waterCd = 5, fertCd = 60;
int lightMin = 1600, lightPwm = 180;
int plantAge = 30, plantStage = 1;
const char* stageName[] = {"Sdl", "Grow", "Flower", "Fruit"};

bool wifiOk = false, timeOk = false, camOk = false, aiOk = false;
bool uploadOk = false;
int uploadCode = 0;
unsigned long wifiTryAt = 0;
struct tm nowTime;
int page = 0;
unsigned long lastBtn = 0;

float aiProb[5] = {0};
int aiClass = -1;
int aiHealth = 0;
float aiConf = 0;
unsigned long aiCost = 0;
unsigned long aiAt = 0;
const char* className[] = {"Healthy", "Mildew", "LeafSpot", "Rust", "Pest"};

const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;
uint8_t* arena = nullptr;

int servoPos = 90;
bool servoReady = false;
unsigned long busLastUs = 0;

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

void waitBusQuiet() {
  unsigned long quiet = micros();
  unsigned long start = millis();
  while (millis() - start < 40) {
    bool got = false;
    while (Serial2.available()) {
      Serial2.read();
      got = true;
    }
    if (got) quiet = micros();
    if (micros() - quiet >= 4500 && micros() - busLastUs >= 4500) return;
    delayMicroseconds(100);
  }
}

void sendBus(const uint8_t* data, int n) {
  waitBusQuiet();
  digitalWrite(RS485_DIR, HIGH);
  delayMicroseconds(30);
  Serial2.write(data, n);
  Serial2.flush();
  digitalWrite(RS485_DIR, LOW);
  busLastUs = micros();
}

int findReadFrame(uint8_t* raw, int n, byte addr, byte count, uint8_t* out) {
  int need = 5 + count * 2;
  for (int p = 0; p + 5 <= n; p++) {
    if (raw[p] != addr) continue;
    if (raw[p + 1] == 0x83 && p + 5 <= n) return -1;
    if (raw[p + 1] != 0x03 || raw[p + 2] != count * 2 || p + need > n) continue;
    uint16_t got = ((uint16_t)raw[p + need - 1] << 8) | raw[p + need - 2];
    if (crc16(raw + p, need - 2) == got) {
      memcpy(out, raw + p, need);
      return need;
    }
  }
  return 0;
}

int modbusRead(byte addr, uint16_t start, uint16_t count, uint8_t* out, int outMax, int waitMs = 120) {
  uint8_t q[8] = {addr, 0x03, (uint8_t)(start >> 8), (uint8_t)start,
                  (uint8_t)(count >> 8), (uint8_t)count, 0, 0};
  uint16_t c = crc16(q, 6);
  q[6] = c & 0xFF;
  q[7] = c >> 8;

  sendBus(q, 8);

  uint8_t raw[64];
  int n = 0;
  unsigned long startMs = millis();
  while (millis() - startMs < (unsigned long)waitMs) {
    if (Serial2.available()) {
      uint8_t v = Serial2.read();
      busLastUs = micros();
      if (n < (int)sizeof(raw)) raw[n++] = v;
      int found = findReadFrame(raw, n, addr, count, out);
      if (found) return found > 0 ? found : 0;
    }
  }
  return 0;
}

bool modbusWrite(byte addr, uint16_t reg, uint16_t val) {
  uint8_t q[8] = {addr, 0x06, (uint8_t)(reg >> 8), (uint8_t)reg,
                  (uint8_t)(val >> 8), (uint8_t)val, 0, 0};
  uint16_t c = crc16(q, 6);
  q[6] = c & 0xFF;
  q[7] = c >> 8;

  sendBus(q, 8);

  uint8_t r[32];
  int n = 0;
  unsigned long t = millis();
  while (millis() - t < 150) {
    if (Serial2.available()) {
      uint8_t v = Serial2.read();
      busLastUs = micros();
      if (n < (int)sizeof(r)) r[n++] = v;
      for (int p = 0; p + 8 <= n; p++) {
        if (memcmp(q, r + p, 6) != 0) continue;
        uint16_t got = ((uint16_t)r[p + 7] << 8) | r[p + 6];
        if (crc16(r + p, 6) == got) return true;
      }
    }
  }
  return false;
}

bool readNode(int i) {
  node[i].tried = millis();
  uint8_t b[32];
  int to = node[i].online ? 50 : 80;
  int n = modbusRead(node[i].addr, 0, 10, b, sizeof(b), to);
  bool was = node[i].online;
  if (!n) {
    if (node[i].fail < 255) node[i].fail++;
    if (node[i].fail >= 3 && was) {
      node[i].online = false;
      onlineCount--;
      Serial.printf("Node %d offline\n", node[i].addr);
    }
    return false;
  }
  node[i].fail = 0;
  node[i].online = true;
  if (!was) {
    onlineCount++;
    Serial.printf("Node %d online\n", node[i].addr);
  }

  node[i].light = ((uint16_t)b[3] << 8) | b[4];
  node[i].soil = ((uint16_t)b[5] << 8) | b[6];
  for (int k = 0; k < 3; k++) {
    int p = 7 + k * 2;
    node[i].pump[k] = ((((uint16_t)b[p] << 8) | b[p + 1]) != 0);
  }
  node[i].lamp = (((uint16_t)b[13] << 8) | b[14]);
  node[i].seen = millis();
  return true;
}

bool readNodeSoil(int i) {
  node[i].soilTried = millis();
  uint8_t b[16];
  int to = node[i].soilOnline ? 120 : 150;
  int n = modbusRead(node[i].soilAddr, 0x0004, 3, b, sizeof(b), to);
  bool was = node[i].soilOnline;
  if (!n) {
    if (node[i].soilFail < 255) node[i].soilFail++;
    if (node[i].soilFail >= 3) {
      node[i].soilOnline = false;
      if (was) Serial.printf("Soil %d offline\n", node[i].soilAddr);
    }
    return false;
  }
  node[i].soilFail = 0;
  node[i].soilOnline = true;
  node[i].soilHum = (((uint16_t)b[3] << 8) | b[4]) / 10.0f;
  node[i].soilTemp = (int16_t)(((uint16_t)b[5] << 8) | b[6]) / 10.0f;
  node[i].ec = ((uint16_t)b[7] << 8) | b[8];
  if (!was) Serial.printf("Soil %d online H=%.1f T=%.1f EC=%d\n", node[i].soilAddr,
                          node[i].soilHum, node[i].soilTemp, node[i].ec);
  return true;
}

void scanNodes() {
  onlineCount = 0;
  for (int i = 0; i < slaveTotal; i++) {
    node[i].addr = firstSlave + i;
    // 第一只传感器保留出厂地址1，后面的要改成34-64
    node[i].soilAddr = i == 0 ? 1 : 33 + i;
    node[i].online = false;
    node[i].soilOnline = false;
    node[i].fail = 0;
    node[i].soilFail = 0;
    readNode(i);
    if (node[i].online || i == 0) readNodeSoil(i);
  }
  Serial.printf("RS485 online: %d\n", onlineCount);
}

void pollOneNode() {
  static int p = 0;
  for (int n = 0; n < slaveTotal; n++) {
    int i = (p + n) % slaveTotal;
    if (node[i].online || millis() - node[i].tried > 3000UL) {
      readNode(i);
      if (millis() - node[i].soilTried > 8000UL) readNodeSoil(i);
      p = (i + 1) % slaveTotal;
      return;
    }
  }
}

void pump(int i, int num, int sec) {
  if (i < 0 || i >= slaveTotal || !node[i].online) return;
  if (modbusWrite(node[i].addr, 7 + num, sec)) node[i].pump[num] = sec > 0;
}

void lamp(int i, int pwm) {
  if (i < 0 || i >= slaveTotal || !node[i].online) return;
  pwm = constrain(pwm, 0, 255);
  if (modbusWrite(node[i].addr, 5, pwm)) node[i].lamp = pwm;
}

void beep(int ms, int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(ms);
    digitalWrite(BUZZER, LOW);
    if (i + 1 < times) delay(80);
  }
}

void bootScreen(const char* step, const char* state = "") {
  oled.clearBuffer();
  oled.setFont(u8g2_font_ncenB08_tr);
  oled.setCursor(0, 12);
  oled.print("Plant Keeper");
  oled.drawHLine(0, 16, 128);
  oled.setCursor(0, 36);
  oled.print(step);
  oled.setCursor(0, 54);
  oled.print(state);
  oled.sendBuffer();
}

void servoPulse(int us) {
  int duty = (int)((long)us * 1023 / 20000);
  if (!ledcWrite(SERVO_PIN, duty)) Serial.println("Servo PWM write failed");
}

void servoStop() {
  servoPulse(1500);
}

void servoTo(int pos) {
  pos = constrain(pos, 20, 160);
  int d = pos - servoPos;
  if (abs(d) < 5) return;
  servoPulse(d > 0 ? 2250 : 750);
  delay(abs(d) * 9);
  servoStop();
  servoPos = pos;
  Serial.printf("Servo pos=%d\n", servoPos);
}

void servoJog(bool right, int ms) {
  ms = constrain(ms, 80, 1500);
  servoPulse(right ? 2250 : 750);
  delay(ms);
  servoStop();
  servoPos += right ? ms / 9 : -ms / 9;
  servoPos = constrain(servoPos, 20, 160);
}

void servoTest() {
  if (!servoReady) return;
  servoJog(false, 180);
  delay(150);
  servoJog(true, 180);
  servoPos = 90;
  servoStop();
}

bool initCamera() {
  // PWDN高电平关机，先拉高再唤醒一次
  pinMode(CAM_PWDN, OUTPUT);
  digitalWrite(CAM_PWDN, HIGH);
  delay(80);
  digitalWrite(CAM_PWDN, LOW);
  delay(120);

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_D0;
  c.pin_d1 = CAM_D1;
  c.pin_d2 = CAM_D2;
  c.pin_d3 = CAM_D3;
  c.pin_d4 = CAM_D4;
  c.pin_d5 = CAM_D5;
  c.pin_d6 = CAM_D6;
  c.pin_d7 = CAM_D7;
  c.pin_xclk = CAM_XCLK;
  c.pin_pclk = CAM_PCLK;
  c.pin_vsync = CAM_VSYNC;
  c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD;
  c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN;
  c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_UXGA;  // 1600x1200
  c.jpeg_quality = 5;
  c.fb_count = psramFound() ? 2 : 1;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  esp_err_t e = esp_camera_init(&c);
  Serial.printf("Camera: 0x%x (%s)\n", e, esp_err_to_name(e));
  if (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_CAMERA_NOT_DETECTED) {
    Serial.println("Camera SCCB no response, check 3V3/GND/SIOD4/SIOC5 and ribbon");
  }
  return e == ESP_OK;
}

bool initAI() {
  if (!psramFound()) {
    Serial.println("AI needs PSRAM");
    return false;
  }

  model = tflite::GetModel(plant_model);
  if (!model || model->version() != TFLITE_SCHEMA_VERSION) return false;

  arena = (uint8_t*)ps_malloc(700 * 1024);
  if (!arena) return false;

  static tflite::MicroMutableOpResolver<7> ops;
  ops.AddQuantize();
  ops.AddConv2D();
  ops.AddMaxPool2D();
  ops.AddMean();
  ops.AddFullyConnected();
  ops.AddSoftmax();
  ops.AddDequantize();

  interpreter = new tflite::MicroInterpreter(model, ops, arena, 700 * 1024);
  if (interpreter->AllocateTensors() != kTfLiteOk) return false;
  input = interpreter->input(0);
  output = interpreter->output(0);
  if (!input || !output || input->type != kTfLiteUInt8 || output->type != kTfLiteFloat32) return false;

  Serial.printf("AI input: %dx%dx%d scale=%.6f zero=%d\n", input->dims->data[2],
                input->dims->data[1], input->dims->data[3], input->params.scale, (int)input->params.zero_point);
  return true;
}

camera_fb_t* photo() {
  if (!camOk) return nullptr;
  for (int i = 0; i < slaveTotal; i++) {
    if (node[i].online) modbusWrite(node[i].addr, 5, 255);
  }
  delay(800);
  camera_fb_t* fb = esp_camera_fb_get();
  return fb;
}

void restoreLamp() {
  for (int i = 0; i < slaveTotal; i++) if (node[i].online) lamp(i, node[i].lamp);
}

bool runAI(camera_fb_t* fb) {
  if (!aiOk || !fb) return false;
  int fw = fb->width, fh = fb->height;
  uint8_t* rgb = (uint8_t*)ps_malloc(fw * fh * 3);
  if (!rgb) return false;
  if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb)) {
    free(rgb);
    return false;
  }

  int gx1=fw,gy1=fh,gx2=0,gy2=0,step=fw>800?4:2;
  for(int y=0;y<fh;y+=step){for(int x=0;x<fw;x+=step){int i=(y*fw+x)*3;uint8_t r=rgb[i],g=rgb[i+1],b=rgb[i+2];if(g>r&&g>b&&g>35){if(x<gx1)gx1=x;if(y<gy1)gy1=y;if(x>gx2)gx2=x;if(y>gy2)gy2=y;}}}

  int cropX,cropY,cropSz;
  if(gx2>gx1+20&&gy2>gy1+20){
    int gw=gx2-gx1,gh=gy2-gy1;
    gx1=gx1>gw/8?gx1-gw/8:0;gy1=gy1>gh/8?gy1-gh/8:0;
    gx2=gx2+gw/8;if(gx2>=fw)gx2=fw-1;gy2=gy2+gh/8;if(gy2>=fh)gy2=fh-1;
    int cx=(gx1+gx2)/2,cy=(gy1+gy2)/2;
    cropSz=gx2-gx1>gy2-gy1?gx2-gx1:gy2-gy1;if(cropSz<32)cropSz=32;
    cropX=cx-cropSz/2;if(cropX<0)cropX=0;cropY=cy-cropSz/2;if(cropY<0)cropY=0;
    if(cropX+cropSz>fw)cropX=fw-cropSz;if(cropY+cropSz>fh)cropY=fh-cropSz;
    Serial.printf("Leaf: %d,%d %dx%d\n",cropX,cropY,cropSz,cropSz);
  }else{
    cropSz=fw<fh?fw:fh;cropX=(fw-cropSz)/2;cropY=(fh-cropSz)/2;
    Serial.println("No leaf, center");
  }

  int h=input->dims->data[1],w=input->dims->data[2];
  uint8_t* dst=input->data.uint8;
  for(int y=0;y<h;y++){int sy=cropY+y*cropSz/h;for(int x=0;x<w;x++){int sx=cropX+x*cropSz/w,si=(sy*fw+sx)*3,di=(y*w+x)*3;dst[di]=rgb[si];dst[di+1]=rgb[si+1];dst[di+2]=rgb[si+2];}}
  free(rgb);

  unsigned long t = millis();
  if (interpreter->Invoke() != kTfLiteOk) return false;
  aiCost = millis() - t;

  aiClass = 0;
  aiConf = output->data.f[0];
  for (int i = 0; i < 5; i++) {
    aiProb[i] = output->data.f[i];
    if (aiProb[i] > aiConf) {
      aiConf = aiProb[i];
      aiClass = i;
    }
  }
  aiHealth = constrain((int)(aiProb[0] * 100), 0, 100);
  aiAt = millis();
  Serial.printf("AI %s %.1f%% %lums\n", className[aiClass], aiConf * 100, aiCost);
  return true;
}

void multiAngleAI() {
  if (!camOk || !aiOk) return;
  int old = servoPos;
  int pos[3] = {55, 90, 125};
  float worst = 2;
  int worstClass = -1, worstHealth = 0;
  float keepProb[5] = {0};

  for (int k = 0; k < 3; k++) {
    servoTo(pos[k]);
    delay(300);
    camera_fb_t* fb = photo();
    if (fb) {
      if (runAI(fb) && aiProb[0] < worst) {
        worst = aiProb[0];
        worstClass = aiClass;
        worstHealth = aiHealth;
        for (int i = 0; i < 5; i++) keepProb[i] = aiProb[i];
      }
      esp_camera_fb_return(fb);
    }
  }
  servoTo(old);
  restoreLamp();

  if (worstClass >= 0) {
    aiClass = worstClass;
    aiHealth = worstHealth;
    for (int i = 0; i < 5; i++) aiProb[i] = keepProb[i];
    aiConf = aiProb[aiClass];
  } else {
    aiClass = -1;
    aiConf = 0;
    aiHealth = 0;
    aiAt = 0;
  }
}

bool inspectAt(int pos) {
  servoTo(pos);
  delay(500);
  if (!camOk || !aiOk) return false;
  camera_fb_t* fb = photo();
  if (!fb) return false;
  bool ok = runAI(fb);
  if (ok && wifiOk) uploadPhoto(fb);
  restoreLamp();
  esp_camera_fb_return(fb);
  return ok;
}

String sensorJson() {
  int i = showNode;
  if (!node[i].online) {
    for (int k = 0; k < slaveTotal; k++) if (node[k].online) { i = k; break; }
  }

  JsonDocument d;
  d["temp"] = airTemp;
  d["humidity"] = airHum;
  d["light"] = node[i].light;
  d["soil"] = node[i].soil;
  d["soil_temp"] = node[i].soilTemp;
  d["soil_hum"] = node[i].soilHum;
  d["ec"] = node[i].ec;
  d["health"] = aiHealth;
  d["ai_class"] = aiClass;
  d["ai_conf"] = aiConf;
  d["ai_ms"] = aiCost;
  d["stage"] = plantStage;
  d["stage_name"] = stageName[plantStage];
  d["slave_count"] = onlineCount;
  d["slave_addr"] = node[i].addr;
  d["pump1"] = node[i].pump[0];
  d["pump2"] = node[i].pump[1];
  d["pump3"] = node[i].pump[2];
  d["light_pwm"] = node[i].lamp;
  d["time"] = timeText();
  String body;
  serializeJson(d, body);
  return body;
}

void upload() {
  if (!wifiOk) return;
  String body = sensorJson();

  HTTPClient http;
  http.begin(upUrl);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  uploadCode = http.POST(body);
  String reply = uploadCode > 0 ? http.getString() : http.errorToString(uploadCode);
  uploadOk = uploadCode >= 200 && uploadCode < 300;
  Serial.printf("Upload: %d %s\n", uploadCode, reply.c_str());
  http.end();
}

bool uploadPhoto(camera_fb_t* fb) {
  if (!wifiOk || !fb) return false;
  String json = sensorJson();
  String boundary = "PK" + String(millis());

  String p1 = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"p.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  String p2 = "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"data\"\r\n\r\n";
  String p3 = "\r\n--" + boundary + "--\r\n";

  size_t total = p1.length() + fb->len + p2.length() + json.length() + p3.length();
  uint8_t* buf = (uint8_t*)malloc(total);
  if (!buf) {
    upload();
    return false;
  }

  size_t pos = 0;
  memcpy(buf + pos, p1.c_str(), p1.length()); pos += p1.length();
  memcpy(buf + pos, fb->buf, fb->len); pos += fb->len;
  memcpy(buf + pos, p2.c_str(), p2.length()); pos += p2.length();
  memcpy(buf + pos, json.c_str(), json.length()); pos += json.length();
  memcpy(buf + pos, p3.c_str(), p3.length());

  HTTPClient http;
  http.begin(upUrl);
  http.setTimeout(20000);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  uploadCode = http.POST(buf, total);
  free(buf);
  http.end();

  uploadOk = uploadCode >= 200 && uploadCode < 300;
  Serial.printf("Photo upload: %d (%u bytes)\n", uploadCode, (unsigned int)total);
  return uploadOk;
}

void readDht() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    airHum = h;
    airTemp = t;
  }
}

void updateStage() {
  if (plantAge < 15) plantStage = 0;
  else if (plantAge < 35) plantStage = 1;
  else if (plantAge < 55) plantStage = 2;
  else plantStage = 3;
}

void decide() {
  unsigned long now = millis();
  bool aiFresh = aiAt && now - aiAt < 30UL * 60UL * 1000UL;
  int ecNeed = ecMin;
  if (plantStage == 0) ecNeed = ecMin * 6 / 10;
  if (plantStage == 2) ecNeed = ecMin * 12 / 10;

  for (int i = 0; i < slaveTotal; i++) {
    if (!node[i].online) continue;
    bool dampDisease = aiFresh && (aiClass == 1 || aiClass == 3) && aiConf > 0.65f;

    bool daytime = true;
    if (timeOk && getLocalTime(&nowTime)) daytime = nowTime.tm_hour >= 6 && nowTime.tm_hour < 20;
    int wantLamp = daytime && node[i].light > 0 && node[i].light < lightMin ? lightPwm : 0;
    if (abs((int)node[i].lamp - wantLamp) >= 10) lamp(i, wantLamp);

    if (node[i].soil > 0 && node[i].soil < soilMin && !node[i].pump[0] &&
        now - node[i].waterAt > (unsigned long)waterCd * 60000UL) {
      if (!dampDisease || node[i].soil < soilMin * 8 / 10) {
        int sec = aiFresh && aiHealth < 40 ? 2 : 5;
        pump(i, 0, sec);
        node[i].waterAt = now;
      }
    }

    if (node[i].soilOnline && node[i].ec > 0 && node[i].ec < ecNeed && node[i].ec < ecMax &&
        !node[i].pump[1] && !node[i].pump[2] &&
        now - node[i].fertAt > (unsigned long)fertCd * 60000UL) {
      if (!aiFresh || aiHealth >= 30 || ecNeed - node[i].ec > 100) {
        pump(i, plantStage <= 1 ? 1 : 2, aiFresh && aiHealth < 50 ? 1 : 3);
        node[i].fertAt = now;
      }
    }

    if (node[i].soilOnline && node[i].ec > ecMax && !node[i].pump[0] &&
        now - node[i].waterAt > (unsigned long)waterCd * 60000UL) {
      pump(i, 0, 5);
      node[i].waterAt = now;
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(300);
  wifiOk = WiFi.status() == WL_CONNECTED;
  if (wifiOk) {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    timeOk = getLocalTime(&nowTime, 5000);
  }
}

void checkWiFi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    if (!wifiOk) {
      wifiOk = true;
      uploadOk = false;
      Serial.printf("WiFi back: %s\n", WiFi.localIP().toString().c_str());
      configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
      timeOk = getLocalTime(&nowTime, 3000);
      fetchConfig();
      upload();
    }
    return;
  }

  if (wifiOk) {
    wifiOk = false;
    uploadOk = false;
    timeOk = false;
    Serial.println("WiFi lost");
  }

  if (millis() - wifiTryAt >= 15000UL) {
    wifiTryAt = millis();
    Serial.println("WiFi reconnect...");
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, pass);
  }
}

String timeText() {
  if (!timeOk || !getLocalTime(&nowTime)) return "----";
  char s[40];
  snprintf(s, sizeof(s), "%04d-%02d-%02d %02d:%02d:%02d", nowTime.tm_year + 1900,
           nowTime.tm_mon + 1, nowTime.tm_mday, nowTime.tm_hour, nowTime.tm_min, nowTime.tm_sec);
  return String(s);
}

void fetchConfig() {
  if (!wifiOk) return;
  HTTPClient http;
  http.begin(cfgUrl);
  http.setTimeout(4000);
  if (http.GET() == 200) {
    JsonDocument d;
    if (!deserializeJson(d, http.getString())) {
      plantAge = d["plant_age_days"] | plantAge;
      if (!d["soil_target_min"].isNull()) soilMin = d["soil_target_min"];
      else soilMin = d["soil_min"] | soilMin;
      if (!d["soil_target_max"].isNull()) soilMax = d["soil_target_max"];
      else soilMax = d["soil_max"] | soilMax;
      if (!d["ec_growth_min"].isNull()) ecMin = d["ec_growth_min"];
      else ecMin = d["ec_min"] | ecMin;
      if (!d["ec_growth_max"].isNull()) ecMax = d["ec_growth_max"];
      else ecMax = d["ec_max"] | ecMax;
      if (!d["watering_cooldown"].isNull()) waterCd = d["watering_cooldown"];
      else waterCd = d["water_cd"] | waterCd;
      if (!d["fertilizer_cooldown"].isNull()) fertCd = d["fertilizer_cooldown"];
      else fertCd = d["fert_cd"] | fertCd;
      lightMin = d["light_min"] | lightMin;
      lightPwm = d["light_pwm"] | lightPwm;
      int p = d["servo_angle"] | servoPos;
      if (p != servoPos) servoTo(p);
    }
  }
  http.end();
  plantAge = constrain(plantAge, 0, 5000);
  soilMin = constrain(soilMin, 100, 4000);
  soilMax = constrain(soilMax, soilMin + 1, 4095);
  ecMin = constrain(ecMin, 10, 5000);
  ecMax = constrain(ecMax, ecMin + 1, 10000);
  waterCd = constrain(waterCd, 1, 1440);
  fertCd = constrain(fertCd, 5, 10080);
  lightMin = constrain(lightMin, 0, 4095);
  lightPwm = constrain(lightPwm, 0, 255);
  updateStage();
}

void ackCommand(int id, bool ok, const char* text) {
  HTTPClient http;
  String url = String(cmdUrl) + "/" + id + "/ack";
  http.begin(url);
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"success\":") + (ok ? "true" : "false") +
                ",\"result\":\"" + text + "\",\"servo_pos\":" + servoPos + "}";
  http.POST(body);
  http.end();
}

void pollCommand() {
  if (!wifiOk) return;
  HTTPClient http;
  http.begin(cmdUrl);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String body = http.getString();
  http.end();

  JsonDocument d;
  if (deserializeJson(d, body) || d["id"].isNull()) return;
  int id = d["id"];
  String type = d["type"] | "";
  bool ok = true;

  if (type == "move") {
    servoTo(d["value"] | servoPos);
  } else if (type == "jog_left") {
    servoJog(false, d["value"] | 180);
  } else if (type == "jog_right") {
    servoJog(true, d["value"] | 180);
  } else if (type == "stop") {
    servoStop();
  } else if (type == "visit") {
    int pos = d["target"]["servo_pos"] | servoPos;
    int addr = d["target"]["slave_addr"] | 0;
    if (addr >= firstSlave && addr < firstSlave + slaveTotal) showNode = addr - firstSlave;
    ok = inspectAt(pos);
  } else if (type == "scan") {
    JsonArray list = d["targets"].as<JsonArray>();
    for (JsonObject t : list) {
      int addr = t["slave_addr"] | 0;
      if (addr >= firstSlave && addr < firstSlave + slaveTotal) showNode = addr - firstSlave;
      if (!inspectAt(t["servo_pos"] | 90)) ok = false;
    }
  } else {
    ok = false;
  }
  ackCommand(id, ok, ok ? "done" : "camera or ai failed");
}

void draw() {
  int i = showNode;
  if (!node[i].online) {
    for (int k = 0; k < slaveTotal; k++) {
      if (node[k].online) {
        i = k;
        showNode = k;
        break;
      }
    }
  }

  oled.clearBuffer();
  oled.setFont(u8g2_font_ncenB08_tr);
  if (page == 0) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.setCursor(0, 10);
    if (timeOk && getLocalTime(&nowTime)) oled.printf("%02d:%02d", nowTime.tm_hour, nowTime.tm_min);
    else oled.print("--:--");
    oled.setCursor(35, 10); oled.print(stageName[plantStage]);
    oled.setCursor(82, 10); oled.printf("%s%s S:%d", wifiOk ? "W" : "x", uploadOk ? "+" : "-", onlineCount);

    oled.setFont(u8g2_font_ncenB08_tr);
    oled.setCursor(0, 24); oled.printf("AI Health: %d%%", aiHealth);
    oled.setCursor(0, 37);
    if (aiClass > 0 && aiConf >= 0.5f) oled.printf("! %s %.0f%%", className[aiClass], aiConf * 100);
    else if (aiClass == 0) oled.printf("Healthy %.0f%%", aiConf * 100);
    else oled.print(aiOk ? "AI waiting..." : "AI offline");

    oled.setCursor(0, 50); oled.printf("T:%.1fC H:%.0f%%", airTemp, airHum);
    oled.setCursor(0, 63); oled.printf("S:%d EC:%d L:%d", node[i].soil, node[i].ec, node[i].light);
  } else if (page == 1) {
    int order = 0;
    for (int k = 0; k <= i; k++) if (node[k].online) order++;
    oled.setCursor(0, 10); oled.printf("Node %d   %d/%d", node[i].addr, order, onlineCount);
    oled.setCursor(0, 24); oled.printf("Pump1:%s Pump2:%s", node[i].pump[0] ? "ON" : "OFF", node[i].pump[1] ? "ON" : "OFF");
    oled.setCursor(0, 37); oled.printf("Pump3:%s Lamp:%d", node[i].pump[2] ? "ON" : "OFF", node[i].lamp);
    oled.setCursor(0, 50); oled.printf("Soil:%d Light:%d", node[i].soil, node[i].light);
    oled.setCursor(0, 63); oled.printf("ST:%.1f SH:%.1f EC:%d", node[i].soilTemp, node[i].soilHum, node[i].ec);
  } else {
    const char* shortName[] = {"Healthy", "Mildew", "LeafSpot", "Rust", "Pest"};
    for (int k = 0; k < 5; k++) {
      oled.setCursor(0, 10 + k * 13);
      oled.printf("%s: %.0f%%", shortName[k], aiProb[k] * 100);
    }
  }
  oled.sendBuffer();
}

void checkButton() {
  static bool down = false;
  static unsigned long pressAt = 0;
  bool pressed = digitalRead(BTN_PIN) == LOW;

  if (pressed && !down) {
    down = true;
    pressAt = millis();
    return;
  }

  if (!pressed && down) {
    down = false;
    unsigned long held = millis() - pressAt;
    if (held < 20) return;

    digitalWrite(BUZZER, HIGH);
    delay(70);
    digitalWrite(BUZZER, LOW);

    if (page == 1 && held < 500 && onlineCount > 1) {
      for (int n = 1; n <= slaveTotal; n++) {
        int k = (showNode + n) % slaveTotal;
        if (node[k].online) { showNode = k; break; }
      }
    } else {
      page = (page + 1) % 3;
    }
    draw();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Plant Keeper host");
  Serial.printf("PSRAM: %s, %lu bytes\n", psramFound() ? "OK" : "NO", (unsigned long)ESP.getPsramSize());

  pinMode(BUZZER, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(RS485_DIR, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);
  digitalWrite(RS485_DIR, LOW);

  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  dht.begin();
  oled.begin();
  oled.clearDisplay();
  oled.clearBuffer();
  oled.sendBuffer();
  delay(30);
  oled.clearDisplay();
  bootScreen("Power on", "Starting...");
  beep(70, 2);

  // 舵机用通道7，下载和串口必须走CH340，不能占用GPIO20的原生USB
  servoReady = ledcAttachChannel(SERVO_PIN, 50, 10, 7);
  Serial.printf("Servo PWM: %s\n", servoReady ? "OK" : "FAIL");
  servoStop();
  servoTest();

  bootScreen("Camera", "Checking OV2640...");
  camOk = initCamera();
  bootScreen("Camera", camOk ? "OK" : "FAIL - check cable");
  delay(400);

  bootScreen("AI model", "Loading...");
  aiOk = initAI();
  bootScreen("AI model", aiOk ? "OK" : "FAIL");
  delay(400);

  bootScreen("WiFi", "Connecting...");
  connectWiFi();
  bootScreen("WiFi", wifiOk ? "Connected" : "Offline mode");
  delay(400);
  if (wifiOk) fetchConfig();

  bootScreen("RS485", "Scanning nodes...");
  scanNodes();
  char rsText[24];
  snprintf(rsText, sizeof(rsText), "%d node online", onlineCount);
  bootScreen("RS485", rsText);
  delay(500);
  readDht();
  if (wifiOk) upload();
  draw();
  Serial.printf("Ready cam=%d ai=%d wifi=%d\n", camOk, aiOk, wifiOk);
}

void loop() {
  unsigned long now = millis();
  static unsigned long tPoll = 0, tSensor = 0, tUp = 0, tCfg = 0, tDraw = 0, tCmd = 0, tWifi = 0;

  if (now - tWifi >= 3000) {
    tWifi = now;
    checkWiFi();
  }

  if (now - tPoll >= 250) {
    tPoll = now;
    pollOneNode();
  }
  if (now - tSensor >= 30000) {
    tSensor = now;
    readDht();
    decide();
  }
  if (now - tUp >= 60000) {
    tUp = now;
    upload();
  }
  if (now - tCmd >= 2000) {
    tCmd = now;
    pollCommand();
  }
  if (now - tCfg >= 3600000) {
    tCfg = now;
    fetchConfig();
  }
  if (now - tDraw >= 1000) {
    tDraw = now;
    draw();
  }
  checkButton();
}
