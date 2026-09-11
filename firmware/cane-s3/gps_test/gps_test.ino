/**
 * GPS 定位模块测试（HS-S78P / ATGM336H）
 *
 * 接线：
 *   GPS 模块      ESP32-S3
 *   G  →          GND
 *   V  →          5V（或3.3V）
 *   R  →          GPIO 15（ESP32 TX，发给GPS，一般不用）
 *   T  →          GPIO 14（ESP32 RX，接收GPS数据）
 *
 * 模块上电后持续输出 NMEA 语句
 * 需要安装 TinyGPS++ 库：工具→管理库→搜索 TinyGPSPlus
 */

#include <TinyGPSPlus.h>

// GPS 用 Serial1
#define GPS_TX_PIN 15   // ESP32 → GPS RX（一般不用，GPS只发不收）
#define GPS_RX_PIN 14   // GPS TX → ESP32 RX

TinyGPSPlus gps;

void setup() {
  Serial.begin(115200);

  // GPS 模块默认波特率 9600
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  Serial.println("\n===== GPS 定位模块测试 =====");
  Serial.println("等待卫星信号...");
  Serial.println("请把模块放到窗户边或室外");
  Serial.println("----------------------------------------");
}

void loop() {
  // 持续读取 GPS 数据
  while (Serial1.available()) {
    int c = Serial1.read();
    gps.encode(c);

    // 打印原始 NMEA 数据（调试用，可删）
    // Serial.write(c);
  }

  // 检查是否有新的位置信息
  if (gps.location.isUpdated()) {
    Serial.println("\n=== 收到定位数据 ===");

    if (gps.location.isValid()) {
      Serial.print("纬度: ");
      Serial.println(gps.location.lat(), 6);

      Serial.print("经度: ");
      Serial.println(gps.location.lng(), 6);

      Serial.print("卫星数: ");
      Serial.println(gps.satellites.value());

      Serial.print("高度: ");
      Serial.print(gps.altitude.meters());
      Serial.println(" m");

      Serial.print("速度: ");
      Serial.print(gps.speed.kmph());
      Serial.println(" km/h");

      Serial.print("航向: ");
      Serial.print(gps.course.deg());
      Serial.println(" 度");
    } else {
      Serial.println("位置无效，继续等待...");
    }

    Serial.println("------------------------");
  }

  // 没有定位时显示状态
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    Serial.print("卫星数: ");
    Serial.print(gps.satellites.value());
    Serial.print(" | 定位状态: ");
    Serial.println(gps.location.isValid() ? "有效" : "无效");
  }
}