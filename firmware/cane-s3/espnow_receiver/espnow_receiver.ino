/**
 * ESP-NOW 接收端（盲杖 ESP32-S3）
 * 接收前哨发来的高位距离数据
 *
 * 不需要连 WiFi，ESP-NOW 是板对板直接通信
 */

#include <esp_now.h>
#include <WiFi.h>

// 前哨的 MAC 地址
uint8_t scoutMac[] = {0x28, 0x84, 0x85, 0x4B, 0xAB, 0xBC};

// 接收的数据结构（和发送端一致）
typedef struct {
  float distance;      // 高位超声波距离
  int level;           // 障碍等级 0=安全 1=提醒 2=警告 3=危险
} ScoutData;

ScoutData recvData;

// 接收回调函数（新版 ESP32 3.x 格式）
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(&recvData, data, sizeof(recvData));

  Serial.print("收到前哨数据: 高位距离=");
  Serial.print(recvData.distance);
  Serial.print("cm 等级=");
  Serial.println(recvData.level);
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  Serial.println("盲杖 ESP-NOW 接收端");
  Serial.print("本机 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失败");
    return;
  }

  // 注册接收回调
  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("等待前哨数据...");
  Serial.println("----------------------------------------");
}

void loop() {
  // 这里可以加上盲杖自己的低位超声波测距
  // 然后和 recvData.distance 做高低位融合判断
}
