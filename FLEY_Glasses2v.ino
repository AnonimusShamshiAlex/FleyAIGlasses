/*
 * FLEY Glasses 1.0 - прошивка умных очков на ESP32-S3 (аналог E.D.I.T.H.)
 * "Видеть. Слышать. Помогать."
 *
 * Что умеет:
 *   - голосовой ассистент: микрофон MAX9814 -> Groq Whisper -> ИИ -> голос в динамики (PAM8403)
 *   - зрение: камера в очках -> Groq vision ("что я вижу", "прочитай", "что написано")
 *   - СКАН ОКРУЖЕНИЯ: "скан" / "оцени опасность" -> уровень угрозы НИЗКИЙ/СРЕДНИЙ/ВЫСОКИЙ
 *   - РЕЖИМ ОХРАНЫ (патруль): сам сканирует обстановку и говорит только если есть опасность
 *   - фото на microSD ("сфотографируй"), громкость ("громче" / "тише")
 *   - ЛИЧНОСТИ ЛЮДЕЙ ПО ЛИЦУ НЕ ОПРЕДЕЛЯЮТСЯ: ИИ описывает количество людей, позы, действия и опасности.
 *
 * ===== ПЛАТА И НАСТРОЙКИ Arduino IDE =====
 *   Ядро: "esp32 by Espressif" версии 3.x (Boards Manager)
 *   Плата: ESP32S3 Dev Module
 *   PSRAM: OPI PSRAM (для модулей N16R8; для N8R2 - QSPI PSRAM)
 *   Flash Size: 16MB (или ваш), Partition Scheme: Huge APP / 16M Flash
 *   USB CDC On Boot: Enabled (чтобы работал Serial)
 *   Библиотека: ArduinoJson (Benoit Blanchon) версии 7.x
 *
 * ===== ПОДКЛЮЧЕНИЕ (пины можно менять ниже) =====
 *   Камера OV2640  -> разъём камеры платы (раскладка Freenove ESP32-S3-WROOM CAM / ESP32-S3-EYE).
 *                     Для очков нужна камера на гибком шлейфе (FPC).
 *   Микрофон MAX9814 OUT -> GPIO1 (АЦП1), VDD -> 3V3, GND -> GND
 *   PAM8403: вход L и R вместе <- GPIO21 через RC-фильтр (1 кОм + 10 нФ на землю), питание 5V/батарея, GND общий
 *   microSD (SD_MMC, 1-bit): CLK=GPIO39, CMD=GPIO38, D0=GPIO40
 *   Кнопка (необязательно): GPIO14 -> GND
 *   Аккумулятор 3.7V -> через плату зарядки/повышайку 5V для PAM8403, 3.3V для ESP32-S3.
 *
 * ВАЖНО: у ESP32-S3 НЕТ ЦАП, поэтому звук в PAM8403 идёт через ШИМ + RC-фильтр.
 * Для чистого звука лучше I2S-усилитель MAX98357A (потребуется небольшая правка play-функции).
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <SD_MMC.h>

// ---- совместимость ШИМ: ядро ESP32 2.x и 3.x ----
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  static inline void pwmBegin(int pin, int freq, int bits) { ledcAttach(pin, freq, bits); }
  static inline void pwmWrite(int pin, int duty) { ledcWrite(pin, duty); }
#else
  #define SPK_LEDC_CH 0
  static inline void pwmBegin(int pin, int freq, int bits) { ledcSetup(SPK_LEDC_CH, freq, bits); ledcAttachPin(pin, SPK_LEDC_CH); }
  static inline void pwmWrite(int pin, int duty) { (void)pin; ledcWrite(SPK_LEDC_CH, duty); }
#endif

// ======================= НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ =======================
// Wi-Fi: очки подключаются к точке доступа телефона (режим модема)
#define WIFI_SSID        "ВАШ_WIFI"
#define WIFI_PASS        "ВАШ_ПАРОЛЬ"

// Ключ Groq (gsk_...). Никому не показывайте и не выкладывайте прошивку с ключом.
#define GROQ_API_KEY     ""
// Ключ для озвучки (OpenAI sk-...). Пусто = без голоса (только текст в Serial и сигналы).
// Groq не умеет русскую озвучку, поэтому голос берётся из OpenAI-совместимого TTS.
#define TTS_API_KEY      ""

#define CALL_ME          "сэр"          // как обращаться
#define WAKE_WORDS_AUTO  0              // 1 = слушать постоянно и отвечать на "Флей"/"Эдит" (расходует лимиты и батарею)
#define PATROL_INTERVAL_MS 20000        // период скана в режиме охраны

// Модели (если Groq переименовал - смотрите console.groq.com/docs/models и /docs/vision)
#define STT_MODEL        "whisper-large-v3-turbo"
#define TEXT_MODEL       "openai/gpt-oss-120b"
#define VISION_MODEL     "qwen/qwen3.8-27b"
#define VISION_MODEL_2   "meta-llama/llama-4-scout-17b-16e-instruct"   // запасная
#define STT_LANG         "ru"

// Озвучка (OpenAI-совместимый /audio/speech, формат pcm 24 кГц 16 бит)
#define TTS_URL          "https://api.openai.com/v1/audio/speech"
#define TTS_MODEL        "gpt-4o-mini-tts"
#define TTS_VOICE        "onyx"

// ======================= ПИНЫ =======================
#define PIN_MIC   1     // MAX9814 OUT (ADC1_CH0)
#define PIN_SPK   21    // ШИМ-звук на PAM8403
#define PIN_BTN   14    // кнопка (к GND), -1 если нет
#define PIN_LED   -1    // индикатор, -1 если нет

// Камера (Freenove ESP32-S3-WROOM CAM / ESP32-S3-EYE)
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_Y9    16
#define CAM_Y8    17
#define CAM_Y7    18
#define CAM_Y6    12
#define CAM_Y5    10
#define CAM_Y4    8
#define CAM_Y3    9
#define CAM_Y2    11
#define CAM_VSYNC 6
#define CAM_HREF  7
#define CAM_PCLK  13

// SD_MMC
#define SD_CLK 39
#define SD_CMD 38
#define SD_D0  40

// ======================= АУДИО-ПАРАМЕТРЫ =======================
#define MIC_SR         16000
#define REC_MAX_SEC    12
#define MIC_GAIN       6        // цифровое усиление (MAX9814 уже усиливает аналогово)
#define SILENCE_FRAMES 45       // 45 * 20 мс = 0.9 с тишины = конец фразы
#define TTS_SR         24000

// ======================= ПРОМПТЫ =======================
static const char* SYS_BASE =
  "Ты - FLEY, ИИ-ассистент в умных очках пользователя, в стиле E.D.I.T.H. из фильмов про Тони Старка: "
  "спокойная, чёткая, с каплей сухого юмора. Тебя создала компания FLEY Industries; не называй модель, на которой работаешь. "
  "Обращайся к пользователю: " CALL_ME ". Отвечай по-русски, не больше двух коротких предложений. "
  "Твой ответ будет озвучен: никакого markdown, списков, эмодзи и спецсимволов.";

static const char* SYS_VISION =
  "Ты - FLEY, ИИ в умных очках, в стиле E.D.I.T.H. Тебе приходит кадр с камеры очков. "
  "Отвечай по-русски, максимум два коротких предложения, без markdown и эмодзи (ответ озвучивается). "
  "Описывай только то, что реально видно; если не уверена - так и скажи. Текст на кадре читай точно. "
  "Никогда не называй имён людей и не определяй личность по лицу: людей описывай по количеству, позе, действиям и одежде. "
  "Обращайся к пользователю: " CALL_ME ".";

static const char* SYS_SCAN =
  "Ты - FLEY, ИИ-система безопасности в умных очках. Режим СКАН ОКРУЖЕНИЯ. Тебе приходит кадр с камеры очков. "
  "Оцени обстановку: сколько людей и что они делают, транспорт, препятствия, ступени, лужи, огонь, дым, острые или опасные предметы, "
  "агрессивное поведение, оружие. Ответ строго такой: первое слово 'УРОВЕНЬ:' и один из вариантов НИЗКИЙ, СРЕДНИЙ или ВЫСОКИЙ, "
  "затем точка, затем одно-два коротких предложения по сути на русском. Ничего не выдумывай: если опасного не видно - НИЗКИЙ. "
  "Никогда не называй имён и не определяй личность по лицу. Без markdown и эмодзи (ответ озвучивается). "
  "Обращайся к пользователю: " CALL_ME ".";

// ======================= ГЛОБАЛЬНОЕ СОСТОЯНИЕ =======================
static int16_t* recBuf = nullptr;
static float noiseFloor = 60.0f;
static int32_t dc16 = -1;
static int volumePct = 70;
static bool patrol = false;
static uint32_t lastPatrol = 0;
static bool sdOk = false;
static bool camOk = false;
static uint32_t photoCounter = 0;

static const int HIST_MAX = 6;
static String histRole[HIST_MAX];
static String histText[HIST_MAX];
static int histCount = 0;

// ======================= УТИЛИТЫ =======================
static void* psalloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

static void led(bool on) { if (PIN_LED >= 0) digitalWrite(PIN_LED, on ? HIGH : LOW); }

// нижний регистр для латиницы и кириллицы (UTF-8)
static String lowerRu(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) {
      o += (char)tolower(c);
    } else if (c == 0xD0 && i + 1 < s.length()) {
      uint8_t d = (uint8_t)s[++i];
      if (d == 0x81) { o += (char)0xD1; o += (char)0x91; }
      else if (d >= 0x90 && d <= 0x9F) { o += (char)0xD0; o += (char)(d + 0x20); }
      else if (d >= 0xA0 && d <= 0xAF) { o += (char)0xD1; o += (char)(d - 0x20); }
      else { o += (char)0xD0; o += (char)d; }
    } else {
      o += (char)c;
    }
  }
  return o;
}

static bool has(const String& l, const char* part) { return l.indexOf(part) >= 0; }

static String jsonEsc(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': break;
      case '\t': o += ' '; break;
      default: if ((uint8_t)c >= 0x20 || (uint8_t)c >= 0x80) o += c; break;
    }
  }
  return o;
}

static String cleanForSpeech(String s) {
  // убрать блок размышлений <think>...</think>
  int a = s.indexOf("<think>");
  int b = s.indexOf("</think>");
  if (a >= 0 && b > a) s = s.substring(0, a) + s.substring(b + 8);
  s.replace("*", ""); s.replace("#", ""); s.replace("`", ""); s.replace("_", " ");
  s.replace("\n", " ");
  s.trim();
  return s;
}

static String clipUtf8(String s, size_t maxBytes) {
  if (s.length() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && (((uint8_t)s[cut]) & 0xC0) == 0x80) cut--;
  s = s.substring(0, cut);
  int dot = s.lastIndexOf('.');
  if (dot > 40) s = s.substring(0, dot + 1);
  return s;
}

static void pushHistory(const char* role, const String& text) {
  if (histCount == HIST_MAX) {
    for (int i = 1; i < HIST_MAX; i++) { histRole[i - 1] = histRole[i]; histText[i - 1] = histText[i]; }
    histCount--;
  }
  histRole[histCount] = role;
  histText[histCount] = text;
  histCount++;
}

// ======================= ЗВУК: ВЫВОД (ШИМ -> PAM8403) =======================
static void spkWrite(int16_t s) {
  int v = ((int32_t)s * volumePct / 100) >> 8;  // 16 бит -> 8 бит
  v += 128;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  pwmWrite(PIN_SPK, v);
}

static void playPcm(const int16_t* p, size_t n, int sr) {
  const int64_t step = (1000000LL << 16) / sr;
  int64_t next = esp_timer_get_time() << 16;
  for (int v = 0; v <= 128; v += 4) { pwmWrite(PIN_SPK, v); delayMicroseconds(150); }  // мягкий старт
  for (size_t i = 0; i < n; i++) {
    while ((esp_timer_get_time() << 16) < next) { }
    next += step;
    spkWrite(p[i]);
    if ((i & 255) == 255) yield();
  }
  for (int v = 128; v >= 0; v -= 4) { pwmWrite(PIN_SPK, v); delayMicroseconds(150); }  // мягкий стоп
}

static void playTone(int freq, int ms) {
  const int sr = 16000;
  const int n = sr * ms / 1000;
  const int64_t step = (1000000LL << 16) / sr;
  int64_t next = esp_timer_get_time() << 16;
  for (int v = 0; v <= 128; v += 8) { pwmWrite(PIN_SPK, v); delayMicroseconds(100); }
  for (int i = 0; i < n; i++) {
    while ((esp_timer_get_time() << 16) < next) { }
    next += step;
    float env = (i < 200) ? i / 200.0f : (i > n - 200 ? (n - i) / 200.0f : 1.0f);
    int16_t s = (int16_t)(sinf(2.0f * PI * freq * i / sr) * 9000.0f * env);
    spkWrite(s);
  }
  for (int v = 128; v >= 0; v -= 8) { pwmWrite(PIN_SPK, v); delayMicroseconds(100); }
}

static void beepReady()  { playTone(1500, 70); }
static void beepOk()     { playTone(1000, 60); playTone(1500, 80); }
static void beepError()  { playTone(300, 200); playTone(220, 250); }
static void beepAlert()  { for (int i = 0; i < 3; i++) { playTone(1800, 90); delay(60); } }

// ======================= ЗВУК: ВВОД (MAX9814 -> АЦП) =======================
static void calibrateNoise() {
  int64_t step = (1000000LL << 16) / MIC_SR;
  int64_t next = esp_timer_get_time() << 16;
  uint32_t acc = 0;
  const int N = MIC_SR / 2;
  for (int i = 0; i < N; i++) {
    while ((esp_timer_get_time() << 16) < next) { }
    next += step;
    int raw = analogRead(PIN_MIC);
    if (dc16 < 0) dc16 = raw << 4;
    dc16 += ((raw << 4) - dc16) >> 9;
    int32_t c = (raw << 4) - dc16;
    acc += abs((c * MIC_GAIN) >> 4);
  }
  noiseFloor = (float)acc / N;
  if (noiseFloor < 20) noiseFloor = 20;
  Serial.printf("[MIC] шум: %.0f\n", noiseFloor);
}

// Возвращает число сэмплов в recBuf, 0 если речи не было.
static size_t recordSpeech(uint32_t waitSpeechMs) {
  const int FR = MIC_SR / 50;                 // кадр 20 мс
  const size_t MAXS = (size_t)REC_MAX_SEC * MIC_SR;
  const size_t PRE = MIC_SR * 3 / 10;         // 300 мс "до начала речи"
  const int64_t step = (1000000LL << 16) / MIC_SR;
  int64_t next = esp_timer_get_time() << 16;
  size_t idx = 0;
  bool speech = false;
  int loudRun = 0, silent = 0, speechFrames = 0;
  uint32_t t0 = millis();

  while (idx + FR <= MAXS) {
    uint32_t acc = 0;
    for (int i = 0; i < FR; i++) {
      while ((esp_timer_get_time() << 16) < next) { }
      next += step;
      int raw = analogRead(PIN_MIC);
      if (dc16 < 0) dc16 = raw << 4;
      dc16 += ((raw << 4) - dc16) >> 9;
      int32_t s = (((raw << 4) - dc16) * MIC_GAIN) >> 4;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      recBuf[idx + i] = (int16_t)s;
      acc += abs(s);
    }
    float lvl = (float)acc / FR;
    float thr = noiseFloor * 2.5f + 100.0f;
    idx += FR;

    if (lvl > thr) {
      loudRun++;
      if (loudRun >= 2) speech = true;
      if (speech) { silent = 0; speechFrames++; }
    } else {
      loudRun = 0;
      if (speech) silent++;
      else noiseFloor = noiseFloor * 0.95f + lvl * 0.05f;
    }

    if (!speech) {
      if (idx > PRE + FR) {                       // держим только хвост "до речи"
        memmove(recBuf, recBuf + idx - PRE, PRE * sizeof(int16_t));
        idx = PRE;
      }
      if (millis() - t0 > waitSpeechMs) return 0;
    } else if (silent >= SILENCE_FRAMES) {
      break;
    }
  }
  if (speechFrames < 12) return 0;                // < 240 мс = щелчок, не речь
  return idx;
}

// ======================= СЕТЬ =======================
static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[WiFi] переподключение...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// POST. outStr - текстовый ответ; outBin - бинарный ответ (для TTS).
static int httpPost(const String& url, const String& bearer, const char* ctype,
                    const uint8_t* data, size_t len,
                    String* outStr, uint8_t** outBin, size_t* outBinLen, size_t binMax) {
  WiFiClientSecure client;
  client.setInsecure();   // TODO: для продакшена добавьте корневой сертификат (client.setCACert)
  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);   // без chunked - проще читать
  http.setConnectTimeout(10000);
  http.setTimeout(40000);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Content-Type", ctype);
  int code = http.POST((uint8_t*)data, len);
  if (code == 200 && outBin) {
    WiFiClient* s = http.getStreamPtr();
    int total = http.getSize();
    uint8_t* buf = (uint8_t*)psalloc(binMax);
    size_t got = 0;
    uint32_t last = millis();
    while (buf && got < binMax && (http.connected() || s->available()) && millis() - last < 15000) {
      int a = s->available();
      if (a > 0) {
        size_t want = min((size_t)a, binMax - got);
        got += s->readBytes(buf + got, want);
        last = millis();
      } else {
        delay(2);
      }
      if (total > 0 && got >= (size_t)total) break;
    }
    *outBin = buf;
    *outBinLen = got;
  } else if (outStr) {
    *outStr = http.getString();
  }
  http.end();
  return code;
}

// ======================= РАСПОЗНАВАНИЕ РЕЧИ (Groq Whisper) =======================
static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static String transcribe(const int16_t* pcm, size_t n) {
  const String B = "----FLEYBOUNDARY7MA4YWxk";
  auto field = [&](const char* name, const String& val) {
    return "--" + B + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + val + "\r\n";
  };
  String head = field("model", STT_MODEL) + field("language", STT_LANG) +
                field("response_format", "json") + field("temperature", "0") +
                field("prompt", "FLEY, Флей, Эдит, очки, скан, сканируй, опасность, посмотри, что это, режим охраны.");
  head += "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + B + "--\r\n";

  size_t dataBytes = n * 2;
  size_t total = head.length() + 44 + dataBytes + tail.length();
  uint8_t* body = (uint8_t*)psalloc(total);
  if (!body) return "";
  uint8_t* p = body;
  memcpy(p, head.c_str(), head.length()); p += head.length();
  memcpy(p, "RIFF", 4); put32(p + 4, 36 + dataBytes); memcpy(p + 8, "WAVEfmt ", 8);
  put32(p + 16, 16); put16(p + 20, 1); put16(p + 22, 1); put32(p + 24, MIC_SR);
  put32(p + 28, MIC_SR * 2); put16(p + 32, 2); put16(p + 34, 16);
  memcpy(p + 36, "data", 4); put32(p + 40, dataBytes);
  p += 44;
  memcpy(p, pcm, dataBytes); p += dataBytes;
  memcpy(p, tail.c_str(), tail.length());

  String resp;
  int code = httpPost("https://api.groq.com/openai/v1/audio/transcriptions", GROQ_API_KEY,
                      ("multipart/form-data; boundary=" + B).c_str(), body, total, &resp, nullptr, nullptr, 0);
  free(body);
  if (code != 200) {
    Serial.printf("[STT] ошибка %d: %s\n", code, resp.c_str());
    return "";
  }
  JsonDocument d;
  if (deserializeJson(d, resp)) return "";
  String t = d["text"].as<String>();
  t.trim();
  return t;
}

static bool isHallucination(const String& text) {
  String l = lowerRu(text);
  if (l.length() < 3) return true;
  const char* bad[] = {"субтитр", "спасибо за просмотр", "продолжение следует", "редактор", "thanks for watching", "dimatorzok"};
  for (auto b : bad) if (has(l, b)) return true;
  return false;
}

// ======================= КАМЕРА =======================
static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_7;   // подальше от каналов, которые берёт ledcAttach
  c.ledc_timer = LEDC_TIMER_3;
  c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
  c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK; c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN; c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;      // 640x480; для чтения мелкого текста можно FRAMESIZE_SVGA
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) { Serial.printf("[CAM] ошибка 0x%x\n", err); return false; }
  sensor_t* s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, 0); s->set_hmirror(s, 0); }   // при перевёрнутой картинке поменяйте на 1
  return true;
}

static camera_fb_t* grabFrame() {
  if (!camOk) return nullptr;
  camera_fb_t* old = esp_camera_fb_get();     // сбросить устаревший кадр
  if (old) esp_camera_fb_return(old);
  return esp_camera_fb_get();
}

static void savePhoto(camera_fb_t* fb) {
  if (!sdOk || !fb) return;
  char path[40];
  snprintf(path, sizeof(path), "/fley_%05lu.jpg", (unsigned long)(++photoCounter));
  File f = SD_MMC.open(path, FILE_WRITE);
  if (f) { f.write(fb->buf, fb->len); f.close(); Serial.printf("[SD] %s\n", path); }
}

// ======================= ИИ: ЧАТ / ЗРЕНИЕ =======================
static String callChatOnce(const char* model, const String& sys, const String& user,
                           const uint8_t* jpg, size_t jpgLen, bool textModel) {
  size_t b64cap = jpg ? 4 * ((jpgLen + 2) / 3) + 4 : 0;
  char* b64 = nullptr;
  size_t b64len = 0;
  if (jpg) {
    b64 = (char*)psalloc(b64cap);
    if (!b64) return "";
    if (mbedtls_base64_encode((unsigned char*)b64, b64cap, &b64len, jpg, jpgLen) != 0) { free(b64); return ""; }
  }

  String pre = String("{\"model\":\"") + model + "\",\"max_tokens\":350,\"temperature\":0.4,";
  if (textModel) pre += "\"reasoning_effort\":\"low\",";
  pre += "\"messages\":[{\"role\":\"system\",\"content\":\"" + jsonEsc(sys) + "\"}";
  for (int i = 0; i < histCount; i++)
    pre += ",{\"role\":\"" + histRole[i] + "\",\"content\":\"" + jsonEsc(histText[i]) + "\"}";
  String post;
  if (jpg) {
    pre += ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"" + jsonEsc(user) +
           "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    post = "\"}}]}]}";
  } else {
    pre += ",{\"role\":\"user\",\"content\":\"" + jsonEsc(user) + "\"}]}";
  }

  size_t total = pre.length() + b64len + post.length();
  uint8_t* body = (uint8_t*)psalloc(total + 1);
  if (!body) { free(b64); return ""; }
  memcpy(body, pre.c_str(), pre.length());
  if (jpg) {
    memcpy(body + pre.length(), b64, b64len);
    memcpy(body + pre.length() + b64len, post.c_str(), post.length());
  }
  free(b64);

  String resp;
  int code = httpPost("https://api.groq.com/openai/v1/chat/completions", GROQ_API_KEY,
                      "application/json", body, total, &resp, nullptr, nullptr, 0);
  free(body);
  if (code != 200) {
    Serial.printf("[LLM %s] ошибка %d: %s\n", model, code, resp.substring(0, 200).c_str());
    return "";
  }
  JsonDocument d;
  if (deserializeJson(d, resp)) return "";
  String out = d["choices"][0]["message"]["content"].as<String>();
  return cleanForSpeech(out);
}

static String askAI(const String& sys, const String& user, camera_fb_t* fb) {
  String out;
  if (fb) {
    out = callChatOnce(VISION_MODEL, sys, user, fb->buf, fb->len, false);
    if (out.length() == 0) out = callChatOnce(VISION_MODEL_2, sys, user, fb->buf, fb->len, false);
  } else {
    out = callChatOnce(TEXT_MODEL, sys, user, nullptr, 0, true);
  }
  return out;
}

// ======================= РЕЧЬ (TTS) =======================
static void say(const String& textIn) {
  String text = clipUtf8(cleanForSpeech(textIn), 420);
  Serial.printf("[FLEY] %s\n", text.c_str());
  if (text.length() == 0) return;
  if (String(TTS_API_KEY).length() < 10) { beepOk(); return; }

  String body = String("{\"model\":\"") + TTS_MODEL + "\",\"voice\":\"" + TTS_VOICE +
                "\",\"response_format\":\"pcm\",\"input\":\"" + jsonEsc(text) +
                "\",\"instructions\":\"Speak Russian calmly and confidently, like a polished AI assistant.\"}";
  uint8_t* pcm = nullptr;
  size_t len = 0;
  String err;
  int code = httpPost(TTS_URL, TTS_API_KEY, "application/json",
                      (const uint8_t*)body.c_str(), body.length(), &err, &pcm, &len, 1500000);
  if (code == 200 && pcm && len > 2000) {
    led(true);
    playPcm((const int16_t*)pcm, len / 2, TTS_SR);
    led(false);
  } else {
    Serial.printf("[TTS] ошибка %d %s\n", code, err.substring(0, 160).c_str());
    beepError();
  }
  if (pcm) free(pcm);
}

// ======================= ЛОГИКА КОМАНД =======================
static void doScan(bool fromPatrol) {
  camera_fb_t* fb = grabFrame();
  if (!fb) { say("Камера недоступна, " CALL_ME "."); return; }
  String r = askAI(SYS_SCAN, "Проведи скан окружения.", fb);
  if (r.length() == 0) { esp_camera_fb_return(fb); if (!fromPatrol) { beepError(); } return; }
  String l = lowerRu(r);
  bool danger = !has(l, "уровень: низкий") && !has(l, "уровень низкий");
  Serial.printf("[SCAN] %s\n", r.c_str());
  if (danger) savePhoto(fb);                      // тревожный кадр сохраняем
  esp_camera_fb_return(fb);
  if (!fromPatrol || danger) {
    if (danger) beepAlert();
    say(r);
  }
}

static void handleCommand(const String& text) {
  String l = lowerRu(text);
  Serial.printf("[ВЫ] %s\n", text.c_str());

  // --- локальные команды ---
  if (has(l, "режим охран") || has(l, "патрул") || has(l, "включи охран")) {
    patrol = true; lastPatrol = millis();
    say("Режим охраны включён. Сканирую обстановку, " CALL_ME ".");
    return;
  }
  if (has(l, "отбой") || has(l, "выключи охран") || has(l, "отключи охран") || has(l, "отключи патрул") || has(l, "хватит охран")) {
    patrol = false;
    say("Режим охраны отключён.");
    return;
  }
  if (has(l, "громче")) { volumePct = min(100, volumePct + 15); say("Громче."); return; }
  if (has(l, "тише"))   { volumePct = max(20, volumePct - 15);  say("Тише."); return; }
  if (has(l, "сфотографир") || has(l, "сделай фото") || has(l, "сделай снимок")) {
    camera_fb_t* fb = grabFrame();
    if (!fb) { say("Камера недоступна."); return; }
    bool ok = sdOk;
    savePhoto(fb);
    esp_camera_fb_return(fb);
    say(ok ? "Снимок сохранён." : "Карта памяти не найдена, снимок не сохранён.");
    return;
  }

  // --- скан / опасность ---
  if (has(l, "скан") || has(l, "опасн") || has(l, "угроз") || has(l, "безопасн") ||
      has(l, "оцени обстановк") || has(l, "проверь обстановк") || has(l, "что вокруг")) {
    doScan(false);
    return;
  }

  // --- зрение ---
  if (has(l, "посмотри") || has(l, "что это") || has(l, "что я вижу") || has(l, "что перед") ||
      has(l, "прочитай") || has(l, "что написано") || has(l, "что здесь") || has(l, "опиши") ||
      has(l, "перед мной") || has(l, "что там") || has(l, "переведи надпись")) {
    camera_fb_t* fb = grabFrame();
    if (!fb) { say("Камера недоступна, " CALL_ME "."); return; }
    String r = askAI(SYS_VISION, text, fb);
    esp_camera_fb_return(fb);
    if (r.length() == 0) { beepError(); return; }
    pushHistory("user", text + " [с камеры очков]");
    pushHistory("assistant", r);
    say(r);
    return;
  }

  // --- обычный разговор ---
  String r = askAI(SYS_BASE, text, nullptr);
  if (r.length() == 0) { beepError(); return; }
  pushHistory("user", text);
  pushHistory("assistant", r);
  say(r);
}

static bool hasWakeWord(const String& text) {
  String l = lowerRu(text);
  return has(l, "флей") || has(l, "fley") || has(l, "эдит") || has(l, "эдис") || has(l, "edith") || has(l, "фли ");
}

static void interaction(bool auto_) {
  if (!auto_) beepReady();
  led(true);
  size_t n = recordSpeech(auto_ ? 1500 : 6000);
  led(false);
  if (n == 0) return;
  Serial.printf("[MIC] записано %.1f с\n", (float)n / MIC_SR);
  String text = transcribe(recBuf, n);
  if (text.length() == 0 || isHallucination(text)) return;
  if (auto_ && !hasWakeWord(text)) return;
  handleCommand(text);
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== FLEY Glasses 1.0 ===");
  if (!psramFound()) Serial.println("[!] PSRAM не найдена: включите OPI PSRAM в настройках платы");

  if (PIN_BTN >= 0) pinMode(PIN_BTN, INPUT_PULLUP);
  if (PIN_LED >= 0) pinMode(PIN_LED, OUTPUT);

  camOk = initCamera();                                  // камеру - первой (занимает LEDC timer 3)
  pwmBegin(PIN_SPK, 156250, 8);                        // ШИМ-звук
  pwmWrite(PIN_SPK, 0);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MIC, ADC_11db);

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true);
  Serial.printf("[SD] %s\n", sdOk ? "ok" : "нет карты");

  recBuf = (int16_t*)psalloc((size_t)REC_MAX_SEC * MIC_SR * sizeof(int16_t));
  if (!recBuf) { Serial.println("[!] нет памяти под запись"); while (true) delay(1000); }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] ");
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(300); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " подключено" : " НЕТ СЕТИ");

  calibrateNoise();
  beepOk();
  if (WiFi.status() == WL_CONNECTED) say("Системы в норме. FLEY на связи, " CALL_ME ".");
  else beepError();
}

void loop() {
  if (!ensureWifi()) { beepError(); delay(3000); return; }

  // кнопка: короткое нажатие = говорить, долгое (1.5 с) = режим охраны вкл/выкл
  if (PIN_BTN >= 0 && digitalRead(PIN_BTN) == LOW) {
    uint32_t t = millis();
    while (digitalRead(PIN_BTN) == LOW && millis() - t < 1500) delay(10);
    if (millis() - t >= 1500) {
      patrol = !patrol;
      lastPatrol = millis();
      while (digitalRead(PIN_BTN) == LOW) delay(10);
      say(patrol ? "Режим охраны включён." : "Режим охраны отключён.");
    } else {
      interaction(false);
    }
    return;
  }

  if (patrol && millis() - lastPatrol > PATROL_INTERVAL_MS) {
    lastPatrol = millis();
    doScan(true);
    return;
  }

#if WAKE_WORDS_AUTO
  interaction(true);
#else
  delay(20);
#endif
}
