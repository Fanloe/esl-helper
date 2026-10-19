// ESL helper — 场景一:联网拉天气 + 日期,渲染到 4.2" 墨水屏
// 数据源:Open-Meteo(免费无需 key);日期直接取 API 返回的时间戳,不需要 NTP
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "epd4in2_V2.h"
#include "epdpaint.h"
#include "fonts.h"
#include "wifi_secrets.h"  // 真实 WiFi 配置,不入库;模板见 include/wifi_secrets.example.h

// ---- 配置 ----
// 上海。要换城市改这两个数(纬度/经度)
static const char* URL =
  "http://api.open-meteo.com/v1/forecast?latitude=31.23&longitude=121.47"
  "&current=temperature_2m,relative_humidity_2m,weather_code"
  "&hourly=uv_index&timezone=Asia%2FShanghai&forecast_days=1";

// 空气质量(Open-Meteo Air Quality API,同样是免 key)
static const char* AQI_URL =
  "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=31.23&longitude=121.47"
  "&current=us_aqi,pm2_5&timezone=Asia%2FShanghai";

static const unsigned long REFRESH_MS = 10UL * 60 * 1000;  // 10 分钟

// ponytail: 明文 HTTP,天气数据非敏感且免 TLS 握手;要加密改 https:// + WiFiClientSecure

// ---- 绘图板 ----
#define EPD_W 400
#define EPD_H 300
#define BLACK 0  // epdpaint IF_INVERT_COLOR=1:传 0 清位=黑
#define WHITE 1  // 传 1 置位=白

Epd epd;
static uint8_t buf[EPD_W * EPD_H / 8];  // 15000 字节
static Paint paint(buf, EPD_W, EPD_H);

struct Weather {
  float temp = 0;
  int hum = 0;
  int code = -1;
  float uv = -1;   // <0 表示未取到
  int aqi = -1;    // US AQI,<0 表示未取到
  float pm25 = -1;
  int year = 0, mon = 0, day = 0;
};

// ---- 天气代码(WMO)→ 英文短词。字库只有 ASCII,故用英文 ----
static const char* wmoText(int c) {
  if (c == 0) return "Clear";
  if (c == 1) return "Mainly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 55) return "Drizzle";
  if (c == 56 || c == 57) return "Freezing drizzle";
  if (c >= 61 && c <= 65) return "Rain";
  if (c == 66 || c == 67) return "Freezing rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Rain showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c == 95) return "Thunderstorm";
  if (c >= 96) return "Thunderstorm hail";
  return "Unknown";
}

// US AQI -> 等级短词
static const char* aqiText(int a) {
  if (a <= 50) return "Good";
  if (a <= 100) return "Moderate";
  if (a <= 150) return "Unhealthy-S";
  if (a <= 200) return "Unhealthy";
  if (a <= 300) return "Very unhealthy";
  return "Hazardous";
}

// Sakamoto 算法求星期
static const char* weekdayOf(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  static const char* names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (m < 3) y -= 1;
  int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
  if (w < 0) w += 7;
  return names[w];
}

// ESP32-C3 只有 2.4GHz,ch>=36 是 5GHz 频段(本板扫不到)
static void scanNetworks() {
  Serial.println("[scan] scanning...");
  int n = WiFi.scanNetworks();
  if (n < 0) {
    Serial.printf("[scan] failed (%d)\n", n);  // -2 = WIFI_SCAN_FAILED
    return;
  }
  Serial.printf("[scan] %d networks:\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("  %-24s ch=%-3d rssi=%-4d %s\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "enc");
  }
  WiFi.scanDelete();
}

static bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  for (int i = 0; i < WIFI_LIST_N; i++) {
    const WifiCred& c = WIFI_LIST[i];
    Serial.printf("[wifi] trying %s ...\n", c.ssid);
    WiFi.begin(c.ssid, c.pass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[wifi] ok: %s ip=%s\n", c.ssid, WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.printf("[wifi] %s failed (status=%d)\n", c.ssid, (int)WiFi.status());  // 1=找不到该SSID
    WiFi.disconnect();
    delay(100);
  }
  return false;
}

static bool fetchWeather(Weather& w) {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(URL)) {
    Serial.println("[http] begin failed");
    return false;
  }
  int code = http.GET();  // HTTPClient 自动解 chunked
  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] GET failed: %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  Serial.printf("[http] %d bytes\n", payload.length());
  Serial.println(payload.substring(0, 200));

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[json] parse failed: %s\n", err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  w.temp = cur["temperature_2m"].as<float>();
  w.hum = cur["relative_humidity_2m"].as<int>();
  w.code = cur["weather_code"].as<int>();

  const char* iso = cur["time"].as<const char*>();
  if (iso && strlen(iso) >= 16) {
    w.year = atoi(iso);      // "2026-10-14T12:30" -> stops at '-'
    w.mon = atoi(iso + 5);
    w.day = atoi(iso + 8);
  }

  // 紫外线:在 hourly 数组里找与当前小时匹配的那一项
  JsonArray ht = doc["hourly"]["time"].as<JsonArray>();
  JsonArray hu = doc["hourly"]["uv_index"].as<JsonArray>();
  for (size_t i = 0; i < ht.size() && i < hu.size(); i++) {
    const char* th = ht[i].as<const char*>();
    if (th && iso && strncmp(th, iso, 13) == 0) {  // "2026-10-14T12"
      w.uv = hu[i].as<float>();
      break;
    }
  }
  Serial.printf("[data] %.1fC hum=%d%% code=%d uv=%.1f %04d-%02d-%02d\n",
                w.temp, w.hum, w.code, w.uv, w.year, w.mon, w.day);
  return true;
}

// 空气质量是附加信息:失败只记日志,不影响整个屏幕
static bool fetchAirQuality(Weather& w) {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(AQI_URL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[aqi] GET failed: %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[aqi] parse failed: %s\n", err.c_str());
    return false;
  }
  JsonObject cur = doc["current"];
  w.aqi = cur["us_aqi"].as<int>();
  w.pm25 = cur["pm2_5"].as<float>();
  Serial.printf("[aqi] us_aqi=%d pm2.5=%.1f\n", w.aqi, w.pm25);
  return w.aqi >= 0;
}

static void render(const Weather& w, const char* err) {
  paint.Clear(WHITE);
  if (err) {
    paint.DrawStringAt(8, 20, err, &Font16, BLACK);
    return;
  }
  char line[64];

  snprintf(line, sizeof line, "%s %04d-%02d-%02d", weekdayOf(w.year, w.mon, w.day),
           w.year, w.mon, w.day);
  paint.DrawStringAt(8, 10, line, &Font16, BLACK);
  for (int x = 0; x < EPD_W; x++) paint.DrawAbsolutePixel(x, 34, BLACK);  // 分隔线

  snprintf(line, sizeof line, "%.1f C", w.temp);  // 字库无 '°',用 C
  paint.DrawStringAt(8, 50, line, &Font24, BLACK);

  paint.DrawStringAt(8, 100, wmoText(w.code), &Font16, BLACK);

  snprintf(line, sizeof line, "Humidity %d%%", w.hum);
  paint.DrawStringAt(8, 135, line, &Font16, BLACK);

  if (w.uv >= 0) snprintf(line, sizeof line, "UV %.0f", w.uv);
  else snprintf(line, sizeof line, "UV --");
  paint.DrawStringAt(8, 165, line, &Font16, BLACK);

  if (w.aqi >= 0) {
    snprintf(line, sizeof line, "AQI %d (%s)", w.aqi, aqiText(w.aqi));
    paint.DrawStringAt(8, 195, line, &Font16, BLACK);
    if (w.pm25 >= 0) {
      snprintf(line, sizeof line, "PM2.5 %.0f", w.pm25);
      paint.DrawStringAt(8, 225, line, &Font16, BLACK);
    }
  } else {
    paint.DrawStringAt(8, 195, "AQI --", &Font16, BLACK);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[esl] weather scenario start");
  epd.Init();
  Serial.println("[epd] init ok");

  WiFi.mode(WIFI_STA);   // 只初始化一次;循环里不再切 WIFI_OFF(会搞坏 WiFi 栈)
  WiFi.setSleep(false);
  delay(100);
  scanNetworks();        // 启动先扫一遍,排查用
}

void loop() {
  Weather w;
  const char* err = nullptr;

  if (!connectWiFi()) {
    err = "WiFi failed";
    WiFi.disconnect();  // 连接失败后栈还在忙,不先断开扫描会返回 -2
    delay(100);
    scanNetworks();
  } else if (!fetchWeather(w)) {
    err = "HTTP/JSON failed";
  } else {
    fetchAirQuality(w);  // 失败不影响天气显示
  }

  render(w, err);
  epd.Display(buf);
  Serial.printf("[epd] displayed%s\n", err ? " (error state)" : "");

  Serial.printf("[loop] sleeping %lu s\n", REFRESH_MS / 1000);
  delay(REFRESH_MS);
}