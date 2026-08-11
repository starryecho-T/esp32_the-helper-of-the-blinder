/**
 * HC-SR04 超声波测距 + BLE 发送
 * 接线：
 *   HC-SR04 VCC → ESP32 5V (或3.3V)
 *   HC-SR04 GND → ESP32 GND
 *   HC-SR04 Trig → GPIO 5
 *   HC-SR04 Echo → GPIO 18
 *
 * 烧录后打开串口监视器(115200)，手机连上 BLE 即可看到距离数据
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

// ====================== 超声波引脚 ======================
#define TRIG_PIN 5
#define ECHO_PIN 18

// ====================== 测距参数 ======================
#define MAX_DISTANCE 400    // 最大测量距离(cm)，超过此值视为无效
#define SAMPLE_COUNT 5      // 每次取几次测量取平均值，减少误报

// ====================== 全局变量 ======================
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool isConnected = false;
String receivedData = "";

// 前向声明
void sendBLEString(String str);
void disconnectBLE();

// ====================== BLE 连接状态回调 ======================
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

// ====================== BLE 数据接收回调 ======================
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    receivedData = pCharacteristic->getValue();
    if (receivedData.length() > 0) {
      Serial.print("收到: ");
      Serial.println(receivedData);
      String cmd = receivedData;
      cmd.trim();
      if (cmd == "QUERY:DIST") {
        // 手机主动查询距离，在 loop() 中处理
      }
    }
  }
};

// ====================== 超声波测距 ======================
float measureDistance() {
  float totalDistance = 0;
  int validSamples = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    // 发送 10μs 触发脉冲
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    // 读取 Echo 高电平持续时间（微秒）
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);  // 超时 30ms

    if (duration > 0) {
      // 声速 343m/s = 0.0343cm/μs，往返距离除以2
      float distance = duration * 0.0343 / 2.0;
      if (distance > 0 && distance < MAX_DISTANCE) {
        totalDistance += distance;
        validSamples++;
      }
    }
    delay(10);  // 两次测量间隔
  }

  if (validSamples > 0) {
    return totalDistance / validSamples;  // 返回平均值
  }
  return -1;  // 无效测量
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== 智能盲杖 - 超声波测距 =====");

  // 初始化超声波引脚
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

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
}

// ====================== 主循环 ======================
void loop() {
  float distance = measureDistance();

  if (distance > 0) {
    // 串口打印
    Serial.print("距离: ");
    Serial.print(distance);
    Serial.println(" cm");

    // BLE 发送给手机
    if (isConnected) {
      String msg = "DIST:" + String((int)distance);
      sendBLEString(msg);
    }
  }

  delay(500);  // 每秒测 2 次
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