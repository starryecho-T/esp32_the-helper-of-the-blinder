// WS2811 灯带最小测试（不含 BLE/WiFi/其他外设，纯点灯）
// 接线：DIN→GPIO17，灯带 VCC/GND 接好，GND 必须和板子 GND 共地
//
// 用法：烧录后灯带应红→绿→蓝→白 循环，串口同步打印
// 若完全不亮：把下面 LED_TYPE 改成 WS2812 再试
// 若颜色不对（红绿互换）：把 LED_ORDER 改成 RGB

#include <FastLED.h>

#define LED_PIN    21
#define LED_NUM    5
#define LED_TYPE   WS2811    // 试完不行就改成 WS2812
#define LED_ORDER  GRB       // 颜色不对就改成 RGB

CRGB leds[LED_NUM];

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<LED_TYPE, LED_PIN, LED_ORDER>(leds, LED_NUM);
  FastLED.setBrightness(255);
  FastLED.clear(true);
  Serial.println("灯带测试开始：红→绿→蓝→白 循环");
}

void loop() {
  const CRGB colors[] = { CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White };
  const char* names[] = { "红", "绿", "蓝", "白" };
  for (int c = 0; c < 4; c++) {
    for (int i = 0; i < LED_NUM; i++) leds[i] = colors[c];
    FastLED.show();
    Serial.printf("当前: %s\n", names[c]);
    delay(1000);
  }
}
