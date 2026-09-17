/**
 * SYN6288 语音合成 ESP32 驱动
 *
 * 接线：
 *   SYN6288 RX  ←  GPIO14
 *   SYN6288 TX  →  GPIO3
 *   VCC → 5V, GND → GND, 喇叭接模块
 */

#include <Arduino.h>

#define SYN6288_TX_PIN 14
#define SYN6288_RX_PIN 3

HardwareSerial synSerial(2);

// 初始化
void syn6288_init() {
  synSerial.begin(9600, SERIAL_8N1, SYN6288_RX_PIN, SYN6288_TX_PIN);
  delay(200);
  Serial.println("SYN6288 初始化，9600");
}

// 发送原始帧（内部用）
void syn6288_sendFrame(uint8_t cmd, uint8_t param, uint8_t *data, uint8_t len) {
  uint8_t buf[210];
  buf[0] = 0xFD;
  buf[1] = (uint8_t)((len + 3) / 256);
  buf[2] = (uint8_t)((len + 3) % 256);
  buf[3] = cmd;
  buf[4] = param;
  if (len > 0) memcpy(&buf[5], data, len);

  uint8_t xor_cal = 0;
  for (uint8_t i = 0; i < len + 5; i++) xor_cal ^= buf[i];
  buf[len + 5] = xor_cal;

  synSerial.write(buf, len + 6);
}

// 播报文字（GB2312字节）
void syn6288_speakBytes(const uint8_t *data, uint8_t len) {
  syn6288_sendFrame(0x01, 0x00, (uint8_t *)data, len);
  Serial.print("播报 ");
  Serial.print(len);
  Serial.println("字节");
  delay(len * 120 + 500);
}

// 播报英文字符串
void syn6288_speak(const char *text) {
  syn6288_speakBytes((const uint8_t *)text, strlen(text));
}

// 设置音量 0-16
// SYN6288 用文字命令 [vN] 设置音量，N=0-16
void syn6288_setVolume(uint8_t vol) {
  if (vol > 16) vol = 16;
  // 构造 [v16] 这样的文字命令
  char cmd[8];
  snprintf(cmd, 8, "[v%d]", vol);

  uint8_t len = strlen(cmd);
  uint8_t buf[20];
  buf[0] = 0xFD;
  buf[1] = (uint8_t)((len + 3) / 256);
  buf[2] = (uint8_t)((len + 3) % 256);
  buf[3] = 0x01;   // 合成命令
  buf[4] = 0x00;   // 参数
  memcpy(&buf[5], cmd, len);

  uint8_t xor_cal = 0;
  for (uint8_t i = 0; i < len + 5; i++) xor_cal ^= buf[i];
  buf[len + 5] = xor_cal;

  synSerial.write(buf, len + 6);

  Serial.print("音量设为: ");
  Serial.println(vol);
  delay(200);
}

// 停止
void syn6288_stop() {
  syn6288_sendFrame(0x02, 0x00, nullptr, 0);
  Serial.println("停止");
}

// 暂停
void syn6288_pause() {
  syn6288_sendFrame(0x03, 0x00, nullptr, 0);
  Serial.println("暂停");
}

// 恢复
void syn6288_resume() {
  syn6288_sendFrame(0x04, 0x00, nullptr, 0);
  Serial.println("恢复");
}
