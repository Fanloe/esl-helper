// 模板:复制为 include/wifi_secrets.h 并填入自己的 WiFi
// 真实文件 wifi_secrets.h 已被 .gitignore 忽略,不会提交
#pragma once

struct WifiCred { const char* ssid; const char* pass; };

// 按顺序尝试,连上就停。ESP32-C3 只有 2.4GHz,5GHz 热点写进来也没用
static const WifiCred WIFI_LIST[] = {
  {"YourSSID", "YourPassword"},
};
static const int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);
