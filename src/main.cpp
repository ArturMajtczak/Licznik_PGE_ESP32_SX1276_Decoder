#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <mbedtls/aes.h>

#include <ctype.h>
#include <math.h>
#include <string.h>

static constexpr int PIN_NSS  = 10;
static constexpr int PIN_MOSI = 11;
static constexpr int PIN_SCK  = 12;
static constexpr int PIN_MISO = 13;
static constexpr int PIN_RST  = 7;
static constexpr int PIN_DIO0 = 4;
static constexpr int PIN_DIO1 = 5;
static constexpr int PIN_DIO2 = 6;

static constexpr float RF_FREQ_MHZ     = 868.95f;
static constexpr float RF_BITRATE_KBPS = 100.0f;
static constexpr float RF_FDEV_KHZ     = 50.0f;
static constexpr float RF_RX_BW_KHZ    = 250.0f;

static constexpr size_t INITIAL_BYTES      = 3;
static constexpr size_t FIFO_BATCH         = 32;
static constexpr size_t MAX_PACKET_SIZE    = 512;
static constexpr size_t MAX_TRIMMED_SIZE   = 384;
static constexpr size_t MAX_DECRYPTED_SIZE = 256;
static constexpr uint32_t BYTE_TIME_US     = 80;

static constexpr uint8_t TARGET_PREFIX_TEMPLATE[] = {
  0xDE, 0x44, 0x01, 0x06, 0x23, 0x80, 0x26, 0x57, 0x01, 0x02
};

static constexpr char DEFAULT_WIFI_SSID[] = "SIEĆ WIFI";
static constexpr char DEFAULT_WIFI_PASS[] = "HASŁO DO WIFI";
static constexpr char DEFAULT_POST_URL[] = "https://strona.www/dane.php";
static constexpr char DEFAULT_METER_NUMBER[] = "57299423";
static constexpr char DEFAULT_METER_KEY_HEX[] = "32004566754340000000000000000000";
static constexpr uint8_t DEFAULT_STATION_NUMBER = 1U;
static constexpr uint16_t DEFAULT_BUFFER_DAYS = 21U;
static constexpr uint16_t MIN_BUFFER_DAYS = 1U;
static constexpr uint16_t MAX_BUFFER_DAYS = 60U;

static constexpr char CONFIG_NAMESPACE[] = "metercfg";
static constexpr char QUEUE_META_PATH[] = "/queue_meta.bin";
static constexpr char QUEUE_DATA_PATH[] = "/queue_data.bin";
static constexpr uint32_t UPLOAD_RETRY_SUCCESS_MS = 1000UL;
static constexpr uint32_t UPLOAD_RETRY_SUCCESS_WHILE_WEB_MS = 15000UL;
static constexpr uint32_t UPLOAD_RETRY_FAILURE_MS = 30000UL;
static constexpr uint32_t UPLOAD_GUARD_WINDOW_MS = 8000UL;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000UL;
static constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000UL;
static constexpr uint32_t CONFIG_AP_MAX_UPTIME_MS = 15UL * 60000UL;
static constexpr uint32_t STATUS_STALE_MS = 130000UL;
static constexpr uint32_t RADIO_RECOVERY_STALE_MS = 4UL * 60000UL;
static constexpr uint32_t RADIO_RECOVERY_COOLDOWN_MS = 2UL * 60000UL;
static constexpr uint32_t FULL_RESTART_STALE_MS = 12UL * 60000UL;
static constexpr uint32_t UPLOAD_STUCK_RECOVERY_MS = 60UL * 60000UL;
static constexpr uint32_t UPLOAD_STUCK_RESTART_MS = 30UL * 60000UL;
static constexpr uint32_t RESTART_AFTER_PACKET_DELAY_MS = 2000UL;
static constexpr uint32_t WEB_ACTIVITY_ACTIVE_MS = 20000UL;
static constexpr uint16_t QUEUE_META_MAGIC = 0x514DU;
static constexpr uint16_t QUEUE_RECORD_MAGIC = 0x524DU;
static constexpr uint8_t QUEUE_VERSION = 1U;
static constexpr uint8_t STATUS_RGB_LEVEL = 40U;
static constexpr uint8_t STATUS_RGB_PIN = 38;
static constexpr uint8_t STATUS_RGB_COUNT = 1;
static constexpr uint8_t STATUS_LED_BRIGHTNESS = STATUS_RGB_LEVEL / 2U;
static constexpr char CONFIG_AP_PASS[] = "licznik123";

static constexpr uint8_t DECODE3OF6_TABLE[64] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0x03, 0xFF, 0x01, 0x02, 0xFF,
  0xFF, 0xFF, 0xFF, 0x07, 0xFF, 0xFF, 0x00, 0xFF,
  0xFF, 0x05, 0x06, 0xFF, 0x04, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0x0B, 0xFF, 0x09, 0x0A, 0xFF,
  0xFF, 0x0F, 0xFF, 0xFF, 0x08, 0xFF, 0xFF, 0xFF,
  0xFF, 0x0D, 0x0E, 0xFF, 0x0C, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

struct ParsedTelegram {
  char id[9];
  bool hasDeviceDateTime;
  char deviceDateTime[20];
  bool hasCurrentPowerConsumptionKw;
  double currentPowerConsumptionKw;
  bool hasTotalEnergyConsumptionKwh;
  double totalEnergyConsumptionKwh;
  bool hasVoltagePhase1V;
  double voltagePhase1V;
  bool hasVoltagePhase2V;
  double voltagePhase2V;
  bool hasVoltagePhase3V;
  double voltagePhase3V;
};

struct UploadResult {
  bool attempted;
  bool ok;
  int httpCode;
};

struct RuntimeConfig {
  char wifiSsid[33];
  char wifiPass[65];
  char postUrl[192];
  char meterNumber[9];
  char meterKeyHex[33];
  uint8_t meterKey[16];
  uint8_t stationNumber;
  uint16_t bufferDays;
};

#pragma pack(push, 1)
struct QueueRecord {
  uint16_t magic;
  uint8_t version;
  uint8_t flags;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  int32_t wsys;
  int64_t kwhpTot;
  uint16_t vl1n;
  uint16_t vl2n;
  uint16_t vl3n;
  uint8_t station;
  uint16_t checksum;
};

struct QueueMeta {
  uint16_t magic;
  uint8_t version;
  uint8_t reserved;
  uint32_t capacity;
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint32_t checksum;
};
#pragma pack(pop)

SPIClass radioSpi(FSPI);
Module radioModule(PIN_NSS, PIN_DIO0, PIN_RST, PIN_DIO1, radioSpi);
SX1276 radio(&radioModule);
Adafruit_NeoPixel statusPixel(STATUS_RGB_COUNT, STATUS_RGB_PIN, NEO_GRB + NEO_KHZ800);
Preferences preferences;
WebServer webServer(80);

static volatile bool fifoNotEmptyIrq = false;

static RuntimeConfig runtimeConfig = {};
static QueueMeta queueMeta = {};
static uint32_t runtimeQueueCapacity = 0U;
static uint8_t runtimeTargetPrefix[sizeof(TARGET_PREFIX_TEMPLATE)] = {0};

static bool radioOk = false;
static bool storageOk = false;
static bool wifiConnectStarted = false;
static bool portalActive = false;
static bool portalAutoStartAllowed = true;
static bool restartRequested = false;
static bool uploadRestartArmed = false;
static uint8_t signalRssiRaw = 0;
static uint32_t lastUploadAttemptMillis = 0;
static uint32_t lastWifiConnectAttemptMillis = 0;
static uint32_t lastUploadOkMillis = 0;
static uint32_t lastUploadRecoveryMillis = 0;
static uint32_t uploadRestartEligibleAtMillis = 0;
static uint32_t lastWebActivityMillis = 0;
static uint32_t greenBlinkUntilMillis = 0;
static uint32_t configPortalStartedMillis = 0;
static uint32_t restartAtMillis = 0;
static uint32_t setupCompletedMillis = 0;
static uint32_t lastRadioRecoveryMillis = 0;
static uint32_t lastSuccessfulTelegramMillis = 0;
static uint32_t lastMatchedTelegramMillis = 0;
static int8_t lastMatchedRssiDbm = 0;
static uint32_t matchedTelegramCount = 0;
static uint32_t decodedTelegramCount = 0;
static uint32_t decryptFailureCount = 0;
static UploadResult lastUploadResult = {false, false, 0};
static bool lastUploadResultValid = false;
static bool lastTelegramDecodedOk = false;
static bool lastTelegramQueued = false;
static char configApSsid[32] = {0};
static char lastRawTelegramHex[(MAX_PACKET_SIZE * 2U) + 1U] = {0};
static ParsedTelegram lastParsedTelegram = {};

static uint8_t encodedPacket[MAX_PACKET_SIZE] = {0};
static uint8_t decodedPacket[MAX_PACKET_SIZE] = {0};
static uint8_t trimmedPacket[MAX_TRIMMED_SIZE] = {0};
static uint8_t decryptedPayload[MAX_DECRYPTED_SIZE] = {0};

static uint16_t checksum16(const uint8_t* data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum = (sum + data[i]) & 0xFFFFU;
  }
  return static_cast<uint16_t>(sum);
}

static uint32_t checksum32(const uint8_t* data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum = (sum * 131U) + data[i];
  }
  return sum;
}

static void setStatusLedColor(uint8_t red, uint8_t green, uint8_t blue) {
  statusPixel.setPixelColor(0, statusPixel.Color(red, green, blue));
  statusPixel.show();
}

static void blinkGreenStatusLed() {
  greenBlinkUntilMillis = millis() + 50UL;
  setStatusLedColor(STATUS_LED_BRIGHTNESS, 0, 0);
}

static void blinkRedStatusLed() {
  greenBlinkUntilMillis = millis() + 50UL;
  setStatusLedColor(0, STATUS_LED_BRIGHTNESS, 0);
}

static void updateStatusLed() {
  if (static_cast<int32_t>(greenBlinkUntilMillis - millis()) > 0) {
    return;
  }

  setStatusLedColor(0, 0, 0);
}

static void copyString(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0U) {
    return;
  }

  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }

  strncpy(dst, src, dstSize - 1U);
  dst[dstSize - 1U] = '\0';
}

static bool isDigitsOnly(const char* value, size_t expectedLen) {
  if (value == nullptr) {
    return false;
  }

  const size_t actualLen = strlen(value);
  if (actualLen != expectedLen) {
    return false;
  }

  for (size_t i = 0; i < actualLen; i++) {
    if (!isdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }

  return true;
}

static int hexNibble(char value) {
  if ((value >= '0') && (value <= '9')) {
    return value - '0';
  }
  if ((value >= 'a') && (value <= 'f')) {
    return value - 'a' + 10;
  }
  if ((value >= 'A') && (value <= 'F')) {
    return value - 'A' + 10;
  }
  return -1;
}

static bool parseMeterKeyHex(const char* hex, uint8_t* outKey) {
  if ((hex == nullptr) || (outKey == nullptr) || (strlen(hex) != 32U)) {
    return false;
  }

  for (size_t i = 0; i < 16U; i++) {
    const int hi = hexNibble(hex[i * 2U]);
    const int lo = hexNibble(hex[i * 2U + 1U]);
    if ((hi < 0) || (lo < 0)) {
      return false;
    }
    outKey[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  return true;
}

static String meterKeyMasked(const char* keyHex) {
  if ((keyHex == nullptr) || (strlen(keyHex) != 32U)) {
    return String("brak");
  }

  String masked;
  masked.reserve(16);
  masked += keyHex[0];
  masked += keyHex[1];
  masked += keyHex[2];
  masked += keyHex[3];
  masked += "....";
  masked += keyHex[28];
  masked += keyHex[29];
  masked += keyHex[30];
  masked += keyHex[31];
  return masked;
}

static void setDefaultRuntimeConfig(RuntimeConfig& config) {
  memset(&config, 0, sizeof(config));
  copyString(config.wifiSsid, sizeof(config.wifiSsid), DEFAULT_WIFI_SSID);
  copyString(config.wifiPass, sizeof(config.wifiPass), DEFAULT_WIFI_PASS);
  copyString(config.postUrl, sizeof(config.postUrl), DEFAULT_POST_URL);
  copyString(config.meterNumber, sizeof(config.meterNumber), DEFAULT_METER_NUMBER);
  copyString(config.meterKeyHex, sizeof(config.meterKeyHex), DEFAULT_METER_KEY_HEX);
  config.stationNumber = DEFAULT_STATION_NUMBER;
  config.bufferDays = DEFAULT_BUFFER_DAYS;
  parseMeterKeyHex(config.meterKeyHex, config.meterKey);
}

static bool buildTargetPrefixFromMeterNumber(const char* meterNumber) {
  if (!isDigitsOnly(meterNumber, 8U)) {
    return false;
  }

  memcpy(runtimeTargetPrefix, TARGET_PREFIX_TEMPLATE, sizeof(runtimeTargetPrefix));
  for (size_t i = 0; i < 4U; i++) {
    const size_t sourcePos = (3U - i) * 2U;
    const uint8_t high = static_cast<uint8_t>(meterNumber[sourcePos] - '0');
    const uint8_t low = static_cast<uint8_t>(meterNumber[sourcePos + 1U] - '0');
    runtimeTargetPrefix[4U + i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

static void refreshRuntimeDerivedState() {
  if (runtimeConfig.bufferDays < MIN_BUFFER_DAYS) {
    runtimeConfig.bufferDays = MIN_BUFFER_DAYS;
  }
  if (runtimeConfig.bufferDays > MAX_BUFFER_DAYS) {
    runtimeConfig.bufferDays = MAX_BUFFER_DAYS;
  }

  runtimeQueueCapacity = static_cast<uint32_t>(runtimeConfig.bufferDays) * 24UL * 60UL;
  if (runtimeQueueCapacity == 0U) {
    runtimeQueueCapacity = static_cast<uint32_t>(DEFAULT_BUFFER_DAYS) * 24UL * 60UL;
  }

  if (!buildTargetPrefixFromMeterNumber(runtimeConfig.meterNumber)) {
    copyString(runtimeConfig.meterNumber, sizeof(runtimeConfig.meterNumber), DEFAULT_METER_NUMBER);
    buildTargetPrefixFromMeterNumber(runtimeConfig.meterNumber);
  }

  if (!parseMeterKeyHex(runtimeConfig.meterKeyHex, runtimeConfig.meterKey)) {
    copyString(runtimeConfig.meterKeyHex, sizeof(runtimeConfig.meterKeyHex), DEFAULT_METER_KEY_HEX);
    parseMeterKeyHex(runtimeConfig.meterKeyHex, runtimeConfig.meterKey);
  }
}

static bool loadRuntimeConfig() {
  RuntimeConfig loaded = {};
  setDefaultRuntimeConfig(loaded);

  if (!preferences.begin(CONFIG_NAMESPACE, true)) {
    runtimeConfig = loaded;
    refreshRuntimeDerivedState();
    return false;
  }

  const String wifiSsid = preferences.getString("wifi_ssid", loaded.wifiSsid);
  const String wifiPass = preferences.getString("wifi_pass", loaded.wifiPass);
  const String postUrl = preferences.getString("post_url", loaded.postUrl);
  const String meterNumber = preferences.getString("meter_num", loaded.meterNumber);
  const String meterKeyHex = preferences.getString("meter_key", loaded.meterKeyHex);
  loaded.stationNumber = static_cast<uint8_t>(preferences.getUChar("station", loaded.stationNumber));
  loaded.bufferDays = static_cast<uint16_t>(preferences.getUShort("buf_days", loaded.bufferDays));
  preferences.end();

  copyString(loaded.wifiSsid, sizeof(loaded.wifiSsid), wifiSsid.c_str());
  copyString(loaded.wifiPass, sizeof(loaded.wifiPass), wifiPass.c_str());
  copyString(loaded.postUrl, sizeof(loaded.postUrl), postUrl.c_str());
  copyString(loaded.meterNumber, sizeof(loaded.meterNumber), meterNumber.c_str());
  copyString(loaded.meterKeyHex, sizeof(loaded.meterKeyHex), meterKeyHex.c_str());

  runtimeConfig = loaded;
  refreshRuntimeDerivedState();
  return true;
}

static bool saveRuntimeConfig(const RuntimeConfig& config) {
  if (!preferences.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }

  const size_t ssidSaved = preferences.putString("wifi_ssid", config.wifiSsid);
  const size_t passSaved = preferences.putString("wifi_pass", config.wifiPass);
  const size_t urlSaved = preferences.putString("post_url", config.postUrl);
  const size_t meterSaved = preferences.putString("meter_num", config.meterNumber);
  const size_t keySaved = preferences.putString("meter_key", config.meterKeyHex);

  const bool ok =
    ((ssidSaved > 0U) || (config.wifiSsid[0] == '\0')) &&
    ((passSaved > 0U) || (config.wifiPass[0] == '\0')) &&
    ((urlSaved > 0U) || (config.postUrl[0] == '\0')) &&
    (meterSaved > 0U) &&
    (keySaved > 0U) &&
    (preferences.putUChar("station", config.stationNumber) == sizeof(uint8_t)) &&
    (preferences.putUShort("buf_days", config.bufferDays) == sizeof(uint16_t));

  preferences.end();
  return ok;
}

static String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

static bool hasWifiCredentials() {
  return runtimeConfig.wifiSsid[0] != '\0';
}

static void startStationConnection() {
  if (!hasWifiCredentials()) {
    wifiConnectStarted = false;
    return;
  }

  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(runtimeConfig.wifiSsid, runtimeConfig.wifiPass);
  wifiConnectStarted = true;
  lastWifiConnectAttemptMillis = millis();
}

static void ensureConfigApIdentity() {
  if (configApSsid[0] != '\0') {
    return;
  }

  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(
    configApSsid,
    sizeof(configApSsid),
    "Licznik-Setup-%04X",
    static_cast<unsigned>(chipId & 0xFFFFULL)
  );
}

static void startConfigPortal() {
  ensureConfigApIdentity();
  portalAutoStartAllowed = true;
  if (!portalActive) {
    WiFi.mode(hasWifiCredentials() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(configApSsid, CONFIG_AP_PASS);
    portalActive = true;
  }
  configPortalStartedMillis = millis();
}

static void stopConfigPortalIfAllowed() {
  if (!portalActive) {
    return;
  }

  const uint32_t uptime = millis() - configPortalStartedMillis;
  const bool portalMaxRuntimeElapsed = uptime >= CONFIG_AP_MAX_UPTIME_MS;
  if (!portalMaxRuntimeElapsed) {
    return;
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive = false;
  portalAutoStartAllowed = false;
}

static void maintainWifiConnection() {
  if (!hasWifiCredentials()) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (!wifiConnectStarted || ((now - lastWifiConnectAttemptMillis) >= WIFI_RECONNECT_INTERVAL_MS)) {
    if (wifiConnectStarted) {
      WiFi.disconnect(false, false);
    }
    startStationConnection();
  }
}

static bool ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  if (!hasWifiCredentials()) {
    return false;
  }

  maintainWifiConnection();
  const uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  while ((WiFi.status() != WL_CONNECTED) && (static_cast<int32_t>(deadline - millis()) > 0)) {
    webServer.handleClient();
    delay(100);
  }

  return WiFi.status() == WL_CONNECTED;
}

static void managePortalLifetime() {
  if (portalAutoStartAllowed && !portalActive && (WiFi.status() != WL_CONNECTED)) {
    startConfigPortal();
  }
  stopConfigPortalIfAllowed();
}

static void markWebActivity() {
  lastWebActivityMillis = millis();
}

static bool isWebUiActive() {
  return (lastWebActivityMillis != 0U) && ((millis() - lastWebActivityMillis) <= WEB_ACTIVITY_ACTIVE_MS);
}

static bool isReceiveGuardWindowActive() {
  if (lastSuccessfulTelegramMillis == 0U) {
    return false;
  }

  const uint32_t elapsed = millis() - lastSuccessfulTelegramMillis;
  const uint32_t timeToNext = (elapsed < 60000UL) ? (60000UL - elapsed) : 0UL;
  return timeToNext <= UPLOAD_GUARD_WINDOW_MS;
}

static const char* uploadProbeHost() {
  static char host[64] = {0};

  const char* url = runtimeConfig.postUrl;
  if ((url == nullptr) || (url[0] == '\0')) {
    return "windyone.pl";
  }

  const char* start = strstr(url, "://");
  start = (start != nullptr) ? (start + 3) : url;
  size_t length = 0U;
  while ((start[length] != '\0') && (start[length] != '/') && (start[length] != ':') && (length < (sizeof(host) - 1U))) {
    host[length] = start[length];
    length++;
  }
  host[length] = '\0';

  return (length > 0U) ? host : "windyone.pl";
}

static bool probeUploadPath() {
  IPAddress resolvedIp;
  if (!WiFi.hostByName(uploadProbeHost(), resolvedIp)) {
    return false;
  }

  WiFiClient client;
  const bool connected = client.connect(resolvedIp, 443, 1500);
  if (connected) {
    client.stop();
  }
  return connected;
}

static void forceWifiRecovery() {
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.mode(WIFI_STA);
  wifiConnectStarted = false;
  startStationConnection();
  lastUploadRecoveryMillis = millis();
}

static void initQueueMetaDefaults() {
  memset(&queueMeta, 0, sizeof(queueMeta));
  queueMeta.magic = QUEUE_META_MAGIC;
  queueMeta.version = QUEUE_VERSION;
  queueMeta.capacity = runtimeQueueCapacity;
}

static bool saveQueueMeta() {
  queueMeta.checksum = checksum32(reinterpret_cast<const uint8_t*>(&queueMeta), sizeof(queueMeta) - sizeof(queueMeta.checksum));

  File file = SPIFFS.open(QUEUE_META_PATH, "w");
  if (!file) {
    return false;
  }

  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&queueMeta), sizeof(queueMeta));
  file.close();
  return written == sizeof(queueMeta);
}

static bool loadQueueMeta() {
  if (!SPIFFS.exists(QUEUE_META_PATH)) {
    initQueueMetaDefaults();
    return saveQueueMeta();
  }

  File file = SPIFFS.open(QUEUE_META_PATH, FILE_READ);
  if (!file) {
    return false;
  }

  QueueMeta loaded = {};
  const size_t readLen = file.read(reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded));
  file.close();
  if (readLen != sizeof(loaded)) {
    initQueueMetaDefaults();
    return saveQueueMeta();
  }

  const uint32_t expected = checksum32(reinterpret_cast<const uint8_t*>(&loaded), sizeof(loaded) - sizeof(loaded.checksum));
  const bool valid = (loaded.magic == QUEUE_META_MAGIC) &&
                     (loaded.version == QUEUE_VERSION) &&
                     (loaded.capacity == runtimeQueueCapacity) &&
                     (loaded.checksum == expected) &&
                     (loaded.head < runtimeQueueCapacity) &&
                     (loaded.tail < runtimeQueueCapacity) &&
                     (loaded.count <= runtimeQueueCapacity);
  if (!valid) {
    initQueueMetaDefaults();
    return saveQueueMeta();
  }

  queueMeta = loaded;
  return true;
}

static File openQueueDataFile(const char* mode) {
  File file = SPIFFS.open(QUEUE_DATA_PATH, mode);
  return file;
}

static bool writeQueueRecordAt(uint32_t index, const QueueRecord& record) {
  File file = openQueueDataFile("r+");
  if (!file) {
    return false;
  }

  const size_t offset = static_cast<size_t>(index) * sizeof(QueueRecord);
  const bool seekOk = file.seek(offset, SeekSet);
  const size_t written = seekOk ? file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) : 0U;
  file.close();
  return seekOk && (written == sizeof(record));
}

static bool readQueueRecordAt(uint32_t index, QueueRecord& record) {
  File file = SPIFFS.open(QUEUE_DATA_PATH, FILE_READ);
  if (!file) {
    return false;
  }

  const size_t offset = static_cast<size_t>(index) * sizeof(QueueRecord);
  const bool seekOk = file.seek(offset, SeekSet);
  const size_t readLen = seekOk ? file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) : 0U;
  file.close();
  if (!seekOk || (readLen != sizeof(record))) {
    return false;
  }

  const uint16_t expected = checksum16(reinterpret_cast<const uint8_t*>(&record), sizeof(record) - sizeof(record.checksum));
  return (record.magic == QUEUE_RECORD_MAGIC) &&
         (record.version == QUEUE_VERSION) &&
         (record.station == runtimeConfig.stationNumber) &&
         (record.checksum == expected);
}

static bool enqueueQueueRecord(QueueRecord& record) {
  record.magic = QUEUE_RECORD_MAGIC;
  record.version = QUEUE_VERSION;
  record.station = runtimeConfig.stationNumber;
  record.checksum = checksum16(reinterpret_cast<const uint8_t*>(&record), sizeof(record) - sizeof(record.checksum));

  if (!writeQueueRecordAt(queueMeta.head, record)) {
    return false;
  }

  if (queueMeta.count == queueMeta.capacity) {
    queueMeta.tail = (queueMeta.tail + 1U) % queueMeta.capacity;
  } else {
    queueMeta.count++;
  }

  queueMeta.head = (queueMeta.head + 1U) % queueMeta.capacity;
  return saveQueueMeta();
}

static bool peekOldestQueueRecord(QueueRecord& record) {
  if (queueMeta.count == 0U) {
    return false;
  }
  return readQueueRecordAt(queueMeta.tail, record);
}

static bool dropOldestQueueRecord() {
  if (queueMeta.count == 0U) {
    return false;
  }

  queueMeta.tail = (queueMeta.tail + 1U) % queueMeta.capacity;
  queueMeta.count--;
  return saveQueueMeta();
}

static uint32_t queuePendingCount() {
  return queueMeta.count;
}

static bool ensureQueueDataFile() {
  const size_t expectedSize = static_cast<size_t>(runtimeQueueCapacity) * sizeof(QueueRecord);

  File existing = SPIFFS.open(QUEUE_DATA_PATH, FILE_READ);
  if (existing) {
    const size_t currentSize = existing.size();
    existing.close();
    if (currentSize == expectedSize) {
      return true;
    }
    SPIFFS.remove(QUEUE_DATA_PATH);
  }

  File file = SPIFFS.open(QUEUE_DATA_PATH, "w");
  if (!file) {
    return false;
  }

  static uint8_t zeros[256] = {0};
  size_t remaining = expectedSize;
  while (remaining > 0U) {
    const size_t chunk = (remaining > sizeof(zeros)) ? sizeof(zeros) : remaining;
    const size_t written = file.write(zeros, chunk);
    if (written != chunk) {
      file.close();
      return false;
    }
    remaining -= chunk;
  }

  file.close();
  return true;
}

static bool initStorage() {
  if (!SPIFFS.begin(true)) {
    return false;
  }
  if (!loadQueueMeta()) {
    return false;
  }
  return ensureQueueDataFile();
}

static String htmlEscape(const String& value) {
  String out;
  out.reserve(value.length() + 16U);
  for (size_t i = 0; i < value.length(); i++) {
    switch (value[i]) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += value[i];
        break;
    }
  }
  return out;
}

static String jsonEscape(const char* value) {
  if (value == nullptr) {
    return String();
  }

  String out;
  for (size_t i = 0; value[i] != '\0'; i++) {
    const char c = value[i];
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

static String jsonBool(bool value) {
  return value ? "true" : "false";
}

static String optionalJsonNumber(bool hasValue, double value, uint8_t decimals) {
  if (!hasValue) {
    return String("null");
  }
  return String(value, static_cast<unsigned int>(decimals));
}

static String optionalJsonString(bool hasValue, const char* value) {
  if (!hasValue) {
    return String("null");
  }
  return String("\"") + jsonEscape(value) + "\"";
}

static String buildStatusJson() {
  const uint32_t now = millis();
  const bool meterReceiving = (lastSuccessfulTelegramMillis != 0U) && ((now - lastSuccessfulTelegramMillis) <= STATUS_STALE_MS);

  String json;
  json.reserve(2200);
  json += "{";
  json += "\"wifi_connected\":";
  json += jsonBool(WiFi.status() == WL_CONNECTED);
  json += ",\"wifi_ssid\":\"";
  json += jsonEscape(runtimeConfig.wifiSsid);
  json += "\",\"wifi_ip\":\"";
  json += jsonEscape((WiFi.status() == WL_CONNECTED) ? ipToString(WiFi.localIP()).c_str() : "");
  json += "\",\"ap_active\":";
  json += jsonBool(portalActive);
  json += ",\"ap_ssid\":\"";
  json += jsonEscape(configApSsid);
  json += "\",\"ap_ip\":\"";
  json += jsonEscape(portalActive ? ipToString(WiFi.softAPIP()).c_str() : "");
  json += "\",\"storage_ok\":";
  json += jsonBool(storageOk);
  json += ",\"radio_ok\":";
  json += jsonBool(radioOk);
  json += ",\"queue_pending\":";
  json += String(queuePendingCount());
  json += ",\"buffer_days\":";
  json += String(runtimeConfig.bufferDays);
  json += ",\"station_number\":";
  json += String(runtimeConfig.stationNumber);
  json += ",\"meter_number\":\"";
  json += jsonEscape(runtimeConfig.meterNumber);
  json += "\",\"post_url\":\"";
  json += jsonEscape(runtimeConfig.postUrl);
  json += "\",\"uptime_sec\":";
  json += String(now / 1000UL);
  json += ",\"meter_receiving\":";
  json += jsonBool(meterReceiving);
  json += ",\"matched_count\":";
  json += String(matchedTelegramCount);
  json += ",\"decoded_count\":";
  json += String(decodedTelegramCount);
  json += ",\"decrypt_failures\":";
  json += String(decryptFailureCount);
  json += ",\"last_packet_ms_ago\":";
  json += (lastSuccessfulTelegramMillis == 0U) ? "null" : String(now - lastSuccessfulTelegramMillis);
  json += ",\"last_match_ms_ago\":";
  json += (lastMatchedTelegramMillis == 0U) ? "null" : String(now - lastMatchedTelegramMillis);
  json += ",\"last_rssi_dbm\":";
  json += (lastMatchedTelegramMillis == 0U) ? "null" : String(lastMatchedRssiDbm);
  json += ",\"last_decode_ok\":";
  json += jsonBool(lastTelegramDecodedOk);
  json += ",\"last_queued\":";
  json += jsonBool(lastTelegramQueued);
  json += ",\"last_upload_attempted\":";
  json += jsonBool(lastUploadResultValid && lastUploadResult.attempted);
  json += ",\"last_upload_ok\":";
  json += jsonBool(lastUploadResultValid && lastUploadResult.ok);
  json += ",\"last_http_code\":";
  json += lastUploadResultValid ? String(lastUploadResult.httpCode) : "null";
  json += ",\"last_raw_telegram\":\"";
  json += jsonEscape(lastRawTelegramHex);
  json += "\",\"last_id\":\"";
  json += jsonEscape(lastParsedTelegram.id);
  json += "\",\"device_date_time\":";
  json += optionalJsonString(lastParsedTelegram.hasDeviceDateTime, lastParsedTelegram.deviceDateTime);
  json += ",\"current_power_consumption_kw\":";
  json += optionalJsonNumber(lastParsedTelegram.hasCurrentPowerConsumptionKw, lastParsedTelegram.currentPowerConsumptionKw, 3);
  json += ",\"total_energy_consumption_kwh\":";
  json += optionalJsonNumber(lastParsedTelegram.hasTotalEnergyConsumptionKwh, lastParsedTelegram.totalEnergyConsumptionKwh, 3);
  json += ",\"voltage_at_phase_1_v\":";
  json += optionalJsonNumber(lastParsedTelegram.hasVoltagePhase1V, lastParsedTelegram.voltagePhase1V, 1);
  json += ",\"voltage_at_phase_2_v\":";
  json += optionalJsonNumber(lastParsedTelegram.hasVoltagePhase2V, lastParsedTelegram.voltagePhase2V, 1);
  json += ",\"voltage_at_phase_3_v\":";
  json += optionalJsonNumber(lastParsedTelegram.hasVoltagePhase3V, lastParsedTelegram.voltagePhase3V, 1);
  json += "}";
  return json;
}

static String buildRootPage() {
  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  const String ssid = htmlEscape(String(runtimeConfig.wifiSsid));
  const String meterNumber = htmlEscape(String(runtimeConfig.meterNumber));
  const String postUrl = htmlEscape(String(runtimeConfig.postUrl));
  const String stationNumber = String(runtimeConfig.stationNumber);
  const String bufferDays = String(runtimeConfig.bufferDays);
  const String keyMask = htmlEscape(meterKeyMasked(runtimeConfig.meterKeyHex));
  const String wifiIp = wifiConnected ? htmlEscape(ipToString(WiFi.localIP())) : String("-");
  const String apIp = portalActive ? htmlEscape(ipToString(WiFi.softAPIP())) : String("-");

  String page;
  page.reserve(7000);
  page += "<!doctype html><html lang='pl'><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Licznik SX1276</title>";
  page += "<style>";
  page += ":root{color-scheme:dark;}";
  page += "body{font-family:Verdana,sans-serif;background:radial-gradient(circle at top,#1f2937 0%,#0f172a 45%,#020617 100%);color:#e5eef7;margin:0;padding:0;}";
  page += ".wrap{max-width:980px;margin:0 auto;padding:20px;}";
  page += ".card{background:rgba(15,23,42,.88);border:1px solid rgba(148,163,184,.18);border-radius:14px;padding:18px;margin-bottom:18px;box-shadow:0 14px 38px rgba(2,6,23,.42);backdrop-filter:blur(8px);}";
  page += "h1,h2{margin:0 0 14px 0;}h1{font-size:28px;color:#f8fafc;}h2{font-size:20px;color:#dbeafe;}";
  page += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}";
  page += ".metric{background:linear-gradient(180deg,rgba(30,41,59,.92),rgba(15,23,42,.92));border:1px solid rgba(96,165,250,.16);border-radius:12px;padding:12px;}";
  page += ".label{font-size:12px;text-transform:uppercase;color:#93c5fd;margin-bottom:4px;letter-spacing:.08em;}";
  page += ".value{font-size:18px;font-weight:700;word-break:break-word;color:#f8fafc;}";
  page += "form{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px;}";
  page += "label{display:block;font-size:14px;font-weight:600;margin-bottom:6px;color:#dbeafe;}";
  page += "input{width:100%;padding:11px 12px;border:1px solid #334155;border-radius:10px;box-sizing:border-box;background:#020617;color:#f8fafc;}";
  page += "input::placeholder{color:#64748b;}";
  page += ".full{grid-column:1/-1;}button{background:linear-gradient(135deg,#0f766e,#0891b2);color:#fff;border:none;border-radius:10px;padding:12px 16px;font-weight:700;cursor:pointer;}";
  page += "button:hover{filter:brightness(1.08);}a{color:#7dd3fc;}.hint{font-size:13px;color:#94a3b8;margin-top:6px;}.mono{font-family:Consolas,monospace;}";
  page += "pre{white-space:pre-wrap;word-break:break-word;background:#020617;color:#dbeafe;border:1px solid rgba(96,165,250,.16);border-radius:12px;padding:12px;min-height:96px;}";
  page += "</style></head><body><div class='wrap'>";
  page += "<div class='card'><h1>Licznik SX1276</h1>";
  page += "<div class='hint'>Hotspot konfiguracji: <span class='mono'>";
  page += htmlEscape(String(configApSsid));
  page += "</span>, haslo: <span class='mono'>";
  page += htmlEscape(String(CONFIG_AP_PASS));
  page += "</span>, AP IP: <span class='mono'>";
  page += apIp;
  page += "</span>, STA IP: <span class='mono'>";
  page += wifiIp;
  page += "</span></div></div>";

  page += "<div class='card'><h2>Status</h2><div class='grid'>";
  page += "<div class='metric'><div class='label'>Wi-Fi</div><div class='value' id='wifiState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Hotspot</div><div class='value' id='apState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Odbior z licznika</div><div class='value' id='meterState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Kolejka</div><div class='value' id='queueState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Ostatni RSSI</div><div class='value' id='rssiState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Ostatni upload</div><div class='value' id='uploadState'>-</div></div>";
  page += "</div></div>";

  page += "<div class='card'><h2>Ostatnie dane z licznika</h2><div class='grid'>";
  page += "<div class='metric'><div class='label'>Numer licznika</div><div class='value' id='meterIdState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Data z licznika</div><div class='value' id='deviceTimeState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Moc chwilowa</div><div class='value' id='powerState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Energia calkowita</div><div class='value' id='energyState'>-</div></div>";
  page += "<div class='metric'><div class='label'>Nap. L1</div><div class='value' id='l1State'>-</div></div>";
  page += "<div class='metric'><div class='label'>Nap. L2</div><div class='value' id='l2State'>-</div></div>";
  page += "<div class='metric'><div class='label'>Nap. L3</div><div class='value' id='l3State'>-</div></div>";
  page += "<div class='metric'><div class='label'>Dopasowane telegramy</div><div class='value' id='matchCountState'>-</div></div>";
  page += "</div><div class='hint'>Status strony laduje sie jednorazowo po otwarciu. Aby pobrac nowsze dane, odswiez strone w przegladarce.</div><div class='hint'>Surowy ostatni telegram:</div><pre id='rawTelegram'>Brak danych</pre></div>";

  page += "<div class='card'><h2>Konfiguracja</h2>";
  page += "<form method='post' action='/save'>";
  page += "<div><label for='ssid'>SSID Wi-Fi</label><input id='ssid' name='ssid' value='" + ssid + "'></div>";
  page += "<div><label for='wifi_pass'>Haslo Wi-Fi</label><input id='wifi_pass' name='wifi_pass' type='password' placeholder='zostaw puste aby nie zmieniac'></div>";
  page += "<div><label for='station_number'>Station number</label><input id='station_number' name='station_number' type='number' min='0' max='255' value='" + stationNumber + "'></div>";
  page += "<div><label for='meter_number'>Numer licznika</label><input id='meter_number' name='meter_number' maxlength='8' value='" + meterNumber + "'></div>";
  page += "<div><label for='meter_key'>METER_KEY (32 znaki HEX)</label><input id='meter_key' name='meter_key' class='mono' maxlength='32' placeholder='zostaw puste aby nie zmieniac'></div>";
  page += "<div class='hint'>Aktualny klucz: <span class='mono'>" + keyMask + "</span></div>";
  page += "<div class='full'><label for='post_url'>Adres wysylki danych</label><input id='post_url' name='post_url' value='" + postUrl + "'></div>";
  page += "<div><label for='buffer_days'>Liczba dni bufora</label><input id='buffer_days' name='buffer_days' type='number' min='1' max='60' value='" + bufferDays + "'></div>";
  page += "<div class='full hint'>Po zapisaniu ustawien urzadzenie zrestartuje sie i odtworzy kolejke zgodnie z nowa liczba dni.</div>";
  page += "<div class='full'><button type='submit'>Zapisz ustawienia</button></div>";
  page += "</form></div>";

  page += "<script>";
  page += "function txt(id,v){document.getElementById(id).textContent=(v===null||v===undefined||v==='')?'-':v;}";
  page += "function fmtNum(v,u){return(v===null||v===undefined)?'-':String(v)+(u?(' '+u):'');}";
  page += "async function refreshStatus(){";
  page += "try{const r=await fetch('/status',{cache:'no-store'});const s=await r.json();";
  page += "txt('wifiState',s.wifi_connected?('polaczone '+s.wifi_ip):'brak polaczenia');";
  page += "txt('apState',s.ap_active?('aktywny '+s.ap_ssid+' '+s.ap_ip):'wylaczony');";
  page += "txt('meterState',s.meter_receiving?('odbiera, ostatni '+Math.round((s.last_packet_ms_ago||0)/1000)+' s temu'):'brak swiezych ramek');";
  page += "txt('queueState',String(s.queue_pending)+' rekordow');";
  page += "txt('rssiState',s.last_rssi_dbm===null?'-':String(s.last_rssi_dbm)+' dBm');";
  page += "txt('uploadState',s.last_http_code===null?'-':((s.last_upload_ok?'OK ':'BLAD ')+String(s.last_http_code)));";
  page += "txt('meterIdState',s.last_id||s.meter_number);";
  page += "txt('deviceTimeState',s.device_date_time);";
  page += "txt('powerState',fmtNum(s.current_power_consumption_kw,'kW'));";
  page += "txt('energyState',fmtNum(s.total_energy_consumption_kwh,'kWh'));";
  page += "txt('l1State',fmtNum(s.voltage_at_phase_1_v,'V'));";
  page += "txt('l2State',fmtNum(s.voltage_at_phase_2_v,'V'));";
  page += "txt('l3State',fmtNum(s.voltage_at_phase_3_v,'V'));";
  page += "txt('matchCountState',String(s.matched_count));";
  page += "txt('rawTelegram',s.last_raw_telegram||'Brak danych');";
  page += "}catch(e){txt('wifiState','blad odczytu statusu');}}";
  page += "refreshStatus();";
  page += "</script></div></body></html>";
  return page;
}

static String trimmedArg(const char* name) {
  String value = webServer.arg(name);
  value.trim();
  return value;
}

static void sendHtmlMessage(int statusCode, const String& message) {
  String html;
  html.reserve(512);
  html += "<!doctype html><html lang='pl'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Licznik SX1276</title></head><body style='font-family:Verdana,sans-serif;padding:24px;background:#020617;color:#e2e8f0'>";
  html += "<p>";
  html += htmlEscape(message);
  html += "</p><p><a href='/' style='color:#7dd3fc'>Powrot</a></p></body></html>";
  webServer.send(statusCode, "text/html; charset=utf-8", html);
}

static void handleRoot() {
  markWebActivity();
  webServer.send(200, "text/html; charset=utf-8", buildRootPage());
}

static void handleStatus() {
  markWebActivity();
  webServer.send(200, "application/json", buildStatusJson());
}

static void scheduleRestartAfter(uint32_t delayMs) {
  restartRequested = true;
  restartAtMillis = millis() + delayMs;
}

static void scheduleRestart() {
  scheduleRestartAfter(1500UL);
}

static void printHealthEvent(const char* status, uint32_t staleMs) {
  Serial.print("{\"status\":\"");
  Serial.print(status);
  Serial.print("\",\"stale_ms\":");
  Serial.print(staleMs);
  Serial.println("}");
}

static void maintainUploadHealth() {
  if (restartRequested) {
    return;
  }
  if (lastMatchedTelegramMillis == 0U) {
    return;
  }

  const uint32_t now = millis();
  if ((now - lastMatchedTelegramMillis) > STATUS_STALE_MS) {
    uploadRestartArmed = false;
    uploadRestartEligibleAtMillis = 0U;
    return;
  }

  const uint32_t uploadReferenceMillis = (lastUploadOkMillis != 0U) ? lastUploadOkMillis : setupCompletedMillis;
  if (uploadReferenceMillis == 0U) {
    return;
  }

  const uint32_t uploadStaleMs = now - uploadReferenceMillis;
  if (uploadStaleMs < UPLOAD_STUCK_RECOVERY_MS) {
    uploadRestartArmed = false;
    uploadRestartEligibleAtMillis = 0U;
    return;
  }

  if (lastUploadRecoveryMillis == 0U) {
    forceWifiRecovery();
    const bool pathOk = probeUploadPath();
    if (pathOk) {
      lastUploadRecoveryMillis = millis();
    }
    uploadRestartEligibleAtMillis = lastUploadRecoveryMillis + UPLOAD_STUCK_RESTART_MS;
    return;
  }

  if (lastUploadOkMillis > lastUploadRecoveryMillis) {
    uploadRestartArmed = false;
    uploadRestartEligibleAtMillis = 0U;
    return;
  }

  if ((uploadRestartEligibleAtMillis != 0U) && ((now - uploadRestartEligibleAtMillis) < 0x80000000UL) && (static_cast<int32_t>(now - uploadRestartEligibleAtMillis) >= 0)) {
    uploadRestartArmed = true;
  }
}

static void handleSave() {
  markWebActivity();
  RuntimeConfig nextConfig = runtimeConfig;

  const String newSsid = trimmedArg("ssid");
  const String newWifiPass = webServer.arg("wifi_pass");
  const String newStation = trimmedArg("station_number");
  const String newMeterNumber = trimmedArg("meter_number");
  String newMeterKey = trimmedArg("meter_key");
  newMeterKey.toUpperCase();
  const String newPostUrl = trimmedArg("post_url");
  const String newBufferDays = trimmedArg("buffer_days");

  if (!newMeterNumber.isEmpty() && !isDigitsOnly(newMeterNumber.c_str(), 8U)) {
    sendHtmlMessage(400, "Numer licznika musi miec dokladnie 8 cyfr.");
    return;
  }
  if (!newMeterKey.isEmpty() && !parseMeterKeyHex(newMeterKey.c_str(), nextConfig.meterKey)) {
    sendHtmlMessage(400, "METER_KEY musi miec 32 znaki HEX.");
    return;
  }

  const int stationValue = newStation.toInt();
  const int bufferDaysValue = newBufferDays.toInt();
  if ((stationValue < 0) || (stationValue > 255)) {
    sendHtmlMessage(400, "Station number musi byc w zakresie 0-255.");
    return;
  }
  if ((bufferDaysValue < MIN_BUFFER_DAYS) || (bufferDaysValue > MAX_BUFFER_DAYS)) {
    sendHtmlMessage(400, "Liczba dni bufora musi byc w zakresie 1-60.");
    return;
  }

  const bool ssidChanged = newSsid != String(runtimeConfig.wifiSsid);
  copyString(nextConfig.wifiSsid, sizeof(nextConfig.wifiSsid), newSsid.c_str());
  if (newSsid.isEmpty()) {
    nextConfig.wifiPass[0] = '\0';
  } else if (ssidChanged || !newWifiPass.isEmpty()) {
    copyString(nextConfig.wifiPass, sizeof(nextConfig.wifiPass), newWifiPass.c_str());
  }

  copyString(nextConfig.postUrl, sizeof(nextConfig.postUrl), newPostUrl.c_str());
  copyString(nextConfig.meterNumber, sizeof(nextConfig.meterNumber), newMeterNumber.c_str());
  nextConfig.stationNumber = static_cast<uint8_t>(stationValue);
  nextConfig.bufferDays = static_cast<uint16_t>(bufferDaysValue);

  if (!newMeterKey.isEmpty()) {
    copyString(nextConfig.meterKeyHex, sizeof(nextConfig.meterKeyHex), newMeterKey.c_str());
  }

  RuntimeConfig previousConfig = runtimeConfig;
  runtimeConfig = nextConfig;
  refreshRuntimeDerivedState();
  if (!saveRuntimeConfig(runtimeConfig)) {
    runtimeConfig = previousConfig;
    refreshRuntimeDerivedState();
    sendHtmlMessage(500, "Nie udalo sie zapisac konfiguracji.");
    return;
  }

  String html;
  html.reserve(512);
  html += "<!doctype html><html lang='pl'><head><meta charset='utf-8'>";
  html += "<meta http-equiv='refresh' content='4;url=/'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Restart</title></head>";
  html += "<body style='font-family:Verdana,sans-serif;padding:24px'><p>Ustawienia zapisane. Urzadzenie restartuje sie.</p></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
  scheduleRestart();
}

static void handleNotFound() {
  markWebActivity();
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

static void startWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/status", HTTP_GET, handleStatus);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
}

#if defined(ESP8266) || defined(ESP32)
ICACHE_RAM_ATTR
#endif
static void dio1FallingAction() {
  fifoNotEmptyIrq = true;
}

static void appendHexByte(char* buffer, size_t bufferSize, size_t& position, uint8_t value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  if ((position + 2U) >= bufferSize) {
    return;
  }
  buffer[position++] = HEX_DIGITS[(value >> 4) & 0x0F];
  buffer[position++] = HEX_DIGITS[value & 0x0F];
  buffer[position] = '\0';
}

static void saveRawTelegramHex(const uint8_t* data, size_t len) {
  size_t position = 0U;
  lastRawTelegramHex[0] = '\0';
  const size_t cappedLen = (len > MAX_PACKET_SIZE) ? MAX_PACKET_SIZE : len;
  for (size_t i = 0; i < cappedLen; i++) {
    appendHexByte(lastRawTelegramHex, sizeof(lastRawTelegramHex), position, data[i]);
  }
}

static bool prefixMatches(const uint8_t* data, size_t len, const uint8_t* prefix, size_t prefixLen) {
  if (len < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; i++) {
    if (data[i] != prefix[i]) {
      return false;
    }
  }

  return true;
}

static size_t encodedSize(size_t decodedSize) {
  return (3U * decodedSize + 1U) / 2U;
}

static bool decode3of6(const uint8_t* input, size_t inputLen, uint8_t* output, size_t& outputLen) {
  const size_t segments = (inputLen * 8U) / 6U;
  outputLen = 0;

  for (size_t i = 0; i < segments; i++) {
    const size_t bitIdx = i * 6U;
    const size_t byteIdx = bitIdx / 8U;
    const size_t bitOffset = bitIdx % 8U;

    uint8_t code = static_cast<uint8_t>(input[byteIdx] << bitOffset);
    if ((bitOffset > 0U) && ((byteIdx + 1U) < inputLen)) {
      code = static_cast<uint8_t>(code | (input[byteIdx + 1U] >> (8U - bitOffset)));
    }
    code = static_cast<uint8_t>(code >> 2);

    const uint8_t nibble = DECODE3OF6_TABLE[code & 0x3F];
    if (nibble == 0xFF) {
      return false;
    }

    if ((i % 2U) == 0U) {
      output[outputLen] = static_cast<uint8_t>(nibble << 4);
      outputLen++;
    } else {
      output[outputLen - 1U] = static_cast<uint8_t>(output[outputLen - 1U] | nibble);
    }
  }

  return true;
}

static bool decodeInitialLField(const uint8_t* input, size_t inputLen, uint8_t& lField) {
  uint8_t tmp[8] = {0};
  size_t decodedLen = 0;

  if (!decode3of6(input, inputLen, tmp, decodedLen) || (decodedLen == 0U)) {
    return false;
  }

  lField = tmp[0];
  return true;
}

static size_t expectedEncodedFrameSize(uint8_t lField) {
  const size_t nrBlocks = (lField < 26U) ? 2U : (((static_cast<size_t>(lField) - 26U) / 16U) + 3U);
  const size_t nrBytes = static_cast<size_t>(lField) + 1U + (2U * nrBlocks);
  return encodedSize(nrBytes);
}

static int32_t readReg(uint8_t reg) {
  return radioModule.SPIgetRegValue(reg);
}

static void writeReg(uint8_t reg, uint8_t value) {
  radioModule.SPIwriteRegister(reg, value);
}

static void writeBurst(uint8_t reg, const uint8_t* data, size_t len) {
  radioModule.SPIwriteRegisterBurst(reg, data, len);
}

static bool fifoHasData() {
  return digitalRead(PIN_DIO1) == LOW;
}

static bool waitForFifoData(uint32_t timeoutUs) {
  const uint32_t start = static_cast<uint32_t>(esp_timer_get_time());
  while (!fifoHasData()) {
    if ((static_cast<uint32_t>(esp_timer_get_time()) - start) >= timeoutUs) {
      return false;
    }
  }
  return true;
}

static size_t readFrameBytes(uint8_t* buffer, size_t length, size_t offset) {
  if (!waitForFifoData(12000U)) {
    return 0;
  }

  const uint32_t t0 = static_cast<uint32_t>(esp_timer_get_time());
  size_t count = 0;

  while (count < length) {
    size_t batch = length - count;
    if (batch > FIFO_BATCH) {
      batch = FIFO_BATCH;
    }

    const uint32_t target = t0 + static_cast<uint32_t>((count + batch - 1U) * BYTE_TIME_US);
    while (static_cast<int32_t>(target - static_cast<uint32_t>(esp_timer_get_time())) > 0) {
    }

    radioModule.SPIreadRegisterBurst(RADIOLIB_SX127X_REG_FIFO, batch, buffer + count);
    count += batch;
  }

  if ((count > 0U) && (offset == INITIAL_BYTES) && (signalRssiRaw == 0U)) {
    signalRssiRaw = static_cast<uint8_t>(readReg(RADIOLIB_SX127X_REG_RSSI_VALUE_FSK));
  }

  return count;
}

static void restartRx() {
  writeReg(RADIOLIB_SX127X_REG_OP_MODE, 0b001);
  delay(5);
  writeReg(RADIOLIB_SX127X_REG_IRQ_FLAGS_2, static_cast<uint8_t>(1U << 4));
  writeReg(RADIOLIB_SX127X_REG_OP_MODE, 0b101);
  delay(5);
  fifoNotEmptyIrq = false;
  signalRssiRaw = 0;
}

static int8_t getLastSignalRssiDbm() {
  const uint8_t rssiNow = static_cast<uint8_t>(readReg(RADIOLIB_SX127X_REG_RSSI_VALUE_FSK));
  const uint8_t rssi = (signalRssiRaw != 0U) ? signalRssiRaw : rssiNow;

  const uint8_t irq2 = static_cast<uint8_t>(readReg(RADIOLIB_SX127X_REG_IRQ_FLAGS_2));
  if (irq2 & (1U << 4)) {
    writeReg(RADIOLIB_SX127X_REG_IRQ_FLAGS_2, static_cast<uint8_t>(1U << 4));
  }

  signalRssiRaw = 0;
  return static_cast<int8_t>(-static_cast<int16_t>(rssi) / 2);
}

static void formatMeterSerial(const uint8_t* decoded, char* out) {
  for (size_t i = 0; i < 4U; i++) {
    const uint8_t value = decoded[7U - i];
    out[i * 2U] = static_cast<char>('0' + ((value >> 4) & 0x0F));
    out[i * 2U + 1U] = static_cast<char>('0' + (value & 0x0F));
  }
  out[8] = '\0';
}

static bool trimFrameFormatA(const uint8_t* input, size_t inputLen, uint8_t* output, size_t& outputLen) {
  if (inputLen < 12U) {
    return false;
  }

  outputLen = 0;
  memcpy(output, input, 10U);
  outputLen = 10U;

  size_t pos = 12U;
  while ((pos + 18U) <= inputLen) {
    if ((outputLen + 16U) > MAX_TRIMMED_SIZE) {
      return false;
    }
    memcpy(output + outputLen, input + pos, 16U);
    outputLen += 16U;
    pos += 18U;
  }

  if (pos < (inputLen - 2U)) {
    const size_t tailLen = (inputLen - 2U) - pos;
    if ((outputLen + tailLen) > MAX_TRIMMED_SIZE) {
      return false;
    }
    memcpy(output + outputLen, input + pos, tailLen);
    outputLen += tailLen;
  }

  if (outputLen == 0U) {
    return false;
  }

  output[0] = static_cast<uint8_t>(outputLen - 1U);
  return true;
}

static bool decryptTplAesCbcIv(const uint8_t* frame, size_t frameLen, uint8_t* plaintext, size_t& plaintextLen) {
  if (frameLen < 15U) {
    return false;
  }

  const uint16_t cfg = static_cast<uint16_t>(frame[13] | (static_cast<uint16_t>(frame[14]) << 8));
  const uint8_t securityMode = static_cast<uint8_t>((cfg >> 8) & 0x1FU);
  if (securityMode != 0x05U) {
    return false;
  }

  const size_t encryptedBlocks = static_cast<size_t>((cfg >> 4) & 0x0FU);
  plaintextLen = encryptedBlocks * 16U;
  if ((plaintextLen == 0U) || (plaintextLen > MAX_DECRYPTED_SIZE) || ((15U + plaintextLen) > frameLen)) {
    return false;
  }

  uint8_t iv[16] = {
    frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9],
    frame[11], frame[11], frame[11], frame[11], frame[11], frame[11], frame[11], frame[11]
  };

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  const int keyState = mbedtls_aes_setkey_dec(&aes, runtimeConfig.meterKey, 128);
  if (keyState != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  const int decryptState = mbedtls_aes_crypt_cbc(
    &aes,
    MBEDTLS_AES_DECRYPT,
    plaintextLen,
    iv,
    frame + 15U,
    plaintext
  );
  mbedtls_aes_free(&aes);
  if (decryptState != 0) {
    return false;
  }

  return (plaintextLen >= 2U) && (plaintext[0] == 0x2FU) && (plaintext[1] == 0x2FU);
}

static uint64_t decodeBcdUnsigned(const uint8_t* data, size_t len) {
  uint64_t value = 0;
  for (size_t i = 0; i < len; i++) {
    const uint8_t byte = data[len - 1U - i];
    value = (value * 100U) + static_cast<uint64_t>(((byte >> 4) & 0x0FU) * 10U + (byte & 0x0FU));
  }
  return value;
}

static double decodeScaledBcd(const uint8_t* data, size_t len, double divisor) {
  return static_cast<double>(decodeBcdUnsigned(data, len)) / divisor;
}

static bool formatDeviceDateTime(const uint8_t* data, char* out, size_t outSize) {
  const int sec = data[0] & 0x3F;
  const int min = data[1] & 0x3F;
  const int hour = data[2] & 0x1F;
  const int day = data[3] & 0x1F;
  const int year1 = (data[3] >> 5) & 0x07;
  const int month = data[4] & 0x0F;
  const int year2 = (data[4] & 0xF0) >> 1;
  const int year = 2000 + year1 + year2;

  if ((sec > 59) || (min > 59) || (hour > 23) || (day < 1) || (day > 31) || (month < 1) || (month > 12)) {
    return false;
  }

  snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, min, sec);
  return true;
}

static bool matchesAt(const uint8_t* data, size_t dataLen, size_t pos, const uint8_t* key, size_t keyLen) {
  if ((pos + keyLen) > dataLen) {
    return false;
  }

  for (size_t i = 0; i < keyLen; i++) {
    if (data[pos + i] != key[i]) {
      return false;
    }
  }

  return true;
}

static void parseAmiplusPayload(const uint8_t* payload, size_t payloadLen, ParsedTelegram& parsed) {
  static constexpr uint8_t KEY_DATE_TIME[] = {0x06, 0x6D};
  static constexpr uint8_t KEY_TOTAL_ENERGY[] = {0x0E, 0x03};
  static constexpr uint8_t KEY_CURRENT_POWER[] = {0x0B, 0x2B};
  static constexpr uint8_t KEY_VOLTAGE_L1[] = {0x0A, 0xFD, 0xC8, 0xFC, 0x01};
  static constexpr uint8_t KEY_VOLTAGE_L2[] = {0x0A, 0xFD, 0xC8, 0xFC, 0x02};
  static constexpr uint8_t KEY_VOLTAGE_L3[] = {0x0A, 0xFD, 0xC8, 0xFC, 0x03};

  for (size_t i = 0; i < payloadLen; i++) {
    if (payload[i] == 0x2F) {
      continue;
    }

    if (!parsed.hasDeviceDateTime && matchesAt(payload, payloadLen, i, KEY_DATE_TIME, sizeof(KEY_DATE_TIME)) && ((i + 8U) <= payloadLen)) {
      parsed.hasDeviceDateTime = formatDeviceDateTime(payload + i + 2U, parsed.deviceDateTime, sizeof(parsed.deviceDateTime));
      i += 7U;
      continue;
    }

    if (!parsed.hasTotalEnergyConsumptionKwh && matchesAt(payload, payloadLen, i, KEY_TOTAL_ENERGY, sizeof(KEY_TOTAL_ENERGY)) && ((i + 8U) <= payloadLen)) {
      parsed.totalEnergyConsumptionKwh = decodeScaledBcd(payload + i + 2U, 6U, 1000.0);
      parsed.hasTotalEnergyConsumptionKwh = true;
      i += 7U;
      continue;
    }

    if (!parsed.hasCurrentPowerConsumptionKw && matchesAt(payload, payloadLen, i, KEY_CURRENT_POWER, sizeof(KEY_CURRENT_POWER)) && ((i + 5U) <= payloadLen)) {
      parsed.currentPowerConsumptionKw = decodeScaledBcd(payload + i + 2U, 3U, 1000.0);
      parsed.hasCurrentPowerConsumptionKw = true;
      i += 4U;
      continue;
    }

    if (!parsed.hasVoltagePhase1V && matchesAt(payload, payloadLen, i, KEY_VOLTAGE_L1, sizeof(KEY_VOLTAGE_L1)) && ((i + 7U) <= payloadLen)) {
      parsed.voltagePhase1V = decodeScaledBcd(payload + i + 5U, 2U, 10.0);
      parsed.hasVoltagePhase1V = true;
      i += 6U;
      continue;
    }

    if (!parsed.hasVoltagePhase2V && matchesAt(payload, payloadLen, i, KEY_VOLTAGE_L2, sizeof(KEY_VOLTAGE_L2)) && ((i + 7U) <= payloadLen)) {
      parsed.voltagePhase2V = decodeScaledBcd(payload + i + 5U, 2U, 10.0);
      parsed.hasVoltagePhase2V = true;
      i += 6U;
      continue;
    }

    if (!parsed.hasVoltagePhase3V && matchesAt(payload, payloadLen, i, KEY_VOLTAGE_L3, sizeof(KEY_VOLTAGE_L3)) && ((i + 7U) <= payloadLen)) {
      parsed.voltagePhase3V = decodeScaledBcd(payload + i + 5U, 2U, 10.0);
      parsed.hasVoltagePhase3V = true;
      i += 6U;
      continue;
    }
  }
}

static bool canUploadParsedTelegram(const ParsedTelegram& parsed) {
  return parsed.hasCurrentPowerConsumptionKw &&
         parsed.hasTotalEnergyConsumptionKwh &&
         parsed.hasVoltagePhase1V &&
         parsed.hasVoltagePhase2V &&
         parsed.hasVoltagePhase3V;
}

static bool parseDateTimeString(const char* value, QueueRecord& record) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  const int parsedCount = sscanf(value, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  if (parsedCount != 6) {
    return false;
  }
  if ((year < 2000) || (year > 2099) || (month < 1) || (month > 12) || (day < 1) || (day > 31) ||
      (hour < 0) || (hour > 23) || (minute < 0) || (minute > 59) || (second < 0) || (second > 59)) {
    return false;
  }

  record.year = static_cast<uint16_t>(year);
  record.month = static_cast<uint8_t>(month);
  record.day = static_cast<uint8_t>(day);
  record.hour = static_cast<uint8_t>(hour);
  record.minute = static_cast<uint8_t>(minute);
  record.second = static_cast<uint8_t>(second);
  record.flags |= 0x01U;
  return true;
}

static void formatRecordDateTime(const QueueRecord& record, char* out, size_t outSize) {
  if ((record.flags & 0x01U) == 0U) {
    out[0] = '\0';
    return;
  }

  snprintf(
    out,
    outSize,
    "%04u-%02u-%02u %02u:%02u:%02u",
    static_cast<unsigned>(record.year),
    static_cast<unsigned>(record.month),
    static_cast<unsigned>(record.day),
    static_cast<unsigned>(record.hour),
    static_cast<unsigned>(record.minute),
    static_cast<unsigned>(record.second)
  );
}

static bool buildQueueRecord(const ParsedTelegram& parsed, QueueRecord& record) {
  if (!canUploadParsedTelegram(parsed)) {
    return false;
  }

  memset(&record, 0, sizeof(record));
  record.wsys = static_cast<int32_t>(llround(parsed.currentPowerConsumptionKw * 10000.0));
  record.kwhpTot = static_cast<int64_t>(llround(parsed.totalEnergyConsumptionKwh * 1000.0));
  record.vl1n = static_cast<uint16_t>(lround(parsed.voltagePhase1V * 10.0));
  record.vl2n = static_cast<uint16_t>(lround(parsed.voltagePhase2V * 10.0));
  record.vl3n = static_cast<uint16_t>(lround(parsed.voltagePhase3V * 10.0));

  if (parsed.hasDeviceDateTime) {
    parseDateTimeString(parsed.deviceDateTime, record);
  }

  return true;
}

static UploadResult uploadQueueRecord(const QueueRecord& record) {
  UploadResult result = {false, false, 0};

  if (runtimeConfig.postUrl[0] == '\0') {
    result.attempted = true;
    result.httpCode = -3;
    return result;
  }
  if (!ensureWifiConnected()) {
    result.attempted = true;
    result.httpCode = -1;
    return result;
  }

  char dateTime[20] = {0};
  formatRecordDateTime(record, dateTime, sizeof(dateTime));

  char body[256] = {0};
  if ((record.flags & 0x01U) != 0U) {
    snprintf(
      body,
      sizeof(body),
      "{\"station\":%u,\"datetime\":\"%s\",\"WSYS\":%ld,\"KWHPTOT\":%lld,\"VL1N\":%ld,\"VL2N\":%ld,\"VL3N\":%ld}",
      static_cast<unsigned>(runtimeConfig.stationNumber),
      dateTime,
      static_cast<long>(record.wsys),
      static_cast<long long>(record.kwhpTot),
      static_cast<long>(record.vl1n),
      static_cast<long>(record.vl2n),
      static_cast<long>(record.vl3n)
    );
  } else {
    snprintf(
      body,
      sizeof(body),
      "{\"station\":%u,\"WSYS\":%ld,\"KWHPTOT\":%lld,\"VL1N\":%ld,\"VL2N\":%ld,\"VL3N\":%ld}",
      static_cast<unsigned>(runtimeConfig.stationNumber),
      static_cast<long>(record.wsys),
      static_cast<long long>(record.kwhpTot),
      static_cast<long>(record.vl1n),
      static_cast<long>(record.vl2n),
      static_cast<long>(record.vl3n)
    );
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, runtimeConfig.postUrl)) {
    result.attempted = true;
    result.httpCode = -2;
    return result;
  }

  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");

  result.attempted = true;
  result.httpCode = http.POST(reinterpret_cast<uint8_t*>(body), strlen(body));
  result.ok = (result.httpCode >= 200) && (result.httpCode < 300);
  http.end();

  return result;
}

static bool processUploadQueue(bool forceNow, UploadResult* lastResult) {
  if (lastResult != nullptr) {
    *lastResult = {false, false, 0};
  }

  if (!storageOk || (queuePendingCount() == 0U)) {
    return false;
  }

  const uint32_t now = millis();
  uint32_t retryIntervalMs =
    (lastUploadResultValid && lastUploadResult.ok) ? UPLOAD_RETRY_SUCCESS_MS : UPLOAD_RETRY_FAILURE_MS;
  if (isWebUiActive() && lastUploadResultValid && lastUploadResult.ok) {
    retryIntervalMs = UPLOAD_RETRY_SUCCESS_WHILE_WEB_MS;
  }
  if (!forceNow && ((now - lastUploadAttemptMillis) < retryIntervalMs)) {
    return false;
  }

  if (!forceNow && isReceiveGuardWindowActive()) {
    return false;
  }

  QueueRecord record = {};
  if (!peekOldestQueueRecord(record)) {
    dropOldestQueueRecord();
    return false;
  }

  lastUploadAttemptMillis = now;
  const UploadResult result = uploadQueueRecord(record);
  lastUploadResult = result;
  lastUploadResultValid = true;
  if (lastResult != nullptr) {
    *lastResult = result;
  }
  if (result.ok) {
    lastUploadOkMillis = now;
    lastUploadRecoveryMillis = 0U;
    uploadRestartEligibleAtMillis = 0U;
    uploadRestartArmed = false;
    dropOldestQueueRecord();
    return true;
  }

  return false;
}

static void printNumberOrNull(bool hasValue, double value, uint8_t decimals) {
  if (!hasValue) {
    Serial.print("null");
    return;
  }

  Serial.print(value, decimals);
}

static void printStringOrNull(bool hasValue, const char* value) {
  if (!hasValue) {
    Serial.print("null");
    return;
  }

  Serial.print("\"");
  Serial.print(value);
  Serial.print("\"");
}

static void printMatchedTelegramJson(
  const ParsedTelegram& parsed,
  int8_t rssiDbm,
  bool queued,
  const UploadResult& uploadResult
) {
  Serial.print("{\"id\":\"");
  Serial.print(parsed.id);
  Serial.print("\"");
  Serial.print(",\"rssi_dbm\":");
  Serial.print(rssiDbm);
  Serial.print(",\"device_date_time\":");
  printStringOrNull(parsed.hasDeviceDateTime, parsed.deviceDateTime);
  Serial.print(",\"current_power_consumption_kw\":");
  printNumberOrNull(parsed.hasCurrentPowerConsumptionKw, parsed.currentPowerConsumptionKw, 3);
  Serial.print(",\"total_energy_consumption_kwh\":");
  printNumberOrNull(parsed.hasTotalEnergyConsumptionKwh, parsed.totalEnergyConsumptionKwh, 3);
  Serial.print(",\"voltage_at_phase_1_v\":");
  printNumberOrNull(parsed.hasVoltagePhase1V, parsed.voltagePhase1V, 1);
  Serial.print(",\"voltage_at_phase_2_v\":");
  printNumberOrNull(parsed.hasVoltagePhase2V, parsed.voltagePhase2V, 1);
  Serial.print(",\"voltage_at_phase_3_v\":");
  printNumberOrNull(parsed.hasVoltagePhase3V, parsed.voltagePhase3V, 1);
  Serial.print(",\"queue_pending\":");
  Serial.print(queuePendingCount());
  Serial.print(",\"queued\":");
  Serial.print(queued ? "true" : "false");
  Serial.print(",\"upload_attempted\":");
  Serial.print(uploadResult.attempted ? "true" : "false");
  Serial.print(",\"upload_ok\":");
  Serial.print(uploadResult.ok ? "true" : "false");
  Serial.print(",\"http_code\":");
  Serial.print(uploadResult.httpCode);
  Serial.print(",\"storage_ok\":");
  Serial.print(storageOk ? "true" : "false");
  Serial.print(",\"raw_telegram\":\"");
  Serial.print(lastRawTelegramHex);
  Serial.println("\"}");
}

static bool decodeAndParseTelegram(const uint8_t* rawTelegram, size_t rawLen, ParsedTelegram& parsed) {
  memset(&parsed, 0, sizeof(parsed));
  formatMeterSerial(rawTelegram, parsed.id);

  size_t trimmedLen = 0;
  if (!trimFrameFormatA(rawTelegram, rawLen, trimmedPacket, trimmedLen)) {
    return false;
  }

  size_t decryptedLen = 0;
  if (!decryptTplAesCbcIv(trimmedPacket, trimmedLen, decryptedPayload, decryptedLen)) {
    return false;
  }

  parseAmiplusPayload(decryptedPayload, decryptedLen, parsed);
  return true;
}

static bool isTargetTelegram(const uint8_t* decoded, size_t decodedLen) {
  return prefixMatches(decoded, decodedLen, runtimeTargetPrefix, sizeof(runtimeTargetPrefix));
}

static bool initWmbusReceiver() {
  radioSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  pinMode(PIN_DIO1, INPUT);
  pinMode(PIN_DIO2, INPUT);

  const int16_t state = radio.beginFSK(
    RF_FREQ_MHZ,
    RF_BITRATE_KBPS,
    RF_FDEV_KHZ,
    RF_RX_BW_KHZ,
    10,
    32,
    false
  );
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("{\"status\":\"begin_fsk_failed\",\"code\":");
    Serial.print(state);
    Serial.println("}");
    return false;
  }

  radio.reset();
  delay(5);

  const int32_t revision = readReg(RADIOLIB_SX127X_REG_VERSION);
  if ((revision < 0x11) || (revision > 0x13)) {
    Serial.print("{\"status\":\"invalid_revision\",\"revision\":");
    Serial.print(revision);
    Serial.println("}");
    return false;
  }

  const uint32_t frequency = 868950000UL;
  const uint32_t frf = static_cast<uint32_t>((static_cast<uint64_t>(frequency) * (1ULL << 19)) / 32000000ULL);
  const uint8_t frfRegs[3] = {
    static_cast<uint8_t>((frf >> 16) & 0xFF),
    static_cast<uint8_t>((frf >> 8) & 0xFF),
    static_cast<uint8_t>(frf & 0xFF)
  };
  writeBurst(0x06, frfRegs, sizeof(frfRegs));

  const uint8_t rxBwRegs[2] = {0x09, 0x10};
  writeBurst(0x12, rxBwRegs, sizeof(rxBwRegs));
  writeReg(0x1A, static_cast<uint8_t>(1U << 4));

  const uint16_t freqDev = 50000U;
  const uint16_t frd = static_cast<uint16_t>((static_cast<uint64_t>(freqDev) * (1ULL << 19)) / 32000000ULL);
  const uint8_t fdevRegs[2] = {
    static_cast<uint8_t>((frd >> 8) & 0xFF),
    static_cast<uint8_t>(frd & 0xFF)
  };
  writeBurst(0x04, fdevRegs, sizeof(fdevRegs));

  const uint32_t bitrate = 100000UL;
  uint32_t br = (32000000UL << 4) / bitrate;
  writeReg(0x5D, static_cast<uint8_t>(br & 0x0F));
  br >>= 4;
  const uint8_t bitrateRegs[2] = {
    static_cast<uint8_t>((br >> 8) & 0xFF),
    static_cast<uint8_t>(br & 0xFF)
  };
  writeBurst(0x02, bitrateRegs, sizeof(bitrateRegs));

  const uint8_t preambleRegs[2] = {0x00, 0x04};
  writeBurst(0x25, preambleRegs, sizeof(preambleRegs));
  writeReg(0x1F, static_cast<uint8_t>((1U << 7) | (1U << 5) | 0x0A));
  writeReg(0x0D, static_cast<uint8_t>((1U << 4) | (1U << 3) | 0b110));
  writeReg(0x24, 0b111);

  const uint8_t syncRegs[3] = {
    static_cast<uint8_t>((1U << 5) | (1U << 4) | (2U - 1U)),
    0x54,
    0x3D
  };
  writeBurst(0x27, syncRegs, sizeof(syncRegs));

  writeReg(0x30, 0x00);
  writeReg(0x32, 0x00);
  writeReg(0x40, static_cast<uint8_t>(0b01 << 4));
  writeReg(0x0E, 0b111);

  attachInterrupt(digitalPinToInterrupt(PIN_DIO1), dio1FallingAction, FALLING);
  restartRx();

  return true;
}

static void recoverRadioReceiver() {
  detachInterrupt(digitalPinToInterrupt(PIN_DIO1));
  fifoNotEmptyIrq = false;
  signalRssiRaw = 0U;
  radioOk = initWmbusReceiver();
}

static void maintainReceiverHealth() {
  if (!radioOk || restartRequested) {
    return;
  }

  const uint32_t now = millis();
  const uint32_t referenceMillis = (lastMatchedTelegramMillis != 0U) ? lastMatchedTelegramMillis : setupCompletedMillis;
  if (referenceMillis == 0U) {
    return;
  }

  const uint32_t staleMs = now - referenceMillis;
  if ((staleMs >= RADIO_RECOVERY_STALE_MS) && ((now - lastRadioRecoveryMillis) >= RADIO_RECOVERY_COOLDOWN_MS)) {
    printHealthEvent("radio_recovery", staleMs);
    recoverRadioReceiver();
    lastRadioRecoveryMillis = now;
    return;
  }

  if (staleMs >= FULL_RESTART_STALE_MS) {
    printHealthEvent("full_restart", staleMs);
    scheduleRestart();
  }
}

static void tryReadPacket() {
  if (!fifoNotEmptyIrq) {
    return;
  }

  fifoNotEmptyIrq = false;
  memset(encodedPacket, 0, sizeof(encodedPacket));

  const size_t initialRead = readFrameBytes(encodedPacket, INITIAL_BYTES, 0);
  if (initialRead != INITIAL_BYTES) {
    restartRx();
    return;
  }

  uint8_t lField = 0;
  if (!decodeInitialLField(encodedPacket, INITIAL_BYTES, lField)) {
    restartRx();
    return;
  }

  const size_t expectedLen = expectedEncodedFrameSize(lField);
  if ((expectedLen <= INITIAL_BYTES) || (expectedLen > MAX_PACKET_SIZE)) {
    restartRx();
    return;
  }

  const size_t remaining = expectedLen - INITIAL_BYTES;
  const size_t restRead = readFrameBytes(encodedPacket + INITIAL_BYTES, remaining, INITIAL_BYTES);
  if (restRead != remaining) {
    restartRx();
    return;
  }

  size_t decodedLen = 0;
  const bool decodedOk = decode3of6(encodedPacket, expectedLen, decodedPacket, decodedLen);
  const int8_t rssiDbm = getLastSignalRssiDbm();
  restartRx();

  if (!decodedOk) {
    return;
  }

  if (!isTargetTelegram(decodedPacket, decodedLen)) {
    return;
  }

  lastMatchedTelegramMillis = millis();
  lastMatchedRssiDbm = rssiDbm;
  matchedTelegramCount++;
  saveRawTelegramHex(decodedPacket, decodedLen);

  ParsedTelegram parsed = {};
  const bool parsedOk = decodeAndParseTelegram(decodedPacket, decodedLen, parsed);
  lastTelegramDecodedOk = parsedOk;
  if (!parsedOk) {
    decryptFailureCount++;
    return;
  }

  decodedTelegramCount++;
  lastSuccessfulTelegramMillis = millis();
  lastParsedTelegram = parsed;

  const bool hadQueuedBacklog = storageOk && (queuePendingCount() > 0U);
  bool queued = false;
  UploadResult uploadResult = {false, false, 0};
  QueueRecord record = {};
  if (storageOk && buildQueueRecord(parsed, record)) {
    if (!hadQueuedBacklog) {
      uploadResult = uploadQueueRecord(record);
      if (!uploadResult.ok) {
        queued = enqueueQueueRecord(record);
        if (!queued) {
          storageOk = false;
        }
      } else {
        lastUploadOkMillis = millis();
        lastUploadRecoveryMillis = 0U;
        uploadRestartEligibleAtMillis = 0U;
        uploadRestartArmed = false;
      }
    } else {
      queued = enqueueQueueRecord(record);
      if (queued) {
        processUploadQueue(true, &uploadResult);
      } else {
        storageOk = false;
        uploadResult = uploadQueueRecord(record);
      }
    }
  }

  lastTelegramQueued = queued;
  lastUploadResult = uploadResult;
  lastUploadResultValid = true;

  if (hadQueuedBacklog) {
    blinkRedStatusLed();
  } else {
    blinkGreenStatusLed();
  }
  printMatchedTelegramJson(parsed, rssiDbm, queued, uploadResult);

  if (uploadRestartArmed && !restartRequested) {
    uploadRestartArmed = false;
    scheduleRestartAfter(RESTART_AFTER_PACKET_DELAY_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  statusPixel.begin();
  statusPixel.setBrightness(STATUS_LED_BRIGHTNESS);
  setStatusLedColor(0, 0, 0);

  loadRuntimeConfig();

  startConfigPortal();
  WiFi.setSleep(false);
  if (hasWifiCredentials()) {
    startStationConnection();
  } else {
    WiFi.mode(WIFI_AP);
  }

  storageOk = initStorage();
  radioOk = initWmbusReceiver();
  setupCompletedMillis = millis();
  startWebServer();
}

void loop() {
  webServer.handleClient();
  if (restartRequested && (static_cast<int32_t>(millis() - restartAtMillis) >= 0)) {
    ESP.restart();
  }

  const bool receiveGuardWindowActive = isReceiveGuardWindowActive();
  if (!receiveGuardWindowActive) {
    maintainWifiConnection();
    managePortalLifetime();
    maintainUploadHealth();
  }
  maintainReceiverHealth();

  if (!radioOk) {
    updateStatusLed();
    delay(20);
    return;
  }

  tryReadPacket();
  if (!receiveGuardWindowActive) {
    processUploadQueue(false, nullptr);
  }
  updateStatusLed();
}
