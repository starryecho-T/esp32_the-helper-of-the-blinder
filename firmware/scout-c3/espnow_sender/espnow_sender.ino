/**
 * 前哨主控 ESP32-S3 CAM 完整固件
 *
 * 功能：
 *   1. 超声波高位测距 → ESP-NOW 发给盲杖
 *   2. 接收盲杖拍照命令 → 拍照 → WiFi 传给手机
 *   3. 摄像头 Web Server（手机可直接访问 /capture 拍照）
 *
 * 接线：
 *   HC-SR04:  Trig→GPIO1, Echo→GPIO14, VCC→5V, GND→GND
 *     （GPIO9被摄像头Y3占用；GPIO2有板载LED下拉导致Echo读不到高电平，改用GPIO14）
 *   摄像头:   板载，不用接线
 *
 * 摄像头占用的引脚（Freenove ESP32-S3 WROOM，不能用）：
 *   4,5,6,7,8,9,10,11,12,13,15,16,17,18
 *   超声波用 GPIO1(Trig) 和 GPIO14(Echo)，不冲突
 *
 * BLE 设备名：无（前哨不连蓝牙，走 ESP-NOW + WiFi）
 */

// 摄像头型号：ESP32-S3-EYE（Freenove ESP32-S3 WROOM 用此型号，引脚与官方例程一致）
#define CAMERA_MODEL_ESP32S3_EYE

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_camera.h>
#include "esp_http_server.h"
#include "camera_pins.h"

// ====================== 引脚 ======================
#define TRIG_PIN 1     // GPIO1 空闲
#define ECHO_PIN 14   // GPIO14 空闲（GPIO2有板载LED下拉，导致Echo读不到高电平）

// ====================== 盲杖 MAC ======================
uint8_t caneMac[] = {0x94, 0xA9, 0x90, 0xCA, 0xAE, 0x64};

// ====================== WiFi ======================
const char* ssid = "starry";       // 改成你的 WiFi
const char* password = "iloveyouso";  // 改成你的 WiFi 密码

// ====================== 数据结构 ======================
// 前哨 → 盲杖：高位距离
typedef struct {
  float distance;
  int level;
} ScoutData;

// 盲杖 → 前哨：命令
typedef struct {
  int cmd;  // 1=拍照
} CaneCmd;

ScoutData sendData;
CaneCmd recvCmd;

// ====================== 超声波测距 ======================
float readDistance() {
  // 临时关中断，防止 ESP-NOW/WiFi 中断打断 pulseIn 导致超时读 0
  noInterrupts();
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  interrupts();
  if (d == 0) return -1.0;
  return d * 0.0343 / 2.0;
}

// 超声波诊断：直接打印 Trig/Echo 电平和 pulseIn 原始值
void diagnoseUltrasonic() {
  Serial.println("\n---------- 超声波诊断 ----------");
  Serial.printf("  Trig脚(GPIO%d) 静态电平: %d\n", TRIG_PIN, digitalRead(TRIG_PIN));
  Serial.printf("  Echo脚(GPIO%d) 静态电平: %d\n", ECHO_PIN, digitalRead(ECHO_PIN));
  Serial.println("  发送10us触发脉冲...");
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(15);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 50000);
  Serial.printf("  pulseIn 原始值: %ld (0=超时没收到回波)\n", d);
  if (d > 0) {
    Serial.printf("  换算距离: %.1f cm\n", d * 0.0343 / 2.0);
  } else {
    Serial.println("  ❌ 没收到回波！检查：");
    Serial.println("    1. VCC 是否接 5V（不是3.3V）");
    Serial.println("    2. GND 是否接好");
    Serial.println("    3. Trig/Echo 是否接反");
    Serial.println("    4. 跳线帽是否已拔掉（GPIO模式）");
    Serial.println("    5. 前方30cm-2m内是否有障碍物");
  }
  Serial.println("------------------------------");
}

int getLevel(float dist) {
  if (dist < 0) return 0;
  if (dist < 30) return 3;
  if (dist < 80) return 2;
  if (dist < 150) return 1;
  return 0;
}

// ====================== 摄像头初始化 ======================
bool cameraInit() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败: 0x%x\n", err);
    return false;
  }

  // 翻转画面（根据实际安装方向调整）
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);

  Serial.println("摄像头初始化成功");
  return true;
}

// ====================== 拍照 ======================
camera_fb_t* takePhoto() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb != NULL) {
    Serial.printf("[拍照] %dx%d %d字节\n", fb->width, fb->height, fb->len);
  } else {
    Serial.println("[拍照] 失败");
  }
  return fb;
}

// ====================== HTTP /capture 接口 ======================
// 手机浏览器访问 http://前哨IP/capture 可以拿到一张 JPEG 照片
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// HTTP /stream 接口（实时视频流）
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=123456789000000000000987654321");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(part_buf, 64, "--123456789000000000000987654321\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
  }
  return res;
}

// 启动 Web 服务器
void startCameraServer() {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &stream_uri);
    Serial.println("Web 服务器启动: /capture (拍照) /stream (实时)");
  }
}

// ====================== ESP-NOW ======================
// 盲杖→前哨：拍照命令。前哨本身不主动上传照片，照片由手机通过 /capture 拉取。
// 这里收到命令后预热摄像头（取一帧即归还），让下次 /capture 响应更快。
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(&recvCmd, data, sizeof(recvCmd));
  if (recvCmd.cmd == 1) {
    Serial.println("[ESP-NOW] 收到拍照命令，预热摄像头");
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n==============================================");
  Serial.println("     Scout-CAM  智能盲杖前哨");
  Serial.println("==============================================");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // 摄像头
  bool camOk = cameraInit();
  Serial.printf("  [摄像头]   %s\n", camOk ? "OK ✓" : "失败 ✗");

  // WiFi：连手机热点。ESP-NOW 信道必须和 WiFi 信道一致，所以连上后读实际信道。
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  Serial.print("  [WiFi]     连接热点中");
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(500); Serial.print("."); wifiTimeout++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n  [WiFi]     OK ✓  IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n  [WiFi]     失败 ✗  ESP-NOW 将用默认信道");
  }
  Serial.print("     本机 MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("     对端(盲杖) MAC: ");
  for (int i = 0; i < 6; i++) { if (caneMac[i] < 0x10) Serial.print("0"); Serial.print(caneMac[i], HEX); if (i < 5) Serial.print(":"); }
  Serial.println();

  // 读取 WiFi 实际信道，ESP-NOW 必须用同一信道，否则 peer channel 报错
  uint8_t primaryChan = 1;
  wifi_second_chan_t secChan = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&primaryChan, &secChan);
  Serial.printf("  [信道]     WiFi 实际信道=%d，ESP-NOW 将用此信道\n", primaryChan);

  // 启动 HTTP 服务器（手机访问 http://<前哨IP>/capture 拉照片）
  startCameraServer();
  Serial.println("  [HTTP]     /capture(拍照) /stream(实时) 就绪");

  // ESP-NOW（和盲杖通信）。用 WiFi 实际信道，与盲杖对齐
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, caneMac, 6);
    peer.channel = primaryChan;   // 用实际信道，不写死
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    Serial.printf("  [ESP-NOW]  就绪 ✓ (信道%d)\n", primaryChan);
  } else {
    Serial.println("  [ESP-NOW]  失败 ✗");
  }

  Serial.println("==============================================");
  Serial.println("初始化完成。手机访问 http://" + WiFi.localIP().toString() + "/capture 拍照");
  Serial.println("串口输入 'dist' 可诊断超声波");
}

// ====================== 串口命令 ======================
void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "dist") {
    diagnoseUltrasonic();
  } else if (cmd == "help") {
    Serial.println("命令: dist(超声波诊断)");
  }
}

// ====================== 主循环 ======================
void loop() {
  handleSerial();
  // 每 500ms 测高位距离，ESP-NOW 发给盲杖
  static unsigned long lastSend = 0;
  static int lastLevel = -1;
  if (millis() - lastSend >= 500) {
    lastSend = millis();
    sendData.distance = readDistance();
    sendData.level = getLevel(sendData.distance);

    // 等级变化时打印，避免刷屏
    if (sendData.level != lastLevel) {
      lastLevel = sendData.level;
      Serial.printf("[前哨] 高位=%.0fcm 等级=%d\n", sendData.distance, sendData.level);
    }

    esp_now_send(caneMac, (uint8_t *)&sendData, sizeof(sendData));
  }
}
