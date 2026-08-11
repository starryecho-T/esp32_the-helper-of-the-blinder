/**
 * MPU6050 姿态检测 + BLE 发送
 * 接线：
 *   MPU6050 VCC → ESP32 3.3V
 *   MPU6050 GND → ESP32 GND
 *   MPU6050 SDA → GPIO 21
 *   MPU6050 SCL → GPIO 22
 *
 * 烧录后打开串口监视器(115200)，手机连上 BLE 即可看到姿态数据
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>

// ====================== BLE 参数 ======================
#define DEVICE_NAME "ESP32_BLE_2026"
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ====================== MPU6050 参数 ======================
#define MPU6050_ADDR 0x68
#define SDA_PIN 21
#define SCL_PIN 22

// ====================== 跌倒检测参数 ======================
#define FALL_THRESHOLD 2.5    // 加速度合矢量阈值(g)，超过此值判定为撞击
#define FREEFALL_THRESHOLD 0.4 // 失重阈值(g)，低于此值判定为失重

// ====================== 全局变量 ======================
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool isConnected = false;
String receivedData = "";

float accX, accY, accZ;     // 加速度 (g)
float gyroX, gyroY, gyroZ;  // 角速度 (°/s)
float accMagnitude;          // 加速度合矢量

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

// ====================== MPU6050 驱动 ======================

// 写 MPU6050 寄存器
void mpuWrite(uint8_t reg, uint8_t data) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(data);
  Wire.endTransmission();
}

// 读 MPU6050 寄存器（2字节）
int16_t mpuRead16(uint8_t reg) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 2);
  return (Wire.read() << 8) | Wire.read();
}

// 初始化 MPU6050
bool mpuInit() {
  Wire.begin(SDA_PIN, SCL_PIN);

  // 扫描 I2C 设备，确认 MPU6050 连接正常
  Wire.beginTransmission(MPU6050_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("错误：找不到 MPU6050！请检查接线");
    return false;
  }

  // 唤醒 MPU6050（退出睡眠模式）
  mpuWrite(0x6B, 0x00);

  // 设置加速度计量程 ±8g
  mpuWrite(0x1C, 0x10);

  // 设置陀螺仪量程 ±500°/s
  mpuWrite(0x1B, 0x08);

  Serial.println("MPU6050 初始化成功");
  return true;
}

// 读取传感器数据
void mpuRead() {
  // 加速度（先读高字节再读低字节）
  accX = mpuRead16(0x3B) / 4096.0;  // ±8g 量程
  accY = mpuRead16(0x3D) / 4096.0;
  accZ = mpuRead16(0x3F) / 4096.0;

  // 角速度
  gyroX = mpuRead16(0x43) / 65.5;   // ±500°/s 量程
  gyroY = mpuRead16(0x45) / 65.5;
  gyroZ = mpuRead16(0x47) / 65.5;

  // 加速度合矢量
  accMagnitude = sqrt(accX * accX + accY * accY + accZ * accZ);
}

// 简单跌倒检测：失重→撞击→静止
// 返回 0=正常, 1=疑似跌倒, 2=确认跌倒
int checkFall() {
  static int fallState = 0;
  static unsigned long fallTimer = 0;

  switch (fallState) {
    case 0:  // 正常状态，等待失重
      if (accMagnitude < FREEFALL_THRESHOLD) {
        fallState = 1;
        fallTimer = millis();
        Serial.println("[跌倒检测] 检测到失重");
      }
      break;

    case 1:  // 已失重，等待撞击
      if (accMagnitude > FALL_THRESHOLD) {
        fallState = 2;
        Serial.print("[跌倒检测] 检测到撞击，加速度=");
        Serial.println(accMagnitude);
      } else if (millis() - fallTimer > 2000) {
        fallState = 0;  // 2秒内没撞击，超时恢复
        Serial.println("[跌倒检测] 失重超时，恢复正常");
      }
      break;

    case 2:  // 已撞击，确认跌倒
      return 2;
  }
  return 0;
}

// 重置跌倒检测状态
void resetFall() {
  // 外部调用，用户取消报警后重置
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== 智能盲杖 - MPU6050 姿态检测 =====");

  // 初始化 MPU6050
  if (!mpuInit()) {
    while (1) {
      delay(1000);  // 初始化失败，卡住
    }
  }

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
  mpuRead();

  // 串口打印
  Serial.print("加速度: X=");
  Serial.print(accX);
  Serial.print(" Y=");
  Serial.print(accY);
  Serial.print(" Z=");
  Serial.print(accZ);
  Serial.print(" | 合矢量=");
  Serial.println(accMagnitude);

  // 跌倒检测
  int fallStatus = checkFall();
  if (fallStatus == 2) {
    Serial.println("!!! 检测到跌倒 !!!");
    if (isConnected) {
      sendBLEString("FALL:1");
    }
    resetFall();  // 测试阶段先自动重置，正式版需要等用户确认
  }

  delay(200);  // 每秒 5 次
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