#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include <PubSubClient.h>

#include <PubSubClient.h>

const char* mqtt_server = nullptr;
bool wifiConnected = false;
bool httpStarted = false;

WiFiClient espClient;
PubSubClient client(espClient);

bool mqttReconnectPending = false;
bool mqttBusy = false;

unsigned long lastMqttRetry = 0;
const unsigned long mqttRetryInterval = 3000;

unsigned long lastWifiRetry = 0;
const unsigned long wifiRetryInterval = 10000;

WebServer server(80);

void handle_root();
void handle_jpg_stream();
void startCamera();
const char* wifiStatusToString(wl_status_t status);
void printSystemStatus();
bool tryConnectToWiFi();
void handleMQTT();
void handleWiFiRoaming();
void startHttpServer();
void stopHttpServer();
void callback(char* topic, byte* payload, unsigned int length);

// const char* ssid = "AndroidAPD236";
// const char* password = "qhdz9766";
// const char* ssid = "ADE-G2392QG1QF";
// const char* password = "mgn-car-2210";


struct WiFiNetwork {
  const char* ssid;
  const char* password;
  const char* mqttServer;
};

WiFiNetwork wifiList[] = {
  { "SG Wi-Fi E", "sgabor95", "192.168.100.47" },
  { "AndroidAPD236", "qhdz9766", "109.100.33.178" },
  { "ADE-G2392QG1QF", "mgn-car-2210", "109.100.33.178" },
  { "SG Wi-Fi P", "sgabor95", "192.168.100.47" }
};

const int networkCount = sizeof(wifiList) / sizeof(WiFiNetwork);


void startHttpServer() {
  if (httpStarted) return;

  server.on("/", handle_root);
  server.on("/stream", HTTP_GET, handle_jpg_stream);
  server.begin();

  httpStarted = true;
  Serial.println("HTTP server started");
}

void stopHttpServer() {
  if (!httpStarted) return;

  server.stop();
  httpStarted = false;
  Serial.println("HTTP server stopped");
}

bool tryConnectToWiFi() {
  for (int i = 0; i < networkCount; i++) {
    Serial.printf("Try WiFi: %s\n", wifiList[i].ssid);

    WiFi.begin(wifiList[i].ssid, wifiList[i].password);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected");
      Serial.print("SSID: ");
      Serial.println(wifiList[i].ssid);
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      client.disconnect();
      espClient.stop();
      delay(100);

      mqtt_server = wifiList[i].mqttServer;
      client.setServer(mqtt_server, 1883);
      mqttReconnectPending = true;

      Serial.print("MQTT server: ");
      Serial.println(mqtt_server);

      startHttpServer();
      return true;
    } else {
      WiFi.disconnect(true);
      Serial.println("\nWiFi failed");
    }
  }

  mqtt_server = nullptr;
  return false;
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT topic: ");
  Serial.println(topic);
}

void handleMQTT() {
  if (!wifiConnected) return;
  if (client.connected()) return;
  if (!mqttReconnectPending) return;
  if (mqttBusy) return;

  unsigned long now = millis();
  if (now - lastMqttRetry < mqttRetryInterval) return;

  mqttBusy = true;
  lastMqttRetry = now;

  Serial.print("MQTT reconnect to ");
  Serial.println(mqtt_server);

  if (client.connect("XIAO_ESP32S3_Client")) {
    Serial.println("MQTT connected");
    client.subscribe("xiao/cmd");
    mqttReconnectPending = false;
    mqttBusy = false;
    return;
  }

  Serial.print("MQTT failed rc=");
  Serial.println(client.state());
  mqttBusy = false;
}

void handleWiFiRoaming() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }

  wifiConnected = false;

  unsigned long now = millis();
  if (now - lastWifiRetry < wifiRetryInterval) return;

  lastWifiRetry = now;

  Serial.println("WiFi lost -> try roaming");
  WiFi.disconnect(true);

  wifiConnected = tryConnectToWiFi();

  if (!wifiConnected) {
    Serial.println("No known WiFi available");
  }
}




static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char* STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void handle_jpg_stream() {
  WiFiClient client = server.client();

  String response = "HTTP/1.1 200 OK\r\n";
  response += "Access-Control-Allow-Origin: *\r\n";
  response += "Content-Type: ";
  response += STREAM_CONTENT_TYPE;
  response += "\r\n\r\n";
  server.sendContent(response);

  Serial.println("Client connected to /stream");

  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      break;
    }

    char header[128];
    snprintf(header, sizeof(header), STREAM_PART, fb->len);

    server.sendContent(STREAM_BOUNDARY);
    server.sendContent(header);
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");

    esp_camera_fb_return(fb);
    delay(30);
  }

  Serial.println("Client disconnected from /stream");
}

void handle_root() {
  Serial.println("Client connected to /");

  String html =
    "<html><body style='margin:0;background:#111;text-align:center;'>"
    "<h2 style='color:white;'>XIAO ESP32S3 Sense Stream</h2>"
    "<img src='/stream' style='width:100%;max-width:960px;'>"
    "</body></html>";

  server.send(200, "text/html", html);
}

void startCamera() {
  Serial.println("Configuring camera...");

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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    Serial.println("PSRAM found");
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    Serial.println("PSRAM not found");
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 16;
    config.fb_count = 1;
  }

  Serial.println("Calling esp_camera_init...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    while (true) delay(1000);
  }

  Serial.println("Camera init OK");
}

// void setup() {
//   Serial.begin(115200);
//   delay(3000);
//   Serial.println("Boot");

//   startCamera();

//   Serial.print("Connecting to WiFi: ");
//   // Serial.println(ssid);

//   // WiFi.begin(ssid, password);

//   int cnt = 0;
//   while (WiFi.status() != WL_CONNECTED && cnt < 30) {
//     delay(500);
//     Serial.print(".");
//     cnt++;
//   }

//   Serial.println();

//   if (WiFi.status() != WL_CONNECTED) {
//     Serial.println("WiFi connection failed");
//     return;
//   }

//   Serial.println("WiFi connected");
//   Serial.print("Open: http://");
//   Serial.println(WiFi.localIP());

//   server.on("/", handle_root);
//   server.on("/stream", HTTP_GET, handle_jpg_stream);
//   server.begin();

//   Serial.println("HTTP server started");
// }

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("Boot");

  startCamera();

  WiFi.mode(WIFI_STA);
  client.setCallback(callback);

  wifiConnected = tryConnectToWiFi();

  if (!wifiConnected) {
    Serial.println("Initial WiFi connection failed");
  }
}

const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

// void printSystemStatus() {
//   Serial.println();
//   Serial.println("===== SYSTEM STATUS =====");

//   // WiFi
//   Serial.print("WiFi status: ");
//   Serial.println(wifiStatusToString(WiFi.status()));

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.print("IP: ");
//     Serial.println(WiFi.localIP());

//     Serial.print("RSSI: ");
//     Serial.print(WiFi.RSSI());
//     Serial.println(" dBm");
//   }

//   // Camera test
//   camera_fb_t *fb = esp_camera_fb_get();
//   if (fb) {
//     Serial.print("Camera OK, frame size: ");
//     Serial.println(fb->len);
//     esp_camera_fb_return(fb);
//   } else {
//     Serial.println("Camera ERROR");
//   }

//   Serial.println("==========================");
// }
void printSystemStatus() {
  Serial.println();
  Serial.println("===== SYSTEM STATUS =====");

  Serial.print("WiFi status: ");
  Serial.println(wifiStatusToString(WiFi.status()));

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }

  Serial.print("MQTT: ");
  Serial.println(client.connected() ? "CONNECTED" : "DISCONNECTED");

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    Serial.print("Camera OK, frame size: ");
    Serial.println(fb->len);
    esp_camera_fb_return(fb);
  } else {
    Serial.println("Camera ERROR");
  }

  Serial.println("==========================");
}

// void loop() {
//   server.handleClient();

// static unsigned long lastPrint = 0;

//   if (millis() - lastPrint > 10000) {   // la fiecare 5 sec
//     printSystemStatus();
//     lastPrint = millis();
//   }

//   delay(1);
 
// }
void loop() {
  handleWiFiRoaming();
  handleMQTT();
  client.loop();

  if (wifiConnected && httpStarted) {
    server.handleClient();
  }

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 10000) {
    printSystemStatus();
    lastPrint = millis();
  }

  delay(1);
}