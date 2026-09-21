/*
 * cloud_upload.ino  ——  ESP32-CAM 主动把画面推送到云服务器
 *
 * 用途：云服务器在公网，ESP32-CAM 在家/校园内网里，服务器反过来拉不到摄像头。
 *      解决办法是让设备主动 POST 一帧 JPEG 到服务器的 /upload 接口，
 *      服务器缓存最新一帧，手机 App 再调 GET /detect 去识别。
 *
 * 优点：不需要公网 IP、不需要路由器端口映射、摄像头不会暴露到公网。
 *
 * 使用：
 *   1. 改下面 4 个配置（WiFi、服务器地址、token、上传间隔）
 *   2. 用 Arduino IDE 烧录到 ESP32-CAM（AI-Thinker 模组，其他模组改 CAMERA_MODEL_*）
 *   3. 串口监视器 115200，看到 "[upload] OK" 即成功
 *   4. 在服务器上 curl http://127.0.0.1:8000/status 能看到帧年龄
 *
 * 这是独立测试用 sketch。真正集成时，把 uploadFrame() 这个函数
 * 和你现有的 espnow_sender.ino 里的拍照逻辑接起来即可。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"

// ====================== 需要你修改的配置 ======================
const char* WIFI_SSID   = "starry";
const char* WIFI_PASS   = "iloveyouso";

// 云服务器地址（把 8.130.xxx.xxx 换成你的阿里云 ECS 公网 IP 或域名）
const char* SERVER_HOST = "http://39.106.216.80:8000";
const char* DEVICE_TOKEN = "ieTfVG2SMjgvYNxO9C18bh6d"; // 与服务器 DEVICE_TOKEN 一致

const unsigned long UPLOAD_INTERVAL_MS = 700;  // 上传间隔，700ms ≈ 1.4 帧/秒
// ============================================================

// ---------- AI-Thinker ESP32-CAM 引脚 ----------
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static bool cameraReady = false;

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // 有 PSRAM 就用更高分辨率，没有就降下来
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;   // 640x480，识别效果与流量的折中
    config.jpeg_quality = 12;              // 数字越小越清晰、越大
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 15;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }
  Serial.println("[cam] ready");
  return true;
}

// 拍一帧并 POST 到云服务器 /upload
bool uploadFrame() {
  if (WiFi.status() != WL_CONNECTED) return false;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[upload] fb get failed");
    return false;
  }

  char url[160];
  snprintf(url, sizeof(url), "%s/upload?token=%s", SERVER_HOST, DEVICE_TOKEN);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(5000);

  int code = http.POST(fb->buf, fb->len);
  Serial.printf("[upload] %u bytes -> HTTP %d\n", fb->len, code);

  http.end();
  esp_camera_fb_return(fb);
  return code == 200;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[wifi] IP = ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] FAILED");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();

  connectWiFi();
  cameraReady = initCamera();
}

unsigned long lastUpload = 0;

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (cameraReady && millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();
    uploadFrame();
  }
  delay(10);
}
