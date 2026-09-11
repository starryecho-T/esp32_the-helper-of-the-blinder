/**
 * 前哨 ESP32-S3 CAM：超声波高位测距 + ESP-NOW 发送
 *
 * 接线：
 *   HC-SR04      ESP32-S3 CAM
 *   VCC   →      5V
 *   GND   →      GND
 *   Trig  →      GPIO 14
 *   Echo  →      GPIO 2
 */

#include <esp_now.h>
#include <WiFi.h>

// 盲杖的 MAC 地址
uint8_t caneMac[] = {0x94, 0xA9, 0x90, 0xCA, 0xAE, 0x64};

// 超声波引脚（避开摄像头占用的引脚）
#define TRIG_PIN 14
#define ECHO_PIN 2

// 发送的数据结构
typedef struct {
  float distance;
  int level;
} ScoutData;

ScoutData sendData;

// 测距函数
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1.0;
  return duration * 0.0343 / 2.0;
}

// 根据距离算等级
int getLevel(float dist) {
  if (dist < 0) return 0;        // 无检测
  if (dist < 30) return 3;      // 危险
  if (dist < 80) return 2;      // 警告
  if (dist < 150) return 1;     // 提醒
  return 0;                     // 安全
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  WiFi.mode(WIFI_STA);

  Serial.println("前哨 ESP-NOW + 超声波");
  Serial.print("本机 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失败");
    return;
  }

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, caneMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("添加 peer 失败");
    return;
  }

  Serial.println("初始化完成，开始测距发送");
  Serial.println("----------------------------------------");
}

void loop() {
  float dist = readDistance();
  sendData.distance = dist;
  sendData.level = getLevel(dist);

  Serial.print("高位距离: ");
  if (dist < 0) {
    Serial.print("无检测");
  } else {
    Serial.print(dist);
    Serial.print("cm");
  }
  Serial.print(" 等级=");
  Serial.println(sendData.level);

  esp_err_t result = esp_now_send(caneMac, (uint8_t *)&sendData, sizeof(sendData));

  if (result == ESP_OK) {
    Serial.println("  → 已发送给盲杖");
  } else {
    Serial.println("  → 发送失败");
  }

  delay(500);
}
