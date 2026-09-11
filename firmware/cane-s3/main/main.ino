#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>

// =====================================================
// GPIO 配置（按你的实际接线）
// =====================================================

static const uint8_t TRIG_PIN = 5;     // 超声波 Trig
static const uint8_t ECHO_PIN = 18;    // 超声波 Echo
static const uint8_t MOTOR_PIN = 4;    // 振动马达

// MPU6050 I2C
static const uint8_t SDA_PIN = 21;
static const uint8_t SCL_PIN = 20;
static const uint8_t MPU6050_ADDR = 0x68;

// =====================================================
// BLE 配置
// =====================================================

static const char* DEVICE_NAME = "SmartCane-S3";

static const char* SERVICE_UUID =
  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";

// 手机 App -> ESP32-S3，Write
static const char* RX_UUID =
  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

// ESP32-S3 -> 手机 App，Notify
static const char* TX_UUID =
  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// =====================================================
// PWM 配置（适用于 Arduino-ESP32 Core 3.x）
// =====================================================

static const uint32_t MOTOR_PWM_FREQ = 5000;
static const uint8_t MOTOR_PWM_BITS = 8;

// =====================================================
// 跌倒检测参数
// =====================================================

static const float FREEFALL_THRESHOLD = 0.4f;  // 失重阈值(g)
static const float FALL_THRESHOLD = 2.5f;       // 撞击阈值(g)

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

String workMode = "DAILY";      // DAILY、NIGHT、SILENT、EMERGENCY
bool manualAlarm = false;       // 主动报警

float currentDistanceCm = -1.0f;
String currentState = "NO_ECHO";  // NO_ECHO、SAFE、NOTICE、WARNING、DANGER

// 报警状态：NORMAL、MANUAL_ACTIVE、FALL_DETECTED、CANCELLED
String alarmState = "NORMAL";

// MPU6050 数据
float accX, accY, accZ;
float gyroX, gyroY, gyroZ;
float accMagnitude;

// 跌倒检测状态机
int fallState = 0;              // 0=正常, 1=失重, 2=撞击
unsigned long fallTimer = 0;
bool fallDetected = false;      // 是否已检测到跌倒

// =====================================================
// 定时变量
// =====================================================

unsigned long lastMeasureMillis = 0;
unsigned long lastNotifyMillis = 0;
unsigned long lastMpuMillis = 0;

// =====================================================
// 函数声明
// =====================================================

void sendText(const String& text);
void handleCommand(String command);
void updateFeedback();
float readDistanceCm();
String classifyDistance(float distance);
void sendCurrentStatus();
bool mpuInit();
void mpuRead();
void mpuWrite(uint8_t reg, uint8_t data);
int16_t mpuRead16(uint8_t reg);
void checkFall();

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
// =====================================================

void sendText(const String& text) {
  if (!deviceConnected || txCharacteristic == nullptr) {
    return;
  }

  String packet = text;

  if (!packet.endsWith("\n")) {
    packet += "\n";
  }

  txCharacteristic->setValue(packet.c_str());
  txCharacteristic->notify();

  Serial.print("TX: ");
  Serial.print(packet);
}

// =====================================================
// MPU6050 驱动
// =====================================================

void mpuWrite(uint8_t reg, uint8_t data) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(data);
  Wire.endTransmission();
}

int16_t mpuRead16(uint8_t reg) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MPU6050_ADDR, 2);
  return (Wire.read() << 8) | Wire.read();
}

bool mpuInit() {
  Wire.begin(SDA_PIN, SCL_PIN);

  Wire.beginTransmission(MPU6050_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("错误：找不到 MPU6050！请检查接线");
    return false;
  }

  // 唤醒
  mpuWrite(0x6B, 0x00);
  // 加速度量程 ±8g
  mpuWrite(0x1C, 0x10);
  // 陀螺仪量程 ±500°/s
  mpuWrite(0x1B, 0x08);

  Serial.println("MPU6050 初始化成功");
  return true;
}

void mpuRead() {
  accX = mpuRead16(0x3B) / 4096.0f;
  accY = mpuRead16(0x3D) / 4096.0f;
  accZ = mpuRead16(0x3F) / 4096.0f;

  gyroX = mpuRead16(0x43) / 65.5f;
  gyroY = mpuRead16(0x45) / 65.5f;
  gyroZ = mpuRead16(0x47) / 65.5f;

  accMagnitude = sqrt(accX * accX + accY * accY + accZ * accZ);
}

// =====================================================
// 跌倒检测状态机：失重 -> 撞击 -> 确认
// =====================================================

void checkFall() {
  switch (fallState) {
    case 0:  // 正常，等待失重
      if (accMagnitude < FREEFALL_THRESHOLD) {
        fallState = 1;
        fallTimer = millis();
        Serial.println("[跌倒检测] 检测到失重");
      }
      break;

    case 1:  // 已失重，等待撞击
      if (accMagnitude > FALL_THRESHOLD) {
        fallState = 2;
        fallDetected = true;
        alarmState = "FALL_DETECTED";
        Serial.print("[跌倒检测] 检测到撞击，加速度=");
        Serial.println(accMagnitude);

        // 立即通知手机
        sendText("FALL:1");
        sendCurrentStatus();
      } else if (millis() - fallTimer > 2000) {
        fallState = 0;  // 超时恢复
        Serial.println("[跌倒检测] 失重超时，恢复正常");
      }
      break;

    case 2:  // 已确认跌倒，等待用户处理
      // 用户通过手机发送 ALARM:CANCEL 后会重置 fallState
      break;
  }
}

// =====================================================
// 发送当前完整状态
// =====================================================

void sendCurrentStatus() {
  String distanceText = "NO_ECHO";

  if (currentDistanceCm >= 0.0f) {
    distanceText = String(currentDistanceCm, 1);
  }

  // 示例：STATUS:MODE=DAILY,DIST=125.4,STATE=NOTICE,ALARM=NORMAL
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

  // 查询当前状态
  if (command == "STATUS" ||
      command == "STATUS?" ||
      command == "STATUS:GET") {

    sendCurrentStatus();
  }

  // 日常模式
  else if (command == "MODE:DAILY" ||
           command == "MODE:NORMAL") {

    workMode = "DAILY";
    manualAlarm = false;
    alarmState = "NORMAL";
    fallDetected = false;
    fallState = 0;

    ledcWrite(MOTOR_PIN, 0);

    sendText("ACK:MODE:DAILY");
    sendCurrentStatus();
  }

  // 夜间模式
  else if (command == "MODE:NIGHT") {
    workMode = "NIGHT";
    manualAlarm = false;
    alarmState = "NORMAL";
    fallDetected = false;
    fallState = 0;

    ledcWrite(MOTOR_PIN, 0);

    sendText("ACK:MODE:NIGHT");
    sendCurrentStatus();
  }

  // 安静模式
  else if (command == "MODE:SILENT") {
    workMode = "SILENT";
    manualAlarm = false;
    alarmState = "NORMAL";
    fallDetected = false;
    fallState = 0;

    ledcWrite(MOTOR_PIN, 0);

    sendText("ACK:MODE:SILENT");
    sendCurrentStatus();
  }

  // 紧急模式
  else if (command == "MODE:EMERGENCY") {
    workMode = "EMERGENCY";
    manualAlarm = true;
    alarmState = "MANUAL_ACTIVE";

    sendText("ACK:MODE:EMERGENCY");
    sendCurrentStatus();
  }

  // 主动报警
  else if (command == "ALARM:MANUAL" ||
           command == "BUZZER:ON") {

    manualAlarm = true;
    alarmState = "MANUAL_ACTIVE";

    sendText("ACK:ALARM:MANUAL");
    sendCurrentStatus();
  }

  // 取消报警（也用于取消跌倒报警）
  else if (command == "ALARM:CANCEL" ||
           command == "BUZZER:OFF") {

    manualAlarm = false;
    alarmState = "CANCELLED";
    fallDetected = false;
    fallState = 0;

    ledcWrite(MOTOR_PIN, 0);

    sendText("ACK:ALARM:CANCEL");
    sendCurrentStatus();
  }

  // 未知命令
  else {
    sendText("ERR:UNKNOWN_COMMAND");
  }
}

// =====================================================
// HC-SR04 测距
// =====================================================

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);

  if (duration == 0) {
    return -1.0f;
  }

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
// 更新振动马达
// =====================================================

void updateFeedback() {
  // 跌倒检测触发：持续强振动
  if (fallDetected) {
    ledcWrite(MOTOR_PIN, 255);
    return;
  }

  // 主动报警或紧急模式
  if (manualAlarm || workMode == "EMERGENCY") {
    ledcWrite(MOTOR_PIN, 220);
    return;
  }

  // 安静模式关闭所有反馈
  if (workMode == "SILENT") {
    ledcWrite(MOTOR_PIN, 0);
    return;
  }

  // 无回波或安全状态关闭反馈
  if (currentState == "SAFE" ||
      currentState == "NO_ECHO") {
    ledcWrite(MOTOR_PIN, 0);
    return;
  }

  // 较远障碍：低强度
  if (currentState == "NOTICE") {
    ledcWrite(MOTOR_PIN, 70);
  }

  // 中距离障碍：中强度
  else if (currentState == "WARNING") {
    ledcWrite(MOTOR_PIN, 150);
  }

  // 近距离障碍：强振动
  else if (currentState == "DANGER") {
    ledcWrite(MOTOR_PIN, 255);
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

  // TX：ESP32-S3 -> 手机，Notify
  txCharacteristic = service->createCharacteristic(
    TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );

  txCharacteristic->addDescriptor(new BLE2902());

  // RX：手机 -> ESP32-S3，Write
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

  Serial.println("\n===== 智能盲杖主固件启动 =====");

  // HC-SR04
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // 振动马达 PWM
  ledcAttach(MOTOR_PIN, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
  ledcWrite(MOTOR_PIN, 0);

  // MPU6050
  if (!mpuInit()) {
    Serial.println("警告：MPU6050 未连接，跌倒检测不可用");
  }

  // BLE
  setupBle();

  Serial.println("初始化完成");
  Serial.println("----------------------------------------");
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
    String newState = classifyDistance(currentDistanceCm);

    // 只在状态变化时打印，减少刷屏
    if (newState != currentState) {
      currentState = newState;
      Serial.print("[状态变化] ");
      Serial.print(currentState);
      if (currentDistanceCm >= 0.0f) {
        Serial.print(" (");
        Serial.print(currentDistanceCm, 0);
        Serial.print("cm)");
      }
      Serial.println();
    }

    updateFeedback();
  }

  // 每 100 ms 读取一次 MPU6050 并检测跌倒
  if (now - lastMpuMillis >= 100) {
    lastMpuMillis = now;

    mpuRead();
    checkFall();
    // 姿态数据不再打印，只在跌倒时打印（checkFall 内部会打印）
  }

  // 每 500 ms 向手机发送一次完整状态
  if (deviceConnected && now - lastNotifyMillis >= 500) {
    lastNotifyMillis = now;
    sendCurrentStatus();
  }

  // 断线后重新开始 BLE 广播
  if (!deviceConnected && oldDeviceConnected) {
    delay(300);
    bleServer->startAdvertising();
    oldDeviceConnected = false;
    Serial.println("[BLE] 断线，重新广播");
  }

  // 新连接建立后发送欢迎消息和当前状态
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
    sendText("HELLO:SMART_CANE_S3");
    sendCurrentStatus();
    Serial.println("[BLE] 手机已连接");
  }

  delay(5);
}