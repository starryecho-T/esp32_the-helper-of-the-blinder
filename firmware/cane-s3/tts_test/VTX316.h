/**
 * VTX316 TTS 语音合成库
 * 对应官方示例中的 VTX316.h
 *
 * 用法：
 *   #include "VTX316.h"
 *   void setup() {
 *     VTX316_Init(17, 16);  // TX=17, RX=16
 *   }
 *   void loop() {
 *     Voice_BOBAO("今天天气晴");
 *     delay(5000);
 *   }
 *
 * 注意：中文需要 GBK 编码
 * 如果 Arduino IDE 保存文件为 UTF-8，中文会乱码
 * 解决：文件 → 首选项 → 取消勾选 "保存时使用 UTF-8"
 * 或者用 SayGBK() 函数传预编码的 GBK 字节数组
 */

#ifndef VTX316_H
#define VTX316_H

#include <Arduino.h>

#ifndef TX_TO_TTS
#define TX_TO_TTS 17   // ESP32 TX → TTS RX
#endif

#ifndef RX_FROM_TTS
#define RX_FROM_TTS 16 // TTS TX → ESP32 RX
#endif

// 初始化 TTS 模块
void VTX316_Init(int txPin, int rxPin) {
  Serial2.begin(9600, SERIAL_8N1, rxPin, txPin);
  delay(200);
}

// 发送原始帧
void _sendFrame(uint8_t cmd, uint8_t sub, const uint8_t* data, int len) {
  uint8_t head[5] = {0xFD, 0x00, (uint8_t)(len + 2), cmd, sub};
  Serial2.write(head, 5);
  delay(2);
  Serial2.write(data, len);
  delay(len + 50);
}

// 播报 GBK 编码的文本（传字节数组）
void SayGBK(const uint8_t* gbkData, int len) {
  _sendFrame(0x01, 0x05, gbkData, len);
}

// 播报文本（如果 .ino 文件是 GBK 编码可以直接用）
void Voice_BOBAO(const char* text) {
  int len = strlen(text);
  _sendFrame(0x01, 0x05, (const uint8_t*)text, len);
}

// 设置音量 0-10
void Voice_VOL(int level) {
  if (level < 0) level = 0;
  if (level > 10) level = 10;

  uint8_t cmd[9] = {
    0xFD, 0x00, 0x06,
    0x01, 0x01,
    0x5B, 0x76,
    (uint8_t)('0' + level),
    0x5D
  };
  Serial2.write(cmd, 9);
  delay(100);
}

// 暂停
void Voice_PAUSE() {
  uint8_t cmd[4] = {0xFD, 0x00, 0x01, 0x03};
  Serial2.write(cmd, 4);
}

// 恢复
void Voice_RESUME() {
  uint8_t cmd[4] = {0xFD, 0x00, 0x01, 0x04};
  Serial2.write(cmd, 4);
}

#endif
