#define LED_BUILTIN 2
// 当您按下复位按钮或为电路板通电时，该设置函数将执行一次。
void setup() {
// 将数字引脚 LED_BUILTIN 初始化为输出模式。
pinMode(LED_BUILTIN，output)；
}
// 该循环函数将无限次重复执行
void loop() {
digitalWrite(LED_BUILTIN，HIGH)；// 点亮 LED（HIGH 为电压电平）
delay(1000); // wait for a second
digitalWrite(LED_BUILTIN，LOW)；// 通过将电压设为 LOW 来关闭 LED 灯
delay(1000); // wait for a second
}