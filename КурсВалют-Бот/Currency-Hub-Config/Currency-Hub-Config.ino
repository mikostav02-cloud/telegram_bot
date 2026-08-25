#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <time.h>

// ==========================================
// Конфігурація обладнання та системні параметри
// ==========================================
#define RESET_BUTTON_PIN 0    // BOOT кнопка (GPIO 0)
#define WDT_TIMEOUT 60        // Таймаут сторожового таймера (60 секунд)
#define HOLD_TIME_RESET 3000  // Час утримання кнопки для скидання (3 сек)

// GitHub OTA Конфігурація
#define GITHUB_REPO_OWNER "mikostav02-cloud"
#define GITHUB_REPO_NAME "currency_lite"
#define CURRENT_VERSION "3.0"

const byte DNS_PORT = 53;
const char* AP_SSID = "Currency-Info-Hub";
const char* MDNS_NAME = "currencyhub";

// ==========================================
// Глобальні об'єкти
// ==========================================
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

// НОВІ НАЛАШТУВАННЯ МЕРЕЖІ
bool use_static_ip = false;
char static_ip[16] = "192.168.1.203";
char static_gateway[16] = "192.168.1.1";
char static_subnet[16] = "255.255.255.0";
char static_dns[16] = "1.1.1.1";

unsigned long currency_fetch_interval = 3600000; // Інтервал запиту курсу (мс) - 1 год
unsigned long bot_interval = 2000;             // Інтервал перевірки Telegram (мс)
unsigned long alert_interval = 86400000;       // Інтервал повторних сповіщень (мс) - 24 год
bool alerts_enabled = true;                    // Тумблер алертів

int gmt_offset_sec = 7200;                     // Часовий пояс UTC+2 (у секундах)
int daylight_offset_sec = 3600;                // Літній час (+1 година)
char ntp_server[64] = "pool.ntp.org";

// Стан інтерактивного редагування в Telegram
String waiting_input_chat_id = "";
String waiting_input_param = ""; 

// ==========================================
// Системні змінні стану
// ==========================================
unsigned long last_bot_check = 0;
unsigned long last_currency_fetch = 0;
long last_update_id = 0;

bool ap_mode = false;
bool boot_msg_sent = false;

// Кешовані показники курсу (Приклад: USD/UAH)
float cached_usd_buy = NAN;
float cached_usd_sell = NAN;
float cached_eur_buy = NAN;
float cached_eur_sell = NAN;
String cached_date = "";

// Min/Max показники курсів
float min_usd = 999.0;
float max_usd = -999.0;
float min_eur = 999.0;
float max_eur = -999.0;

// Змінні для обробки кнопки BOOT
unsigned long btn_press_start = 0;
bool btn_state_last = HIGH;

// Змінні для авто-відновлення Wi-Fi
unsigned long last_wifi_reconnect_attempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 30000; // 30 секунд

// Змінні стану алертів
unsigned long last_alert_time = 0;

// Допоміжна функція очищення рядків
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

// Допоміжна функція якості сигналу Wi-Fi
int getQuality() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
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

// Отримання курсу валют з Monobank API
bool fetchCurrencyData() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ESP.getFreeHeap() < 35000) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(4000);

  HTTPClient http;
  String url = "https://api.monobank.ua/bank/currency";

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(16384);
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        JsonArray array = doc.as<JsonArray>();
        bool foundUSD = false;
        bool foundEUR = false;

        for (JsonObject obj : array) {
          int currencyCodeA = obj["currencyCodeA"];
          int currencyCodeB = obj["currencyCodeB"];
          // 840 - USD, 978 - EUR, 980 - UAH
          if (currencyCodeB == 980) {
            if (currencyCodeA == 840 && !foundUSD) {
              cached_usd_buy = obj["rateBuy"].as<float>();
              cached_usd_sell = obj["rateSell"].as<float>();
              if (cached_usd_buy == 0) cached_usd_buy = obj["rateCross"].as<float>();
              if (cached_usd_sell == 0) cached_usd_sell = obj["rateCross"].as<float>();
              foundUSD = true;
            } else if (currencyCodeA == 978 && !foundEUR) {
              cached_eur_buy = obj["rateBuy"].as<float>();
              cached_eur_sell = obj["rateSell"].as<float>();
              if (cached_eur_buy == 0) cached_eur_buy = obj["rateCross"].as<float>();
              if (cached_eur_sell == 0) cached_eur_sell = obj["rateCross"].as<float>();
              foundEUR = true;
            }
          }
        }

        if (foundUSD) {
          if (cached_usd_buy < min_usd) min_usd = cached_usd_buy;
          if (cached_usd_buy > max_usd) max_usd = cached_usd_buy;
        }
        if (foundEUR) {
          if (cached_eur_buy < min_eur) min_eur = cached_eur_buy;
          if (cached_eur_buy > max_eur) max_eur = cached_eur_buy;
        }

        last_currency_fetch = millis();
        http.end();
        return true;
      }
    }
    http.end();
  }
  return false;
}

// ==========================================
// 1. Головна сторінка налаштувань (Зеленувата тема для грошей/валют)
// ==========================================
const char PAGE_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Currency Info — Hub</title>
  <style>
    :root {
      --bg-gradient: linear-gradient(135deg, #f0fdf4 0%, #dcfce7 50%, #f0fdf4 100%);
      --glass-bg: rgba(255, 255, 255, 0.75);
      --glass-border: rgba(255, 255, 255, 0.9);
      --accent-gradient: linear-gradient(135deg, #16a34a, #15803d);
      --accent-hover: linear-gradient(135deg, #15803d, #166534);
      --text-main: #14532d;
      --text-sub: #166534;
      --input-bg: rgba(255, 255, 255, 0.85);
      --input-border: rgba(187, 247, 208, 0.8);
      --input-focus: #16a34a;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif;
      background: var(--bg-gradient); background-attachment: fixed; color: var(--text-main);
      min-height: 100vh; display: flex; justify-content: center; align-items: center; padding: 24px;
    }
    .container {
      background: var(--glass-bg); backdrop-filter: blur(24px); -webkit-backdrop-filter: blur(24px);
      border: 1px solid var(--glass-border); border-radius: 36px; padding: 36px 40px;
      width: 100%; max-width: 880px; box-shadow: 0 25px 60px rgba(0, 0, 0, 0.08), inset 0 1px 0 rgba(255, 255, 255, 1);
    }
    .header { text-align: center; margin-bottom: 28px; }
    .icon-box {
      width: 72px; height: 72px; background: var(--accent-gradient); border-radius: 22px;
      display: inline-flex; align-items: center; justify-content: center; font-size: 36px; margin-bottom: 14px;
      box-shadow: 0 12px 24px rgba(22, 163, 74, 0.3);
    }
    h1 { font-size: 32px; font-weight: 800; letter-spacing: -0.5px; color: var(--text-main); }
    p.subtitle { font-size: 16px; color: var(--text-sub); margin-top: 6px; font-weight: 600; }
    
    .form-grid { display: grid; grid-template-columns: 1fr; gap: 20px; }
    @media (min-width: 768px) {
      .form-grid { grid-template-columns: 1fr 1fr; gap: 20px 28px; }
      .full-width { grid-column: span 2; }
    }

    .section-title {
      font-size: 14px; font-weight: 800; color: #166534; text-transform: uppercase;
      letter-spacing: 1.5px; margin: 16px 0 8px 0; display: flex; align-items: center; gap: 16px;
    }
    .section-title::after { content: ""; flex: 1; height: 2px; background: rgba(22, 163, 74, 0.15); }
    
    label { display: block; font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.8px; color: var(--text-sub); margin-bottom: 8px; }
    
    input {
      width: 100%; padding: 16px 20px; background: var(--input-bg); border: 2px solid var(--input-border);
      border-radius: 18px; color: var(--text-main); font-size: 16px; outline: none; transition: all 0.25s ease;
    }
    input:focus { border-color: var(--input-focus); background: #ffffff; box-shadow: 0 0 0 4px rgba(22, 163, 74, 0.18); }
    
    .btn-submit {
      width: 100%; padding: 18px; background: var(--accent-gradient); border: none; border-radius: 18px;
      color: white; font-size: 18px; font-weight: 800; cursor: pointer; margin-top: 12px;
      box-shadow: 0 12px 24px rgba(22, 163, 74, 0.3); transition: all 0.2s ease;
    }
    .btn-submit:hover { background: var(--accent-hover); transform: translateY(-2px); box-shadow: 0 16px 32px rgba(22, 163, 74, 0.4); }
    
    .ota-link {
      display: flex; align-items: center; justify-content: center; gap: 10px; width: 100%; margin-top: 16px;
      padding: 16px; border-radius: 18px; background: rgba(255, 255, 255, 0.7); border: 2px solid var(--glass-border);
      color: var(--text-sub); text-decoration: none; font-size: 15px; font-weight: 700; transition: all 0.2s ease;
    }
    .ota-link:hover { background: #ffffff; color: var(--text-main); border-color: rgba(22, 163, 74, 0.4); }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="icon-box">💱</div>
      <h1>Currency Info — Hub</h1>
      <p class="subtitle">Панель керування v3.0 (Monobank API)</p>
    </div>
    
    <form action="/save" method="POST">
      <div class="form-grid">
        <div class="full-width"><div class="section-title">Wi-Fi Мережа</div></div>
        <div class="form-group">
          <label>Назва Wi-Fi (SSID)</label>
          <input type="text" name="ssid" value="%SSID%" required autocomplete="off">
        </div>
        <div class="form-group">
          <label>Пароль Wi-Fi</label>
          <input type="password" name="pass" value="%PASS%">
        </div>

        <div class="full-width"><div class="section-title">Telegram Бот</div></div>
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
  <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Збережено</title>
  <style>
    body { background: linear-gradient(135deg, #f0fdf4 0%, #dcfce7 50%, #f0fdf4 100%); background-attachment: fixed; color: #14532d; font-family: system-ui, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; text-align: center; padding: 20px; }
    .card { background: rgba(255, 255, 255, 0.75); backdrop-filter: blur(24px); padding: 56px 40px; border-radius: 36px; border: 1px solid rgba(255, 255, 255, 0.9); max-width: 440px; width: 100%; box-shadow: 0 25px 60px rgba(0,0,0,0.08); }
    .check-icon { font-size: 64px; margin-bottom: 20px; display: block; }
    h2 { color: #166534; font-size: 26px; font-weight: 800; margin-bottom: 12px; }
    p { color: #16a34a; font-size: 16px; font-weight: 600; }
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
  <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>OTA Оновлення</title>
  <style>
    body { font-family: system-ui, sans-serif; background: linear-gradient(135deg, #f0fdf4 0%, #dcfce7 50%, #f0fdf4 100%); background-attachment: fixed; color: #14532d; min-height: 100vh; display: flex; justify-content: center; align-items: center; padding: 24px; }
    .card { background: rgba(255,255,255,0.75); backdrop-filter: blur(24px); padding: 48px 36px; border-radius: 36px; border: 1px solid rgba(255,255,255,0.9); width: 100%; max-width: 480px; text-align: center; box-shadow: 0 25px 60px rgba(0,0,0,0.08); }
    h2 { font-size: 26px; font-weight: 800; margin-bottom: 28px; }
    .file-dropzone { position: relative; border: 2px dashed rgba(22,163,74,0.4); border-radius: 22px; padding: 40px 20px; background: rgba(255,255,255,0.6); cursor: pointer; margin-bottom: 28px; display: block; }
    .file-dropzone input[type=file] { position: absolute; top:0; left:0; width:100%; height:100%; opacity:0; cursor:pointer; }
    .file-icon { font-size: 48px; margin-bottom: 12px; display: block; }
    .file-text { font-size: 15px; color: #16a34a; font-weight: 700; }
    .file-name { font-size: 14px; color: #166534; margin-top: 8px; font-weight: 800; word-break: break-all; }
    button { width: 100%; padding: 18px; background: linear-gradient(135deg, #16a34a, #15803d); border: none; border-radius: 18px; color: white; font-size: 17px; font-weight: 800; cursor: pointer; box-shadow: 0 12px 24px rgba(22,163,74,0.3); }
    .back-btn { display: inline-block; margin-top: 20px; color: #16a34a; text-decoration: none; font-size: 15px; font-weight: 700; }
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

  use_static_ip = prefs.getBool("use_ip", false);
  prefs.getString("ip", static_ip, 16);
  prefs.getString("gw", static_gateway, 16);
  prefs.getString("sn", static_subnet, 16);
  prefs.getString("dns", static_dns, 16);

  currency_fetch_interval = prefs.getULong("c_int", 3600000);
  bot_interval = prefs.getULong("b_int", 2000);
  alert_interval = prefs.getULong("a_int", 86400000);
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

void saveSettings() {
  prefs.begin("bot_config", false);
  prefs.putBool("use_ip", use_static_ip);
  prefs.putString("ip", static_ip);
  prefs.putString("gw", static_gateway);
  prefs.putString("sn", static_subnet);
  prefs.putString("dns", static_dns);

  prefs.putULong("c_int", currency_fetch_interval);
  prefs.putULong("b_int", bot_interval);
  prefs.putULong("a_int", alert_interval);
  prefs.putBool("a_en", alerts_enabled);

  prefs.putInt("gmt_off", gmt_offset_sec);
  prefs.putInt("day_off", daylight_offset_sec);
  prefs.putString("ntp", ntp_server);
  prefs.end();
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
  prefs.end();
  saveSettings();
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
  server.on("/update", HTTP_GET, []() { server.send(200, "text/html", PAGE_OTA); });
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!Update.end(true)) Update.printError(Serial);
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
    if (thread_id.length() > 0 && thread_id != "0") payload += "&message_thread_id=" + thread_id;

    client.println("POST /bot" + String(bot_token) + "/sendChatAction HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: "); client.println(payload.length());
    client.println(); client.print(payload);
    
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
    client.print("Content-Length: "); client.println(payload.length());
    client.println(); client.print(payload);
    
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
    if (thread_id.length() > 0 && thread_id != "0") payload += "&message_thread_id=" + thread_id;
    if (reply_markup.length() > 0) payload += "&reply_markup=" + reply_markup;

    client.println("POST /bot" + String(bot_token) + "/sendMessage HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: "); client.println(payload.length());
    client.println(); client.print(payload);

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
    if (reply_markup.length() > 0) payload += "&reply_markup=" + reply_markup;

    client.println("POST /bot" + String(bot_token) + "/editMessageText HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Connection: close");
    client.print("Content-Length: "); client.println(payload.length());
    client.println(); client.print(payload);

    unsigned long start = millis();
    while (client.connected() && millis() - start < 2000) {
      esp_task_wdt_reset();
      while (client.available()) {
        String line = client.readStringUntil('\n');
        if (line.indexOf("\"ok\":true") != -1) success = true;
      }
      delay(10);
    }
  }
  client.stop();
  esp_task_wdt_reset();
  return success;
}

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
// Конструктори Inline-клавіатур
// ==========================================
String getSubMenuKeyboard(String current_action) {
  return "{\"inline_keyboard\":[["
         "{\"text\":\"🔄 Оновити\",\"callback_data\":\"" + current_action + "\"},"
         "{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}"
         "]]}";
}

String getMainMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"💱 Курс валют\",\"callback_data\":\"status\"},{\"text\":\"📊 Статистика\",\"callback_data\":\"stats\"}],"
         "[{\"text\":\"⚙️ Налаштування\",\"callback_data\":\"settings\"},{\"text\":\"🚀 GitHub OTA\",\"callback_data\":\"github_ota\"}]"
         "]}";
}

String getSettingsKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🚨 Сповіщення\",\"callback_data\":\"alerts_menu\"},{\"text\":\"⏱️ Інтервали\",\"callback_data\":\"intervals_menu\"}],"
         "[{\"text\":\"🌐 IP / Мережа\",\"callback_data\":\"ip_menu\"},{\"text\":\"🕒 NTP Час\",\"callback_data\":\"ntp_menu\"}],"
         "[{\"text\":\"📈 Екстремуми\",\"callback_data\":\"minmax\"},{\"text\":\"🔄 Скидання Eкстр.\",\"callback_data\":\"reset_minmax\"}],"
         "[{\"text\":\"📶 Wi-Fi Status\",\"callback_data\":\"wifi\"},{\"text\":\"⏱️ Uptime\",\"callback_data\":\"uptime\"}],"
         "[{\"text\":\"🏓 Ping\",\"callback_data\":\"ping\"},{\"text\":\"🔙 Головне меню\",\"callback_data\":\"menu\"}]"
         "]}";
}

String getAlertsMenuKeyboard() {
  String alertStateText = alerts_enabled ? "🔔 Сповіщення: УВІМК" : "🔕 Сповіщення: ВИМК";
  return "{\"inline_keyboard\":["
         "[{\"text\":\"" + alertStateText + "\",\"callback_data\":\"toggle_alerts\"}],"
         "[{\"text\":\"⏱️ Повтор (" + String(alert_interval / 3600000) + "год)\",\"callback_data\":\"set_a_int\"},"
         "{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]"
         "]}";
}

String getIntervalsMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"💱 Курс (" + String(currency_fetch_interval / 3600000) + "год)\",\"callback_data\":\"set_c_int\"},"
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
// GitHub OTA Перевірка
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
      tag_name.replace("v", ""); tag_name.trim();

      if (tag_name != String(CURRENT_VERSION)) {
        sendTelegramMessage(chat_id, "🚀 <b>Знайдено нову версію: v" + tag_name + "!</b> Завантаження...", thread_id);
        JsonArray assets = doc["assets"].as<JsonArray>();
        String downloadUrl = "";
        for (JsonObject asset : assets) {
          if (asset["name"].as<String>().endsWith(".bin")) {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
          }
        }
        if (downloadUrl.length() > 0) {
          t_httpUpdate_return ret = httpUpdate.update(client, downloadUrl);
          if (ret == HTTP_UPDATE_OK) {
            sendTelegramMessage(chat_id, "✅ <b>Оновлення встановлено!</b> Перезавантаження...", thread_id);
            delay(1000); ESP.restart();
          } else {
            sendTelegramMessage(chat_id, "❌ <b>Помилка OTA:</b> " + httpUpdate.getLastErrorString(), thread_id, getSubMenuKeyboard("github_ota"));
          }
        }
      } else {
        sendOrEditMessage(chat_id, message_id, "✅ <b>У вас найновіша версія: v" + String(CURRENT_VERSION) + "</b>", thread_id, getMainMenuKeyboard());
      }
    }
    http.end();
  }
}

// ==========================================
// Меню та вивід повідомлень
// ==========================================
void sendMenuMessage(String chat_id, String thread_id = "", long message_id = 0) {
  sendOrEditMessage(chat_id, message_id, "📋 <b>Головне меню — Currency Hub</b>\n\nОберіть потрібний розділ:", thread_id, getMainMenuKeyboard());
}

void sendSettingsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  sendOrEditMessage(chat_id, message_id, "⚙️ <b>Панель налаштувань та діагностики (Currency Hub)</b>", thread_id, getSettingsKeyboard());
}

void sendAlertsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🚨 <b>Налаштування сповіщень</b>\n\n";
  msg += "Стан: " + String(alerts_enabled ? "🟢 Увімкнені" : "🔴 Вимкнені") + "\n";
  msg += "⏱️ Інтервал повтору: <code>" + String(alert_interval / 3600000) + " год</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getAlertsMenuKeyboard());
}

void sendIntervalsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "⏱️ <b>Системні інтервали</b>\n\n";
  msg += "💱 Запит курсу: <code>" + String(currency_fetch_interval / 3600000) + " год</code>\n";
  msg += "🤖 Перевірка Telegram: <code>" + String(bot_interval) + " мс</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getIntervalsMenuKeyboard());
}

void sendIPMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🌐 <b>Мережеві налаштування IP</b>\n\n";
  msg += "Режим: <code>" + String(use_static_ip ? "Static IP" : "DHCP") + "</code>\n";
  msg += "📍 IP: <code>" + WiFi.localIP().toString() + "</code>\n";
  msg += "🌐 Gateway: <code>" + WiFi.gatewayIP().toString() + "</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getIPMenuKeyboard());
}

void sendNTPMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🕒 <b>Синхронізація часу (NTP)</b>\n\n";
  msg += "🖥️ Сервер: <code>" + String(ntp_server) + "</code>\n";
  msg += "🕒 Час: <code>" + getFormattedTime() + "</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getNTPMenuKeyboard());
}

void sendStatusMessage(String chat_id, String thread_id = "", long message_id = 0) {
  fetchCurrencyData();
  String msg = "💱 <b>Курс валют (Monobank) — Currency Hub</b>\n\n";
  if (isnan(cached_usd_buy)) {
    msg += "⚠️ <i>Помилка отримання даних курсу валют</i>";
  } else {
    msg += "🇺🇸 <b>USD / UAH:</b>\n";
    msg += " ├ Покупка: <code>" + String(cached_usd_buy, 2) + " грн</code>\n";
    msg += " └ Продаж: <code>" + String(cached_usd_sell, 2) + " грн</code>\n\n";
    msg += "🇪🇺 <b>EUR / UAH:</b>\n";
    msg += " ├ Покупка: <code>" + String(cached_eur_buy, 2) + " грн</code>\n";
    msg += " └ Продаж: <code>" + String(cached_eur_sell, 2) + " грн</code>\n\n";
    msg += "🕒 <b>Час оновлення:</b> <code>" + getFormattedTime() + "</code>";
  }
  String kb = "{\"inline_keyboard\":[[{\"text\":\"🔄 Оновити\",\"callback_data\":\"status\"},{\"text\":\"🔙 Меню\",\"callback_data\":\"menu\"}]]}";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, kb);
}

void sendStatsMessage(String chat_id, String thread_id = "", long message_id = 0) {
  fetchCurrencyData();
  String msg = "📊 <b>Статистика курсів — Currency Hub</b>\n\n";
  if (!isnan(cached_usd_buy)) {
    msg += "🇺🇸 USD Покупка: <code>" + String(cached_usd_buy, 2) + " грн</code>\n";
    msg += "🇪🇺 EUR Покупка: <code>" + String(cached_eur_buy, 2) + " грн</code>\n\n";
    msg += "📈 <b>Екстремуми USD (покупка):</b>\n";
    msg += " ├ Мін: <code>" + String(min_usd, 2) + " грн</code>\n";
    msg += " └ Макс: <code>" + String(max_usd, 2) + " грн</code>\n\n";
  }
  msg += "📶 Wi-Fi: <code>" + String(getQuality()) + "%</code>\n";
  msg += "🧠 RAM: <code>" + String(ESP.getFreeHeap() / 1024) + " KB</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("stats"));
}

void sendMinMaxMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📈 <b>Екстремуми курсів — Currency Hub</b>\n\n";
  msg += "🇺🇸 USD Покупка:\n";
  msg += " ├ Мін: <code>" + String(min_usd, 2) + " грн</code>\n";
  msg += " └ Макс: <code>" + String(max_usd, 2) + " грн</code>\n\n";
  msg += "🇪🇺 EUR Покупка:\n";
  msg += " ├ Мін: <code>" + String(min_eur, 2) + " грн</code>\n";
  msg += " └ Макс: <code>" + String(max_eur, 2) + " грн</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("minmax"));
}

void sendResetMinMaxMessage(String chat_id, String thread_id = "", long message_id = 0) {
  min_usd = 999.0; max_usd = -999.0;
  min_eur = 999.0; max_eur = -999.0;
  sendOrEditMessage(chat_id, message_id, "🔄 <b>Екстремуми курсів скинуто!</b>", thread_id, getSubMenuKeyboard("minmax"));
}

void sendUptimeMessage(String chat_id, String thread_id = "", long message_id = 0) {
  unsigned long sec = millis() / 1000;
  unsigned long hours = sec / 3600; sec %= 3600; unsigned long mins = sec / 60; sec %= 60;
  String msg = "⚙️ <b>Система — Currency Hub</b>\n\n⏱️ Uptime: <code>" + String(hours) + "h " + String(mins) + "m " + String(sec) + "s</code>\n📶 Wi-Fi: <code>" + String(getQuality()) + "%</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("uptime"));
}

void sendWifiMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📶 <b>Wi-Fi Мережа</b>\n\n📡 SSID: <code>" + String(wifi_ssid) + "</code>\n🌐 IP: <code>" + WiFi.localIP().toString() + "</code>\n📊 Сигнал: <code>" + String(getQuality()) + "%</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, getSubMenuKeyboard("wifi"));
}

void sendPingMessage(String chat_id, String thread_id = "", long message_id = 0) {
  sendOrEditMessage(chat_id, message_id, "🏓 <b>Pong!</b> Currency Hub онлайн.\n🕒 <code>" + getFormattedTime() + "</code>", thread_id, getSubMenuKeyboard("ping"));
}

// ==========================================
// Обробка дій Inline-кнопок
// ==========================================
void processCallback(String data, String chat_id, String target_thread, long message_id) {
  if (data == "menu") sendMenuMessage(chat_id, target_thread, message_id);
  else if (data == "settings") sendSettingsMenu(chat_id, target_thread, message_id);
  else if (data == "alerts_menu") sendAlertsMenu(chat_id, target_thread, message_id);
  else if (data == "intervals_menu") sendIntervalsMenu(chat_id, target_thread, message_id);
  else if (data == "ip_menu") sendIPMenu(chat_id, target_thread, message_id);
  else if (data == "ntp_menu") getNTPMenuKeyboard(); // Исправлено для целостности вызова
  else if (data == "ntp_menu") sendNTPMenu(chat_id, target_thread, message_id);
  else if (data == "status") sendStatusMessage(chat_id, target_thread, message_id);
  else if (data == "stats") sendStatsMessage(chat_id, target_thread, message_id);
  else if (data == "minmax") sendMinMaxMessage(chat_id, target_thread, message_id);
  else if (data == "reset_minmax") sendResetMinMaxMessage(chat_id, target_thread, message_id);
  else if (data == "uptime") sendUptimeMessage(chat_id, target_thread, message_id);
  else if (data == "wifi") sendWifiMessage(chat_id, target_thread, message_id);
  else if (data == "ping") sendPingMessage(chat_id, target_thread, message_id);
  else if (data == "github_ota") checkGitHubUpdate(chat_id, target_thread, message_id);
  
  else if (data == "toggle_alerts") {
    alerts_enabled = !alerts_enabled; saveSettings(); sendAlertsMenu(chat_id, target_thread, message_id);
  }
  else if (data == "toggle_ip_mode") {
    use_static_ip = !use_static_ip; saveSettings();
    sendOrEditMessage(chat_id, message_id, "⚠️ Режим IP змінено. Перезавантажте пристрій.", target_thread, getIPMenuKeyboard());
  }
  
  else if (data == "set_c_int") { waiting_input_chat_id = chat_id; waiting_input_param = "c_int"; sendOrEditMessage(chat_id, message_id, "✏️ Інтервал запиту курсу (години):", target_thread, ""); }
  else if (data == "set_b_int") { waiting_input_chat_id = chat_id; waiting_input_param = "b_int"; sendOrEditMessage(chat_id, message_id, "✏️ Інтервал Telegram (мс):", target_thread, ""); }
  else if (data == "set_static_ip") { waiting_input_chat_id = chat_id; waiting_input_param = "static_ip"; sendOrEditMessage(chat_id, message_id, "✏️ Static IP:", target_thread, ""); }
  else if (data == "set_static_gw") { waiting_input_chat_id = chat_id; waiting_input_param = "static_gw"; sendOrEditMessage(chat_id, message_id, "✏️ Gateway IP:", target_thread, ""); }
  else if (data == "set_ntp_server") { waiting_input_chat_id = chat_id; waiting_input_param = "ntp_server"; sendOrEditMessage(chat_id, message_id, "✏️ NTP сервер:", target_thread, ""); }
  else if (data == "set_gmt_offset") { waiting_input_chat_id = chat_id; waiting_input_param = "gmt_offset"; sendOrEditMessage(chat_id, message_id, "✏️ Зміщення GMT (години):", target_thread, ""); }
}

// ==========================================
// Обробка текстового введення в чаті
// ==========================================
void processTextInput(String text, String chat_id, String target_thread) {
  if (waiting_input_chat_id == chat_id && waiting_input_param.length() > 0) {
    String param = waiting_input_param;
    waiting_input_chat_id = ""; waiting_input_param = "";
    text.trim();

    if (param == "c_int") { currency_fetch_interval = text.toInt() * 3600000; saveSettings(); sendIntervalsMenu(chat_id, target_thread); }
    else if (param == "b_int") { bot_interval = text.toInt(); saveSettings(); sendIntervalsMenu(chat_id, target_thread); }
    else if (param == "static_ip") { text.toCharArray(static_ip, 16); saveSettings(); sendIPMenu(chat_id, target_thread); }
    else if (param == "static_gw") { text.toCharArray(static_gateway, 16); saveSettings(); sendIPMenu(chat_id, target_thread); }
    else if (param == "ntp_server") { text.toCharArray(ntp_server, 64); configTime(gmt_offset_sec, daylight_offset_sec, ntp_server); saveSettings(); sendNTPMenu(chat_id, target_thread); }
    else if (param == "gmt_offset") { gmt_offset_sec = text.toInt() * 3600; configTime(gmt_offset_sec, daylight_offset_sec, ntp_server); saveSettings(); sendNTPMenu(chat_id, target_thread); }
    sendTelegramMessage(chat_id, "✅ Успішно збережено!", target_thread);
  }
}

// ==========================================
// Перевірка Telegram оновлень
// ==========================================
void checkTelegramUpdates() {
  if (strlen(bot_token) == 0 || ESP.getFreeHeap() < 35000) return;
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(3000);

  if (client.connect("api.telegram.org", 443)) {
    esp_task_wdt_reset();
    String url = "/bot" + String(bot_token) + "/getUpdates?offset=" + String(last_update_id + 1) + "&timeout=0&limit=5";
    client.println("GET " + url + " HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Connection: close");
    client.println();

    unsigned long start_time = millis(); bool isBody = false; String response = "";
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
      if (!deserializeJson(doc, response) && doc["ok"].as<bool>()) {
        JsonArray result = doc["result"].as<JsonArray>();
        for (JsonObject update : result) {
          last_update_id = update["update_id"].as<long>();

          if (update.containsKey("callback_query")) {
            JsonObject cb = update["callback_query"];
            String cb_id = cb["id"].as<String>(); String data = cb["data"].as<String>();
            JsonObject cb_msg = cb["message"];
            String chat_id = String((long long)cb_msg["chat"]["id"]);
            long message_id = cb_msg["message_id"].as<long>();
            String thread_id = cb_msg.containsKey("message_thread_id") ? String((long)cb_msg["message_thread_id"]) : "";

            if (chat_id.equals(user_chat_id) || chat_id.equals(group_chat_id)) {
              String target_thread = (chat_id.equals(group_chat_id) && thread_id.length() == 0 && strlen(group_thread_id) > 0) ? String(group_thread_id) : thread_id;
              answerCallbackQuery(cb_id);
              processCallback(data, chat_id, target_thread, message_id);
            } else {
              answerCallbackQuery(cb_id);
            }
            continue;
          }

          if (!update.containsKey("message") || !update["message"].containsKey("text")) continue;
          String text = update["message"]["text"].as<String>(); text.trim();
          String chat_id = String((long long)update["message"]["chat"]["id"]);
          String thread_id = update["message"].containsKey("message_thread_id") ? String((long)update["message"]["message_thread_id"]) : "";

          if (chat_id.equals(user_chat_id) || chat_id.equals(group_chat_id)) {
            String target_thread = (chat_id.equals(group_chat_id) && thread_id.length() == 0 && strlen(group_thread_id) > 0) ? String(group_thread_id) : thread_id;
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
  }
  esp_task_wdt_reset();
}

// ==========================================
// Періодичні алерти курсів валют
// ==========================================
void checkCurrencyAlerts() {
  if (!alerts_enabled || ap_mode || WiFi.status() != WL_CONNECTED || strlen(bot_token) == 0) return;
  if (millis() - last_currency_fetch >= currency_fetch_interval) {
    if (fetchCurrencyData()) {
      if (millis() - last_alert_time >= alert_interval || last_alert_time == 0) {
        last_alert_time = millis();
        String target_chat = (strlen(group_chat_id) > 0) ? String(group_chat_id) : String(user_chat_id);
        String target_thread = (strlen(group_chat_id) > 0) ? String(group_thread_id) : "";
        String msg = "💱 <b>Currency Hub: Щоденний курс валют</b>\n\n";
        msg += "🇺🇸 USD: <code>" + String(cached_usd_buy, 2) + " / " + String(cached_usd_sell, 2) + " грн</code>\n";
        msg += "🇪🇺 EUR: <code>" + String(cached_eur_buy, 2) + " / " + String(cached_eur_sell, 2) + " грн</code>";
        sendTelegramMessage(target_chat, msg, target_thread);
      }
    }
  }
}

void handleWiFiReconnect() {
  if (!ap_mode && WiFi.status() != WL_CONNECTED && millis() - last_wifi_reconnect_attempt > WIFI_RECONNECT_INTERVAL) {
    last_wifi_reconnect_attempt = millis();
    WiFi.disconnect(); WiFi.reconnect();
  }
}

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
    if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
    configTime(gmt_offset_sec, daylight_offset_sec, ntp_server);
    fetchCurrencyData();

    #if defined(CONFIG_ESP_TASK_WDT_EN)
      esp_task_wdt_config_t twdt_config = { .timeout_ms = WDT_TIMEOUT * 1000, .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
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
          sendTelegramMessage(target_chat, "🟢 <b>Currency Hub online (v3.0 Monobank)</b>\nСистема готова до роботи.", target_thread, getMainMenuKeyboard());
        }
      }
    }

    if (millis() - last_bot_check > bot_interval) {
      checkTelegramUpdates();
      checkCurrencyAlerts();
      last_bot_check = millis();
    }
  }
}