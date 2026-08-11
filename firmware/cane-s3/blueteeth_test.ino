// 引入官方BLE 库
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ====================== 在这里修改参数 ======================
#define DEVICE_NAME "ESP32_BLE_2026"   // 设备名称，手机上搜索到的名字

// 通用 Nordic UART Service UUID （无需修改！所有APP 都支持这个标准）
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// 全局变量
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool isConnected = false;
String receivedData = "";

// 前向声明
void sendBLEString(String str);
void disconnectBLE();

// ============= 连接状态回调函数 =============
class MyServerCallbacks: public BLEServerCallbacks {
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

// ============= 数据接收回调函数 =============
class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    receivedData = pCharacteristic->getValue();

    if (receivedData.length() > 0) {
      Serial.print("收到手机数据: ");
      Serial.println(receivedData);

      // ===== 在这里添加收到数据后的处理逻辑 =====
      String cmd = receivedData;
      cmd.trim();

      if (cmd == "HELLO") {
        sendBLEString("你好，手机！");
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("\n===== ESP32 BLE 串口透传初始化 =====");

  // 1. 初始化BLE 设备，设置蓝牙名称
  BLEDevice::init(DEVICE_NAME);

  // 2. 创建BLE 服务器（ESP32 作为从机等待连接）
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // 3. 创建通用串口服务
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // 4. 创建TX 通道（ESP32 → 手机）
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());

  // 5. 创建RX 通道（手机 → ESP32）
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_RX,
                        BLECharacteristic::PROPERTY_WRITE
                      );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // 6. 启动服务并开始广播
  pService->start();
  pServer->getAdvertising()->start();

  Serial.print("蓝牙名称: ");
  Serial.println(DEVICE_NAME);
  Serial.println("初始化完成，等待手机连接...");
  Serial.println("----------------------------------------");
}

void loop() {
  if (isConnected) {
    sendBLEString("Hello from ESP32!");
    delay(2000);
  }
}

// ============= 工具函数 =============

void sendBLEString(String str) {
  if (isConnected) {
    pTxCharacteristic->setValue(str.c_str());
    pTxCharacteristic->notify();
    Serial.print("发送数据: ");
    Serial.println(str);
  }
}

void disconnectBLE() {
  if (isConnected) {
    pServer->disconnect(pServer->getConnId());
    isConnected = false;
  }
}