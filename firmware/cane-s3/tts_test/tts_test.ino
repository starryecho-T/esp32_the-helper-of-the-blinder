/**
 * SYN6288 最简测试 - 之前能出声的版本
 */

#include "syn6288.h"

// "测试"
uint8_t msg_test[] = {0xB2,0xE2,0xCA,0xD4};

// "你好"
uint8_t msg_hello[] = {0xC4,0xE3,0xBA,0xC3};

void setup() {
  Serial.begin(115200);
  Serial.println("\n===== SYN6288 测试 =====");

  syn6288_init();
  delay(500);

  // 不设音量，直接播报
  Serial.println("播报: 测试");
  syn6288_speakBytes(msg_test, sizeof(msg_test));
  delay(3000);

  Serial.println("播报: 你好");
  syn6288_speakBytes(msg_hello, sizeof(msg_hello));
  delay(3000);

  Serial.println("完成");
}

void loop() {
}
