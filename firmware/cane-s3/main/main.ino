/**
 * 盲杖主控 ESP32-S3 完整固件
 *
 * 功能：
 *   1. 超声波低位测距 + 前哨高位融合判断
 *   2. ESP-NOW 接收前哨高位距离 + 发送拍照命令
 *   3. MPU6050 跌倒检测 + 30秒倒计时
 *   4. 旋转编码器：旋转180°切模式 / 三连按报警 / 长按1.5秒拍照
 *   5. 振动马达反馈 + 蜂鸣器距离编码（频率随危险等级变化）
 *   6. SYN6288 语音播报（模式切换/跌倒/报警等，专注语音不再播障碍）
 *   6. 电量检测
 *   7. GPS 定位（TinyGPS++ 解析，BLE 上报经纬度）
 *   8. BLE 发送状态给手机 App（含接收 App 下发的模式/取消指令）
 *
 * 接线：
 *   HC-SR04:   Trig→GPIO5, Echo→GPIO18, VCC→5V, GND→GND
 *   振动马达:  IN→GPIO4, VCC→5V, GND→GND
 *   MPU6050:   SDA→GPIO10, SCL→GPIO9, VCC→3.3V, GND→GND
 *   编码器EC11(5脚带按钮): A→GPIO6, B→GPIO7, SW(按钮)→GPIO8, VCC→3.3V, GND→GND
 *   SYN6288:   RX←GPIO14, TX→GPIO3, VCC→5V, GND→GND, 喇叭接模块
 *   电量检测:  电池分压→GPIO1
 *   蜂鸣器:    →GPIO11（频率编码距离/危险等级）
 *   WS2811灯带: DIN→GPIO17, VCC→5V, GND→GND（夜间模式常亮警示，危险等级变色）
 *   GPS:       TX→GPIO15, RX→GPIO16, VCC→5V, GND→GND（注意原 gps_test 用 GPIO14 与 TTS 冲突，已改）
 *
 * 注：蜂鸣器做距离反馈（频率随等级变化），SYN6288 专注语音播报。
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>
#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <FastLED.h>

// ====================== PWM ======================
#define MOTOR_PWM_FREQ 5000
#define MOTOR_PWM_BITS 8

// SYN6288 音量：[v0]静音 ~ [v16]最大。默认 8（中等偏小，演示不吵）
#define TTS_DEFAULT_VOL 4

// ====================== BLE ======================
#define DEVICE_NAME "SmartCane-S3"
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ====================== WiFi（连手机热点，与前哨同网段，ESP-NOW 信道对齐）======================
const char* ssid = "starry";
const char* password = "iloveyouso";

// ====================== 引脚 ======================
#define TRIG_PIN 5
#define ECHO_PIN 18
#define MOTOR_PIN 4
#define BUZZER_PIN 11    // 蜂鸣器：频率随危险等级变化（反馈距离）
#define BUTTON_PIN 8     // 原 GPIO0 与 BOOT 冲突，改到 GPIO8（非 strapping）
#define BATT_PIN 1
#define MPU6050_ADDR 0x68
#define SDA_PIN 10       // I2C 改到 GPIO10/GPIO9，避开 USB_D+ 的 GPIO20
#define SCL_PIN 9
#define ENC_A 6
#define ENC_B 7
#define TTS_TX_PIN 14   // ESP32 TX → SYN6288 RX
#define TTS_RX_PIN 3    // SYN6288 TX → ESP32 RX
#define GPS_RX_PIN 15   // GPS TX → ESP32 RX（注意：原 gps_test 用 GPIO14，与 TTS 冲突，已改到 15）
#define GPS_TX_PIN 16   // ESP32 TX → GPS RX（一般不用）
#define WS2811_PIN 17   // WS2811 灯带数据引脚
#define WS2811_COUNT 5  // 灯珠数量（根据实际灯带改）

// ====================== 前哨 MAC ======================
uint8_t scoutMac[] = {0x28, 0x84, 0x85, 0x4B, 0xAB, 0xBC};

// ====================== 数据结构 ======================
typedef struct { float distance; int level; } ScoutData;
typedef struct { int cmd; } CaneCmd;
ScoutData recvData;
CaneCmd sendCmd;

// ====================== 全局状态 ======================
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool espNowReady = false;   // ESP-NOW 初始化结果（自检用）
bool autoLoop = true;       // false=暂停自动测距/反馈，便于单独调试外设

float lowDist = -1;
float highDist = -1;
unsigned long highDistTime = 0;   // 最近一次收到前哨高位距离的时间
int fusedLevel = 0;
float batteryPct = 100;

// 前哨高位距离过期时间（毫秒）。超过则视为无数据，避免掉线后永久误报。
#define HIGH_DIST_TIMEOUT 1500

int workMode = 0;  // 0=正常 1=安静 2=夜间
const char* modeNames[] = {"正常", "安静", "夜间"};

// 交通灯状态（App 识别后经 BLE 上报：RED/GREEN/YELLOW/NONE）
int trafficLight = 0;   // 0=NONE 1=RED 2=YELLOW 3=GREEN
const char* lightNames[] = {"无", "红灯", "黄灯", "绿灯"};

// 编码器
volatile long encoderCount = 0;
long encoderBase = 0;          // 切模式用的基准
long encoderBaseFall = 0;       // 跌倒取消检测用的独立基准（与切模式分开）

// 按钮
volatile int buttonClickCount = 0;
volatile unsigned long lastClickTime = 0;   // 最近一次"有效点击"时刻（连按窗口以此判定）
volatile bool buttonHeld = false;
volatile unsigned long buttonDownTime = 0;
volatile bool longPressTriggered = false;
#define LONGPRESS_MS 1500   // 长按判定时长（太短易与慢速连按混淆）
#define CLICK_GAP_MS 800    // 连按间隔窗口：两次点击间隔超过此值即判定结束
int buttonPressedLevel = LOW;   // 按下时的电平（默认 LOW；若按钮接法相反则改 HIGH）
int buttonIdleLevel = HIGH;     // 松开时的电平

// 跌倒
float accX, accY, accZ, accMag;
int fallState = 0;
unsigned long fallTimer = 0;
bool fallDetected = false;
bool fallCountdown = false;
unsigned long fallCountdownStart = 0;
bool fallVoicePlayed = false;
bool mpuOnline = false;   // MPU6050 是否在线（掉线时跳过跌倒检测，避免 0 值误报）

// 报警
bool manualAlarm = false;

// TTS 串口
HardwareSerial synSerial(2);

// GPS（Serial1，9600 波特率，NMEA 语句）
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;
double gpsLat = 0, gpsLng = 0;
bool gpsValid = false;

// ====================== GB2312 预编码语音短语 ======================
// "您似乎跌倒了"
uint8_t msg_fall[] = {0xC4,0xFA,0xCB,0xC6,0xBA,0xF5,0xB5,0xF8,0xB5,0xB9,0xC1,0xCB};
// "三十秒后自动报警"
uint8_t msg_countdown[] = {0xC8,0xFD,0xCA,0xAE,0xC3,0xEB,0xBA,0xF3,0xD7,0xD4,0xB6,0xAF,0xB1,0xA8,0xBE,0xAF};
// "拨动旋钮取消"
uint8_t msg_cancel[] = {0xB2,0xA6,0xB6,0xAF,0xD0,0xFD,0xC5,0xA5,0xC8,0xA1,0xCF,0xFB};
// "您似乎跌倒了，三十秒后自动报警，拨动旋钮取消"（合并成一句，三段之间无停顿空隙）
uint8_t msg_fall_full[] = {
  0xC4,0xFA,0xCB,0xC6,0xBA,0xF5,0xB5,0xF8,0xB5,0xB9,0xC1,0xCB,  // 您似乎跌倒了
  0xA3,0xAC,                                                      // ，
  0xC8,0xFD,0xCA,0xAE,0xC3,0xEB,0xBA,0xF3,0xD7,0xD4,0xB6,0xAF,0xB1,0xA8,0xBE,0xAF,  // 三十秒后自动报警
  0xA3,0xAC,                                                      // ，
  0xB2,0xA6,0xB6,0xAF,0xD0,0xFD,0xC5,0xA5,0xC8,0xA1,0xCF,0xFB    // 拨动旋钮取消
};
// "前方有障碍物"
uint8_t msg_obstacle[] = {0xC7,0xB0,0xB7,0xBD,0xD3,0xD0,0xD5,0xCF,0xB0,0xAD,0xCE,0xEF};
// "注意安全"
uint8_t msg_safe[] = {0xD7,0xA2,0xD2,0xE2,0xB0,0xB2,0xC8,0xAB};
// "注意脚下"
uint8_t msg_low[] = {0xD7,0xA2,0xD2,0xE2,0xBD,0xC5,0xCF,0xC2};
// "注意头部"
uint8_t msg_high[] = {0xD7,0xA2,0xD2,0xE2,0xCD,0xB7,0xB2,0xBF};
// "红灯，请等待"
uint8_t msg_red[] = {0xBA,0xEC,0xB5,0xC6,0xA3,0xAC,0xC7,0xEB,0xB5,0xC8,0xB4,0xFD};
// "黄灯，请注意"
uint8_t msg_yellow[] = {0xBB,0xC6,0xB5,0xC6,0xA3,0xAC,0xC7,0xEB,0xD7,0xA2,0xD2,0xE2};
// "绿灯，可以通行"
uint8_t msg_green[] = {0xC2,0xCC,0xB5,0xC6,0xA3,0xAC,0xBF,0xC9,0xD2,0xD4,0xCD,0xA8,0xD0,0xD0};
// "电池电量低"
uint8_t msg_lowbattery[] = {0xB5,0xE7,0xB3,0xD8,0xB5,0xE7,0xC1,0xBF,0xB5,0xCD};
// "已报警"
uint8_t msg_alarmed[] = {0xD2,0xD1,0xB1,0xA8,0xBE,0xAF};
// "正常模式"
uint8_t msg_mode_normal[] = {0xD5,0xFD,0xB3,0xA3,0xC4,0xA3,0xCA,0xBD};
// "安静模式"
uint8_t msg_mode_silent[] = {0xB0,0xB2,0xBE,0xB2,0xC4,0xA3,0xCA,0xBD};
// "夜间模式"
uint8_t msg_mode_night[] = {0xD2,0xB9,0xBC,0xE4,0xC4,0xA3,0xCA,0xBD};

// ====================== SYN6288 语音（非阻塞队列） ======================
// 非阻塞设计：tts_speak 只入队即返回，不阻塞 loop()；
// 真正发送在 tts_tick() 中按"上一句播完"的节奏进行，保证测距/跌倒检测实时性。
#define TTS_QUEUE_SIZE 6
struct TtsItem { const uint8_t* data; uint8_t len; };
TtsItem ttsQueue[TTS_QUEUE_SIZE];
uint8_t ttsHead = 0, ttsTail = 0;
unsigned long ttsBusyUntil = 0;

void tts_sendFrame(const uint8_t *data, uint8_t len) {
  uint8_t buf[210];
  buf[0] = 0xFD;
  buf[1] = (uint8_t)((len + 3) / 256);
  buf[2] = (uint8_t)((len + 3) % 256);
  buf[3] = 0x01;
  buf[4] = 0x00;
  memcpy(&buf[5], data, len);
  uint8_t xor_cal = 0;
  for (uint8_t i = 0; i < len + 5; i++) xor_cal ^= buf[i];
  buf[len + 5] = xor_cal;
  synSerial.write(buf, len + 6);
}

// 入队（非阻塞）。队列满则丢弃最旧的一条，保证最新语音优先。
void tts_speak(const uint8_t *data, uint8_t len) {
  ttsQueue[ttsTail] = { data, len };
  ttsTail = (ttsTail + 1) % TTS_QUEUE_SIZE;
  if (ttsTail == ttsHead) ttsHead = (ttsHead + 1) % TTS_QUEUE_SIZE;  // 满，丢最旧
}

// 在 loop() 中调用：空闲且队列非空时发下一句
void tts_tick() {
  if (ttsHead == ttsTail) return;             // 队列空
  if (millis() < ttsBusyUntil) return;       // 上一句还没播完
  TtsItem item = ttsQueue[ttsHead];
  ttsHead = (ttsHead + 1) % TTS_QUEUE_SIZE;
  tts_sendFrame(item.data, item.len);
  // 估算播报时长：SYN6288 收帧后约 300ms 才出声，语速约 3 字/秒（每字 2 字节）
  // 留足余量，避免下一帧把上一句尾部掐断（之前 120ms/字节 会吞掉句尾字）
  ttsBusyUntil = millis() + (unsigned long)item.len * 200 + 800;
}

// ====================== 模式切换（统一入口） ======================
// 旋钮 / 串口命令 / App 指令都走这里：改模式 + 串口打印 + BLE 通知 + 语音播报
void setMode(int m) {
  if (m < 0 || m > 2) return;
  workMode = m;
  Serial.printf("[模式] 切换: %s\n", modeNames[workMode]);
  sendBLE(String("MODE:") + (workMode==0?"NORMAL":workMode==1?"SILENT":"NIGHT") + "\n");
  const uint8_t* voice = (workMode==0) ? msg_mode_normal : (workMode==1) ? msg_mode_silent : msg_mode_night;
  tts_speak(voice, 8);
}

// ====================== 交通灯（统一入口） ======================
// l: 0=NONE 1=RED 2=YELLOW 3=GREEN。仅状态变化时播报一次，
// App 持续上报同一灯色不会重复播。
void setTrafficLight(int l) {
  if (l < 0 || l > 3) return;
  if (l == trafficLight) return;   // 状态没变，不播报
  trafficLight = l;
  Serial.printf("[交通灯] %s\n", lightNames[trafficLight]);
  switch (trafficLight) {
    case 1: tts_speak(msg_red,    sizeof(msg_red));    break;  // 红灯，请等待
    case 2: tts_speak(msg_yellow, sizeof(msg_yellow)); break;  // 黄灯，请注意
    case 3: tts_speak(msg_green,  sizeof(msg_green)); break;  // 绿灯，可以通行
    // 0=NONE：不播报
  }
}

// 设置音量 0-16（发 [vN] 文字命令给 SYN6288）
void tts_setVolume(uint8_t vol) {
  if (vol > 16) vol = 16;
  char cmd[8];
  snprintf(cmd, sizeof(cmd), "[v%d]", vol);
  uint8_t len = strlen(cmd);
  uint8_t buf[16];
  buf[0] = 0xFD; buf[1] = 0; buf[2] = (uint8_t)(len + 3);
  buf[3] = 0x01; buf[4] = 0x00;
  memcpy(&buf[5], cmd, len);
  uint8_t x = 0;
  for (uint8_t i = 0; i < len + 5; i++) x ^= buf[i];
  buf[len + 5] = x;
  synSerial.write(buf, len + 6);
  delay(150);
  Serial.printf("[TTS] 音量=%d (0-16)\n", vol);
}

void tts_init() {
  synSerial.begin(9600, SERIAL_8N1, TTS_RX_PIN, TTS_TX_PIN);
  delay(200);
  tts_setVolume(TTS_DEFAULT_VOL);
  Serial.println("SYN6288 语音就绪");
}

// ====================== 蜂鸣器距离编码 ======================
// 用频率编码障碍位置、用节奏（重复间隔）编码危险程度，事件触发、障碍消失即停。
//   type: 1=大型(上下都有) 2=下方/低位 3=上方/悬空  danger: 0~3
//   下方→低频(800Hz)；上方→高频(2500Hz)；上下都有→先用低频再用高频交替。
//   危险等级越高 → 蜂鸣越密集（间隔越短），越远越稀疏。
void playObstacleBeep(int type, int danger, unsigned long now) {
  static unsigned long lastBeep = 0;
  static int altFlag = 0;   // 上下都有时交替高低频
  // 危险等级越高，节奏越密集
  unsigned long interval;
  if (danger >= 3) interval = 120;      // 极近：急促
  else if (danger == 2) interval = 400; // 中近：中速
  else interval = 900;                  // 较远：缓
  if (now - lastBeep < interval) return;
  lastBeep = now;
  // 频率编码位置
  int freq;
  if (type == 2) freq = 800;            // 下方→低频
  else if (type == 3) freq = 2500;      // 上方→高频
  else { freq = (altFlag ^= 1) ? 800 : 2500; }  // 上下都有→交替
  ledcWriteTone(BUZZER_PIN, freq);
  // 音量控制：通过设置占空比调小音量（值越小声音越小）
  ledcWrite(BUZZER_PIN, 80);   // 占空比 80/255 ≈ 31%，降低音量
  // 蜂鸣音长：越近越短促（让节奏感更强）
  unsigned long dur = danger >= 3 ? 50 : (danger == 2 ? 80 : 120);
  // 用非阻塞：记下关闭时间，由 updateBuzzer 在主循环统一关
  // 这里用 delay 短时阻塞（≤120ms）可接受，保持简单
  delay((int)dur);
  ledcWriteTone(BUZZER_PIN, 0);
}

// ====================== ESP-NOW ======================
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(&recvData, data, sizeof(recvData));
  highDist = recvData.distance;
  highDistTime = millis();
}
void sendCmdToScout(int cmd) {
  sendCmd.cmd = cmd;
  esp_now_send(scoutMac, (uint8_t *)&sendCmd, sizeof(sendCmd));
}

// ====================== BLE 回调 ======================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { deviceConnected = true; Serial.println("[BLE] 手机连接"); }
  void onDisconnect(BLEServer* s) override { deviceConnected = false; s->startAdvertising(); Serial.println("[BLE] 手机断开"); }
};
class RxCbs : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String val = c->getValue();
    if (val.length() == 0) return;
    Serial.println("[BLE] 收到: " + val);
    // 处理 App 下发的模式切换指令：MODE:0/1/2
    if (val.startsWith("MODE:")) {
      int m = val.substring(5).toInt();
      if (m >= 0 && m <= 2) {
        Serial.println("[BLE] App 请求切换模式");
        setMode(m);
      }
    } else if (val == "ALARM:CANCEL") {
      // App 远程取消报警
      manualAlarm = false;
      fallDetected = false; fallCountdown = false; fallState = 0;
      ledcWrite(MOTOR_PIN, 0);
      Serial.println("[BLE] App 取消报警");
    } else if (val == "RED" || val == "YELLOW" || val == "GREEN" || val == "NONE") {
      // App 交通灯识别结果上报
      setTrafficLight(val == "RED" ? 1 : val == "YELLOW" ? 2 : val == "GREEN" ? 3 : 0);
    }
  }
};

// ====================== 超声波 ======================
float readDistance() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  if (d == 0) return -1.0;
  return d * 0.0343 / 2.0;
}

// ====================== 融合判断 ======================
int fuseDistance(float low, float high) {
  bool lo = (low > 0 && low < 150);
  bool hi = (high > 0 && high < 150);
  if (lo && hi) return 1;  // 大型障碍
  if (lo && !hi) return 2; // 低位障碍
  if (!lo && hi) return 3; // 悬空障碍
  return 0;
}
String obstacleName(int t) {
  switch(t) { case 1: return "LARGE"; case 2: return "LOW"; case 3: return "HIGH"; default: return "SAFE"; }
}
int dangerLevel(float low, float high) {
  float m = 999;
  if (low > 0 && low < m) m = low;
  if (high > 0 && high < m) m = high;
  if (m >= 150) return 0;
  if (m < 30) return 3;
  if (m < 80) return 2;
  return 1;
}

// ====================== WS2811 灯带（FastLED） ======================
CRGB leds[WS2811_COUNT];

void ledStripInit() {
  // WS2811 模板自带 400kHz 时序（WS2812 才是 800kHz）
  FastLED.addLeds<WS2811, WS2811_PIN, GRB>(leds, WS2811_COUNT);
  FastLED.setBrightness(255);
  FastLED.clear(true);
}

// 灯带更新：根据模式和危险等级控制
void updateLEDStrip(int mode, int danger) {
  if (mode == 2) {
    // 夜间模式：常亮警示，危险等级越高越亮/越红
    int brightness = 60 + danger * 65;   // 等级0=60，等级3=255
    if (brightness > 255) brightness = 255;
    if (danger >= 3) {
      // 急促红色闪烁
      static unsigned long lastBlink = 0;
      static bool on = false;
      if (millis() - lastBlink > 150) { lastBlink = millis(); on = !on; }
      CRGB color = on ? CRGB(brightness, 0, 0) : CRGB::Black;
      for (int i = 0; i < WS2811_COUNT; i++) leds[i] = color;
    } else if (danger >= 2) {
      // 中危：橙色
      for (int i = 0; i < WS2811_COUNT; i++) leds[i] = CRGB(brightness, brightness/2, 0);
    } else {
      // 低危/安全：暖白警示
      for (int i = 0; i < WS2811_COUNT; i++) leds[i] = CRGB(60, 50, 25);
    }
  } else if (mode == 0) {
    // 正常模式：呼吸灯（绿色），提示设备在线
    static unsigned long lastBreath = 0;
    static int breath = 0;
    static int dir = 1;
    if (millis() - lastBreath > 15) {
      lastBreath = millis();
      breath += dir * 2;
      if (breath >= 90 || breath <= 0) dir = -dir;
    }
    for (int i = 0; i < WS2811_COUNT; i++) leds[i] = CRGB(0, breath, 0);
  } else {
    // 安静模式：关闭
    for (int i = 0; i < WS2811_COUNT; i++) leds[i] = CRGB::Black;
  }
  FastLED.show();
}

// ====================== MPU6050 ======================
void mpuWrite(uint8_t r, uint8_t d) { Wire.beginTransmission(MPU6050_ADDR); Wire.write(r); Wire.write(d); Wire.endTransmission(); }
int16_t mpuRead16(uint8_t r) { Wire.beginTransmission(MPU6050_ADDR); Wire.write(r); Wire.endTransmission(false); Wire.requestFrom((int)MPU6050_ADDR, 2); return (Wire.read()<<8)|Wire.read(); }
void mpuRead() {
  if (!mpuOnline) return;   // 掉线时不读，保持上一次值，避免 0 值误判失重
  accX = mpuRead16(0x3B)/4096.0; accY = mpuRead16(0x3D)/4096.0; accZ = mpuRead16(0x3F)/4096.0;
  accMag = sqrt(accX*accX+accY*accY+accZ*accZ);
}
void checkFall() {
  if (!mpuOnline) return;   // MPU 掉线时不做跌倒检测，避免误报
  switch (fallState) {
    case 0:
      if (accMag < 0.4) { fallState = 1; fallTimer = millis(); }
      break;
    case 1:
      if (accMag > 2.5) {
        fallState = 2; fallDetected = true; fallCountdown = true;
        fallCountdownStart = millis(); fallVoicePlayed = false;
        encoderBaseFall = encoderCount;  // 记录跌倒起始基准，用于取消检测
        Serial.println("[跌倒] 确认！开始30秒倒计时");
        sendBLE("FALL:1\n");
      } else if (millis() - fallTimer > 2000) fallState = 0;
      break;
  }
}

// ====================== 电量 ======================
float readBattery() {
  int raw = analogRead(BATT_PIN);
  float v = (raw/4095.0)*3.3*2.0;
  float p = (v-3.0)/(4.2-3.0)*100;
  if (p<0) p=0; if (p>100) p=100;
  return p;
}

// ====================== 编码器中断 ======================
void IRAM_ATTR encodeA() { if (digitalRead(ENC_A)==digitalRead(ENC_B)) encoderCount++; else encoderCount--; }
void IRAM_ATTR encodeB() { if (digitalRead(ENC_A)==digitalRead(ENC_B)) encoderCount--; else encoderCount++; }

// ====================== 按钮（轮询状态机，代替中断计数） ======================
// 编码器机械开关抖动又长又脏，中断边沿计数容易漏/多计。
// 改为 loop() 里轮询：电平稳定 30ms 才算翻转，逐次打印点击数，可靠且可观测。
void pollButton(unsigned long now) {
  static int lastReading = -1;        // 上次原始读数
  static int stableState = -1;        // 已确认的稳定电平
  static unsigned long lastChange = 0;

  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {       // 原始电平变了，重新计时
    lastReading = reading;
    lastChange = now;
  }
  if (stableState != -1 && (now - lastChange) < 30) return;  // 未稳定，忽略
  if (reading == stableState) return;                        // 无翻转

  // ---- 电平稳定翻转，确定是一次真实的按下或松开 ----
  stableState = reading;
  if (reading == buttonPressedLevel) {
    // 按下
    buttonDownTime = now;
    buttonHeld = true;
    longPressTriggered = false;
  } else {
    // 松开：长按后的松开不计入点击；其余计一次
    buttonHeld = false;
    if (!longPressTriggered && (now - buttonDownTime) < LONGPRESS_MS) {
      buttonClickCount++;
      lastClickTime = now;
      Serial.printf("[按钮] 已计 %d 次点击%s\n", buttonClickCount,
                    buttonClickCount >= 3 ? "（够了！）" : "");
    }
  }
}

// ====================== 振动 ======================
void updateMotor(int level) {
  int pwm = level==3?255 : level==2?150 : level==1?80 : 0;
  ledcWrite(MOTOR_PIN, pwm);
}

// ====================== BLE 发送 ======================
void sendBLE(String msg) {
  if (deviceConnected) { pTxCharacteristic->setValue(msg.c_str()); pTxCharacteristic->notify(); }
}

// ====================== 展示 / 调试 ======================
// 启动横幅
void printBanner() {
  Serial.println();
  Serial.println("==============================================");
  Serial.println("     SmartCane-S3  智能盲杖主控");
  Serial.println("     本机 MAC: " + WiFi.macAddress());
  Serial.print("     对端(前哨) MAC: ");
  for (int i = 0; i < 6; i++) { if (scoutMac[i] < 0x10) Serial.print("0"); Serial.print(scoutMac[i], HEX); if (i < 5) Serial.print(":"); }
  Serial.println("\n==============================================");
}

// 启动自检：逐个报告模块状态
void selfTest() {
  Serial.println("\n---------- 启动自检 ----------");
  Wire.beginTransmission(MPU6050_ADDR);
  bool mpuOk = (Wire.endTransmission() == 0);
  Serial.printf("  [MPU6050]  %s\n", mpuOk ? "OK ✓" : "未找到 ✗（跌倒检测将禁用）");
  Serial.printf("  [SYN6288]  %s\n", "串口就绪 ✓");
  Serial.printf("  [GPS]      %s\n", gpsSerial.available() ? "有数据流 ✓" : "等待信号…");
  Serial.printf("  [ESP-NOW]  %s\n", espNowReady ? "就绪 ✓" : "失败 ✗");
  Serial.printf("  [前哨]     %s\n", (millis() - highDistTime < 2000) ? "在线 ✓" : "等待数据…");
  Serial.printf("  [BLE]      %s\n", "广播中…");
  Serial.println("------------------------------");
}

// 完整状态汇总（status 命令触发，不再自动刷屏）
void printStatus() {
  Serial.println("\n---------- 当前状态 ----------");
  Serial.printf("  低距=%.0fcm  高距=%.0fcm  融合等级=%d\n", lowDist, highDist, fusedLevel);
  Serial.printf("  模式=%s  电量=%.0f%%\n", modeNames[workMode], batteryPct);
  Serial.printf("  加速度 X=%.2f Y=%.2f Z=%.2f  合=%.2fg  跌倒状态=%d  MPU=%s\n", accX, accY, accZ, accMag, fallState, mpuOnline ? "在线" : "离线");
  Serial.printf("  主动报警=%d  倒计时=%d  编码器计数=%ld\n", manualAlarm ? 1 : 0, fallCountdown ? 1 : 0, encoderCount);
  Serial.printf("  GPS: %s  (%.6f, %.6f)  卫星=%d\n", gpsValid ? "有效" : "无效", gpsLat, gpsLng, gps.satellites.value());
  Serial.printf("  BLE=%s  前哨=%s  自动控制=%s\n",
                deviceConnected ? "已连" : "未连",
                (highDist >= 0) ? "在线" : "离线",
                autoLoop ? "开" : "暂停");
  Serial.println("------------------------------");
}

// 命令分发：演示/调试用。输入 help 查看全部命令。
void processCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  Serial.println("CMD> " + cmd);

  if (cmd == "help" || cmd == "?") {
    Serial.println("命令列表:");
    Serial.println("  status    打印完整状态");
    Serial.println("  selftest  重新自检模块");
    Serial.println("  tts       测试语音（依次播报）");
    Serial.println("  vol <0-16>     设置语音音量（0静音 16最大）");
    Serial.println("  buzzer         蜂鸣器扫频测试（低频→高频）");
    Serial.println("  motor <0-255>  设置振动强度");
    Serial.println("  mpu       打印加速度");
    Serial.println("  gps       打印 GPS 数据");
    Serial.println("  dist      打印当前距离");
    Serial.println("  btn       打印按钮电平/状态（排查按钮接线）");
    Serial.println("  btnflip   翻转按钮极性（按下电平判断反了时用）");
    Serial.println("  mode <0-2> 切换模式(0正常 1安静 2夜间)");
    Serial.println("  light <RED|GREEN|YELLOW|NONE>  模拟交通灯识别结果");
    Serial.println("  led       灯带测试（红→绿→蓝→白 各1秒）");
    Serial.println("  fall      模拟跌倒(触发30秒倒计时)");
    Serial.println("  alarm     切换主动报警");
    Serial.println("  photo     拍照(通知前哨)");
    Serial.println("  pause / resume  暂停/恢复自动控制");
  } else if (cmd == "led") {
    // 灯带排查：四色循环。全不亮=供电/数据线问题；颜色不对=改 GRB/RGB
    Serial.println("灯带测试：红→绿→蓝→白（各1秒）…");
    const CRGB colors[] = { CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White };
    const char* names[] = { "红", "绿", "蓝", "白" };
    for (int c = 0; c < 4; c++) {
      for (int i = 0; i < WS2811_COUNT; i++) leds[i] = colors[c];
      FastLED.show();
      Serial.printf("  %s\n", names[c]);
      delay(1000);
    }
    FastLED.clear(true);
    Serial.println("灯带测试结束");
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "selftest") {
    selfTest();
  } else if (cmd == "tts") {
    Serial.println("测试语音…");
    tts_speak(msg_fall, sizeof(msg_fall));
    tts_speak(msg_lowbattery, sizeof(msg_lowbattery));
    tts_speak(msg_alarmed, sizeof(msg_alarmed));
  } else if (cmd.startsWith("vol ")) {
    int v = cmd.substring(4).toInt();
    if (v >= 0 && v <= 16) { tts_setVolume(v); tts_speak(msg_alarmed, sizeof(msg_alarmed)); }
    else { Serial.println("用法: vol <0-16>"); }
  } else if (cmd.startsWith("motor ")) {
    int v = cmd.substring(6).toInt();
    if (v < 0) v = 0; if (v > 255) v = 255;
    ledcWrite(MOTOR_PIN, v);
    Serial.printf("振动马达 PWM=%d（pause 下不会被覆盖）\n", v);
  } else if (cmd == "motor") {
    ledcWrite(MOTOR_PIN, 0);
    Serial.println("振动马达关闭");
  } else if (cmd == "buzzer") {
    Serial.println("蜂鸣器扫频测试…");
    for (int f = 600; f <= 3000; f += 400) {
      Serial.printf("  %dHz\n", f);
      ledcWriteTone(BUZZER_PIN, f);
      delay(250);
    }
    ledcWriteTone(BUZZER_PIN, 0);
    Serial.println("完成");
  } else if (cmd == "mpu") {
    mpuRead();
    Serial.printf("加速度 X=%.2f Y=%.2f Z=%.2f 合=%.2fg\n", accX, accY, accZ, accMag);
  } else if (cmd == "gps") {
    Serial.printf("GPS 有效=%d  (%.6f, %.6f)  卫星=%d\n", gpsValid, gpsLat, gpsLng, gps.satellites.value());
  } else if (cmd == "dist") {
    Serial.printf("低距=%.0fcm  高距=%.0fcm\n", lowDist, highDist);
  } else if (cmd == "btn") {
    // 实时按钮状态：按住按钮输入此命令，可看电平是否变化、中断是否触发
    Serial.printf("按钮电平=%d  松开态=%d  按下态=%d  buttonHeld=%d  点击数=%d\n",
      digitalRead(BUTTON_PIN), buttonIdleLevel, buttonPressedLevel,
      buttonHeld ? 1 : 0, buttonClickCount);
  } else if (cmd == "btnflip") {
    // 翻转按钮极性（若按住时电平没变到"按下态"，说明极性判断反了）
    int t = buttonPressedLevel; buttonPressedLevel = buttonIdleLevel; buttonIdleLevel = t;
    Serial.printf("已翻转：松开态=%d 按下态=%d\n", buttonIdleLevel, buttonPressedLevel);
  } else if (cmd.startsWith("mode ")) {
    int m = cmd.substring(5).toInt();
    if (m >= 0 && m <= 2) {
      setMode(m);
    } else { Serial.println("用法: mode <0-2>"); }
  } else if (cmd.startsWith("light ")) {
    String l = cmd.substring(6); l.trim(); l.toUpperCase();
    if (l == "RED") setTrafficLight(1);
    else if (l == "YELLOW") setTrafficLight(2);
    else if (l == "GREEN") setTrafficLight(3);
    else if (l == "NONE") setTrafficLight(0);
    else Serial.println("用法: light <RED|GREEN|YELLOW|NONE>");
  } else if (cmd == "fall") {
    Serial.println(">>> 模拟跌倒，开始30秒倒计时（旋转旋钮或按按钮可取消）");
    fallDetected = true; fallCountdown = true; fallState = 2;
    fallCountdownStart = millis(); fallVoicePlayed = false;
    encoderBaseFall = encoderCount;
    sendBLE("FALL:1\n");
  } else if (cmd == "alarm") {
    manualAlarm = !manualAlarm;
    if (manualAlarm) {
      Serial.println("主动报警：开");
      sendBLE("ALARM:MANUAL\n"); ledcWrite(MOTOR_PIN, 255);
    } else {
      Serial.println("主动报警：关");
      sendBLE("ALARM:CANCEL\n"); ledcWrite(MOTOR_PIN, 0);
    }
  } else if (cmd == "photo" || cmd == "capture") {
    Serial.println("拍照：通知前哨预热，并通知手机拉取 /capture");
    sendCmdToScout(1); sendBLE("CAMERA:CAPTURE\n");
  } else if (cmd == "pause") {
    autoLoop = false;
    ledcWrite(MOTOR_PIN, 0);
    Serial.println("自动控制已暂停（测距/反馈停止），可单独调试外设");
  } else if (cmd == "resume") {
    autoLoop = true;
    Serial.println("自动控制已恢复");
  } else {
    Serial.println("未知命令，输入 help 查看列表");
  }
}

// 非阻塞串口按行读取：逐字节进缓冲，遇换行才执行命令，不阻塞主循环
void handleSerialCommand() {
  static String lineBuf;
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineBuf.length() > 0) { processCommand(lineBuf); lineBuf = ""; }
    } else {
      lineBuf += (char)c;
      if (lineBuf.length() > 64) lineBuf = "";  // 防止缓冲过长
    }
  }
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== 盲杖启动 =====");

  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT); digitalWrite(TRIG_PIN, LOW);
  ledcAttach(MOTOR_PIN, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
  ledcAttach(BUZZER_PIN, 2000, MOTOR_PWM_BITS);  // 蜂鸣器，初始 2kHz

  // WS2811 灯带
  ledStripInit();
  // 开机自检：白光闪一下，确认灯带接线正常（不亮=查供电/数据线）
  for (int i = 0; i < WS2811_COUNT; i++) leds[i] = CRGB(80, 80, 80);
  FastLED.show();
  delay(400);
  FastLED.clear(true);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // 开机检测按钮静态电平作为"松开态"（开机时勿按按钮）
  buttonIdleLevel = digitalRead(BUTTON_PIN);
  buttonPressedLevel = (buttonIdleLevel == HIGH) ? LOW : HIGH;
  Serial.printf("[按钮] 静态电平=%d，按下电平=%d（轮询模式）\n", buttonIdleLevel, buttonPressedLevel);

  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encodeA, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encodeB, CHANGE);

  analogReadResolution(12);

  // MPU6050
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.beginTransmission(MPU6050_ADDR);
  if (Wire.endTransmission() == 0) {
    mpuWrite(0x6B, 0x00); mpuWrite(0x1C, 0x10);
    mpuOnline = true;
    mpuRead();   // 初始化后立即读一次，让自检有真实值，而非全 0
    Serial.println("MPU6050 OK");
  } else {
    Serial.println("[警告] MPU6050 未找到，跌倒检测已禁用！请检查 SDA/SCL 接线");
  }

  // SYN6288
  tts_init();

  // GPS（Serial1，与 TTS 用不同引脚，互不冲突）
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS 就绪（等待卫星信号）");

  // WiFi：连手机热点（与前哨同热点，ESP-NOW 信道自动对齐）
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("WiFi 连接热点中");
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(500); Serial.print("."); wifiTimeout++;
  }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nWiFi 已连 IP: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nWiFi 连接失败，ESP-NOW 用默认信道");

  // ESP-NOW（双向）。用 WiFi 实际信道，与前哨对齐
  uint8_t primaryChan = 1;
  wifi_second_chan_t secChan = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&primaryChan, &secChan);
  if (esp_now_init() == ESP_OK) {
    espNowReady = true;
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, scoutMac, 6);
    peer.channel = primaryChan;   // 用实际信道，不写死
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    Serial.printf("ESP-NOW 就绪 (信道%d)\n", primaryChan);
  }

  // BLE
  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *svc = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic *rx = svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCbs());
  svc->start(); pServer->getAdvertising()->start();

  // 启动横幅 + 自检报告
  printBanner();
  selfTest();
  Serial.println("初始化完成。当前模式：正常");
  Serial.println("操作：三按报警 / 旋转切模式 / 长按拍照");
  Serial.println("串口输入 help 查看调试命令");
}

// ====================== 主循环 ======================
void loop() {
  unsigned long now = millis();
  tts_tick();   // 非阻塞发送队列中的语音
  handleSerialCommand();   // 串口调试命令
  pollButton(now);   // 按钮轮询状态机（消抖+点击计数）

  // ---- 每 200ms 测距 ----
  static unsigned long lastMeasure = 0;
  static int lastType = -1;
  static int lastDanger = -1;
  if (autoLoop && now - lastMeasure >= 200) {
    lastMeasure = now;
    // 前哨掉线/超时则忽略高位距离，避免永久误报悬空障碍
    if (highDist > 0 && (now - highDistTime > HIGH_DIST_TIMEOUT)) highDist = -1;
    lowDist = readDistance();
    int type = fuseDistance(lowDist, highDist);
    int danger = dangerLevel(lowDist, highDist);
    if (fallDetected || manualAlarm) danger = 3;
    fusedLevel = danger;
    updateMotor(danger);

    // 蜂鸣器距离编码：障碍存在时按节奏发短音（事件触发、障碍消失即停）。
    // 正常/夜间模式响；安静模式不响蜂鸣器（仅振动），避免吵闹。
    if (workMode != 1 && type != 0 && danger != 0 && !fallDetected && !manualAlarm) {
      playObstacleBeep(type, danger, now);
    } else {
      ledcWriteTone(BUZZER_PIN, 0);  // 安静模式或无障碍时关蜂鸣器
    }

    // 调试打印：每 500ms 持续打印一次，便于实时观察数值变化
    static unsigned long lastPrint = 0;
    if (now - lastPrint > 500) {
      lastPrint = now;
      const char* typeName = (type==1)?"大型(上下都有)":(type==2)?"低位(下方)":(type==3)?"悬空(上方)":"安全";
      const char* beepName = (type==2)?"低频800Hz":(type==3)?"高频2500Hz":(type==1)?"交替800/2500Hz":"静音";
      Serial.printf("[测距] 低=%5.0f 高=%5.0f 类型=%-6s 等级=%d 模式=%s 蜂鸣=%s\n",
        lowDist, highDist,
        obstacleName(type).c_str(), danger, modeNames[workMode],
        (workMode!=1 && type!=0 && danger!=0) ? beepName : "关");
    }
  }

  // ---- 灯带更新（每次循环都更新，呼吸灯/闪烁需要高频刷新）----
  int curDanger = (fallDetected || manualAlarm) ? 3 : fusedLevel;
  updateLEDStrip(workMode, curDanger);

  // 状态汇总：不再每 3 秒自动刷屏，改为仅在跌倒状态变化时打印，或用 status 命令查看
  static int lastFallState = -1;
  if (fallState != lastFallState) {
    lastFallState = fallState;
    Serial.printf("[MPU] 加速度合=%.2fg 跌倒状态机=%d\n", accMag, fallState);
  }

  // ---- 每 100ms MPU6050 ----
  static unsigned long lastMpu = 0;
  if (now - lastMpu >= 100) { lastMpu = now; mpuRead(); checkFall(); }

  // ---- GPS：非阻塞读取 NMEA ----
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
  if (gps.location.isUpdated()) {
    gpsValid = gps.location.isValid();
    if (gpsValid) { gpsLat = gps.location.lat(); gpsLng = gps.location.lng(); }
  }

  // ---- 每 5 秒电量 ----
  static unsigned long lastBatt = 0;
  static bool lowBattPlayed = false;   // 低电量语音是否已播报（全局共享）
  if (now - lastBatt >= 5000) {
    lastBatt = now;
    batteryPct = readBattery();
    if (batteryPct < 20) {
      // 低电量语音提醒（安静模式下也播报）
      if (!lowBattPlayed) { tts_speak(msg_lowbattery, sizeof(msg_lowbattery)); lowBattPlayed = true; }
    } else {
      lowBattPlayed = false;  // 电量恢复，允许下次再次播报
    }
  }

  // ---- 每 500ms BLE 发送 ----
  static unsigned long lastBle = 0;
  if (now - lastBle >= 500) {
    lastBle = now;
    int type = fuseDistance(lowDist, highDist);
    int danger = dangerLevel(lowDist, highDist);
    String msg = "DIST:" + String((int)lowDist) + ",HIGH:" + String((int)highDist)
      + ",TYPE:" + obstacleName(type) + ",LEVEL:" + String(danger)
      + ",MODE:" + String(workMode) + ",BATT:" + String((int)batteryPct)
      + ",ALARM:" + String(manualAlarm ? "MANUAL" : (fallDetected ? "FALL" : "NORMAL"))
      + ",FALLST:" + String(fallState)
      + ",ENC:" + String(encoderCount)
      + ",ACC:" + String(accMag, 2)
      + ",MPU:" + String(mpuOnline ? 1 : 0)
      + ",LAT:" + (gpsValid ? String(gpsLat, 6) : "0")
      + ",LNG:" + (gpsValid ? String(gpsLng, 6) : "0")
      + ",LIGHT:" + String(lightNames[trafficLight]) + "\n";
    sendBLE(msg);
  }

  // ---- 按钮长按 1.5 秒 → 拍照 ----
  if (buttonHeld && !longPressTriggered && (now - buttonDownTime > LONGPRESS_MS)) {
    longPressTriggered = true;
    Serial.println("[长按] 拍照！通知前哨");
    // 短振一下作为确认反馈（按住 1.5 秒时能感觉到）
    ledcWrite(MOTOR_PIN, 200); delay(150); ledcWrite(MOTOR_PIN, 0);
    sendCmdToScout(1);
    for (int i = 0; i < 3; i++) { sendBLE("CAMERA:CAPTURE\n"); delay(80); }
  }

  // ---- 连按判定：松开超过 800ms 且期间无新点击 → 结算次数 ----
  if (buttonClickCount > 0 && !buttonHeld && (now - lastClickTime > CLICK_GAP_MS)) {
    if (buttonClickCount >= 3) {
      manualAlarm = !manualAlarm;
      if (manualAlarm) {
        Serial.println("[三连按] 主动报警！");
        for (int i = 0; i < 3; i++) { sendBLE("ALARM:MANUAL\n"); delay(100); }
        ledcWrite(MOTOR_PIN, 255);
        tts_speak(msg_alarmed, sizeof(msg_alarmed));
      } else {
        Serial.println("[三连按] 取消报警");
        for (int i = 0; i < 3; i++) { sendBLE("ALARM:CANCEL\n"); delay(80); }
        ledcWrite(MOTOR_PIN, 0);
        Serial.println("报警已取消，振动停止");
      }
    } else {
      Serial.printf("[按钮] %d 连按（无动作，需 3 连按触发报警）\n", buttonClickCount);
    }
    buttonClickCount = 0;
  }

  // ---- 旋转切换模式（EC11 每圈20脉冲，10脉冲≈半圈）----
  long delta = encoderCount - encoderBase;
  if (abs(delta) >= 10) {
    encoderBase = encoderCount;
    setMode((workMode + 1) % 3);
  }

  // ---- 跌倒 30 秒倒计时 ----
  if (fallCountdown) {
    static unsigned long lastPrompt = 0;
    static int lastSec = -1;
    int remaining = 30 - (int)((now - fallCountdownStart) / 1000);

    // 进入跌倒时播报一次（合成一帧发送，句间由逗号自然停顿，无空隙）
    if (!fallVoicePlayed) {
      fallVoicePlayed = true;
      tts_speak(msg_fall_full, sizeof(msg_fall_full));
    }

    // 每秒振动短促 + 串口提示（无蜂鸣器，用振动反馈倒计时）
    if (remaining != lastSec && remaining > 0) {
      lastSec = remaining;
      Serial.printf("[跌倒] 倒计时 %d秒\n", remaining);
      ledcWrite(MOTOR_PIN, 200);
      delay(60);
      ledcWrite(MOTOR_PIN, 255);  // 跌倒期间持续强振
    }

    // 每 3 秒重发 FALL:1
    if (now - lastPrompt > 3000) {
      lastPrompt = now;
      sendBLE("FALL:1\n");
    }

    // 30 秒到 → 确认报警
    if (now - fallCountdownStart > 30000) {
      fallCountdown = false;
      Serial.println("[跌倒] 30秒到！自动家属报警");
      for (int i = 0; i < 3; i++) { sendBLE("FALL:CONFIRMED\n"); delay(100); }
      ledcWrite(MOTOR_PIN, 255);
      tts_speak(msg_alarmed, sizeof(msg_alarmed));
    }

    // 旋钮或按键取消（用跌倒起始基准，与切模式互不影响）
    if (abs(encoderCount - encoderBaseFall) >= 4 || buttonClickCount > 0) {
      fallCountdown = false;
      fallDetected = false;
      fallState = 0;
      buttonClickCount = 0;
      // 同步切模式基准，避免取消动作被误判为切模式
      encoderBase = encoderCount;
      Serial.println("[跌倒] 已取消");
      sendBLE("FALL:CANCELLED\n");
      ledcWrite(MOTOR_PIN, 0);
    }
  }
}
