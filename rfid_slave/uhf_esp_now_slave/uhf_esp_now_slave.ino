/**
 * UHF RFID Reader - ESP-NOW + UART (No WiFi SSID)
 * ================================================
 * Streams RFID EPC to Raspberry Pi via ESP-NOW -> Master Gateway -> UART.
 * Pi sends STOP/START via UART -> Master -> ESP-NOW. No MQTT, no WiFi login.
 *
 * - No SSID/password (ESP-NOW uses WiFi STA but does not connect to AP)
 * - Same UHF serial protocol as uhf.ino (Chafon-style frames)
 * - Packet to Master: [0x52, 0x46, rssi, epc_len, epc_bytes...] (RF magic + rssi + EPC)
 * - Command from Pi: 0x01 = STOP inventory, 0x02 = START inventory
 */

#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>

// ============== CONFIGURATION ==============
// Master Gateway MAC = the ESP32 connected to the Pi (UART or USB).
// Get it from the Pi: when you run main_controller.py, the Pi prints
//   ">>> Set slave's masterMAC to: XX:XX:XX:XX:XX:XX <<<"
// Use that value here (not another reader's MAC like 90:A4 if that's a different device).
uint8_t masterMAC[] = {0xC0, 0xCD, 0xD6, 0x85, 0x70, 0xCC};

// RFID UART pins (same as uhf.ino)
const int RX_PIN = 16;
const int TX_PIN = 17;
const int BAUD_RATE = 115200;
const int RX_BUFFER_SIZE = 1024;

// Timing
const unsigned long FRAME_TIMEOUT_MS = 200;
const unsigned long DUPLICATE_FILTER_MS = 500;
const unsigned long WATCHDOG_TIMEOUT_S = 30;

// Commands from Pi (1 byte)
const uint8_t CMD_STOP = 0x01;
const uint8_t CMD_START = 0x02;

// RFID packet magic (RF)
const uint8_t PKT_MAGIC[] = {0x52, 0x46};  // 'R','F'

// Slave beacon magic: sent periodically so Pi can discover this reader
// even when inventory is stopped. Format: [0x48, 0x42] = "HB" (heartbeat)
const uint8_t BEACON_MAGIC[] = {0x48, 0x42};
const unsigned long BEACON_INTERVAL_MS = 5000;

// ============== GLOBALS ==============
HardwareSerial uhf(2);
Preferences preferences;

uint8_t frameBuffer[256];
int frameIndex = 0;
unsigned long frameStartTime = 0;
bool frameInProgress = false;

char lastEPC[65] = "";
unsigned long lastTagTime = 0;
unsigned long totalTagsRead = 0;
unsigned long totalErrors = 0;
unsigned long consecutiveSendFails = 0;

bool readerStopped = false;
int rfPower = 26;

// ============== FORWARD DECLARATIONS ==============
void sendTagToMaster(const char *epcStr, int8_t rssi);
void sendBeacon();
void processSerialData();
bool validateAndProcessFrame();
bool processTagFrame();
void handleFrameTimeout();
void resetFrame();
void flushBuffer();
bool configureReader();
bool startContinuousInventory();
bool stopInventory();
void loadSettings();
void saveSettings();

// ============== ESP-NOW Callbacks ==============
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  if (status == ESP_NOW_SEND_SUCCESS) {
    consecutiveSendFails = 0;
  } else {
    totalErrors++;
    consecutiveSendFails++;
    if (consecutiveSendFails >= 50) {
      Serial.println("[WARN] 50 consecutive send failures — Master may be offline");
      consecutiveSendFails = 0;  // reset counter, keep trying
    }
  }
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 1)
    return;
  uint8_t cmd = data[0];
  if (cmd == CMD_STOP && !readerStopped) {
    stopInventory();
    readerStopped = true;
    saveSettings();
  } else if (cmd == CMD_START && readerStopped) {
    readerStopped = false;
    saveSettings();
    startContinuousInventory();
  }
}

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(500);

  // WiFi STA first so MAC is valid (needed for ESP-NOW and for printing)
  WiFi.mode(WIFI_STA);
  delay(200);

  Serial.println("\n=============================================");
  Serial.println("  UHF RFID Reader - ESP-NOW (No SSID)");
  Serial.print("  My MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("=============================================\n");

  loadSettings();

  uhf.setRxBufferSize(RX_BUFFER_SIZE);
  uhf.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(200);
  flushBuffer();

  // Disable task watchdog to avoid "IDLE1 did not reset" when loop is busy
  esp_task_wdt_deinit();

  // ESP-NOW init with retry (up to 5 attempts)
  bool espNowReady = false;
  for (int attempt = 1; attempt <= 5; attempt++) {
    if (esp_now_init() == ESP_OK) {
      espNowReady = true;
      break;
    }
    Serial.printf("[ERR] ESP-NOW init failed (attempt %d/5)\n", attempt);
    esp_now_deinit();
    delay(1000);
  }
  if (!espNowReady) {
    Serial.println("[ERR] ESP-NOW init failed after 5 attempts. Rebooting...");
    delay(1000);
    ESP.restart();
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Register Master peer with retry
  bool peerAdded = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, masterMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      peerAdded = true;
      break;
    }
    Serial.printf("[ERR] Failed to add Master peer (attempt %d/3)\n", attempt);
    delay(500);
  }
  if (!peerAdded) {
    Serial.println("[ERR] Could not add Master peer. Rebooting...");
    delay(1000);
    ESP.restart();
  }
  Serial.println("[OK] Master peer registered (no SSID needed)");

  for (int r = 0; r < 3; r++) {
    if (configureReader())
      break;
    delay(500);
  }

  if (!readerStopped) {
    for (int r = 0; r < 3; r++) {
      if (startContinuousInventory())
        break;
      delay(500);
    }
  }
  Serial.println("\n[READY] Streaming tags to Pi via ESP-NOW\n");
}

void loop() {
  // Send periodic beacon so Pi always knows we exist (even when stopped)
  static unsigned long lastBeacon = 0;
  if (millis() - lastBeacon >= BEACON_INTERVAL_MS) {
    lastBeacon = millis();
    sendBeacon();
  }

  if (!readerStopped) {
    processSerialData();
    if (frameInProgress && (millis() - frameStartTime > FRAME_TIMEOUT_MS)) {
      handleFrameTimeout();
    }
  }
  delay(1);  // yield so IDLE task can run
}

// ============== Send EPC to Master (ESP-NOW) ==============
void sendTagToMaster(const char *epcStr, int8_t rssi) {
  size_t epcLen = strlen(epcStr);
  if (epcLen > 64)
    epcLen = 64;
  // EPC string is hex (2 chars per byte)
  size_t numBytes = epcLen / 2;
  if (numBytes > 32)
    numBytes = 32;

  uint8_t pkt[2 + 1 + 1 + 32];
  pkt[0] = PKT_MAGIC[0];
  pkt[1] = PKT_MAGIC[1];
  pkt[2] = (uint8_t)rssi;
  pkt[3] = (uint8_t)numBytes;
  for (size_t i = 0; i < numBytes; i++) {
    char hex[3] = { epcStr[i * 2], epcStr[i * 2 + 1], '\0' };
    pkt[4 + i] = (uint8_t)strtol(hex, NULL, 16);
  }
  size_t pktLen = 4 + numBytes;
  esp_now_send(masterMAC, pkt, pktLen);
}

// ============== NVS ==============
void loadSettings() {
  preferences.begin("rfid", true);
  rfPower = preferences.getInt("power", 26);
  // Always start inventory on boot — do NOT persist readerStopped.
  // The Pi controls START/STOP at runtime; a power-cycle means "start fresh".
  readerStopped = false;
  preferences.end();
}

void saveSettings() {
  preferences.begin("rfid", false);
  preferences.putInt("power", rfPower);
  // We intentionally do NOT save readerStopped to NVS anymore.
  preferences.end();
}

void sendBeacon() {
  // Send a 2-byte "HB" beacon to Master so Pi can discover this reader's MAC
  esp_now_send(masterMAC, BEACON_MAGIC, sizeof(BEACON_MAGIC));
}

// ============== RFID reader commands ==============
bool configureReader() {
  uint16_t powerValue = rfPower * 100;
  uint8_t powerHigh = (powerValue >> 8) & 0xFF;
  uint8_t powerLow = powerValue & 0xFF;
  uint8_t checksum = 0x00 + 0xB6 + 0x00 + 0x02 + powerHigh + powerLow;
  uint8_t powerCmd[] = {0xBB, 0x00, 0xB6, 0x00, 0x02, powerHigh, powerLow, checksum, 0x7E};
  uhf.write(powerCmd, sizeof(powerCmd));
  delay(100);
  flushBuffer();
  return true;
}

bool startContinuousInventory() {
  uint8_t cmd[] = {0xBB, 0x00, 0x27, 0x00, 0x03, 0x22, 0xFF, 0xFF, 0x4A, 0x7E};
  uhf.write(cmd, sizeof(cmd));
  return true;
}

bool stopInventory() {
  uint8_t cmd[] = {0xBB, 0x00, 0x28, 0x00, 0x00, 0x28, 0x7E};
  uhf.write(cmd, sizeof(cmd));
  delay(50);
  flushBuffer();
  return true;
}

// ============== Frame processing (same as uhf.ino) ==============
void processSerialData() {
  int bytesRead = 0;
  while (uhf.available()) {
    if (++bytesRead > 64) {
      yield();
      bytesRead = 0;
    }
    uint8_t b = uhf.read();
    if (!frameInProgress) {
      if (b == 0xBB) {
        frameBuffer[0] = b;
        frameIndex = 1;
        frameInProgress = true;
        frameStartTime = millis();
      }
      continue;
    }
    if (frameIndex < (int)sizeof(frameBuffer)) {
      frameBuffer[frameIndex++] = b;
    } else {
      resetFrame();
      totalErrors++;
      continue;
    }
    if (b == 0x7E && frameIndex >= 7) {
      if (!validateAndProcessFrame())
        totalErrors++;
      resetFrame();
    }
  }
}

bool validateAndProcessFrame() {
  if (frameIndex < 7)
    return false;
  uint8_t calculatedChecksum = 0;
  for (int i = 1; i < frameIndex - 2; i++)
    calculatedChecksum += frameBuffer[i];
  if (calculatedChecksum != frameBuffer[frameIndex - 2])
    return false;

  uint8_t frameType = frameBuffer[1];
  uint8_t command = frameBuffer[2];
  if (frameType == 0x02 && command == 0x22)
    return processTagFrame();
  if (frameType == 0x02 && command == 0xFF) {
    startContinuousInventory();
    return true;
  }
  return true;
}

bool processTagFrame() {
  if (frameIndex < 15)
    return false;
  int8_t rssi = (int8_t)frameBuffer[5];
  uint16_t pc = (frameBuffer[6] << 8) | frameBuffer[7];
  int epcLen = ((pc >> 11) & 0x1F) * 2;
  if (epcLen <= 0 || epcLen > 62 || (8 + epcLen) > frameIndex - 2)
    return false;

  char epcStr[65] = "";
  for (int i = 0; i < epcLen && i < 32; i++)
    sprintf(epcStr + (i * 2), "%02X", frameBuffer[8 + i]);

  if (strcmp(epcStr, lastEPC) == 0 && (millis() - lastTagTime < DUPLICATE_FILTER_MS))
    return true;
  strcpy(lastEPC, epcStr);
  lastTagTime = millis();
  totalTagsRead++;

  Serial.printf("[TAG] %s | RSSI: %d | #%lu\n", epcStr, rssi, totalTagsRead);
  sendTagToMaster(epcStr, rssi);
  return true;
}

void handleFrameTimeout() {
  resetFrame();
  flushBuffer();
  totalErrors++;
}

void resetFrame() {
  frameIndex = 0;
  frameInProgress = false;
  frameStartTime = 0;
}

void flushBuffer() {
  while (uhf.available())
    uhf.read();
}
