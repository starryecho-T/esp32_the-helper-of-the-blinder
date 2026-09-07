#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =====================================================
// GPIO 配置
// =====================================================

static const uint8_t TRIG_PIN = 4;
static const uint8_t ECHO_PIN = 5;
static const uint8_t MOTOR_PIN = 6;
static const uint8_t BUZZER_PIN = 7;

// =====================================================
// BLE 配置
// =====================================================

static const char* DEVICE_NAME = "SmartCane-S3";

// BLE 服务 UUID
static const char* SERVICE_UUID =
  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";

// 手机 App -> ESP32-S3，Write
static const char* RX_UUID =
  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

// ESP32-S3 -> 手机 App，Notify
static const char* TX_UUID =
  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// =====================================================
// PWM 配置
// 适用于 Arduino-ESP32 Core 3.x
// =====================================================

static const uint32_t MOTOR_PWM_FREQ = 5000;
static const uint8_t MOTOR_PWM_BITS = 8;

static const uint32_t BUZZER_PWM_FREQ = 2000;
static const uint8_t BUZZER_PWM_BITS = 8;

// =====================================================
// BLE 对象
// =====================================================

BLEServer* bleServer = nullptr;
BLECharacteristic* txCharacteristic = nullptr;
BLECharacteristic* rxCharacteristic = nullptr;

// =====================================================
// 工作状态
// =====================================================

bool deviceConnected = false;
bool oldDeviceConnected = false;

// 当前工作模式
// DAILY、NIGHT、SILENT、EMERGENCY
String workMode = "DAILY";

// 是否处于主动报警或紧急反馈状态
bool manualAlarm = false;

// 当前测距结果
float currentDistanceCm = -1.0f;

// 当前障碍状态
// NO_ECHO、SAFE、NOTICE、WARNING、DANGER
String currentState = "NO_ECHO";

// 报警状态
// NORMAL、MANUAL_ACTIVE、CANCELLED
String alarmState = "NORMAL";

// =====================================================
// 定时变量
// =====================================================

unsigned long lastMeasureMillis = 0;
unsigned long lastNotifyMillis = 0;
unsigned long lastBuzzerMillis = 0;
unsigned long buzzerOnUntilMillis = 0;

// =====================================================
// 函数声明
// =====================================================

void sendText(const String& text);
void handleCommand(String command);
void stopBuzzer();
void updateFeedback();
float readDistanceCm();
String classifyDistance(float distance);
void sendCurrentStatus();

// =====================================================
// BLE 服务器回调
// =====================================================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    deviceConnected = true;

    Serial.println("BLE client connected");
  }

  void onDisconnect(BLEServer* server) override {
    deviceConnected = false;

    Serial.println("BLE client disconnected");
  }
};

// =====================================================
// BLE 接收回调
// 手机向 RX_UUID 写入数据时触发
// =====================================================

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();

    Serial.print("RX: ");
    Serial.println(value);

    value.trim();

    if (value.length() == 0) {
      return;
    }

    /*
       每次 BLE Write 默认作为一条完整命令处理。

       例如 App 发送：
       MODE:DAILY

       或者：
       MODE:DAILY\n

       两种情况都可以处理。
    */

    int startIndex = 0;

    for (int i = 0; i <= value.length(); i++) {
      bool endOfCommand = false;

      if (i == value.length()) {
        endOfCommand = true;
      } else {
        char c = value.charAt(i);

        if (c == '\n' || c == '\r') {
          endOfCommand = true;
        }
      }

      if (endOfCommand) {
        String oneCommand = value.substring(startIndex, i);
        oneCommand.trim();

        if (oneCommand.length() > 0) {
          handleCommand(oneCommand);
        }

        startIndex = i + 1;
      }
    }
  }
};

// =====================================================
// BLE 发送字符串
// ESP32-S3 通过 TX_UUID Notify 给手机
// =====================================================

void sendText(const String& text) {
  if (!deviceConnected || txCharacteristic == nullptr) {
    return;
  }

  String packet = text;

  // 每条 Notify 数据以换行结束
  if (!packet.endsWith("\n")) {
    packet += "\n";
  }

  txCharacteristic->setValue(packet.c_str());
  txCharacteristic->notify();

  Serial.print("TX: ");
  Serial.print(packet);
}

// =====================================================
// 停止蜂鸣器
// =====================================================

void stopBuzzer() {
  ledcWriteTone(BUZZER_PIN, 0);
  ledcWrite(BUZZER_PIN, 0);

  buzzerOnUntilMillis = 0;
}

// =====================================================
// 发送当前完整状态
// =====================================================

void sendCurrentStatus() {
  String distanceText = "NO_ECHO";

  if (currentDistanceCm >= 0.0f) {
    distanceText = String(currentDistanceCm, 1);
  }

  /*
     示例：

     STATUS:MODE=DAILY,DIST=125.4,STATE=NOTICE,ALARM=NORMAL
  */

  String statusText =
    "STATUS:MODE=" + workMode +
    ",DIST=" + distanceText +
    ",STATE=" + currentState +
    ",ALARM=" + alarmState;

  sendText(statusText);
}

// =====================================================
// 处理手机发送的命令
// =====================================================

void handleCommand(String command) {
  command.trim();
  command.toUpperCase();

  Serial.print("COMMAND: ");
  Serial.println(command);

  // ===================================================
  // 查询当前状态
  // 支持：
  // STATUS
  // STATUS?
  // STATUS:GET
  // ===================================================

  if (command == "STATUS" ||
      command == "STATUS?" ||
      command == "STATUS:GET") {

    sendCurrentStatus();
  }

  // ===================================================
  // 日常模式
  // 兼容 MODE:NORMAL
  // ===================================================

  else if (command == "MODE:DAILY" ||
           command == "MODE:NORMAL") {

    workMode = "DAILY";
    manualAlarm = false;
    alarmState = "NORMAL";

    stopBuzzer();

    sendText("ACK:MODE:DAILY");
    sendCurrentStatus();
  }

  // ===================================================
  // 夜间模式
  // ===================================================

  else if (command == "MODE:NIGHT") {

    workMode = "NIGHT";
    manualAlarm = false;
    alarmState = "NORMAL";

    stopBuzzer();

    /*
       当前代码还没有连接 WS2812 灯带。
       目前先完成模式记录和反馈逻辑。
    */

    sendText("ACK:MODE:NIGHT");
    sendCurrentStatus();
  }

  // ===================================================
  // 安静模式
  // 关闭振动和蜂鸣器反馈
  // ===================================================

  else if (command == "MODE:SILENT") {

    workMode = "SILENT";
    manualAlarm = false;
    alarmState = "NORMAL";

    stopBuzzer();
    ledcWrite(MOTOR_PIN, 0);

    sendText("ACK:MODE:SILENT");
    sendCurrentStatus();
  }

  // ===================================================
  // 紧急模式
  // 持续振动和蜂鸣
  // ===================================================

  else if (command == "MODE:EMERGENCY") {

    workMode = "EMERGENCY";
    manualAlarm = true;
    alarmState = "MANUAL_ACTIVE";

    sendText("ACK:MODE:EMERGENCY");
    sendCurrentStatus();
  }

  // ===================================================
  // 主动报警
  // ===================================================

  else if (command == "ALARM:MANUAL" ||
           command == "BUZZER:ON") {

    manualAlarm = true;
    alarmState = "MANUAL_ACTIVE";

    sendText("ACK:ALARM:MANUAL");
    sendCurrentStatus();
  }

  // ===================================================
  // 取消报警
  // ===================================================

  else if (command == "ALARM:CANCEL" ||
           command == "BUZZER:OFF") {

    manualAlarm = false;
    alarmState = "CANCELLED";

    stopBuzzer();
    ledcWrite(MOTOR_PIN, 0);

    sendText("ACK:ALARM:CANCEL");
    sendCurrentStatus();
  }

  // ===================================================
  // 未知命令
  // ===================================================

  else {
    sendText("ERR:UNKNOWN_COMMAND");
  }
}

// =====================================================
// HC-SR04 测距
// 返回：距离，单位 cm
// 返回 -1：没有收到有效回波
// =====================================================

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // 最长等待 30 ms，避免程序长时间阻塞
  unsigned long duration =
    pulseIn(ECHO_PIN, HIGH, 30000UL);

  if (duration == 0) {
    return -1.0f;
  }

  // 声速约为 0.0343 cm/us
  // 声音经历往返，所以除以 2
  float distance = duration * 0.0343f / 2.0f;

  return distance;
}

// =====================================================
// 根据距离判断障碍等级
// =====================================================

String classifyDistance(float distance) {
  if (distance < 0.0f) {
    return "NO_ECHO";
  }

  if (distance < 30.0f) {
    return "DANGER";
  }

  if (distance < 80.0f) {
    return "WARNING";
  }

  if (distance < 150.0f) {
    return "NOTICE";
  }

  return "SAFE";
}

// =====================================================
// 蜂鸣器短音
// =====================================================

void beepOnce(uint16_t frequency, uint16_t durationMs) {
  ledcWriteTone(BUZZER_PIN, frequency);

  buzzerOnUntilMillis = millis() + durationMs;
}

// =====================================================
// 更新振动马达和蜂鸣器
// =====================================================

void updateFeedback() {
  unsigned long now = millis();

  // ===================================================
  // 主动报警或紧急模式
  // ===================================================

  if (manualAlarm || workMode == "EMERGENCY") {
    ledcWrite(MOTOR_PIN, 220);
    ledcWriteTone(BUZZER_PIN, 2000);

    return;
  }

  // ===================================================
  // 安静模式关闭所有反馈
  // ===================================================

  if (workMode == "SILENT") {
    ledcWrite(MOTOR_PIN, 0);
    stopBuzzer();

    return;
  }

  // ===================================================
  // 无回波或安全状态关闭反馈
  // ===================================================

  if (currentState == "SAFE" ||
      currentState == "NO_ECHO") {

    ledcWrite(MOTOR_PIN, 0);
    stopBuzzer();

    return;
  }

  // ===================================================
  // 蜂鸣器短音结束后关闭
  // ===================================================

  if (buzzerOnUntilMillis != 0 &&
      now >= buzzerOnUntilMillis) {

    stopBuzzer();
  }

  // ===================================================
  // 较远障碍：低强度、低频率
  // ===================================================

  if (currentState == "NOTICE") {
    ledcWrite(MOTOR_PIN, 70);

    if (now - lastBuzzerMillis >= 1200) {
      lastBuzzerMillis = now;

      beepOnce(1200, 180);
    }
  }

  // ===================================================
  // 中距离障碍：中强度、中频率
  // ===================================================

  else if (currentState == "WARNING") {
    ledcWrite(MOTOR_PIN, 150);

    if (now - lastBuzzerMillis >= 600) {
      lastBuzzerMillis = now;

      beepOnce(1800, 180);
    }
  }

  // ===================================================
  // 近距离障碍：强振动、持续蜂鸣
  // ===================================================

  else if (currentState == "DANGER") {
    ledcWrite(MOTOR_PIN, 255);
    ledcWriteTone(BUZZER_PIN, 2400);
  }
}

// =====================================================
// 初始化 BLE
// =====================================================

void setupBle() {
  BLEDevice::init(DEVICE_NAME);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService* service =
    bleServer->createService(SERVICE_UUID);

  // ---------------------------------------------------
  // TX：ESP32-S3 -> 手机，Notify
  // ---------------------------------------------------

  txCharacteristic = service->createCharacteristic(
    TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );

  txCharacteristic->addDescriptor(new BLE2902());

  // ---------------------------------------------------
  // RX：手机 -> ESP32-S3，Write
  // ---------------------------------------------------

  rxCharacteristic = service->createCharacteristic(
    RX_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );

  rxCharacteristic->setCallbacks(new RxCallbacks());

  service->start();

  // ---------------------------------------------------
  // BLE 广播
  // ---------------------------------------------------

  BLEAdvertising* advertising =
    BLEDevice::getAdvertising();

  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();

  Serial.println("BLE advertising started");
  Serial.print("Device name: ");
  Serial.println(DEVICE_NAME);
}

// =====================================================
// Arduino 初始化
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  // HC-SR04
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(TRIG_PIN, LOW);

  // ESP32 Arduino Core 3.x PWM API
  ledcAttach(
    MOTOR_PIN,
    MOTOR_PWM_FREQ,
    MOTOR_PWM_BITS
  );

  ledcAttach(
    BUZZER_PIN,
    BUZZER_PWM_FREQ,
    BUZZER_PWM_BITS
  );

  ledcWrite(MOTOR_PIN, 0);
  stopBuzzer();

  setupBle();

  Serial.println("Smart cane firmware started");
}

// =====================================================
// 主循环
// =====================================================

void loop() {
  unsigned long now = millis();

  // ---------------------------------------------------
  // 每 200 ms 测量一次距离
  // ---------------------------------------------------

  if (now - lastMeasureMillis >= 200) {
    lastMeasureMillis = now;

    currentDistanceCm = readDistanceCm();
    currentState = classifyDistance(currentDistanceCm);

    updateFeedback();

    Serial.print("DIST=");

    if (currentDistanceCm < 0.0f) {
      Serial.print("NO_ECHO");
    } else {
      Serial.print(currentDistanceCm, 1);
      Serial.print(" cm");
    }

    Serial.print(", STATE=");
    Serial.println(currentState);
  }

  // ---------------------------------------------------
  // 每 500 ms 向手机发送一次完整状态
  // ---------------------------------------------------

  if (deviceConnected &&
      now - lastNotifyMillis >= 500) {

    lastNotifyMillis = now;

    sendCurrentStatus();
  }

  // ---------------------------------------------------
  // 断线后重新开始 BLE 广播
  // ---------------------------------------------------

  if (!deviceConnected && oldDeviceConnected) {
    delay(300);

    bleServer->startAdvertising();

    oldDeviceConnected = false;

    Serial.println("BLE advertising restarted");
  }

  // ---------------------------------------------------
  // 新连接建立后发送欢迎消息和当前状态
  // ---------------------------------------------------

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;

    sendText("HELLO:SMART_CANE_S3");
    sendCurrentStatus();
  }

  delay(5);
}