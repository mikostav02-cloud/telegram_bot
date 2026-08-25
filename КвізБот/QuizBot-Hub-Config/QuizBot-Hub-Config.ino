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
#define GITHUB_REPO_NAME "quiz_hub"
#define CURRENT_VERSION "3.0"

const byte DNS_PORT = 53;
const char* AP_SSID = "Quiz-Hub-AP";
const char* MDNS_NAME = "quizhub";

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

// Мережеві налаштування
bool use_static_ip = false;
char static_ip[16] = "192.168.1.204";
char static_gateway[16] = "192.168.1.1";
char static_subnet[16] = "255.255.255.0";
char static_dns[16] = "1.1.1.1";

unsigned long bot_interval = 2000;             // Інтервал перевірки Telegram (мс)
int gmt_offset_sec = 7200;                     // Часовий пояс UTC+2 (у секундах)
int daylight_offset_sec = 3600;                // Літній час (+1 година)
char ntp_server[64] = "pool.ntp.org";

// Стан інтерактивного редагування в Telegram
String waiting_input_chat_id = "";
String waiting_input_param = ""; 

// ==========================================
// Системні змінні стану вікторини
// ==========================================
unsigned long last_bot_check = 0;
long last_update_id = 0;

bool ap_mode = false;
bool boot_msg_sent = false;

// Змінні для обробки кнопки BOOT
unsigned long btn_press_start = 0;
bool btn_state_last = HIGH;

// Змінні для авто-відновлення Wi-Fi
unsigned long last_wifi_reconnect_attempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 30000; // 30 секунд

// Статистика квізу
int total_quizzes_played = 0;
int total_correct_answers = 0;
int total_wrong_answers = 0;

// Структура питання вікторини
struct QuizQuestion {
  String question;
  String options[4];
  int correctIndex;
  String explanation;
};

// База запитань
const int TOTAL_QUESTIONS = 5;
QuizQuestion quizDatabase[TOTAL_QUESTIONS] = {
  {
    "Яка планета є найбільшою у Сонячній системі?",
    {"Марс", "Юпітер", "Сатурн", "Венера"},
    1,
    "Юпітер — найбільший газовий гігант нашої системи."
  },
  {
    "Скільки бітів в одному байті?",
    {"4", "8", "16", "32"},
    1,
    "Один байт складається з 8 бітів."
  },
  {
    "Який елемент має хімічний символ 'Au'?",
    {"Срібло", "Золото", "Арґентум", "Алюміній"},
    1,
    "Au походить від латинського слова Aurum (золото)."
  },
  {
    "У якому році відбувся перший політ людини в космос?",
    {"1957", "1961", "1969", "1975"},
    1,
    "Юрій Гагарін здійснив політ у космос 12 квітня 1961 року."
  },
  {
    "Який стандартний порт використовується для HTTPS за замовчуванням?",
    {"80", "443", "8080", "21"},
    1,
    "Порт 443 використовується для захищених HTTPS з'єднань."
  }
};

// Стан активних сесій квізу для користувачів [ChatID -> QuestionIndex]
struct ActiveQuizSession {
  String chatId;
  int currentQuestionIndex;
  int score;
  bool active;
};
ActiveQuizSession activeSessions[5];

ActiveQuizSession* getSession(String chatId) {
  for (int i = 0; i < 5; i++) {
    if (activeSessions[i].chatId == chatId && activeSessions[i].active) {
      return &activeSessions[i];
    }
  }
  for (int i = 0; i < 5; i++) {
    if (!activeSessions[i].active) {
      activeSessions[i].chatId = chatId;
      activeSessions[i].currentQuestionIndex = 0;
      activeSessions[i].score = 0;
      activeSessions[i].active = true;
      return &activeSessions[i];
    }
  }
  return &activeSessions[0]; // fallback
}

void endSession(String chatId) {
  for (int i = 0; i < 5; i++) {
    if (activeSessions[i].chatId == chatId) {
      activeSessions[i].active = false;
      activeSessions[i].chatId = "";
    }
  }
}

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

// ==========================================
// 1. Головна сторінка налаштувань (Сучасний світлий/темний стиль з glassmorphism)
// ==========================================
const char PAGE_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="uk">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Quiz Bot Hub</title>
  <style>
    :root {
      --bg-gradient: linear-gradient(135deg, #f8fafc 0%, #e2e8f0 50%, #f1f5f9 100%);
      --glass-bg: rgba(255, 255, 255, 0.8);
      --glass-border: rgba(255, 255, 255, 0.95);
      --accent-gradient: linear-gradient(135deg, #6366f1, #4f46e5);
      --accent-hover: linear-gradient(135deg, #4f46e5, #4338ca);
      --text-main: #1e293b;
      --text-sub: #475569;
      --input-bg: rgba(255, 255, 255, 0.9);
      --input-border: rgba(203, 213, 225, 0.8);
      --input-focus: #6366f1;
    }
    @media (prefers-color-scheme: dark) {
      :root {
        --bg-gradient: linear-gradient(135deg, #0f172a 0%, #1e293b 50%, #0f172a 100%);
        --glass-bg: rgba(30, 41, 59, 0.75);
        --glass-border: rgba(51, 65, 85, 0.8);
        --accent-gradient: linear-gradient(135deg, #818cf8, #6366f1);
        --accent-hover: linear-gradient(135deg, #6366f1, #4f46e5);
        --text-main: #f8fafc;
        --text-sub: #cbd5e1;
        --input-bg: rgba(15, 23, 42, 0.8);
        --input-border: rgba(51, 65, 85, 0.9);
        --input-focus: #818cf8;
      }
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
      width: 100%; max-width: 880px; box-shadow: 0 25px 60px rgba(0, 0, 0, 0.12), inset 0 1px 0 rgba(255, 255, 255, 0.2);
    }
    .header { text-align: center; margin-bottom: 28px; }
    .icon-box {
      width: 72px; height: 72px; background: var(--accent-gradient); border-radius: 22px;
      display: inline-flex; align-items: center; justify-content: center; font-size: 36px; margin-bottom: 14px;
      box-shadow: 0 12px 24px rgba(99, 102, 241, 0.3);
    }
    h1 { font-size: 32px; font-weight: 800; letter-spacing: -0.5px; color: var(--text-main); }
    p.subtitle { font-size: 16px; color: var(--text-sub); margin-top: 6px; font-weight: 600; }
    
    .form-grid { display: grid; grid-template-columns: 1fr; gap: 20px; }
    @media (min-width: 768px) {
      .form-grid { grid-template-columns: 1fr 1fr; gap: 20px 28px; }
      .full-width { grid-column: span 2; }
    }

    .section-title {
      font-size: 14px; font-weight: 800; color: var(--text-sub); text-transform: uppercase;
      letter-spacing: 1.5px; margin: 16px 0 8px 0; display: flex; align-items: center; gap: 16px;
    }
    .section-title::after { content: ""; flex: 1; height: 2px; background: rgba(99, 102, 241, 0.2); }
    
    label { display: block; font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.8px; color: var(--text-sub); margin-bottom: 8px; }
    
    input {
      width: 100%; padding: 16px 20px; background: var(--input-bg); border: 2px solid var(--input-border);
      border-radius: 18px; color: var(--text-main); font-size: 16px; outline: none; transition: all 0.25s ease;
    }
    input:focus { border-color: var(--input-focus); box-shadow: 0 0 0 4px rgba(99, 102, 241, 0.18); }
    
    .btn-submit {
      width: 100%; padding: 18px; background: var(--accent-gradient); border: none; border-radius: 18px;
      color: white; font-size: 18px; font-weight: 800; cursor: pointer; margin-top: 12px;
      box-shadow: 0 12px 24px rgba(99, 102, 241, 0.3); transition: all 0.2s ease;
    }
    .btn-submit:hover { background: var(--accent-hover); transform: translateY(-2px); box-shadow: 0 16px 32px rgba(99, 102, 241, 0.4); }
    
    .ota-link {
      display: flex; align-items: center; justify-content: center; gap: 10px; width: 100%; margin-top: 16px;
      padding: 16px; border-radius: 18px; background: var(--input-bg); border: 2px solid var(--input-border);
      color: var(--text-sub); text-decoration: none; font-size: 15px; font-weight: 700; transition: all 0.2s ease;
    }
    .ota-link:hover { color: var(--text-main); border-color: var(--input-focus); }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="icon-box">🧠</div>
      <h1>Quiz Bot Hub</h1>
      <p class="subtitle">Панель керування v3.0 (Telegram Quiz Bot)</p>
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
    body { background: #0f172a; color: #f8fafc; font-family: system-ui, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; text-align: center; padding: 20px; }
    .card { background: rgba(30, 41, 59, 0.8); backdrop-filter: blur(24px); padding: 56px 40px; border-radius: 36px; border: 1px solid rgba(51, 65, 85, 0.8); max-width: 440px; width: 100%; box-shadow: 0 25px 60px rgba(0,0,0,0.3); }
    .check-icon { font-size: 64px; margin-bottom: 20px; display: block; }
    h2 { color: #818cf8; font-size: 26px; font-weight: 800; margin-bottom: 12px; }
    p { color: #cbd5e1; font-size: 16px; font-weight: 600; }
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
    body { font-family: system-ui, sans-serif; background: #0f172a; color: #f8fafc; min-height: 100vh; display: flex; justify-content: center; align-items: center; padding: 24px; }
    .card { background: rgba(30, 41, 59, 0.8); backdrop-filter: blur(24px); padding: 48px 36px; border-radius: 36px; border: 1px solid rgba(51, 65, 85, 0.8); width: 100%; max-width: 480px; text-align: center; box-shadow: 0 25px 60px rgba(0,0,0,0.3); }
    h2 { font-size: 26px; font-weight: 800; margin-bottom: 28px; }
    .file-dropzone { position: relative; border: 2px dashed rgba(99, 102, 241, 0.4); border-radius: 22px; padding: 40px 20px; background: rgba(15, 23, 42, 0.6); cursor: pointer; margin-bottom: 28px; display: block; }
    .file-dropzone input[type=file] { position: absolute; top:0; left:0; width:100%; height:100%; opacity:0; cursor:pointer; }
    .file-icon { font-size: 48px; margin-bottom: 12px; display: block; }
    .file-text { font-size: 15px; color: #818cf8; font-weight: 700; }
    .file-name { font-size: 14px; color: #cbd5e1; margin-top: 8px; font-weight: 800; word-break: break-all; }
    button { width: 100%; padding: 18px; background: linear-gradient(135deg, #6366f1, #4f46e5); border: none; border-radius: 18px; color: white; font-size: 17px; font-weight: 800; cursor: pointer; box-shadow: 0 12px 24px rgba(99, 102, 241, 0.3); }
    .back-btn { display: inline-block; margin-top: 20px; color: #818cf8; text-decoration: none; font-size: 15px; font-weight: 700; }
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

  bot_interval = prefs.getULong("b_int", 2000);
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
  prefs.putULong("b_int", bot_interval);
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
// Конструктори Inline-клавіатур Квізу
// ==========================================
String getMainMenuKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🧠 Почати вікторину\",\"callback_data\":\"start_quiz\"}],"
         "[{\"text\":\"📊 Статистика\",\"callback_data\":\"stats\"},{\"text\":\"⚙️ Налаштування\",\"callback_data\":\"settings\"}],"
         "[{\"text\":\"🚀 GitHub OTA\",\"callback_data\":\"github_ota\"}]"
         "]}";
}

String getSettingsKeyboard() {
  return "{\"inline_keyboard\":["
         "[{\"text\":\"🤖 Bot Poll (" + String(bot_interval) + "мс)\",\"callback_data\":\"set_b_int\"}],"
         "[{\"text\":\"🕒 NTP Час\",\"callback_data\":\"ntp_menu\"},{\"text\":\"📶 Wi-Fi Status\",\"callback_data\":\"wifi\"}],"
         "[{\"text\":\"🔙 Головне меню\",\"callback_data\":\"menu\"}]"
         "]}";
}

String getQuizQuestionKeyboard(int qIndex) {
  QuizQuestion q = quizDatabase[qIndex];
  String kb = "{\"inline_keyboard\":[";
  for (int i = 0; i < 4; i++) {
    kb += "[{\"text\":\"" + q.options[i] + "\",\"callback_data\":\"ans_" + String(qIndex) + "_" + String(i) + "\"}],";
  }
  kb += "[{\"text\":\"❌ Завершити квіз\",\"callback_data\":\"end_quiz\"}]";
  kb += "]}";
  return kb;
}

// ==========================================
// Логіка вікторини
// ==========================================
void sendQuizQuestion(String chat_id, String thread_id, long message_id) {
  ActiveQuizSession* session = getSession(chat_id);
  if (session->currentQuestionIndex >= TOTAL_QUESTIONS) {
    // Квіз завершено
    total_quizzes_played++;
    String msg = "🎉 <b>Вікторину завершено!</b>\n\n";
    msg += "Ваш результат: <b>" + String(session->score) + " з " + String(TOTAL_QUESTIONS) + "</b> правильних відповідей.";
    endSession(chat_id);
    sendOrEditMessage(chat_id, message_id, msg, thread_id, getMainMenuKeyboard());
    return;
  }

  int qIdx = session->currentQuestionIndex;
  QuizQuestion q = quizDatabase[qIdx];
  String msg = "🧠 <b>Запитання " + String(qIdx + 1) + " з " + String(TOTAL_QUESTIONS) + "</b>\n\n";
  msg += "❓ <b>" + q.question + "</b>";

  sendOrEditMessage(chat_id, message_id, msg, thread_id, getQuizQuestionKeyboard(qIdx));
}

void processAnswer(String chat_id, String thread_id, long message_id, int qIdx, int chosenOpt) {
  ActiveQuizSession* session = getSession(chat_id);
  QuizQuestion q = quizDatabase[qIdx];

  String msg = "";
  if (chosenOpt == q.correctIndex) {
    session->score++;
    total_correct_answers++;
    msg = "✅ <b>Правильно!</b>\n\n";
  } else {
    total_wrong_answers++;
    msg = "❌ <b>Неправильно.</b>\nПравильна відповідь: <i>" + q.options[q.correctIndex] + "</i>\n\n";
  }
  msg += "💡 <i>" + q.explanation + "</i>";

  // Відправляємо пояснення з кнопкою "Наступне"
  String nextKb = "{\"inline_keyboard\":[[{\"text\":\"➡️ Наступне запитання\",\"callback_data\":\"next_q\"}]]}";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, nextKb);
  session->currentQuestionIndex++;
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
            sendTelegramMessage(chat_id, "❌ <b>Помилка OTA:</b> " + httpUpdate.getLastErrorString(), thread_id, getMainMenuKeyboard());
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
  sendOrEditMessage(chat_id, message_id, "📋 <b>Головне меню — Quiz Hub</b>\n\nОберіть потрібний розділ:", thread_id, getMainMenuKeyboard());
}

void sendSettingsMenu(String chat_id, String thread_id = "", long message_id = 0) {
  sendOrEditMessage(chat_id, message_id, "⚙️ <b>Панель налаштувань (Quiz Hub)</b>", thread_id, getSettingsKeyboard());
}

void sendStatsMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📊 <b>Статистика квізу — Quiz Hub</b>\n\n";
  msg += "🎮 Зіграно вікторин: <code>" + String(total_quizzes_played) + "</code>\n";
  msg += "✅ Правильних відповідей: <code>" + String(total_correct_answers) + "</code>\n";
  msg += "❌ Неправильних відповідей: <code>" + String(total_wrong_answers) + "</code>\n\n";
  msg += "📶 Wi-Fi: <code>" + String(getQuality()) + "%</code>\n";
  msg += "🧠 RAM: <code>" + String(ESP.getFreeHeap() / 1024) + " KB</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, "{\"inline_keyboard\":[[{\"text\":\"🔙 Меню\",\"callback_data\":\"menu\"}]]}");
}

void sendWifiMessage(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "📶 <b>Wi-Fi Мережа</b>\n\n📡 SSID: <code>" + String(wifi_ssid) + "</code>\n🌐 IP: <code>" + WiFi.localIP().toString() + "</code>\n📊 Сигнал: <code>" + String(getQuality()) + "%</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, "{\"inline_keyboard\":[[{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]]}");
}

void sendNTPMenu(String chat_id, String thread_id = "", long message_id = 0) {
  String msg = "🕒 <b>Синхронізація часу (NTP)</b>\n\n";
  msg += "🖥️ Сервер: <code>" + String(ntp_server) + "</code>\n";
  msg += "🕒 Час: <code>" + getFormattedTime() + "</code>";
  sendOrEditMessage(chat_id, message_id, msg, thread_id, "{\"inline_keyboard\":[[{\"text\":\"🔙 Налаштування\",\"callback_data\":\"settings\"}]]}");
}

// ==========================================
// Обробка дій Inline-кнопок
// ==========================================
void processCallback(String data, String chat_id, String target_thread, long message_id) {
  if (data == "menu") {
    endSession(chat_id);
    sendMenuMessage(chat_id, target_thread, message_id);
  }
  else if (data == "settings") sendSettingsMenu(chat_id, target_thread, message_id);
  else if (data == "stats") sendStatsMessage(chat_id, target_thread, message_id);
  else if (data == "wifi") sendWifiMessage(chat_id, target_thread, message_id);
  else if (data == "ntp_menu") sendNTPMenu(chat_id, target_thread, message_id);
  else if (data == "github_ota") checkGitHubUpdate(chat_id, target_thread, message_id);
  
  else if (data == "start_quiz") {
    ActiveQuizSession* s = getSession(chat_id);
    s->currentQuestionIndex = 0;
    s->score = 0;
    sendQuizQuestion(chat_id, target_thread, message_id);
  }
  else if (data == "next_q") {
    sendQuizQuestion(chat_id, target_thread, message_id);
  }
  else if (data == "end_quiz") {
    endSession(chat_id);
    sendOrEditMessage(chat_id, message_id, "❌ Вікторину скасовано.", target_thread, getMainMenuKeyboard());
  }
  else if (data.startsWith("ans_")) {
    int firstUnderscore = data.indexOf('_');
    int secondUnderscore = data.lastIndexOf('_');
    int qIdx = data.substring(firstUnderscore + 1, secondUnderscore).toInt();
    int chosenOpt = data.substring(secondUnderscore + 1).toInt();
    processAnswer(chat_id, target_thread, message_id, qIdx, chosenOpt);
  }
  else if (data == "set_b_int") {
    waiting_input_chat_id = chat_id;
    waiting_input_param = "b_int";
    sendOrEditMessage(chat_id, message_id, "✏️ Введіть новий інтервал перевірки Telegram (мс):", target_thread, "");
  }
}

// ==========================================
// Обробка текстового введення в чаті
// ==========================================
void processTextInput(String text, String chat_id, String target_thread) {
  if (waiting_input_chat_id == chat_id && waiting_input_param.length() > 0) {
    String param = waiting_input_param;
    waiting_input_chat_id = ""; waiting_input_param = "";
    text.trim();

    if (param == "b_int") {
      bot_interval = text.toInt();
      saveSettings();
      sendSettingsMenu(chat_id, target_thread);
    }
    sendTelegramMessage(chat_id, "✅ Налаштування успішно збережено!", target_thread);
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

  for(int i=0; i<5; i++) { activeSessions[i].active = false; }

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
          sendTelegramMessage(target_chat, "🟢 <b>Quiz Bot Hub online (v3.0)</b>\nСистема вікторини готова до роботи.", target_thread, getMainMenuKeyboard());
        }
      }
    }

    if (millis() - last_bot_check > bot_interval) {
      checkTelegramUpdates();
      last_bot_check = millis();
    }
  }
}