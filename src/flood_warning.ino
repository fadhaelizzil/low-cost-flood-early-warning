#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ctype.h>
#include <math.h>

// =========================
// USER CONFIGURATION
// =========================

const char* WIFI_SSID = "WISMA MAN INSAN CENDEKIA";
const char* WIFI_PASSWORD = "manics1996";

const char* IFTTT_KEY = "tdkhrpT7TgqawPvq43hG6";
const char* IFTTT_ALERT_EVENT = "Flood_Detected";
const char* IFTTT_CLEAR_EVENT = "flood_clear";

// Assumed wiring for NodeMCU ESP8266
constexpr uint8_t TRIG_PIN = D5;
constexpr uint8_t ECHO_PIN = D6;
constexpr uint8_t BUZZER_PIN = D7;

// Set to true for analog water sensor on A0.
// Set to false for a digital water sensor module on WATER_SENSOR_DIGITAL_PIN.
constexpr bool WATER_SENSOR_USE_ANALOG = true;
constexpr uint8_t WATER_SENSOR_DIGITAL_PIN = D2;

// Calibrate these on-site.
// If the sensor is mounted above the water surface, danger usually means
// "distance becomes smaller", so keep ALERT_WHEN_DISTANCE_IS_LESS = true.
constexpr bool ALERT_WHEN_DISTANCE_IS_LESS = true;
constexpr float ULTRASONIC_ALERT_DISTANCE_CM = 25.0f;
constexpr float ULTRASONIC_CLEAR_DISTANCE_CM = 30.0f;

constexpr int WATER_LEVEL_ALERT_THRESHOLD = 650;
constexpr int WATER_LEVEL_CLEAR_THRESHOLD = 550;

// Timing
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long SENSOR_INTERVAL_MS = 500;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long WEBHOOK_RETRY_INTERVAL_MS = 15000;
constexpr unsigned long ALERT_CONFIRM_MS = 7000;
constexpr unsigned long CLEAR_CONFIRM_MS = 15000;

// Ultrasonic tuning
constexpr uint8_t ULTRASONIC_SAMPLES = 5;
constexpr unsigned long ULTRASONIC_TIMEOUT_US = 30000;

// Buzzer pattern
constexpr bool BUZZER_ACTIVE_HIGH = true;
constexpr unsigned long BUZZER_ON_MS = 250;
constexpr unsigned long BUZZER_OFF_MS = 250;

// =========================
// INTERNAL STATE
// =========================

enum class SystemState : uint8_t {
  SAFE,
  PENDING_ALERT,
  ALERT,
  PENDING_CLEAR
};

struct SensorSnapshot {
  float distanceCm = NAN;
  int waterRaw = 0;
  bool ultrasonicValid = false;
  bool alertCandidate = false;
  bool keepAlarm = false;
};

SystemState systemState = SystemState::SAFE;
SensorSnapshot currentReading;

unsigned long lastSensorReadMs = 0;
unsigned long lastWiFiAttemptMs = 0;
unsigned long stateChangedMs = 0;
unsigned long lastWebhookAttemptMs = 0;
unsigned long lastBuzzerToggleMs = 0;

bool buzzerOutputState = false;
bool alertWebhookPending = false;
bool clearWebhookPending = false;

// =========================
// HELPERS
// =========================

const char* stateToString(SystemState state) {
  switch (state) {
    case SystemState::SAFE: return "SAFE";
    case SystemState::PENDING_ALERT: return "PENDING_ALERT";
    case SystemState::ALERT: return "ALERT";
    case SystemState::PENDING_CLEAR: return "PENDING_CLEAR";
    default: return "UNKNOWN";
  }
}

String formatDistance(float value) {
  if (isnan(value)) {
    return "nan";
  }
  return String(value, 1);
}

String urlEncode(const String& input) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(input.length() * 3);

  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(input[i]);

    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

bool compareDistanceToThreshold(float distanceCm, float thresholdCm) {
  if (ALERT_WHEN_DISTANCE_IS_LESS) {
    return distanceCm <= thresholdCm;
  }
  return distanceCm >= thresholdCm;
}

void applyBuzzer(bool on) {
  digitalWrite(BUZZER_PIN, (on == BUZZER_ACTIVE_HIGH) ? HIGH : LOW);
}

bool buzzerEnabledForState() {
  return systemState == SystemState::ALERT || systemState == SystemState::PENDING_CLEAR;
}

void updateBuzzer() {
  if (!buzzerEnabledForState()) {
    if (buzzerOutputState) {
      buzzerOutputState = false;
      applyBuzzer(false);
    }
    return;
  }

  const unsigned long now = millis();
  const unsigned long interval = buzzerOutputState ? BUZZER_ON_MS : BUZZER_OFF_MS;

  if (lastBuzzerToggleMs == 0 || now - lastBuzzerToggleMs >= interval) {
    buzzerOutputState = !buzzerOutputState;
    lastBuzzerToggleMs = now;
    applyBuzzer(buzzerOutputState);
  }
}

void connectWiFi() {
  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (lastWiFiAttemptMs == 0 || millis() - lastWiFiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    connectWiFi();
  }
}

float readUltrasonicDistanceOnceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  const unsigned long durationUs = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);

  if (durationUs == 0) {
    return NAN;
  }

  const float distanceCm = durationUs * 0.0343f / 2.0f;

  if (distanceCm <= 0.0f || distanceCm > 500.0f) {
    return NAN;
  }

  return distanceCm;
}

float readMedianUltrasonicDistanceCm() {
  float samples[ULTRASONIC_SAMPLES];
  uint8_t validCount = 0;

  for (uint8_t i = 0; i < ULTRASONIC_SAMPLES; ++i) {
    const float value = readUltrasonicDistanceOnceCm();

    if (!isnan(value)) {
      samples[validCount++] = value;
    }

    delay(35);
    yield();
  }

  if (validCount == 0) {
    return NAN;
  }

  for (uint8_t i = 1; i < validCount; ++i) {
    float key = samples[i];
    int8_t j = i - 1;

    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      --j;
    }

    samples[j + 1] = key;
  }

  if (validCount % 2 == 1) {
    return samples[validCount / 2];
  }

  const uint8_t upper = validCount / 2;
  const uint8_t lower = upper - 1;
  return (samples[lower] + samples[upper]) / 2.0f;
}

int readWaterSensorRaw() {
  if (WATER_SENSOR_USE_ANALOG) {
    return analogRead(A0);
  }

  return digitalRead(WATER_SENSOR_DIGITAL_PIN) == HIGH ? 1023 : 0;
}

SensorSnapshot readSensors() {
  SensorSnapshot snapshot;

  snapshot.distanceCm = readMedianUltrasonicDistanceCm();
  snapshot.ultrasonicValid = !isnan(snapshot.distanceCm);
  snapshot.waterRaw = readWaterSensorRaw();

  const bool ultrasonicAlert =
      snapshot.ultrasonicValid &&
      compareDistanceToThreshold(snapshot.distanceCm, ULTRASONIC_ALERT_DISTANCE_CM);

  const bool ultrasonicKeep =
      snapshot.ultrasonicValid &&
      compareDistanceToThreshold(snapshot.distanceCm, ULTRASONIC_CLEAR_DISTANCE_CM);

  const bool waterAlert = snapshot.waterRaw >= WATER_LEVEL_ALERT_THRESHOLD;
  const bool waterKeep = snapshot.waterRaw >= WATER_LEVEL_CLEAR_THRESHOLD;

  snapshot.alertCandidate = ultrasonicAlert && waterAlert;
  snapshot.keepAlarm = ultrasonicKeep && waterKeep;

  return snapshot;
}

void transitionTo(SystemState nextState) {
  if (nextState == systemState) {
    return;
  }

  Serial.printf("STATE: %s -> %s\n", stateToString(systemState), stateToString(nextState));
  systemState = nextState;
  stateChangedMs = millis();
}

bool sendIFTTTEvent(const char* eventName, const String& value1, const String& value2, const String& value3) {
  if (eventName == nullptr || eventName[0] == '\0') {
    return true;
  }

  if (IFTTT_KEY == nullptr || IFTTT_KEY[0] == '\0') {
    Serial.println("IFTTT skipped: key is empty");
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("IFTTT skipped: WiFi not connected");
    return false;
  }

  String url = String("https://maker.ifttt.com/trigger/") +
               eventName +
               "/with/key/" +
               IFTTT_KEY +
               "?value1=" + urlEncode(value1) +
               "&value2=" + urlEncode(value2) +
               "&value3=" + urlEncode(value3);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(client, url)) {
    Serial.println("IFTTT error: unable to start HTTPS request");
    return false;
  }

  const int httpCode = https.GET();
  https.end();

  Serial.printf("IFTTT event=%s http=%d\n", eventName, httpCode);
  return httpCode > 0 && httpCode < 400;
}

void processPendingWebhooks() {
  if (!alertWebhookPending && !clearWebhookPending) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (lastWebhookAttemptMs != 0 && millis() - lastWebhookAttemptMs < WEBHOOK_RETRY_INTERVAL_MS) {
    return;
  }

  lastWebhookAttemptMs = millis();

  const String distanceText = formatDistance(currentReading.distanceCm) + " cm";
  const String waterText = String(currentReading.waterRaw);

  if (alertWebhookPending) {
    const bool ok = sendIFTTTEvent(
      IFTTT_ALERT_EVENT,
      "ALERT",
      distanceText,
      waterText
    );

    if (ok) {
      alertWebhookPending = false;
    }
    return;
  }

  if (clearWebhookPending) {
    const bool ok = sendIFTTTEvent(
      IFTTT_CLEAR_EVENT,
      "SAFE",
      distanceText,
      waterText
    );

    if (ok) {
      clearWebhookPending = false;
    }
  }
}

void updateStateMachine(const SensorSnapshot& snapshot) {
  const unsigned long now = millis();

  switch (systemState) {
    case SystemState::SAFE:
      if (snapshot.alertCandidate) {
        transitionTo(SystemState::PENDING_ALERT);
      }
      break;

    case SystemState::PENDING_ALERT:
      if (!snapshot.alertCandidate) {
        transitionTo(SystemState::SAFE);
      } else if (now - stateChangedMs >= ALERT_CONFIRM_MS) {
        transitionTo(SystemState::ALERT);
        alertWebhookPending = true;
        clearWebhookPending = false;
      }
      break;

    case SystemState::ALERT:
      if (!snapshot.keepAlarm) {
        transitionTo(SystemState::PENDING_CLEAR);
      }
      break;

    case SystemState::PENDING_CLEAR:
      if (snapshot.keepAlarm) {
        transitionTo(SystemState::ALERT);
      } else if (now - stateChangedMs >= CLEAR_CONFIRM_MS) {
        transitionTo(SystemState::SAFE);
        alertWebhookPending = false;
        clearWebhookPending = true;
      }
      break;
  }
}

void printSnapshot(const SensorSnapshot& snapshot) {
  String distanceText = snapshot.ultrasonicValid ? formatDistance(snapshot.distanceCm) : "nan";

  Serial.printf(
    "state=%s wifi=%s distance_cm=%s water_raw=%d alertCandidate=%d keepAlarm=%d ip=%s\n",
    stateToString(systemState),
    WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED",
    distanceText.c_str(),
    snapshot.waterRaw,
    snapshot.alertCandidate ? 1 : 0,
    snapshot.keepAlarm ? 1 : 0,
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-"
  );
}

// =========================
// ARDUINO SETUP/LOOP
// =========================

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  Serial.println("Flood Early Warning System booting...");

  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);

  pinMode(ECHO_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  applyBuzzer(false);

  if (!WATER_SENSOR_USE_ANALOG) {
    pinMode(WATER_SENSOR_DIGITAL_PIN, INPUT);
  }

  stateChangedMs = millis();
  connectWiFi();
}

void loop() {
  ensureWiFiConnected();

  if (millis() - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = millis();

    currentReading = readSensors();
    updateStateMachine(currentReading);
    printSnapshot(currentReading);
  }

  updateBuzzer();
  processPendingWebhooks();
  yield();
}
