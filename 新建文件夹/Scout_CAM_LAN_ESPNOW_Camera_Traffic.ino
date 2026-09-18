/**
 * Scout-CAM 智能盲杖前哨 - 局域网版
 *
 * 功能：
 *   1. HC-SR04 超声波高位测距 → ESP-NOW 发给盲杖
 *   2. 接收盲杖拍照命令 → 预热摄像头
 *   3. ESP32-S3-CAM 连接普通 2.4GHz 局域网 WiFi
 *   4. 提供 HTTP /capture 拍照接口
 *   5. 提供 HTTP /status 状态接口，方便检查网络/信道/IP
 *   6. 配合 Python FastAPI + YOLO 交通灯后端：
 *        Python → http://<ESP32_IP>/capture → YOLO → RED/YELLOW/GREEN/NONE
 *
 * 注意：
 *   - ESP32 不在本机执行 YOLO；交通灯识别仍由 Python 后端完成。
 *   - ESP32 与运行 Python 后端的电脑、盲人端手机应位于可互相访问的同一局域网。
 *   - WiFi 必须是 2.4GHz（ESP32-S3 不连接 5GHz-only SSID）。
 *   - ESP-NOW 与 STA WiFi 共用同一射频，因此盲杖和本机必须使用同一 WiFi 信道。
 *     本版把 ESP-NOW peer.channel 设为 0，跟随本机当前信道。
 *
 * 接线：
 *   HC-SR04: Trig → GPIO9, Echo → GPIO2, VCC → 5V, GND → GND
 *   摄像头：板载
 */

// 摄像头型号：ESP32-S3-CAM（带 LCD 版本）
// 必须在 #include "camera_pins.h" 之前定义
#define CAMERA_MODEL_ESP32S3_CAM_LCD

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_camera.h>
#include "esp_http_server.h"
#include "camera_pins.h"

// ====================== 引脚 ======================
#define TRIG_PIN 9
#define ECHO_PIN 2

// ====================== 盲杖 MAC ======================
uint8_t caneMac[] = {0x94, 0xA9, 0x90, 0xCA, 0xAE, 0x64};

// ====================== WiFi ======================
// 修改为你的 2.4GHz 路由器/手机热点 WiFi
const char* ssid = "starry";
const char* password = "iloveyouso";

// WiFi 连接等待时间
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

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

bool espNowInitialized = false;

// ====================== 超声波测距 ======================
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  if (d == 0) return -1.0;
  return d * 0.0343 / 2.0;
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

  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  // 交通灯通常比较小，给 YOLO 更大的原始图像比 QVGA 更有利。
  // 有 PSRAM 时使用 VGA；没有 PSRAM 时降到 QVGA，优先保证稳定。
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_QVGA; // 320x240
    config.jpeg_quality = 12;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[摄像头] 初始化失败: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    // 根据你的安装方向调整；当前沿用原工程的上下翻转。
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
  }

  Serial.printf("[摄像头] 初始化成功，分辨率：%s\n",
                psramFound() ? "VGA 640x480" : "QVGA 320x240");
  return true;
}

// ====================== HTTP /capture ======================
// Python 后端会从这里拉取 JPEG，然后送入 YOLO 做交通灯识别。
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[HTTP] /capture 拍照失败");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

  Serial.printf("[HTTP] /capture -> %ux%u, %u bytes, res=%d\n",
                fb->width, fb->height, fb->len, (int)res);

  esp_camera_fb_return(fb);
  return res;
}

// ====================== HTTP /status ======================
// 返回简单状态，方便手机/电脑检查 ESP32 是否已经连入局域网。
static esp_err_t status_handler(httpd_req_t *req) {
  String json = "{";
  json += "\"wifi\":\"";
  json += (WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  json += "\",";
  json += "\"ssid\":\"" + String(ssid) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"gateway\":\"" + WiFi.gatewayIP().toString() + "\",";
  json += "\"channel\":" + String(WiFi.channel()) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI());
  json += "}";

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

// ====================== HTTP / ======================
static esp_err_t root_handler(httpd_req_t *req) {
  String text;
  text += "SmartCane Scout-CAM OK\n";
  text += "IP: " + WiFi.localIP().toString() + "\n";
  text += "Channel: " + String(WiFi.channel()) + "\n";
  text += "/capture  - JPEG snapshot\n";
  text += "/status   - WiFi status JSON\n";
  text += "/stream   - MJPEG stream\n";

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_send(req, text.c_str(), text.length());
  return ESP_OK;
}

// ====================== HTTP /stream ======================
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[96];
  const char *boundary = "123456789000000000000987654321";

  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=123456789000000000000987654321");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(part_buf, sizeof(part_buf),
                           "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                           boundary, fb->len);

    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
    delay(30);
  }

  return res;
}

// ====================== 启动 Web 服务器 ======================
void startCameraServer() {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;

  httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_handler,
    .user_ctx = NULL
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &stream_uri);
    Serial.println("[HTTP] Web 服务器启动成功");
    Serial.println("       /capture  拍照");
    Serial.println("       /status   WiFi 状态");
    Serial.println("       /stream   实时视频");
  } else {
    Serial.println("[HTTP] Web 服务器启动失败");
  }
}

// ====================== WiFi ======================
// 连接局域网。连接成功后 ESP32 会拿到路由器分配的局域网 IP。
bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 降低 /capture 延迟并减少 WiFi 睡眠导致的瞬时掉线

  Serial.println("[WiFi] 开始连接局域网...");
  Serial.printf("[WiFi] SSID: %s\n", ssid);

  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] 连接失败，status=%d\n", (int)WiFi.status());
    return false;
  }

  Serial.println("[WiFi] 连接成功 ✓");
  Serial.printf("[WiFi] IP      : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] Gateway : %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("[WiFi] Channel : %d\n", WiFi.channel());
  Serial.printf("[WiFi] RSSI    : %d dBm\n", WiFi.RSSI());
  Serial.println("[WiFi] Camera: http://<上述IP>/capture");

  return true;
}

// ====================== ESP-NOW ======================
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (data == nullptr || len < (int)sizeof(CaneCmd)) {
    Serial.println("[ESP-NOW] 收到长度异常的数据，忽略");
    return;
  }

  memcpy(&recvCmd, data, sizeof(recvCmd));

  if (recvCmd.cmd == 1) {
    Serial.println("[ESP-NOW] 收到拍照命令，预热摄像头");

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      Serial.printf("[ESP-NOW] 预热帧：%ux%u %u bytes\n", fb->width, fb->height, fb->len);
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[ESP-NOW] 预热拍照失败");
    }
  }
}

bool initEspNow() {
  if (espNowInitialized) return true;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ESP-NOW] WiFi 尚未连接，暂不能初始化");
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] 初始化失败 ✗");
    return false;
  }

  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, caneMac, 6);

  // 0 = 使用本机当前 WiFi 信道。
  // 因为本机现在是 STA 模式，信道由连接的路由器决定。
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  if (esp_now_is_peer_exist(caneMac)) {
    esp_now_del_peer(caneMac);
  }

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ESP-NOW] 添加盲杖 peer 失败 ✗");
    return false;
  }

  espNowInitialized = true;

  Serial.printf("[ESP-NOW] 就绪 ✓，当前信道=%d，peer.channel=0(跟随当前信道)\n", WiFi.channel());
  Serial.println("[ESP-NOW] 注意：盲杖端必须使用相同 WiFi 信道");
  return true;
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==============================================");
  Serial.println("     Scout-CAM 智能盲杖前哨（局域网版）");
  Serial.println("==============================================");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Serial.print("[本机] MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("[盲杖] MAC: ");
  for (int i = 0; i < 6; i++) {
    if (caneMac[i] < 0x10) Serial.print("0");
    Serial.print(caneMac[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  // 1. 摄像头
  bool camOk = cameraInit();
  Serial.printf("[摄像头] %s\n", camOk ? "OK ✓" : "失败 ✗");

  // 2. 连接普通局域网 WiFi
  bool wifiOk = connectToWiFi();

  // 即使 WiFi 暂时没连上，也先把摄像头 HTTP 服务启动起来；
  // 等 WiFi 恢复后，地址会从 Serial 中显示出来。
  startCameraServer();

  // 3. ESP-NOW
  // 必须在 WiFi STA 建立后初始化，这样 peer.channel=0 会跟随当前 WiFi 信道。
  bool nowOk = false;
  if (wifiOk) {
    nowOk = initEspNow();
  } else {
    Serial.println("[ESP-NOW] 暂不初始化：WiFi 尚未连接，等 WiFi 恢复后自动初始化");
  }

  Serial.println("==============================================");
  Serial.println("初始化完成");

  if (wifiOk) {
    String ip = WiFi.localIP().toString();
    Serial.printf("手机/电脑访问： http://%s/capture\n", ip.c_str());
    Serial.printf("状态检查：     http://%s/status\n", ip.c_str());
    Serial.printf("Python 后端 cam： http://%s/capture\n", ip.c_str());
  } else {
    Serial.println("WiFi 未连接，请检查 SSID/密码/2.4GHz 网络。");
  }

  Serial.printf("摄像头：%s | WiFi：%s | ESP-NOW：%s\n",
                camOk ? "OK" : "FAIL",
                wifiOk ? "OK" : "FAIL",
                nowOk ? "OK" : "WAIT/FAIL");
  Serial.println("==============================================\n");
}

// ====================== 主循环 ======================
void loop() {
  // 每 500ms 测高位距离，ESP-NOW 发给盲杖
  static unsigned long lastSend = 0;
  static int lastLevel = -1;

  if (millis() - lastSend >= 500) {
    lastSend = millis();

    sendData.distance = readDistance();
    sendData.level = getLevel(sendData.distance);

    if (sendData.level != lastLevel) {
      lastLevel = sendData.level;
      Serial.printf("[前哨] 高位=%.0fcm 等级=%d\n", sendData.distance, sendData.level);
    }

    if (WiFi.status() == WL_CONNECTED && esp_now_is_peer_exist(caneMac)) {
      esp_err_t result = esp_now_send(caneMac, (uint8_t *)&sendData, sizeof(sendData));
      if (result != ESP_OK) {
        Serial.printf("[ESP-NOW] 发送失败：0x%x\n", result);
      }
    }
  }

  // 若 WiFi 掉线，每 10 秒尝试恢复连接。
  // peer.channel=0 会继续使用恢复后的当前 WiFi 信道。
  static unsigned long lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect >= 10000) {
    lastReconnect = millis();
    Serial.println("[WiFi] 检测到掉线，重新连接...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }

  // 初次连接失败后，WiFi 恢复时自动把 ESP-NOW 也重新初始化。
  if (WiFi.status() == WL_CONNECTED && !espNowInitialized) {
    static unsigned long lastEspNowInit = 0;
    if (millis() - lastEspNowInit >= 1000) {
      lastEspNowInit = millis();
      if (initEspNow()) {
        Serial.println("[ESP-NOW] WiFi 恢复后初始化成功 ✓");
      }
    }
  }
}
