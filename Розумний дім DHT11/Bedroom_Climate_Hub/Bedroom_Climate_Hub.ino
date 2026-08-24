#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <time.h>

// ==========================================
// Конфігурація обладнання та системні параметри
// ==========================================
#define DHTPIN 4
#define DHTTYPE DHT11
#define RESET_BUTTON_PIN 0   // BOOT кнопка (GPIO 0)
#define WDT_TIMEOUT 60       // Таймаут сторожового таймера (60 секунд)
#define HOLD_TIME_RESET 3000 // Час утримання кнопки для скидання (3 сек)

// GitHub OTA Конфігурація
#define GITHUB_REPO_OWNER "mikostav02-cloud"
#define GITHUB_REPO_NAME "update_lite"
#define CURRENT_VERSION "3.0"

const byte DNS_PORT = 53;
const char* AP_SSID = "Bedroom-Climate-Hub";
const char* MDNS_NAME = "bedroom1";

// ==========================================
// Глобальні об'єкти
// ==========================================
DHT dht(DHTPIN, DHTTYPE);
Preferences prefs;
DNSServer dnsServer;
WebServer server(80);

// ==========================================
// Налаштування (зберігаються в NVRAM/Flash)
// ==========================================
char wifi_ssid[33] = "";
char wifi_pass[65] = "";
char bot_token[70] = "";
char user_chat_id[25] = "";
char group_chat_id[25] = "";
char group_thread_id[15] = "";

// Динамічні параметри алертів (порогові значення та гістерезис)
float alert_temp_high = 28.0;
float alert_temp_low = 16.0;
float alert_hum_high = 70.0;
float alert_hum_low = 30.0;
const float HYSTERESIS_TEMP = 0.5;
const float HYSTERESIS_HUM = 2.0;

// НОВІ НАЛАШТУВАННЯ
bool use_static_ip = false;
char static_ip[16] = "192.168.1.200";
char static_gateway[16] = "192.168.1.1";
char static_subnet[16] = "255.255.255.0";
char static_dns[16] = "1.1.1.1";

float temp_offset = 0.0;
float hum_offset = 0.0;

unsigned long sensor_read_interval = 5000;  // Інтервал опитування DHT (мс)
unsigned long bot_interval = 2000;          // Інтервал перевірки Telegram (мс)
unsigned long alert_interval = 900000;       // Інтервал повторних сповіщень (мс) - 15 хв
bool alerts_enabled = true;                  // Тумблер алертів

int gmt_offset_sec = 7200;                  // Часовий пояс UTC+2 (у секундах)
int daylight_offset_sec = 3600;             // Літній час (+1 година)
char ntp_server[64] = "pool.ntp.org";

// Стан інтерактивного редагування в Telegram
String waiting_input_chat_id = "";
String waiting_input_param = ""; 

// ==========================================
// Системні змінні стану
// ==========================================
unsigned long last_bot_check = 0;
unsigned long last_sensor_read = 0;
long last_update_id = 0;

bool ap_mode = false;
bool boot_msg_sent = false;

// Показники датчика з урахуванням калибровки
float cached_temp = NAN;
float cached_hum = NAN;

// Змінні для збереження Min/Max показників
float min_temp = 999.0;
float max_temp = -999.0;
float min_hum = 999.0;
float max_hum = -999.0;

// Змінні для обробки кнопки BOOT
unsigned long btn_press_start = 0;
bool btn_state_last = HIGH;

// Змінні для авто-відновлення Wi-Fi
unsigned long last_wifi_reconnect_attempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 30000; // 30 секунд

// Змінні стану алертів
bool temp_high_alert_active = false;
bool temp_low_alert_active = false;
bool hum_high_alert_active = false;
bool hum_low_alert_active = false;
unsigned long last_alert_time = 0;

// Допоміжна функція для видалення некоректних символів
String cleanString(String str) {
  str.trim();
  String clean = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c != '\r' && c != '\n' && c != ' ' && c != '\t') {
      clean += c;
    }
  }
  return clean;
}

// Допоміжна функція розрахунку якості сигналу Wi-Fi у відсотках
int getQuality() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

// Допоміжна функція розрахунку точки роси (Формула Магнуса-Тетенса)
float calculateDewPoint(float temp, float humidity) {
  float a = 17.27;
  float b = 237.7;
  float alpha = ((a * temp) / (b + temp)) + log(humidity / 100.0);
  return (b * alpha) / (a - alpha);
}

// Отримання форматованого часу з NTP
String getFormattedTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "Час не синхронізовано";
  }
  char timeStringBuff[30];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// Читання датчика з урахуванням калибровки
void readSensorData() {
  if (millis() - last_sensor_read >= sensor_read_interval || isnan(cached_temp)) {
    last_sensor_read = millis();
    float raw_t = dht.readTemperature();
    float raw_h = dht.readHumidity();

    if (!isnan(raw_t)) {
      cached_temp = raw_t + temp_offset;
      if (cached_temp < min_temp) min_temp = cached_temp;
      if (cached_temp > max_temp) max_temp = cached_temp;
    } else {
      cached_temp = NAN;
    }

    if (!isnan(raw_h)) {
      cached_hum = raw_h + hum_offset;
      if (cached_hum < min_hum) min_hum = cached_hum;
      if (cached_hum > max_hum) max_hum = cached_hum;
    } else {
      cached_hum = NAN;
    }
  }
}

// ==========================================
// 1. Головна сторінка налаштувань (збільшена)
// ==========================================
const char PAGE_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Bedroom 1 — Sensor Hub</title>
  <style>
    :root {
      --bg-gradient: linear-gradient(135deg, #f0f4ff 0%, #e0e7ff 50%, #f3e8ff 100%);
      --glass-bg: rgba(255, 255, 255, 0.75);
      --glass-border: rgba(255, 255, 255, 0.9);
      --accent-gradient: linear-gradient(135deg, #6366f1, #4f46e5);
      --accent-hover: linear-gradient(135deg, #4338ca, #3730a3);
      --text-main: #0f172a;
      --text-sub: #475569;
      --input-bg: rgba(255, 255, 255, 0.85);
      --input-border: rgba(203, 213, 225, 0.8);
      --input-focus: #6366f1;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif;
      background: var(--bg-gradient);
      background-attachment: fixed;
      color: var(--text-main);
      min-height: 100vh;
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 24px;
    }
    .container {
      background: var(--glass-bg);
      backdrop-filter: blur(24px);
      -webkit-backdrop-filter: blur(24px);
      border: 1px solid var(--glass-border);
      border-radius: 36px;
      padding: 36px 40px;
      width: 100%;
      max-width: 880px;
      box-shadow: 0 25px 60px rgba(0, 0, 0, 0.08), inset 0 1px 0 rgba(255, 255, 255, 1);
      animation: fadeIn 0.5s ease-out;
    }
    .header { text-align: center; margin-bottom: 28px; }
    .icon-box {
      width: 72px; height: 72px;
      background: var(--accent-gradient);
      border-radius: 22px;
      display: inline-flex; align-items: center; justify-content: center;
      font-size: 36px; margin-bottom: 14px;
      box-shadow: 0 12px 24px rgba(99, 102, 241, 0.3);
      animation: pulse 4s infinite ease-in-out;
    }
    h1 { font-size: 32px; font-weight: 800; letter-spacing: -0.5px; color: var(--text-main); }
    p.subtitle { font-size: 16px; color: var(--text-sub); margin-top: 6px; font-weight: 600; }
    
    .form-grid {
      display: grid;
      grid-template-columns: 1fr;
      gap: 20px;
    }
    @media (min-width: 768px) {
      .form-grid { grid-template-columns: 1fr 1fr; gap: 20px 28px; }
      .full-width { grid-column: span 2; }
    }

    .section-title {
      font-size: 14px; font-weight: 800; color: #4f46e5;
      text-transform: uppercase; letter-spacing: 1.5px; margin: 16px 0 8px 0;
      display: flex; align-items: center; gap: 16px;
    }
    .section-title::after { content: ""; flex: 1; height: 2px; background: rgba(99, 102, 241, 0.15); }
    
    .form-group { margin-bottom: 0; }
    label { display: block; font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.8px; color: var(--text-sub); margin-bottom: 8px; }
    
    input {
      width: 100%; padding: 16px 20px; background: var(--input-bg);
      border: 2px solid var(--input-border); border-radius: 18px;
      color: var(--text-main); font-size: 16px; outline: none;
      transition: all 0.25s ease;
      box-shadow: inset 0 2px 4px rgba(0,0,0,0.02);
    }
    input:focus {
      border-color: var(--input-focus);
      background: #ffffff;
      box-shadow: 0 0 0 4px rgba(99, 102, 241, 0.18);
    }
    
    .btn-submit {
      position: relative; overflow: hidden;
      width: 100%; padding: 18px; background: var(--accent-gradient);
      border: none; border-radius: 18px; color: white; font-size: 18px; font-weight: 800;
      cursor: pointer; margin-top: 12px;
      box-shadow: 0 12px 24px rgba(99, 102, 241, 0.3);
      transition: all 0.2s ease;
    }
    .btn-submit:hover { background: var(--accent-hover); transform: translateY(-2px); box-shadow: 0 16px 32px rgba(99, 102, 241, 0.4); }
    .btn-submit:active { transform: scale(0.98); }
    
    .ota-link {
      display: flex; align-items: center; justify-content: center; gap: 10px;
      width: 100%; margin-top: 16px; padding: 16px; border-radius: 18px;
      background: rgba(255, 255, 255, 0.7); border: 2px solid var(--glass-border);
      color: var(--text-sub); text-decoration: none; font-size: 15px; font-weight: 700;
      transition: all 0.2s ease;
    }
    .ota-link:hover { background: #ffffff; color: var(--text-main); border-color: rgba(99, 102, 241, 0.4); }

    @keyframes fadeIn { from { opacity: 0; transform: translateY(15px); } to { opacity: 1; transform: translateY(0); } }
    @keyframes pulse { 0%, 100% { transform: scale(1); } 50% { transform: scale(1.05); } }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="icon-box">🛏️</div>
      <h1>Спальня 1 — Hub</h1>
      <p class="subtitle">Панель керування v3.0</p>
    </div>
    
    <form action="/save" method="POST">
      <div class="form-grid">
        <div class="full-width">
          <div class="section-title">Wi-Fi Мережа</div>
        </div>
        <div class="form-group">
          <label>Назва Wi-Fi (SSID)</label>
          <input type="text" name="ssid" value="%SSID%" required autocomplete="off">
        </div>
        <div class="form-group">
          <label>Пароль Wi-Fi</label>
          <input type="password" name="pass" value="%PASS%">
        </div>

        <div class="full-width">
          <div class="section-title">Telegram Бот</div>
        </div>
        <div class="form-group">
          <label>Токен Бота</label>
          <input type="password" name="token" value="%TOKEN%" required>
        </div>
        <div class="form-group">
          <label>Особистий Chat ID</label>
          <input type="text" name="user_id" value="%USER_ID%" required>
        </div>
        <div class="form-group">
          <label>Chat ID Групи</label>
          <input type="text" name="group_id" value="%GROUP_ID%">
        </div>
        <div class="form-group">
          <label>ID Гілки / Топіка</label>
          <input type="text" name="thread_id" value="%THREAD_ID%">
        </div>

        <div class="full-width">
          <button type="submit" class="btn-submit">Зберегти та перезавантажити</button>
        </div>
      </div>
    </form>

    <a href="/update" class="ota-link">⚙️ OTA Оновлення прошивки (.bin)</a>
  </div>
</body>
</html>
)rawliteral";

// ==========================================
// 2. Сторінка успішного збереження
// ==========================================
const char PAGE_SUCCESS[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Збережено</title>
  <style>
    body {
      background: linear-gradient(135deg, #f0f4ff 0%, #e0e7ff 50%, #f3e8ff 100%);
      background-attachment: fixed;
      color: #0f172a; font-family: system-ui, -apple-system, sans-serif;
      display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; text-align: center;
      padding: 20px;
    }
    .card {
      background: rgba(255, 255, 255, 0.75); backdrop-filter: blur(24px); -webkit-backdrop-filter: blur(24px);
      padding: 56px 40px; border-radius: 36px; border: 1px solid rgba(255, 255, 255, 0.9);
      max-width: 440px; width: 100%; box-shadow: 0 25px 60px rgba(0, 0, 0, 0.08);
      animation: popIn 0.4s ease-out;
    }
    .check-icon { font-size: 64px; margin-bottom: 20px; display: block; }
    h2 { color: #059669; font-size: 26px; font-weight: 800; margin-bottom: 12px; }
    p { color: #475569; font-size: 16px; font-weight: 600; }
    @keyframes popIn { from { opacity: 0; transform: scale(0.95); } to { opacity: 1; transform: scale(1); } }
  </style>
</head>
<body>
  <div class="card">
    <span class="check-icon">✨</span>
    <h2>Налаштування збережено!</h2>
    <p>Пристрій перезавантажується...</p>
  </div>
</body>
</html>
)rawliteral";

// ==========================================
// 3. Сторінка OTA оновлення
// ==========================================
const char PAGE_OTA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>OTA Оновлення</title>
  <style>
    :root {
      --bg-gradient: linear-gradient(135deg, #f0f4ff 0%, #e0e7ff 50%, #f3e8ff 100%);
      --glass-bg: rgba(255, 255, 255, 0.75);
      --glass-border: rgba(255, 255, 255, 0.9);
      --accent-gradient: linear-gradient(135deg, #6366f1, #4f46e5);
      --accent-hover: linear-gradient(135deg, #4338ca, #3730a3);
      --text-main: #0f172a;
      --text-sub: #475569;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: system-ui, -apple-system, sans-serif; background: var(--bg-gradient);
      background-attachment: fixed; color: var(--text-main); min-height: 100vh;
      display: flex; justify-content: center; align-items: center; padding: 24px;
    }
    .card {
      background: var(--glass-bg); backdrop-filter: blur(24px); -webkit-backdrop-filter: blur(24px);
      padding: 48px 36px; border-radius: 36px; border: 1px solid rgba(255, 255, 255, 0.9);
      width: 100%; max-width: 480px; text-align: center; box-shadow: 0 25px 60px rgba(0, 0, 0, 0.08);
    }
    h2 { font-size: 26px; font-weight: 800; margin-bottom: 28px; color: var(--text-main); }
    
    .file-dropzone {
      position: relative; border: 2px dashed rgba(99, 102, 241, 0.4); border-radius: 22px;
      padding: 40px 20px; background: rgba(255, 255, 255, 0.6); cursor: pointer;
      transition: all 0.25s ease; margin-bottom: 28px; display: block;
    }
    .file-dropzone:hover { border-color: #6366f1; background: rgba(255, 255, 255, 0.9); }
    .file-dropzone input[type=file] { position: absolute; top: 0; left: 0; width: 100%; height: 100%; opacity: 0; cursor: pointer; }
    .file-icon { font-size: 48px; margin-bottom: 12px; display: block; }
    .file-text { font-size: 15px; color: var(--text-sub); font-weight: 700; }
    .file-name { font-size: 14px; color: #4f46e5; margin-top: 8px; font-weight: 800; word-break: break-all; }
    
    button {
      width: 100%; padding: 18px; background: var(--accent-gradient); border: none; border-radius: 18px;
      color: white; font-size: 17px; font-weight: 800; cursor: pointer;
      box-shadow: 0 12px 24px rgba(99, 102, 241, 0.3); transition: all 0.2s ease;
    }
    button:hover { background: var(--accent-hover); transform: translateY(-2px); box-shadow: 0 16px 32px rgba(99, 102, 241, 0.4); }
    button:active { transform: scale(0.98); }
    .back-btn { display: inline-block; margin-top: 20px; color: var(--text-sub); text-decoration: none; font-size: 15px; font-weight: 700; }
    .back-btn:hover { color: var(--text-main); }
  </style>
</head>
<body>
  <div class="card">
    <h2>⚙️ OTA Оновлення</h2>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <label class="file-dropzone">
        <span class="file-icon">📁</span>
        <span class="file-text">Оберіть або перетягніть файл .bin</span>
        <div id="fileName" class="file-name"></div>
        <input type="file" name="update" accept=".bin" required onchange="document.getElementById('fileName').innerText = this.files[0]?.name || ''">
      </label>
      <button type="submit">Завантажити та оновити</button>
    </form>
    <a href="/" class="back-btn">← Назад до налаштувань</a>
  </div>
</body>
</html>
)rawliteral";

// ==========================================
// Функції управління конфігурацією
// ==========================================
void loadConfig() {
  prefs.begin("bot_config", true);
  String t_ssid = prefs.getString("ssid", "");
  String t_pass = prefs.getString("pass", "");
  String t_tok = prefs.getString("token", "");
  String t_usr = prefs.getString("user_id", "");
  String t_grp = prefs.getString("group_id", "");
  String t_thr = prefs.getString("thread_id", "");

  alert_temp_high = prefs.getFloat("th", 28.0);
  alert_temp_low = prefs.getFloat("tl", 16.0);
  alert_hum_high = prefs.getFloat("hh", 70.0);
  alert_hum_low = prefs.getFloat("hl", 30.0);

  use_static_ip = prefs.getBool("use_ip", false);
  prefs.getString("ip", static_ip, 16);
  prefs.getString("gw", static_gateway, 16);
  prefs.getString("sn", static_subnet, 16);
  prefs.getString("dns", static_dns, 16);

  temp_offset = prefs.getFloat("t_off", 0.0);
  hum_offset = prefs.getFloat("h_off", 0.0);

  sensor_read_interval = prefs.getULong("s_int", 5000);
  bot_interval = prefs.getULong("b_int", 2000);
  alert_interval = prefs.getULong("a_int", 900000);
  alerts_enabled = prefs.getBool("a_en", true);

  gmt_offset_sec = prefs.getInt("gmt_off", 7200);
  daylight_offset_sec = prefs.getInt("day_off", 3600);
  prefs.getString("ntp", ntp_server, 64);

  prefs.end();

  t_ssid = cleanString(t_ssid);
  t_pass.trim();
  t_tok = cleanString(t_tok);
  t_usr = cleanString(t_usr);
  t_grp = cleanString(t_grp);
  t_thr = cleanString(t_thr);

  t_ssid.toCharArray(wifi_ssid, 33);
  t_pass.toCharArray(wifi_pass, 65);
  t_tok.toCharArray(bot_token, 70);
  t_usr.toCharArray(user_chat_id, 25);
  t_grp.toCharArray(group_chat_id, 25);
  t_thr.toCharArray(group_thread_id, 15);
}

void saveConfig(String s, String p, String t, String u, String g, String th) {
  s = cleanString(s);
  p.trim();
  t = cleanString(t);
  u = cleanString(u);
  g = cleanString(g);
  th = cleanString(th);

  prefs.begin("bot_config", false);
  prefs.clear();
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.putString("token", t);
  prefs.putString("user_id", u);
  prefs.putString("group_id", g);
  prefs.putString("thread_id", th);

  prefs.putFloat("th", alert_temp_high);
  prefs.putFloat("tl", alert_temp_low);
  prefs.putFloat("hh", alert_hum_high);
  prefs.putFloat("hl", alert_hum_low);

  prefs.putBool("use_ip", use_static_ip);
  prefs.putString("ip", static_ip);
  prefs.putString("gw", static_gateway);
  prefs.putString("sn", static_subnet);
  prefs.putString("dns", static_dns);

  prefs.putFloat("t_off", temp_offset);
  prefs.putFloat("h_off", hum_offset);

  prefs.putULong("s_int", sensor_read_interval);
  prefs.putULong("b_int", bot_interval);
  prefs.putULong("a_int", alert_interval);
  prefs.putBool("a_en", alerts_enabled);

  prefs.putInt("gmt_off", gmt_offset_sec);
  prefs.putInt("day_off", daylight_offset_sec);
  prefs.putString("ntp", ntp_server);

  prefs.end();
}

void saveSettings() {
  prefs.begin("bot_config", false);
  prefs.putFloat("th", alert_temp_high);
  prefs.putFloat("tl", alert_temp_low);
  prefs.putFloat("hh", alert_hum_high);
  prefs.putFloat("hl", alert_hum_low);

  prefs.putBool("use_ip", use_static_ip);
  prefs.putString("ip", static_ip);
  prefs.putString("gw", static_gateway);
  prefs.putString("sn", static_subnet);
  prefs.putString("dns", static_dns);

  prefs.putFloat("t_off", temp_offset);
  prefs.putFloat("h_off", hum_offset);

  prefs.putULong("s_int", sensor_read_interval);
  prefs.putULong("b_int", bot_interval);
  prefs.putULong("a_int", alert_interval);
  prefs.putBool("a_en", alerts_enabled);

  prefs.putInt("gmt_off", gmt_offset_sec);
  prefs.putInt("day_off", daylight_offset_sec);
  prefs.putString("ntp", ntp_server);
  prefs.end();
}

void factoryReset() {
  prefs.begin("bot_config", false);
  prefs.clear();
  prefs.end();
  delay(500);
  ESP.restart();
}

// ==========================================
// Web Server Routings
// ==========================================
void handleRoot() {
  String page = PAGE_MAIN;
  page.replace("%SSID%", wifi_ssid);
  page.replace("%PASS%", wifi_pass);
  page.replace("%TOKEN%", bot_token);
  page.replace("%USER_ID%", user_chat_id);
  page.replace("%GROUP_ID%", group_chat_id);
  page.replace("%THREAD_ID%", group_thread_id);
  server.send(200, "text/html", page);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("token")) {
    saveConfig(
      server.arg("ssid"),
      server.arg("pass"),
      server.arg("token"),
      server.arg("user_id"),
      server.arg("group_id"),
      server.arg("thread_id")
    );
    server.send(200, "text/html", PAGE_SUCCESS);
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleNotFound() {
  if (ap_mode) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

void setupWebServerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", PAGE_OTA);
  });
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.onNotFound(handleNotFound);
}

// ==========================================
// Telegram API
// ==========================================
void sendChatAction(String chat_id, String action = "typing", String thread_id = "") {
  if (strlen(bot_token) == 0 || chat_id.length() == 0) return;
  if (ESP.getFreeHeap() < 35000) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(2000);

  if (client.connect("api.telegram.org", 443)) {
    esp_task_wdt_reset();

    String payload = "chat_id=" + chat_id + "&action=" + action;
    if (thread_id.length() > 0 && thread_id != "0") {
      payload += "&message_thread_id=" + thread_id;
    }

    client.println("POST /bot" + String(bot_token) + "/sendChatAction HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(payload.length());
    client.println();
    client.print(payload);

    unsigned long start = millis();
    while (client.connected() && millis() - start < 1500) {
      esp_task_wdt_reset();
      while (client.available()) client.read();
      delay(10);
    }
  }
  client.stop();
  esp_task_wdt_reset();
}

void answerCallbackQuery(String callback_query_id) {
  if (strlen(bot_token) == 0 || callback_query_id.length() == 0) return;
  if (ESP.getFreeHeap() < 35000) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(2000);

  if (client.connect("api.telegram.org", 443)) {
    esp_task_wdt_reset();

    String payload = "callback_query_id=" + callback_query_id;

    client.println("POST /bot" + String(bot_token) + "/answerCallbackQuery HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(payload.length());
    client.println();
    client.print(payload);

    unsigned long start = millis();
    while (client.connected() && millis() - start < 1500) {
      esp_task_wdt_reset();
      while (client.available()) client.read();
      delay(10);
    }
  }
  client.stop();
  esp_task_wdt_reset();
}

void sendTelegramMessage(String chat_id, String text, String thread_id = "", String reply_markup = "") {
  if (strlen(bot_token) == 0 || chat_id.length() == 0) return;
  if (ESP.getFreeHeap() < 35000) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);
  
  if (client.connect("api.telegram.org", 443)) {
    esp_task_wdt_reset();
    
    String payload = "chat_id=" + chat_id + "&text=" + text + "&parse_mode=HTML";
    if (thread_id.length() > 0 && thread_id != "0") {
      payload += "&message_thread_id=" + thread_id;
    }
    if (reply_markup.length() > 0) {
      payload += "&reply_markup=" + reply_markup;
    }

    client.println("POST /bot" + String(bot_token) + "/sendMessage HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(payload.length());
    client.println();
    client.print(payload);

    unsigned long start = millis();
    while (client.connected() && millis() - start < 2000) {
      esp_task_wdt_reset();
      while (client.available()) client.read();
      delay(10);
    }
  }
  client.stop();
  esp_task_wdt_reset();
}

// Редагування існуючого повідомлення замість створення нового
bool editTelegramMessage(String chat_id, long message_id, String text, String reply_markup = "") {
  if (strlen(bot_token) == 0 || chat_id.length() == 0 || message_id == 0) return false;
  if (ESP.getFreeHeap() < 35000) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);

  bool success = false;
  if (client.connect("api.telegram.org", 443)) {
    esp_task_wdt_reset();

    String payload = "chat_id=" + chat_id + "&message_id=" + String(message_id) + "&text=" + text + "&parse_mode=HTML";
    if (reply_markup.length() > 0) {
      payload += "&reply_markup=" + reply_markup;
    }

    client.println("POST /bot" + String(bot_token) + "/editMessageText HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(payload.length());
    client.println();
    client.print(payload);

    unsigned long start = millis();
    while (client.connected() && millis() - start < 2000) {
      esp_task_wdt_reset();
      while (client.available()) {
        String line = client.readStringUntil('\n');
        if (line.indexOf("\"ok\":true") != -1) {
          success = true;
        }
      }
      delay(10);
    }
  }
  client.stop();
  esp_task_wdt_reset();
  return success;
}

// Допоміжна функція для універсального виводу (редагувати або надіслати нове)
void sendOrEditMessage(String chat_id, long message_id, String text, String thread_id, String reply_markup) {
  if (message_id > 0) {
    if (!editTelegramMessage(chat_id, message_id, text, reply_markup)) {
      sendTelegramMessage(chat_id, text, thread_id, reply_markup);
    }
  } else {
    sendTelegramMessage(chat_id, text, thread_id, reply_markup);
  }
}

// ==========================================
// Конструктори Inline-клавіатур (Компактні: по 2 в ряд)
// ==========================================

String getSubMenuKeyboard(String current_action) {
  return "{\"inline_keyboard\":[["
         "{\"text\":\"🔄 Оновити\",\"callback_data\":\"" + current_action + "\"},"
         "{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}"
         "]]}";
}

String getMainMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🌡️ Клімат\",\"callback_data\":\"status\"},{\"text\":\"💧 Точка роси\",\"callback_data\":\"dewpoint\"}],"
         "[{\"text\":\"🔥 Індекс тепла\",\"callback_data\":\"heatindex\"},{\"text\":\"⚙️ Налаштування\",\"callback_data\":\"settings\"}],"
         "[{\"text\":\"🚀 GitHub OTA\",\"callback_data\":\"github_ota\"}]"
         "]}";
}

String getSettingsKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🚨 Алерти/Пороги\",\"callback_data\":\"alerts_menu\"},{\"text\":\"🎯 Офсети\",\"callback_data\":\"offsets_menu\"}],"
         "[{\"text\":\"⏱️ Інтервали\",\"callback_data\":\"intervals_menu\"},{\"text\":\"🌐 IP / Мережа\",\"callback_data\":\"ip_menu\"}],"
         "[{\"text\":\"🕒 NTP Час\",\"callback_data\":\"ntp_menu\"},{\"text\":\"📊 Статистика\",\"callback_data\":\"stats\"}],"
         "[{\"text\":\"📈 Екстремуми\",\"callback_data\":\"minmax\"},{\"text\":\"🔄 Скидання Eкстр.\",\"callback_data\":\"reset_minmax\"}],"
         "[{\"text\":\"📶 Wi-Fi Status\",\"callback_data\":\"wifi\"},{\"text\":\"⏱️ Uptime\",\"callback_data\":\"uptime\"}],"
         "[{\"text\":\"🏓 Ping\",\"callback_data\":\"ping\"},{\"text\":\"🔙 Головне меню\",\"callback_data\":\"menu\"}]"
         "]}";
}

String getAlertsMenuKeyboard() {
  String alertStateText = alerts_enabled ? "🔔 Сповіщення: УВІМК" : "🔕 Сповіщення: ВИМК";
  return "{\"inline_keyboard\":["
         "[{\"text\":\"" + alertStateText + "\",\"callback_data\":\"toggle_alerts\"}],"
         "[{\"text\":\"🔥 High T (" + String(alert_temp_high, 1) + "°C)\",\"callback_data\":\"set_th\"},"
         "{\"text\":\"❄️ Low T (" + String(alert_temp_low, 1) + "°C)\",\"callback_data\":\"set_tl\"}],"
         "[{\"text\":\"💦 High H (" + String(alert_hum_high, 1) + "%)\",\"callback_data\":\"set_hh\"},"
         "{\"text\":\"🌵 Low H (" + String(alert_hum_low, 1) + "%)\",\"callback_data\":\"set_hl\"}],"
         "[{\"text\":\"⏱️ Повтор (" + String(alert_interval / 60000) + "хв)\",\"callback_data\":\"set_a_int\"},"
         "{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]"
         "]}";
}

String getOffsetsMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🌡️ Офсет T (" + String(temp_offset, 1) + "°C)\",\"callback_data\":\"set_toff\"},"
         "{\"text\":\"💧 Офсет H (" + String(hum_offset, 1) + "%)\",\"callback_data\":\"set_hoff\"}],"
         "[{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]"
         "]}";
}

String getIntervalsMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🌡️ DHT Poll (" + String(sensor_read_interval / 1000) + "с)\",\"callback_data\":\"set_s_int\"},"
         "{\"text\":\"🤖 Bot Poll (" + String(bot_interval) + "мс)\",\"callback_data\":\"set_b_int\"}],"
         "[{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]"
         "]}";
}

String getIPMenuKeyboard() {
  String ipModeText = use_static_ip ? "📌 Режим: Static IP" : "🎲 Режим: DHCP";
  return "{\"inline_keyboard\":["
         "[{\"text\":\"" + ipModeText + "\",\"callback_data\":\"toggle_ip_mode\"}],"
         "[{\"text\":\"📍 Static IP\",\"callback_data\":\"set_static_ip\"},"
         "{\"text\":\"🌐 Gateway\",\"callback_data\":\"set_static_gw\"}],"
         "[{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]"
         "]}";
}

String getNTPMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🖥️ NTP Сервер\",\"callback_data\":\"set_ntp_server\"},"
         "{\"text\":\"🌍 Зсув UTC\",\"callback_data\":\"set_gmt_offset\"}],"
         "[{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]"
         "]}";
}

// ==========================================
// Перевірка оновлень з GitHub
// ==========================================
void checkGitHubUpdate(String chat_id, String thread_id = "", long message_id = 0) {
  sendOrEditMessage(chat_id, message_id, "🔍 <b>Перевірка оновлень на GitHub...</b>", thread_id, "");

  WiFiClientSecure client;
  client.setInsecure();

  String url = "https://api.github.com/repos/" + String(GITHUB_REPO_OWNER) + "/" + String(GITHUB_REPO_NAME) + "/releases/latest";
  
  HTTPClient http;
  if (http.begin(client, url)) {
    http.addHeader("User-Agent", "ESP32-HTTP-Client");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(4096);
      deserializeJson(doc, payload);

      String tag_name = doc["tag_name"].as<String>();
      tag_name.replace("v", "");
      tag_name.trim();

      if (tag_name != String(CURRENT_VERSION)) {
        sendTelegramMessage(chat_id, "🚀 <b>Знайдено нову версію: v" + tag_name + "!</b>\nПочинаю завантаження та прошивку...", thread_id);
        
        JsonArray assets = doc["assets"].as<JsonArray>();
        String downloadUrl = "";
        for (JsonObject asset : assets) {
          String name = asset["name"].as<String>();
          if (name.endsWith(".bin")) {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
          }
        }

        if (downloadUrl.length() > 0) {
          t_httpUpdate_return ret = httpUpdate.update(client, downloadUrl);

          switch (ret) {
            case HTTP_UPDATE_FAILED:
              sendTelegramMessage(chat_id, "❌ <b>Помилка OTA:</b> " + httpUpdate.getLastErrorString(), thread_id, getSubMenuKeyboard("github_ota"));
              break;
            case HTTP_UPDATE_NO_UPDATES:
              sendTelegramMessage(chat_id, "ℹ️ Немає доступних оновлень.", thread_id, getSubMenuKeyboard("github_ota"));
              break;
            case HTTP_UPDATE_OK:
              sendTelegramMessage(chat_id, "✅ <b>Оновлення успішно встановлено!</b> Пристрій перезавантажується...", thread_id);
              delay(1000);
              ESP.restart();
              break;
          }
        } else {
          sendTelegramMessage(chat_id, "❌ <b>Помилка:</b> Файл .bin не знайдено в останньому релізі.", thread_id, getSubMenuKeyboard("github_ota"));
        }
      } else {
        sendOrEditMessage(chat_id, message_id, "✅ <b>У вас встановлена найновіша версія: v" + String(CURRENT_VERSION) + "</b>", thread_id, getMainMenuKeyboard());
      }
    } else {
      sendOrEditMessage(chat_id, message_id, "⚠️ Помилка з'єднання з GitHub API (Код: " + String(httpCode) + ")", thread_id, getMainMenuKeyboard());
    }
    http.end();
  } else {
    sendOrEditMessage(chat_id, message_id, "❌ Помилка ініціалізації з'єднання.", thread_id, getMainMenuKeyboard());
  }
}

// ==========================================
// Меню та діалогові екрани
// ==========================================
void sendMenuMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📋 <b>Головне меню — Спальня 1</b>\n\nОберіть потрібний розділ:";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getMainMenuKeyboard());
}

void sendSettingsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "⚙️ <b>Панель налаштувань та діагностики</b>\n\nОберіть категорію:";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSettingsKeyboard());
}

void sendAlertsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🚨 <b>Налаштування сповіщень та порогів</b>\n\n";
  msg += "Стан: " + String(alerts_enabled ? "🟢 Увімкнені" : "🔴 Вимкнені") + "\n";
  msg += "🔥 Висока T: <code>" + String(alert_temp_high, 1) + " °C</code>\n";
  msg += "❄️ Низька T: <code>" + String(alert_temp_low, 1) + " °C</code>\n";
  msg += "💦 Висока H: <code>" + String(alert_hum_high, 1) + " %</code>\n";
  msg += "🌵 Низька H: <code>" + String(alert_hum_low, 1) + " %</code>\n";
  msg += "⏱️ Повтор: <code>" + String(alert_interval / 60000) + " хв</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getAlertsMenuKeyboard());
}

void sendOffsetsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🎯 <b>Калібрування датчика (Офсети)</b>\n\n";
  msg += "🌡️ Офсет температури: <code>" + String(temp_offset, 1) + " °C</code>\n";
  msg += "💧 Офсет вологості: <code>" + String(hum_offset, 1) + " %</code>\n\n";
  msg += "<i>Вкажіть поправку (можна від'ємне, наприклад -1.5).</i>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getOffsetsMenuKeyboard());
}

void sendIntervalsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "⏱️ <b>Системні інтервали</b>\n\n";
  msg += "🌡️ Опитування DHT: <code>" + String(sensor_read_interval / 1000) + " сек</code>\n";
  msg += "🤖 Перевірка Telegram: <code>" + String(bot_interval) + " мс</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getIntervalsMenuKeyboard());
}

void sendIPMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🌐 <b>Мережеві налаштування IP</b>\n\n";
  msg += "Режим: <code>" + String(use_static_ip ? "Static IP" : "DHCP") + "</code>\n";
  msg += "📍 IP: <code>" + WiFi.localIP().toString() + "</code>\n";
  msg += "🌐 Gateway: <code>" + WiFi.gatewayIP().toString() + "</code>\n";
  msg += "🎭 Subnet: <code>" + WiFi.subnetMask().toString() + "</code>\n";
  msg += "🔍 DNS: <code>" + WiFi.dnsIP().toString() + "</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getIPMenuKeyboard());
}

void sendNTPMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🕒 <b>Синхронізація часу (NTP)</b>\n\n";
  msg += "🖥️ Сервер: <code>" + String(ntp_server) + "</code>\n";
  msg += "🌍 Зміщення GMT: <code>" + String(gmt_offset_sec / 3600) + " год</code>\n";
  msg += "🕒 Поточний час: <code>" + getFormattedTime() + "</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getNTPMenuKeyboard());
}

void sendStatusMessage(String chat_id, String thread_id = "", long message_id = 0) {
  readSensorData();

  String msg = "🌡️ <b>Клімат — Спальня 1</b>\n\n";
  if (isnan(cached_temp) || isnan(cached_hum)) {
    msg += "⚠️ <i>Помилка датчика DHT11</i>";
  } else {
    msg += "<b>Температура:</b> <code>" + String(cached_temp, 1) + " °C</code>\n";
    msg += "<b>Вологість:</b> <code>" + String(cached_hum, 1) + " %</code>\n";
    msg += "🕒 <b>Час:</b> <code>" + getFormattedTime() + "</code>";
  }

  String kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 Оновити\",\"callback_data\":\"status\"},{\"text\":\"🔙 Меню\",\"callback_data\":\"menu\"}]]}";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, kb);
}

void sendDewPointMessage(String chat_id, String thread_id = "", long message_id = 0) {
  readSensorData();

  String msg = "💧 <b>Точка роси — Спальня 1</b>\n\n";
  if (isnan(cached_temp) || isnan(cached_hum)) {
    msg += "⚠️ <i>Помилка датчика DHT11</i>";
  } else {
    float dp = calculateDewPoint(cached_temp, cached_hum);
    msg += "<b>Значення:</b> <code>" + String(dp, 1) + " °C</code>\n\n";
    msg += "<b>Оцінка комфорту:</b>\n";
    if (dp < 10.0) msg += "❄️ <i>Сухо</i>";
    else if (dp <= 16.0) msg += "🟢 <i>Ідеальний комфорт</i>";
    else if (dp <= 18.0) msg += "🟡 <i>Помірно волого</i>";
    else if (dp <= 21.0) msg += "🟠 <i>Волого та душно</i>";
    else msg += "🔴 <i>Критична духота</i>";
  }

  String kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 Оновити\",\"callback_data\":\"dewpoint\"},{\"text\":\"🔙 Меню\",\"callback_data\":\"menu\"}]]}";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, kb);
}

void sendHeatIndexMessage(String chat_id, String thread_id = "", long message_id = 0) {
  readSensorData();

  String msg = "🌡️ <b>Індекс тепла — Спальня 1</b>\n\n";
  if (isnan(cached_temp) || isnan(cached_hum)) {
    msg += "⚠️ <i>Помилка датчика DHT11</i>";
  } else {
    float hi = dht.computeHeatIndex(cached_temp, cached_hum, false);
    msg += "<b>Фактична температура:</b> <code>" + String(cached_temp, 1) + " °C</code>\n";
    msg += "<b>Відчувається як:</b> <code>" + String(hi, 1) + " °C</code>";
  }

  String kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 Оновити\",\"callback_data\":\"heatindex\"},{\"text\":\"🔙 Меню\",\"callback_data\":\"menu\"}]]}";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, kb);
}

void sendStatsMessage(String chat_id, String thread_id = "", long message_id = 0) {
  readSensorData();

  String msg = "📊 <b>Зведена статистика — Спальня 1</b>\n\n";
  if (isnan(cached_temp) || isnan(cached_hum)) {
    msg += "⚠️ <i>Помилка датчика DHT11</i>\n\n";
  } else {
    float dp = calculateDewPoint(cached_temp, cached_hum);
    float hi = dht.computeHeatIndex(cached_temp, cached_hum, false);

    msg += "🌡️ <b>Температура:</b> <code>" + String(cached_temp, 1) + " °C</code>\n";
    msg += "💧 <b>Вологість:</b> <code>" + String(cached_hum, 1) + " %</code>\n";
    msg += "🌡️ <b>Відчувається як:</b> <code>" + String(hi, 1) + " °C</code>\n";
    msg += "💦 <b>Точка роси:</b> <code>" + String(dp, 1) + " °C</code>\n\n";

    msg += "📈 <b>Екстремуми:</b>\n";
    msg += " ├ T: <code>" + String(min_temp, 1) + "</code> ... <code>" + String(max_temp, 1) + " °C</code>\n";
    msg += " └ H: <code>" + String(min_hum, 1) + "</code> ... <code>" + String(max_hum, 1) + " %</code>\n\n";
  }

  msg += "📶 <b>Сигнал Wi-Fi:</b> <code>" + String(getQuality()) + "%</code>\n";
  msg += "🧠 <b>Пам'ять:</b> <code>" + String(ESP.getFreeHeap() / 1024) + " KB</code>\n";
  msg += "🕒 <b>Час:</b> <code>" + getFormattedTime() + "</code>";

  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("stats"));
}

void sendMinMaxMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📈 <b>Екстремуми (сеанс)</b>\n\n";
  if (min_temp == 999.0 || min_hum == 999.0) {
    msg += "⚠️ <i>Дані відсутні.</i>";
  } else {
    msg += "🌡️ <b>Температура:</b> <code>" + String(min_temp, 1) + "</code> ... <code>" + String(max_temp, 1) + " °C</code>\n";
    msg += "💧 <b>Вологість:</b> <code>" + String(min_hum, 1) + "</code> ... <code>" + String(max_hum, 1) + " %</code>";
  }

  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("minmax"));
}

void sendResetMinMaxMessage(String chat_id, String thread_id = "", long message_id = 0) {
  min_temp = 999.0;
  max_temp = -999.0;
  min_hum = 999.0;
  max_hum = -999.0;
  String msg = "🔄 <b>Екстремуми успішно скинуто!</b>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("minmax"));
}

void sendUptimeMessage(String chat_id, String thread_id = "", long message_id = 0) {
  unsigned long sec = millis() / 1000;
  unsigned long days = sec / 86400;
  sec %= 86400;
  unsigned long hours = sec / 3600;
  sec %= 3600;
  unsigned long mins = sec / 60;
  sec %= 60;

  String uptimeStr = "";
  if (days > 0) uptimeStr += String(days) + "d ";
  uptimeStr += String(hours) + "h " + String(mins) + "m " + String(sec) + "s";

  String msg = "⚙️ <b>Система — Спальня 1</b>\n\n";
  msg += "⏱️ <b>Uptime:</b> <code>" + uptimeStr + "</code>\n";
  msg += "🧠 <b>Free RAM:</b> <code>" + String(ESP.getFreeHeap() / 1024) + " KB</code>\n";
  msg += "📶 <b>Wi-Fi:</b> <code>" + String(getQuality()) + "% (" + String(WiFi.RSSI()) + " dBm)</code>\n";
  msg += "🕒 <b>NTP Час:</b> <code>" + getFormattedTime() + "</code>";

  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("uptime"));
}

void sendWifiMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📶 <b>Wi-Fi Мережа</b>\n\n";
  msg += "📡 <b>SSID:</b> <code>" + String(wifi_ssid) + "</code>\n";
  msg += "🌐 <b>IP:</b> <code>" + WiFi.localIP().toString() + "</code>\n";
  msg += "📊 <b>Сигнал:</b> <code>" + String(getQuality()) + "% (" + String(WiFi.RSSI()) + " dBm)</code>\n";
  msg += "🏷️ <b>MAC:</b> <code>" + WiFi.macAddress() + "</code>";

  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("wifi"));
}

void sendPingMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🏓 <b>Pong!</b> Node є онлайн.\n🕒 <code>" + getFormattedTime() + "</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("ping"));
}

// ==========================================
// Обробка дій Inline-кнопок
// ==========================================
void processCallback(String data, String chat_id, String target_thread, long message_id) {
  if (data == "menu") sendMenuMessage(chat_id, target_thread, message_id);
  else if (data == "settings") sendSettingsMenu(chat_id, target_thread, message_id);
  else if (data == "alerts_menu") sendAlertsMenu(chat_id, target_thread, message_id);
  else if (data == "offsets_menu") sendOffsetsMenu(chat_id, target_thread, message_id);
  else if (data == "intervals_menu") sendIntervalsMenu(chat_id, target_thread, message_id);
  else if (data == "ip_menu") sendIPMenu(chat_id, target_thread, message_id);
  else if (data == "ntp_menu") sendNTPMenu(chat_id, target_thread, message_id);
  else if (data == "status") sendStatusMessage(chat_id, target_thread, message_id);
  else if (data == "stats") sendStatsMessage(chat_id, target_thread, message_id);
  else if (data == "dewpoint") sendDewPointMessage(chat_id, target_thread, message_id);
  else if (data == "heatindex") sendHeatIndexMessage(chat_id, target_thread, message_id);
  else if (data == "minmax") sendMinMaxMessage(chat_id, target_thread, message_id);
  else if (data == "reset_minmax") sendResetMinMaxMessage(chat_id, target_thread, message_id);
  else if (data == "uptime") sendUptimeMessage(chat_id, target_thread, message_id);
  else if (data == "wifi") sendWifiMessage(chat_id, target_thread, message_id);
  else if (data == "ping") sendPingMessage(chat_id, target_thread, message_id);
  else if (data == "github_ota") checkGitHubUpdate(chat_id, target_thread, message_id);
  
  // Нові функції тумблерів
  else if (data == "toggle_alerts") {
    alerts_enabled = !alerts_enabled;
    saveSettings();
    sendAlertsMenu(chat_id, target_thread, message_id);
  }
  else if (data == "toggle_ip_mode") {
    use_static_ip = !use_static_ip;
    saveSettings();
    sendOrEditMessage(chat_id, message_id, "⚠️ <b>Режим IP змінено!</b> Для застосування змін пристрій треба перезавантажити.", target_thread, getIPMenuKeyboard());
  }
  
  // Команди очікування введення даних
  else if (data == "set_th") {
    waiting_input_chat_id = chat_id; waiting_input_param = "th";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть порог <b>високої температури</b> (°C):", target_thread, "");
  }
  else if (data == "set_tl") {
    waiting_input_chat_id = chat_id; waiting_input_param = "tl";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть порог <b>низької температури</b> (°C):", target_thread, "");
  }
  else if (data == "set_hh") {
    waiting_input_chat_id = chat_id; waiting_input_param = "hh";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть порог <b>високої вологості</b> (%):", target_thread, "");
  }
  else if (data == "set_hl") {
    waiting_input_chat_id = chat_id; waiting_input_param = "hl";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть порог <b>низької вологості</b> (%):", target_thread, "");
  }
  else if (data == "set_a_int") {
    waiting_input_chat_id = chat_id; waiting_input_param = "a_int";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>інтервал алертів</b> у хвилинах (наприклад 15):", target_thread, "");
  }
  else if (data == "set_toff") {
    waiting_input_chat_id = chat_id; waiting_input_param = "toff";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>офсет температури</b> (°C, наприклад -0.5 або 1.2):", target_thread, "");
  }
  else if (data == "set_hoff") {
    waiting_input_chat_id = chat_id; waiting_input_param = "hoff";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>офсет вологості</b> (%, наприклад 3.0 або -2.0):", target_thread, "");
  }
  else if (data == "set_s_int") {
    waiting_input_chat_id = chat_id; waiting_input_param = "s_int";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>інтервал опитування датчика</b> в секундах (мін. 2):", target_thread, "");
  }
  else if (data == "set_b_int") {
    waiting_input_chat_id = chat_id; waiting_input_param = "b_int";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>інтервал перевірки Telegram</b> у мілісекундах (500 - 10000):", target_thread, "");
  }
  else if (data == "set_static_ip") {
    waiting_input_chat_id = chat_id; waiting_input_param = "static_ip";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>Static IP</b> (наприклад 192.168.1.200):", target_thread, "");
  }
  else if (data == "set_static_gw") {
    waiting_input_chat_id = chat_id; waiting_input_param = "static_gw";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>Gateway IP</b> (наприклад 192.168.1.1):", target_thread, "");
  }
  else if (data == "set_ntp_server") {
    waiting_input_chat_id = chat_id; waiting_input_param = "ntp_server";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>адресу NTP сервера</b> (наприклад pool.ntp.org):", target_thread, "");
  }
  else if (data == "set_gmt_offset") {
    waiting_input_chat_id = chat_id; waiting_input_param = "gmt_offset";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть <b>зміщення GMT у годинах</b> (наприклад 2 для UTC+2):", target_thread, "");
  }
}

// ==========================================
// Обробка введення даних з чату
// ==========================================
void processTextInput(String text, String chat_id, String target_thread) {
  if (waiting_input_chat_id == chat_id && waiting_input_param.length() > 0) {
    String param = waiting_input_param;
    waiting_input_chat_id = "";
    waiting_input_param = "";

    text.replace(',', '.');

    if (param == "th") {
      alert_temp_high = text.toFloat();
      saveSettings();
      sendTelegramMessage(chat_id, "✅ High T збережено!", target_thread);
      sendAlertsMenu(chat_id, target_thread);
    } 
    else if (param == "tl") {
      alert_temp_low = text.toFloat();
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Low T збережено!", target_thread);
      sendAlertsMenu(chat_id, target_thread);
    } 
    else if (param == "hh") {
      alert_hum_high = text.toFloat();
      saveSettings();
      sendTelegramMessage(chat_id, "✅ High H збережено!", target_thread);
      sendAlertsMenu(chat_id, target_thread);
    } 
    else if (param == "hl") {
      alert_hum_low = text.toFloat();
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Low H збережено!", target_thread);
      sendAlertsMenu(chat_id, target_thread);
    } 
    else if (param == "a_int") {
      unsigned long val = text.toInt();
      if (val >= 1) {
        alert_interval = val * 60000;
        saveSettings();
        sendTelegramMessage(chat_id, "✅ Інтервал алертів збережено!", target_thread);
      } else {
        sendTelegramMessage(chat_id, "❌ Некоректне значення!", target_thread);
      }
      sendAlertsMenu(chat_id, target_thread);
    } 
    else if (param == "toff") {
      temp_offset = text.toFloat();
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Офсет температури збережено!", target_thread);
      sendOffsetsMenu(chat_id, target_thread);
    } 
    else if (param == "hoff") {
      hum_offset = text.toFloat();
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Офсет вологості збережено!", target_thread);
      sendOffsetsMenu(chat_id, target_thread);
    } 
    else if (param == "s_int") {
      unsigned long val = text.toInt();
      if (val >= 2) {
        sensor_read_interval = val * 1000;
        saveSettings();
        sendTelegramMessage(chat_id, "✅ Інтервал датчика збережено!", target_thread);
      } else {
        sendTelegramMessage(chat_id, "❌ Мінімум 2 секунди!", target_thread);
      }
      sendIntervalsMenu(chat_id, target_thread);
    } 
    else if (param == "b_int") {
      unsigned long val = text.toInt();
      if (val >= 500 && val <= 20000) {
        bot_interval = val;
        saveSettings();
        sendTelegramMessage(chat_id, "✅ Інтервал Telegram збережено!", target_thread);
      } else {
        sendTelegramMessage(chat_id, "❌ Діапазон від 500 до 20000 мс!", target_thread);
      }
      sendIntervalsMenu(chat_id, target_thread);
    } 
    else if (param == "static_ip") {
      text.toCharArray(static_ip, 16);
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Static IP збережено! Перезавантажте пристрій для застосування.", target_thread);
      sendIPMenu(chat_id, target_thread);
    } 
    else if (param == "static_gw") {
      text.toCharArray(static_gateway, 16);
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Gateway збережено!", target_thread);
      sendIPMenu(chat_id, target_thread);
    } 
    else if (param == "ntp_server") {
      text.toCharArray(ntp_server, 64);
      configTime(gmt_offset_sec, daylight_offset_sec, ntp_server);
      saveSettings();
      sendTelegramMessage(chat_id, "✅ NTP сервер збережено!", target_thread);
      sendNTPMenu(chat_id, target_thread);
    } 
    else if (param == "gmt_offset") {
      int val = text.toInt();
      gmt_offset_sec = val * 3600;
      configTime(gmt_offset_sec, daylight_offset_sec, ntp_server);
      saveSettings();
      sendTelegramMessage(chat_id, "✅ Часовий пояс збережено!", target_thread);
      sendNTPMenu(chat_id, target_thread);
    }
  }
}

// ==========================================
// Перевірка Telegram оновлень
// ==========================================
void checkTelegramUpdates() {
  if (strlen(bot_token) == 0) return;
  if (ESP.getFreeHeap() < 35000) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);

  if (client.connect("api.telegram.org", 443)) {
    esp_task_wdt_reset();
    
    String url = "/bot" + String(bot_token) + "/getUpdates?offset=" + String(last_update_id + 1) + "&timeout=0&limit=5";
    client.println("GET " + url + " HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Connection: close");
    client.println();

    unsigned long start_time = millis();
    bool isBody = false;
    String response = "";

    while (client.connected() && (millis() - start_time < 2000)) {
      esp_task_wdt_reset();
      while (client.available()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") isBody = true;
        if (isBody) response += line;
      }
      delay(10);
    }
    client.stop();

    if (response.length() > 0) {
      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, response);

      if (!error && doc["ok"].as<bool>()) {
        JsonArray result = doc["result"].as<JsonArray>();
        for (JsonObject update : result) {
          last_update_id = update["update_id"].as<long>();

          if (update.containsKey("callback_query")) {
            JsonObject cb = update["callback_query"];
            String cb_id = cb["id"].as<String>();
            String data = cb["data"].as<String>();
            JsonObject cb_msg = cb["message"];
            String chat_id = String((long long)cb_msg["chat"]["id"]);
            long message_id = cb_msg["message_id"].as<long>();
            
            String thread_id = "";
            if (cb_msg.containsKey("message_thread_id")) {
              thread_id = String((long)cb_msg["message_thread_id"]);
            }

            bool isUser = chat_id.equals(user_chat_id);
            bool isGroup = chat_id.equals(group_chat_id);

            if (isUser || isGroup) {
              String target_thread = thread_id;
              if (isGroup && target_thread.length() == 0 && strlen(group_thread_id) > 0) {
                target_thread = String(group_thread_id);
              }

              if (isGroup && strlen(group_thread_id) > 0 && target_thread != String(group_thread_id)) {
                answerCallbackQuery(cb_id);
                continue;
              }

              answerCallbackQuery(cb_id);
              processCallback(data, chat_id, target_thread, message_id);
            } else {
              answerCallbackQuery(cb_id);
            }
            continue;
          }

          if (!update.containsKey("message")) continue;
          JsonObject message = update["message"];
          if (!message.containsKey("text")) continue;

          String text = message["text"].as<String>();
          text.trim();
          String chat_id = String((long long)message["chat"]["id"]);
          
          String thread_id = "";
          if (message.containsKey("message_thread_id")) {
            thread_id = String((long)message["message_thread_id"]);
          }

          bool isUser = chat_id.equals(user_chat_id);
          bool isGroup = chat_id.equals(group_chat_id);

          if (!isUser && !isGroup) continue;

          String target_thread = thread_id;
          if (isGroup && target_thread.length() == 0 && strlen(group_thread_id) > 0) {
            target_thread = String(group_thread_id);
          }

          if (isGroup && strlen(group_thread_id) > 0 && target_thread != String(group_thread_id)) {
            continue;
          }

          if (text.startsWith("/start") || text.startsWith("/menu")) {
            sendMenuMessage(chat_id, target_thread, 0);
          } else if (waiting_input_chat_id == chat_id) {
            sendChatAction(chat_id, "typing", target_thread);
            processTextInput(text, chat_id, target_thread);
          }
        }
      }
    }
  }
  esp_task_wdt_reset();
}

// ==========================================
// Перевірка алертів
// ==========================================
void checkClimateAlerts() {
  if (!alerts_enabled || ap_mode || WiFi.status() != WL_CONNECTED) return;
  if (strlen(bot_token) == 0) return;

  readSensorData();

  if (isnan(cached_temp) || isnan(cached_hum)) return;

  String target_chat = (strlen(group_chat_id) > 0) ? String(group_chat_id) : String(user_chat_id);
  String target_thread = (strlen(group_chat_id) > 0) ? String(group_thread_id) : "";

  bool can_send = (millis() - last_alert_time >= alert_interval || last_alert_time == 0);

  if (cached_temp >= alert_temp_high && !temp_high_alert_active) {
    if (can_send) {
      temp_high_alert_active = true;
      last_alert_time = millis();
      sendTelegramMessage(target_chat, "🚨 <b>УВАГА: Висока температура!</b>\nПоточна: <code>" + String(cached_temp, 1) + " °C</code> (Порог: " + String(alert_temp_high, 1) + " °C)", target_thread);
    }
  } else if (cached_temp < (alert_temp_high - HYSTERESIS_TEMP) && temp_high_alert_active) {
    temp_high_alert_active = false;
    sendTelegramMessage(target_chat, "✅ <b>Температура нормалізувалася:</b> <code>" + String(cached_temp, 1) + " °C</code>", target_thread);
  }

  if (cached_temp <= alert_temp_low && !temp_low_alert_active) {
    if (can_send) {
      temp_low_alert_active = true;
      last_alert_time = millis();
      sendTelegramMessage(target_chat, "❄️ <b>УВАГА: Низька температура!</b>\nПоточна: <code>" + String(cached_temp, 1) + " °C</code> (Порог: " + String(alert_temp_low, 1) + " °C)", target_thread);
    }
  } else if (cached_temp > (alert_temp_low + HYSTERESIS_TEMP) && temp_low_alert_active) {
    temp_low_alert_active = false;
    sendTelegramMessage(target_chat, "✅ <b>Температура нормалізувалася:</b> <code>" + String(cached_temp, 1) + " °C</code>", target_thread);
  }

  if (cached_hum >= alert_hum_high && !hum_high_alert_active) {
    if (can_send) {
      hum_high_alert_active = true;
      last_alert_time = millis();
      sendTelegramMessage(target_chat, "💦 <b>УВАГА: Висока вологість!</b>\nПоточна: <code>" + String(cached_hum, 1) + " %</code> (Порог: " + String(alert_hum_high, 1) + " %)", target_thread);
    }
  } else if (cached_hum < (alert_hum_high - HYSTERESIS_HUM) && hum_high_alert_active) {
    hum_high_alert_active = false;
    sendTelegramMessage(target_chat, "✅ <b>Вологість нормалізувалася:</b> <code>" + String(cached_hum, 1) + " %</code>", target_thread);
  }

  if (cached_hum <= alert_hum_low && !hum_low_alert_active) {
    if (can_send) {
      hum_low_alert_active = true;
      last_alert_time = millis();
      sendTelegramMessage(target_chat, "🌵 <b>УВАГА: Низька вологість!</b>\nПоточна: <code>" + String(cached_hum, 1) + " %</code> (Порог: " + String(alert_hum_low, 1) + " %)", target_thread);
    }
  } else if (cached_hum > (alert_hum_low + HYSTERESIS_HUM) && hum_low_alert_active) {
    hum_low_alert_active = false;
    sendTelegramMessage(target_chat, "✅ <b>Вологість нормалізувалася:</b> <code>" + String(cached_hum, 1) + " %</code>", target_thread);
  }
}

// ==========================================
// Авто-відновлення Wi-Fi
// ==========================================
void handleWiFiReconnect() {
  if (ap_mode) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - last_wifi_reconnect_attempt > WIFI_RECONNECT_INTERVAL) {
      last_wifi_reconnect_attempt = millis();
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }
}

// ==========================================
// Точка доступу
// ==========================================
void startAPMode() {
  ap_mode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  setupWebServerRoutes();
  server.begin();
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  dht.begin();
  loadConfig();

  if (strlen(wifi_ssid) > 0) {
    WiFi.mode(WIFI_STA);

    if (use_static_ip) {
      IPAddress ip, gw, sn, dns;
      if (ip.fromString(static_ip) && gw.fromString(static_gateway) && sn.fromString(static_subnet)) {
        dns.fromString(static_dns);
        WiFi.config(ip, gw, sn, dns);
      }
    }

    WiFi.begin(wifi_ssid, wifi_pass);

    unsigned long start_attempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start_attempt < 12000) {
      delay(250);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    startAPMode();
  } else {
    setupWebServerRoutes();
    server.begin();

    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
    }

    configTime(gmt_offset_sec, daylight_offset_sec, ntp_server);

    #if defined(CONFIG_ESP_TASK_WDT_EN)
      esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
      };
      esp_task_wdt_reconfigure(&twdt_config);
    #endif
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();
  }
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  bool btn_state = digitalRead(RESET_BUTTON_PIN);
  if (btn_state == LOW && btn_state_last == HIGH) {
    btn_press_start = millis();
  } else if (btn_state == LOW && btn_state_last == LOW) {
    if (millis() - btn_press_start >= HOLD_TIME_RESET) {
      if (!ap_mode && strlen(user_chat_id) > 0 && strlen(bot_token) > 0) {
        String target_chat = (strlen(group_chat_id) > 0) ? String(group_chat_id) : String(user_chat_id);
        String target_thread = (strlen(group_chat_id) > 0) ? String(group_thread_id) : "";
        sendTelegramMessage(target_chat, "⚙️ <b>Скидання налаштувань...</b> Пристрій перезавантажується в режим точки доступу.", target_thread);
      }
      factoryReset();
    }
  }
  btn_state_last = btn_state;

  if (ap_mode) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    handleWiFiReconnect();
    esp_task_wdt_reset();
    server.handleClient();

    if (!boot_msg_sent && WiFi.status() == WL_CONNECTED) {
      boot_msg_sent = true;
      if (strlen(bot_token) > 0) {
        String target_chat = (strlen(group_chat_id) > 0) ? String(group_chat_id) : String(user_chat_id);
        String target_thread = (strlen(group_chat_id) > 0) ? String(group_thread_id) : "";
        
        if (target_chat.length() > 0) {
          sendTelegramMessage(target_chat, "🟢 <b>Спальня 1 online (v3.0)</b>\nСистема готова до роботи.", target_thread, getMainMenuKeyboard());
        }
      }
    }

    if (millis() - last_bot_check > bot_interval) {
      checkTelegramUpdates();
      checkClimateAlerts();
      last_bot_check = millis();
    }
  }
}