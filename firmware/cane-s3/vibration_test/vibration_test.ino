/**
 * 振动马达测试 + 超声波联动
 * 距离越近，振动越强（这就是盲杖的核心反馈逻辑）
 *
 * 接线：
 *   振动马达 IN → GPIO 4（PWM 引脚）
 *   振动马达 VCC → 5V（外接电源更稳）
 *   振动马达 GND → GND
 *   HC-SR04 Trig → GPIO 5
 *   HC-SR04 Echo → GPIO 18
 *
 * 烧录后：
 *   - 串口看距离 + 振动等级
 *   - 手靠近超声波，振动会变强
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ====================== BLE 参数 ======================
#define DEVICE_NAME "ESP32_BLE_2026"
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ====================== 引脚定义 ======================
#define TRIG_PIN 5
#define ECHO_PIN 18
#define VIB_PIN 4        // 振动马达控制引脚（PWM）

// ====================== 测距与振动参数 ======================
#define MAX_DISTANCE 400
#define SAMPLE_COUNT 5

// 危险距离阈值（cm），分三级
#define DIST_SAFE 150      // >150cm：安全，不振动
#define DIST_WARN 80       // 80-150cm：提醒，慢速弱振动
#define DIST_DANGER 30     // 30-80cm：警告，快速中振动
                          // <30cm：危险，持续强振动

// ====================== 全局变量 ======================
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool isConnected = false;
String receivedData = "";

// 振动控制参数
int vibChannel = 0;        // LEDC 通道
int vibFreq = 1000;        // PWM 频率 1kHz
int vibResolution = 8;     // 8位分辨率（0-255）

// 前向声明
void sendBLEString(String str);
void disconnectBLE();

// ====================== BLE 回调 ======================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    isConnected = true;
    Serial.println("手机已成功连接");
  }
  void onDisconnect(BLEServer* pServer) {
    isConnected = false;
    Serial.println("手机已断开连接");
    pServer->startAdvertising();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    receivedData = pCharacteristic->getValue();
    if (receivedData.length() > 0) {
      Serial.print("收到: ");
      Serial.println(receivedData);
    }
  }
};

// ====================== 超声波测距 ======================
float measureDistance() {
  float totalDistance = 0;
  int validSamples = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration > 0) {
      float distance = duration * 0.0343 / 2.0;
      if (distance > 0 && distance < MAX_DISTANCE) {
        totalDistance += distance;
        validSamples++;
      }
    }
    delay(10);
  }

  if (validSamples > 0) {
    return totalDistance / validSamples;
  }
  return -1;
}

// ====================== 振动马达控制 ======================

// 设置振动强度 0-255
void setVibration(int intensity) {
  if (intensity < 0) intensity = 0;
  if (intensity > 255) intensity = 255;
  ledcWrite(VIB_PIN, intensity);
}

// 根据距离控制振动等级
// 返回值：0=安全, 1=提醒, 2=警告, 3=危险
int getVibrationLevel(float distance) {
  if (distance < 0 || distance > DIST_SAFE) {
    setVibration(0);
    return 0;
  } else if (distance < DIST_DANGER) {
    setVibration(255);
    return 3;
  } else if (distance < DIST_WARN) {
    setVibration(180);
    return 2;
  } else {
    setVibration(80);
    return 1;
  }
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== 智能盲杖 - 超声波+振动联动 =====");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // 初始化振动马达 PWM（LEDC）
  ledcAttachChannel(VIB_PIN, vibFreq, vibResolution, vibChannel);

  // 初始化 BLE
  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_RX,
                        BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();
  pServer->getAdvertising()->start();

  Serial.println("初始化完成");
  Serial.println("----------------------------------------");
  Serial.println("危险等级: 0=安全 1=提醒 2=警告 3=危险");
  Serial.println("----------------------------------------");
}

// ====================== 主循环 ======================
void loop() {
  float distance = measureDistance();
  int level = getVibrationLevel(distance);

  if (distance > 0) {
    Serial.print("距离: ");
    Serial.print((int)distance);
    Serial.print(" cm | 振动等级: ");
    Serial.println(level);

    if (isConnected) {
      String msg = "DIST:" + String((int)distance);
      sendBLEString(msg);
    }
  }

  delay(300);
}

// ====================== 工具函数 ======================
void sendBLEString(String str) {
  if (isConnected) {
    pTxCharacteristic->setValue(str.c_str());
    pTxCharacteristic->notify();
  }
}

void disconnectBLE() {
  if (isConnected) {
    pServer->disconnect(pServer->getConnId());
    isConnected = false;
  }
}