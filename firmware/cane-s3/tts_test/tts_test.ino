/**
 * TTS 语音合成模块测试（HS-S77-PL / VTX316）
 * 官方代码适配 ESP32-S3
 *
 * 接线（只需 3 根线）：
 *   TTS 模块      ESP32-S3
 *   G  →          GND
 *   V  →          5V
 *   RX →          GPIO 17（ESP32 发，TTS 收）
 *   TX →          不接
 */

// ESP32-S3 用 Serial2 代替 SoftwareSerial
// Serial2.begin(波特率, 配置, RX引脚, TX引脚)
// RX=16(不用), TX=17(接TTS的RX)
#define TTS_RX_PIN 16   // TTS TX → ESP32 RX（不接也行）
#define TTS_TX_PIN 17   // ESP32 TX → TTS RX（必须接）

/*********************************
 * 播报文本
 *********************************/
void Voice_BOBAO(const char *message) {
  size_t byteLength = strlen(message);

  // 帧头
  uint8_t headOfFrame[5] = {
    0xFD,
    0x00,
    (uint8_t)(byteLength + 2),
    0x01,
    0x05
  };

  Serial2.write(headOfFrame, sizeof(headOfFrame));
  delay(2);

  Serial2.print(message);

  delay(byteLength * 12 + 300);

  Serial.print("播报: ");
  Serial.println(message);
}

/*********************************
 * 设置音量 0-10
 *********************************/
void Voice_YIN_LIANG(uint8_t YINLIANG) {
  uint8_t command[9] = {0xFD, 0x00, 0x06, 0x01, 0x01, 0x5B, 0x76, 0x30 + YINLIANG, 0x5D};
  Serial2.write(command, sizeof(command));

  Serial.print("音量设为: ");
  Serial.println(YINLIANG);
}

/*********************************
 * 暂停
 *********************************/
void Voice_ZANTING() {
  uint8_t command[4] = {0xFD, 0x00, 0x01, 0x03};
  Serial2.write(command, sizeof(command));
}

/*********************************
 * 恢复
 *********************************/
void Voice_HUIFU() {
  uint8_t command[4] = {0xFD, 0x00, 0x01, 0x04};
  Serial2.write(command, sizeof(command));
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // 初始化 TTS 串口，波特率 115200
  Serial2.begin(115200, SERIAL_8N1, TTS_RX_PIN, TTS_TX_PIN);

  Serial.println("\n===== TTS 语音合成测试 =====");
  delay(1000);

  // 测试1：低音量播报
  Voice_YIN_LIANG(1);
  delay(100);
  Voice_BOBAO("雨天路面湿滑，请注意安全");
  delay(3000);

  // 测试2：高音量播报
  Voice_YIN_LIANG(7);
  delay(100);
  Voice_BOBAO("今天天气晴，适合外出");
  delay(3000);
}

void loop() {
  // 串口输入文本可以自定义播报
  if (Serial.available()) {
    String text = Serial.readStringUntil('\n');
    text.trim();
    if (text.length() > 0) {
      Voice_BOBAO(text.c_str());
    }
  }
}
