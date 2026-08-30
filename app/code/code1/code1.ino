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

static const char* SERVICE_UUID =
  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";

// ESP32-S3 -> 手机，Notify
static const char* TX_UUID =
  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

// 手机 -> ESP32-S3，Write
static const char* RX_UUID =
  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// =====================================================
// PWM 配置，适用于 ESP32 Arduino Core 3.x
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

String rxCommand = "";
String workMode = "NORMAL";

// true 时强制开启振动和蜂鸣器
bool manualBuzzer = false;

float currentDistanceCm = -1.0f;
String currentState = "NO_ECHO";

// =====================================================
// 定时变量
// =====================================================
unsigned long lastMeasureMillis = 0;
unsigned long lastNotifyMillis = 0;
unsigned long lastBuzzerMillis = 0;
unsigned long buzzerOnUntilMillis = 0;

// =====================================================
// 函数声明
// 必须放在回调类之前，避免“函数未声明”编译错误
// =====================================================
void sendText(const String& text);
void handleCommand(String command);
void stopBuzzer();
void updateFeedback();
float readDistanceCm();
String classifyDistance(float distance);

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
// 注意：getValue() 在当前 ESP32 BLE 库中返回 Arduino String
// =====================================================
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();

    Serial.print("RX: ");
    Serial.println(value);

    // 逐个字符解析，支持一条写入中包含多条命令
    for (int i = 0; i < value.length(); i++) {
      char c = value.charAt(i);

      if (c == '\n' || c == '\r') {
        if (rxCommand.length() > 0) {
          handleCommand(rxCommand);
          rxCommand = "";
        }
      } else {
        // 限制命令长度，避免异常数据无限增长
        if (rxCommand.length() < 80) {
          rxCommand += c;
        }
      }
    }
  }
};

// =====================================================
// BLE 发送字符串
// =====================================================
void sendText(const String& text) {
  if (!deviceConnected || txCharacteristic == nullptr) {
    return;
  }

  String packet = text;

  // 每条数据统一以换行符结束
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
// 处理手机发送的命令
// 手机发送的每条命令必须以 \n 或 \r\n 结束
// =====================================================
void handleCommand(String command) {
  command.trim();
  command.toUpperCase();

  Serial.print("COMMAND: ");
  Serial.println(command);

  if (command == "STATUS?" || command == "STATUS") {
    String distanceText = "NO_ECHO";

    if (currentDistanceCm >= 0.0f) {
      distanceText = String(currentDistanceCm, 1);
    }

    sendText("STATUS:MODE=" + workMode +
             ",DIST=" + distanceText +
             ",STATE=" + currentState);
  }
  else if (command == "MODE:NORMAL") {
    workMode = "NORMAL";
    manualBuzzer = false;
    sendText("ACK:MODE:NORMAL");
  }
  else if (command == "MODE:SILENT") {
    workMode = "SILENT";
    manualBuzzer = false;
    stopBuzzer();
    ledcWrite(MOTOR_PIN, 0);
    sendText("ACK:MODE:SILENT");
  }
  else if (command == "MODE:NIGHT") {
    // 当前阶段暂时只记录模式，尚未连接灯带
    workMode = "NIGHT";
    manualBuzzer = false;
    sendText("ACK:MODE:NIGHT");
  }
  else if (command == "BUZZER:ON") {
    manualBuzzer = true;
    sendText("ACK:BUZZER:ON");
  }
  else if (command == "BUZZER:OFF") {
    manualBuzzer = false;
    stopBuzzer();
    ledcWrite(MOTOR_PIN, 0);
    sendText("ACK:BUZZER:OFF");
  }
  else {
    sendText("ERR:UNKNOWN_COMMAND");
  }
}

// =====================================================
// HC-SR04 测距
// 返回值：距离，单位 cm
// 返回 -1：没有收到有效回波
// =====================================================
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // 30000 us 超时，避免无回波时程序长时间阻塞
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);

  if (duration == 0) {
    return -1.0f;
  }

  // 声速约为 0.0343 cm/us，除以 2 是因为声音往返传播
  return duration * 0.0343f / 2.0f;
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
// 蜂鸣器短音控制
// =====================================================
void beepOnce(uint16_t frequency, uint16_t durationMs) {
  ledcWriteTone(BUZZER_PIN, frequency);
  buzzerOnUntilMillis = millis() + durationMs;
}

// =====================================================
// 振动和蜂鸣器反馈
// =====================================================
void updateFeedback() {
  unsigned long now = millis();

  // 强制报警模式
  if (manualBuzzer) {
    ledcWrite(MOTOR_PIN, 180);
    ledcWriteTone(BUZZER_PIN, 1800);
    return;
  }

  // 静音、无回波或安全状态都关闭反馈
  if (workMode == "SILENT" ||
      currentState == "SAFE" ||
      currentState == "NO_ECHO") {
    ledcWrite(MOTOR_PIN, 0);
    stopBuzzer();
    return;
  }

  // 蜂鸣音结束后关闭蜂鸣器
  if (buzzerOnUntilMillis != 0 && now >= buzzerOnUntilMillis) {
    stopBuzzer();
  }

  if (currentState == "NOTICE") {
    ledcWrite(MOTOR_PIN, 70);

    // 每 1200 ms 发出一次 180 ms 的低频提示音
    if (now - lastBuzzerMillis >= 1200) {
      lastBuzzerMillis = now;
      beepOnce(1200, 180);
    }
  }
  else if (currentState == "WARNING") {
    ledcWrite(MOTOR_PIN, 150);

    // 每 600 ms 发出一次 180 ms 的中频提示音
    if (now - lastBuzzerMillis >= 600) {
      lastBuzzerMillis = now;
      beepOnce(1800, 180);
    }
  }
  else if (currentState == "DANGER") {
    ledcWrite(MOTOR_PIN, 255);

    // 危险状态持续鸣叫
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

  BLEService* service = bleServer->createService(SERVICE_UUID);

  // TX：ESP32-S3 向手机发送通知
  txCharacteristic = service->createCharacteristic(
    TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  txCharacteristic->addDescriptor(new BLE2902());

  // RX：手机向 ESP32-S3 写入命令
  rxCharacteristic = service->createCharacteristic(
    RX_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxCharacteristic->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
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

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // ESP32 Arduino Core 3.x PWM API
  ledcAttach(MOTOR_PIN, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
  ledcAttach(BUZZER_PIN, BUZZER_PWM_FREQ, BUZZER_PWM_BITS);

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

  // 每 200 ms 测量一次距离
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

  // 每 500 ms 向手机发送一次距离和状态
  if (deviceConnected && now - lastNotifyMillis >= 500) {
    lastNotifyMillis = now;

    String distanceText = "NO_ECHO";
    if (currentDistanceCm >= 0.0f) {
      distanceText = String(currentDistanceCm, 1);
    }

    sendText("DIST:" + distanceText);
    sendText("STATE:" + currentState);
  }

  // 断线后重新开始广播
  if (!deviceConnected && oldDeviceConnected) {
    delay(300);
    bleServer->startAdvertising();
    oldDeviceConnected = false;
    Serial.println("BLE advertising restarted");
  }

  // 新连接建立后发送欢迎消息
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
    sendText("HELLO:SMART_CANE_S3");
  }

  delay(5);
}