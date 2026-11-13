/*
 * FLEY Glasses 1.0 - версия для ESP32-C3 Super Mini (без камеры и без SD-карты)
 * "Видеть. Слышать. Помогать."  ->  в этой версии: Слышать. Отвечать.
 *
 * Что умеет:
 *   - голосовой ассистент: микрофон MAX9814 -> Groq Whisper -> ИИ (Groq) -> голос в динамики (PAM8403)
 *   - громкость: "громче" / "тише"
 *   - активация: кнопка (короткое нажатие = говорить) или слово "Флей" (WAKE_WORDS_AUTO 1)
 *
 * ===== НАСТРОЙКИ Arduino IDE =====
 *   Ядро:   "esp32 by Espressif" версии 3.x
 *   Плата:  "ESP32C3 Dev Module"
 *   USB CDC On Boot: Enabled        (иначе не будет работать Serial по USB)
 *   Flash Size: 4MB, Partition Scheme: Default 4MB (или Huge APP)
 *   Библиотека: ArduinoJson (Benoit Blanchon) 7.x
 *   Если плата не прошивается: зажмите BOOT, нажмите RESET, отпустите BOOT, и снова нажмите "Загрузить".
 *
 * ===== ПОДКЛЮЧЕНИЕ (ESP32-C3 Super Mini) =====
 *   MAX9814:  OUT -> GPIO1,  VDD -> 3V3,  GND -> GND
 *   PAM8403:  вход L и R вместе <- GPIO3 через RC-фильтр (1 кОм последовательно + 10 нФ на землю)
 *             (если на модуле PAM8403 нет входных конденсаторов - добавьте 1 мкФ последовательно)
 *             питание VCC -> +5V (или напрямую от аккумулятора 3.7V), GND общий с ESP32
 *   Кнопка:   встроенная BOOT (GPIO9) работает сразу. Для очков - внешняя кнопка между GPIO9 и GND.
 *   Индикатор: встроенный светодиод платы (GPIO8)
 *   Аккумулятор 3.7V -> плата зарядки TP4056 -> повышайка 5V -> пин 5V платы Super Mini и PAM8403.
 *
 * ВАЖНО: у ESP32-C3 НЕТ ЦАП, звук идёт через ШИМ + RC-фильтр, поэтому он "телефонного" качества.
 *
 * ===== ПАМЯТЬ =====
 * У C3 нет PSRAM, поэтому: запись 8 кГц и максимум REC_MAX_SEC секунд, а голос от TTS приходит потоком
 * и проигрывается кусками по несколько секунд. Если в Serial видите ошибки -32512 / "нет памяти" -
 * уменьшите REC_MAX_SEC до 4.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_timer.h>

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
// Wi-Fi: очки подключаются к точке доступа телефона (режим модема, 2.4 ГГц!)
#define WIFI_SSID        "ВАШ_WIFI"
#define WIFI_PASS        "ВАШ_ПАРОЛЬ"

// Ключ Groq (gsk_...). Никому не показывайте и не выкладывайте прошивку с ключом.
#define GROQ_API_KEY     ""
// Ключ для озвучки (OpenAI sk-...). Пусто = без голоса (только текст в Serial и сигналы).
#define TTS_API_KEY      ""

#define CALL_ME          "сэр"          // как обращаться
#define WAKE_WORDS_AUTO  0              // 1 = слушать постоянно и отвечать на "Флей" (расходует лимиты и батарею)
#define WIFI_LOW_TX      1              // 1 = снизить мощность Wi-Fi (помогает на многих Super Mini, которые не подключаются)

// Модели (если Groq переименовал - смотрите console.groq.com/docs/models)
#define STT_MODEL        "whisper-large-v3-turbo"
#define TEXT_MODEL       "openai/gpt-oss-120b"
#define STT_LANG         "ru"

// Озвучка (OpenAI-совместимый /audio/speech, формат pcm 24 кГц 16 бит)
#define TTS_URL          "https://api.openai.com/v1/audio/speech"
#define TTS_MODEL        "gpt-4o-mini-tts"
#define TTS_VOICE        "onyx"

// ======================= ПИНЫ (ESP32-C3 Super Mini) =======================
#define PIN_MIC   1     // MAX9814 OUT (ADC1_CH1; ADC2 на C3 с Wi-Fi не работает, не используйте GPIO5)
#define PIN_SPK   3     // ШИМ-звук на PAM8403
#define PIN_BTN   9     // кнопка (к GND). GPIO9 = встроенная кнопка BOOT. -1 если не нужна
#define PIN_LED   8     // встроенный светодиод. -1 если не нужен
#define LED_ACTIVE_LOW 1  // на большинстве Super Mini светодиод включается нулём

// ======================= АУДИО-ПАРАМЕТРЫ =======================
#define MIC_SR         8000     // частота записи и воспроизведения (8 кГц - экономия памяти)
#define REC_MAX_SEC    5        // максимум секунд записи фразы (5 сек = 80 КБ ОЗУ)
#define MIC_GAIN       6        // цифровое усиление (MAX9814 уже усиливает аналогово)
#define SILENCE_FRAMES 40       // 40 * 20 мс = 0.8 с тишины = конец фразы
#define TTS_SR         24000    // частота, в которой отдаёт TTS (в 3 раза больше MIC_SR, ниже делим на 3)

// ======================= ПРОМПТЫ =======================
static const char* SYS_BASE =
  "Ты - FLEY, ИИ-ассистент в умных очках пользователя, в стиле E.D.I.T.H. из фильмов про Тони Старка: "
  "спокойная, чёткая, с каплей сухого юмора. Тебя создала компания FLEY Industries; не называй модель, на которой работаешь. "
  "В этих очках нет камеры, изображение ты не видишь. "
  "Обращайся к пользователю: " CALL_ME ". Отвечай по-русски, не больше двух коротких предложений. "
  "Твой ответ будет озвучен: никакого markdown, списков, эмодзи и спецсимволов.";

// ======================= ГЛОБАЛЬНОЕ СОСТОЯНИЕ =======================
#define HEAD_RES 1024           // место перед аудио под заголовок multipart + WAV
#define TAIL_RES 64             // место после аудио под хвост multipart

static uint8_t* bigBuf = nullptr;   // один общий буфер: [HEAD_RES][аудио int16][TAIL_RES]
static int16_t* recBuf = nullptr;   // указатель на аудио-часть
static size_t recCap = 0;           // ёмкость в сэмплах
static float noiseFloor = 60.0f;
static int32_t dc16 = -1;
static int volumePct = 70;

static const int HIST_MAX = 6;
static String histRole[HIST_MAX];
static String histText[HIST_MAX];
static int histCount = 0;

// ======================= УТИЛИТЫ =======================
static void led(bool on) {
  if (PIN_LED >= 0) digitalWrite(PIN_LED, ((on ? 1 : 0) ^ LED_ACTIVE_LOW) ? HIGH : LOW);
}

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
      default: if ((uint8_t)c >= 0x20) o += c; break;
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
  const size_t MAXS = recCap;
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
  if (speechFrames < 10) return 0;                // < 200 мс = щелчок, не речь
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

// POST с текстовым ответом.
static int httpPost(const String& url, const String& bearer, const char* ctype,
                    const uint8_t* data, size_t len, String* outStr) {
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
  if (outStr) *outStr = http.getString();
  http.end();
  return code;
}

// ======================= РАСПОЗНАВАНИЕ РЕЧИ (Groq Whisper) =======================
static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

// Без копирования: заголовок multipart + WAV пишем прямо перед аудио в том же буфере.
static String transcribe(size_t n) {
  const String B = "----FLEYBOUNDARY7MA4YWxk";
  auto field = [&](const char* name, const String& val) {
    return "--" + B + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + val + "\r\n";
  };
  String head = field("model", STT_MODEL) + field("language", STT_LANG) +
                field("response_format", "json") + field("temperature", "0") +
                field("prompt", "FLEY, Флей, Эдит, очки.");
  head += "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + B + "--\r\n";

  size_t dataBytes = n * 2;
  size_t pre = head.length() + 44;
  if (pre > HEAD_RES || tail.length() > TAIL_RES) { Serial.println("[STT] мало места под заголовок"); return ""; }

  uint8_t* start = (uint8_t*)recBuf - pre;
  uint8_t* p = start;
  memcpy(p, head.c_str(), head.length()); p += head.length();
  memcpy(p, "RIFF", 4); put32(p + 4, 36 + dataBytes); memcpy(p + 8, "WAVEfmt ", 8);
  put32(p + 16, 16); put16(p + 20, 1); put16(p + 22, 1); put32(p + 24, MIC_SR);
  put32(p + 28, MIC_SR * 2); put16(p + 32, 2); put16(p + 34, 16);
  memcpy(p + 36, "data", 4); put32(p + 40, dataBytes);
  memcpy((uint8_t*)recBuf + dataBytes, tail.c_str(), tail.length());

  size_t total = pre + dataBytes + tail.length();
  String resp;
  int code = httpPost("https://api.groq.com/openai/v1/audio/transcriptions", GROQ_API_KEY,
                      ("multipart/form-data; boundary=" + B).c_str(), start, total, &resp);
  if (code != 200) {
    Serial.printf("[STT] ошибка %d: %s\n", code, resp.substring(0, 200).c_str());
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

// ======================= ИИ: ЧАТ =======================
static String askAI(const String& user) {
  String body = String("{\"model\":\"") + TEXT_MODEL + "\",\"max_tokens\":300,\"temperature\":0.4,"
                "\"reasoning_effort\":\"low\",\"messages\":[{\"role\":\"system\",\"content\":\"" + jsonEsc(SYS_BASE) + "\"}";
  for (int i = 0; i < histCount; i++)
    body += ",{\"role\":\"" + histRole[i] + "\",\"content\":\"" + jsonEsc(histText[i]) + "\"}";
  body += ",{\"role\":\"user\",\"content\":\"" + jsonEsc(user) + "\"}]}";

  String resp;
  int code = httpPost("https://api.groq.com/openai/v1/chat/completions", GROQ_API_KEY,
                      "application/json", (const uint8_t*)body.c_str(), body.length(), &resp);
  if (code != 200) {
    Serial.printf("[LLM] ошибка %d: %s\n", code, resp.substring(0, 200).c_str());
    return "";
  }
  JsonDocument d;
  if (deserializeJson(d, resp)) return "";
  String out = d["choices"][0]["message"]["content"].as<String>();
  return cleanForSpeech(out);
}

// ======================= РЕЧЬ (TTS, потоковое воспроизведение) =======================
// TTS отдаёт PCM 24 кГц. Читаем потоком, на лету делим частоту в 3 раза (усреднение 3 сэмплов -> 8 кГц)
// и копируем в recBuf (запись уже не нужна). Когда буфер полон или поток закончился - проигрываем кусок.
static void say(const String& textIn) {
  String text = clipUtf8(cleanForSpeech(textIn), 300);
  Serial.printf("[FLEY] %s\n", text.c_str());
  if (text.length() == 0) return;
  if (String(TTS_API_KEY).length() < 10) { beepOk(); return; }

  String body = String("{\"model\":\"") + TTS_MODEL + "\",\"voice\":\"" + TTS_VOICE +
                "\",\"response_format\":\"pcm\",\"input\":\"" + jsonEsc(text) +
                "\",\"instructions\":\"Speak Russian calmly and confidently, like a polished AI assistant.\"}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  if (!http.begin(client, TTS_URL)) { beepError(); return; }
  http.addHeader("Authorization", String("Bearer ") + TTS_API_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  if (code != 200) {
    Serial.printf("[TTS] ошибка %d %s\n", code, http.getString().substring(0, 160).c_str());
    http.end();
    beepError();
    return;
  }

  WiFiClient* s = http.getStreamPtr();
  const int total = http.getSize();            // -1 если длина неизвестна
  long got = 0;
  alignas(4) static uint8_t tmp[1536];         // кратно 6 байтам (3 сэмпла по 2 байта)
  size_t tl = 0;
  bool eof = false;
  uint32_t last = millis();
  bool played = false;

  led(true);
  while (!eof) {
    size_t ns = 0;
    while (!eof && ns + sizeof(tmp) / 6 <= recCap) {
      int a = s->available();
      if (a <= 0) {
        if (!s->connected() || millis() - last > 15000) { eof = true; break; }
        delay(2);
        continue;
      }
      size_t want = min((size_t)a, sizeof(tmp) - tl);
      size_t r = s->readBytes(tmp + tl, want);
      tl += r;
      got += r;
      last = millis();
      size_t frames = tl / 6;
      const int16_t* in = (const int16_t*)tmp;
      for (size_t f = 0; f < frames; f++) {
        int32_t sum = (int32_t)in[3 * f] + in[3 * f + 1] + in[3 * f + 2];
        recBuf[ns++] = (int16_t)(sum / 3);
      }
      size_t used = frames * 6;
      tl -= used;
      memmove(tmp, tmp + used, tl);
      if (total > 0 && got >= total) eof = true;
    }
    if (ns > MIC_SR / 5) { playPcm(recBuf, ns, MIC_SR); played = true; }
  }
  led(false);
  http.end();
  if (!played) { Serial.println("[TTS] пустой ответ"); beepError(); }
}

// ======================= ЛОГИКА КОМАНД =======================
static void handleCommand(const String& text) {
  String l = lowerRu(text);
  Serial.printf("[ВЫ] %s\n", text.c_str());

  // --- локальные команды ---
  if (has(l, "громче")) { volumePct = min(100, volumePct + 15); say("Громче."); return; }
  if (has(l, "тише"))   { volumePct = max(20, volumePct - 15);  say("Тише."); return; }

  // --- просьбы про камеру: её в этой версии нет ---
  if (has(l, "посмотри") || has(l, "что я вижу") || has(l, "прочитай") || has(l, "что написано") ||
      has(l, "сфотографир") || has(l, "сделай фото") || has(l, "скан") || has(l, "режим охран")) {
    say("В этой версии очков камеры нет, " CALL_ME ". Я могу только слушать и отвечать.");
    return;
  }

  // --- обычный разговор ---
  String r = askAI(text);
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
  String text = transcribe(n);
  if (text.length() == 0 || isHallucination(text)) return;
  if (auto_ && !hasWakeWord(text)) return;
  handleCommand(text);
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== FLEY Glasses 1.0 (ESP32-C3) ===");

  if (PIN_BTN >= 0) pinMode(PIN_BTN, INPUT_PULLUP);
  if (PIN_LED >= 0) { pinMode(PIN_LED, OUTPUT); led(false); }

  pwmBegin(PIN_SPK, 156250, 8);                        // ШИМ-звук
  pwmWrite(PIN_SPK, 0);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MIC, ADC_11db);

  // Общий буфер под запись и воспроизведение. Берём до того, как Wi-Fi съест память.
  size_t sec = REC_MAX_SEC;
  while (sec >= 2 && !bigBuf) {
    size_t bytes = HEAD_RES + sec * MIC_SR * sizeof(int16_t) + TAIL_RES;
    bigBuf = (uint8_t*)malloc(bytes);
    if (bigBuf) recCap = sec * MIC_SR; else sec--;
  }
  if (!bigBuf) { Serial.println("[!] нет памяти под запись"); while (true) delay(1000); }
  recBuf = (int16_t*)(bigBuf + HEAD_RES);
  Serial.printf("[MEM] буфер %u с, свободно %u байт\n", (unsigned)(recCap / MIC_SR), (unsigned)ESP.getFreeHeap());

  WiFi.mode(WIFI_STA);
#if WIFI_LOW_TX
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] ");
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(300); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " подключено" : " НЕТ СЕТИ");
  Serial.printf("[MEM] после Wi-Fi свободно %u байт\n", (unsigned)ESP.getFreeHeap());

  calibrateNoise();
  beepOk();
  if (WiFi.status() == WL_CONNECTED) say("Системы в норме. FLEY на связи, " CALL_ME ".");
  else beepError();
}

void loop() {
  if (!ensureWifi()) { beepError(); delay(3000); return; }

  // кнопка: нажатие = говорить
  if (PIN_BTN >= 0 && digitalRead(PIN_BTN) == LOW) {
    delay(30);                                   // антидребезг
    if (digitalRead(PIN_BTN) == LOW) {
      while (digitalRead(PIN_BTN) == LOW) delay(10);
      interaction(false);
    }
    return;
  }

#if WAKE_WORDS_AUTO
  interaction(true);
#else
  delay(20);
#endif
}
