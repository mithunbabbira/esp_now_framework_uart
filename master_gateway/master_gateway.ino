/**>
 * ESP-NOW Master Gateway (Production)
 * ====================================
 * Transparent bridge between Raspberry Pi (UART) and ESP-NOW slaves.
 *
 * Pi  ←──UART──→  Master  ←──ESP-NOW──→  Slave(s)
 *
 * Protocol (Pi ↔ Master):
 *   Pi  receives:  RX:<SRC_MAC>:<HEX>\n       (data from any slave)
 *   Pi  receives:  HEARTBEAT\n                 (every 2 s, so Pi sees Master quickly)
 *   Pi  receives:  MAC:<MASTER_MAC>\n          (on boot + every 2 s)
 *   Pi  sends:     TX:<DST_MAC>:<HEX>\n       (data to a specific slave)
 *
 * Wiring (Pi ↔ ESP32):
 *   Pi GPIO 14 (TX)  →  ESP32 PI_RX_PIN (default 16)
 *   Pi GPIO 15 (RX)  ←  ESP32 PI_TX_PIN (default 17)
 *   Pi GND           ↔  ESP32 GND
 *
 * Notes:
 *   - The boot messages (POWERON_RESET, phy_comm gpio reserved, etc.)
 *     come from the ESP32 ROM/bootloader and cannot be suppressed.
 *     They are harmless; the Pi bridge ignores them.
 *   - Any slave that sets masterMAC to this Master's MAC can send/receive.
 *   - Peers are auto-registered on first TX command; no pre-pairing needed.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <HardwareSerial.h>
#include <string.h>

// ── Pi UART pins (ESP32 side) ──────────────────────────────
#define PI_RX_PIN  16   // ESP32 receives from Pi TX (GPIO 14)
#define PI_TX_PIN  17   // ESP32 transmits to Pi RX (GPIO 15)

// ── Timing ─────────────────────────────────────────────────
// Send HEARTBEAT every 2s so Pi sees Master within 2s of opening port (no restart needed)
#define HEARTBEAT_INTERVAL_MS  2000
#define SERIAL_INPUT_MAX       256
#define SERIAL_LINE_TIMEOUT_MS 5000

// ── UART to Pi ─────────────────────────────────────────────
HardwareSerial PiSerial(1);

// ── Serial input buffer ────────────────────────────────────
static char inputBuf[SERIAL_INPUT_MAX];
static uint16_t inputLen = 0;
static unsigned long lastLineActivity = 0;

// ════════════════════════════════════════════════════════════
//  Helpers
// ════════════════════════════════════════════════════════════

static void macToStr(const uint8_t *mac, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool strToMac(const char *str, uint8_t *mac) {
  unsigned int v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x",
             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
  return true;
}

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

static int hexToBytes(const char *hex, uint8_t *out, int maxLen) {
  int len = strlen(hex);
  int n = 0;
  for (int i = 0; i + 1 < len && n < maxLen; i += 2) {
    out[n++] = (hexNibble(hex[i]) << 4) | hexNibble(hex[i + 1]);
  }
  return n;
}

// ════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ════════════════════════════════════════════════════════════

static void onRecv(const esp_now_recv_info_t *info,
                   const uint8_t *data, int len) {
  char mac[18];
  macToStr(info->src_addr, mac);

  // Send to Pi: RX:<MAC>:<HEX>\n
  PiSerial.print("RX:");
  PiSerial.print(mac);
  PiSerial.print(':');
  for (int i = 0; i < len; i++) {
    if (data[i] < 0x10) PiSerial.print('0');
    PiSerial.print(data[i], HEX);
  }
  PiSerial.println();
}

static void onSent(const wifi_tx_info_t * /* info */,
                   esp_now_send_status_t status) {
  PiSerial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "ERR:Send");
}

// ════════════════════════════════════════════════════════════
//  Process TX command from Pi
// ════════════════════════════════════════════════════════════

static void processCommand(const char *cmd) {
  // PING — Pi asks "are you alive?" → respond immediately
  if (strcmp(cmd, "PING") == 0) {
    sendHeartbeat();
    return;
  }

  // Expected: TX:XX:XX:XX:XX:XX:XX:HEXDATA
  if (strncmp(cmd, "TX:", 3) != 0) return;

  // Find the 6th colon (end of MAC)
  const char *p = cmd + 3;
  int colonCount = 0;
  const char *macStart = p;
  const char *hexStart = NULL;
  for (; *p; p++) {
    if (*p == ':') {
      colonCount++;
      if (colonCount == 5) {
        // Next 2 chars + colon = end of MAC
        // MAC is from macStart to p+3
        if (*(p + 1) && *(p + 2) && *(p + 3) == ':') {
          hexStart = p + 4;
        } else if (*(p + 1) && *(p + 2) && *(p + 3) == '\0') {
          // No payload
          hexStart = p + 3;
        }
        break;
      }
    }
  }

  if (colonCount < 5 || !hexStart) {
    PiSerial.println("ERR:Format");
    return;
  }

  // Extract MAC string (17 chars: XX:XX:XX:XX:XX:XX)
  char macStr[18] = {0};
  int macLen = (int)(hexStart - macStart);
  if (hexStart > macStart && *(hexStart - 1) == ':') macLen--;
  if (macLen != 17) {
    PiSerial.println("ERR:MAC");
    return;
  }
  memcpy(macStr, macStart, 17);

  uint8_t peer[6];
  if (!strToMac(macStr, peer)) {
    PiSerial.println("ERR:MAC");
    return;
  }

  // Auto-register peer if needed
  if (!esp_now_is_peer_exist(peer)) {
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, peer, 6);
    pi.channel = 0;
    pi.encrypt = false;
    if (esp_now_add_peer(&pi) != ESP_OK) {
      PiSerial.println("ERR:Peer");
      return;
    }
  }

  // Convert hex payload to bytes
  uint8_t buf[250];
  int n = hexToBytes(hexStart, buf, sizeof(buf));

  if (n > 0) {
    esp_now_send(peer, buf, n);
    // onSent callback will print OK/ERR
  } else {
    // Empty payload is valid (e.g. ping)
    esp_now_send(peer, NULL, 0);
  }
}

// ════════════════════════════════════════════════════════════
//  Heartbeat: send MAC + alive signal to Pi
// ════════════════════════════════════════════════════════════

static void sendHeartbeat() {
  PiSerial.println("HEARTBEAT");
  PiSerial.print("MAC:");
  PiSerial.println(WiFi.macAddress());
}

// ════════════════════════════════════════════════════════════
//  Setup
// ════════════════════════════════════════════════════════════

void setup() {
  // USB serial for debug only (optional, view in Arduino Serial Monitor)
  Serial.begin(115200);

  // UART to Pi
  PiSerial.begin(115200, SERIAL_8N1, PI_RX_PIN, PI_TX_PIN);
  // Flush any stale bytes so first Pi command is read cleanly
  while (PiSerial.available()) PiSerial.read();

  // WiFi STA (no AP join — ESP-NOW only)
  WiFi.mode(WIFI_STA);

  // ESP-NOW init with retry
  bool ready = false;
  for (int i = 1; i <= 5; i++) {
    if (esp_now_init() == ESP_OK) {
      ready = true;
      break;
    }
    Serial.printf("ESP-NOW init failed (attempt %d/5)\n", i);
    PiSerial.println("ERR:ESP-NOW init retry");
    esp_now_deinit();
    delay(1000);
  }
  if (!ready) {
    PiSerial.println("ERR:ESP-NOW init failed");
    Serial.println("ERR:ESP-NOW init failed — rebooting");
    delay(1000);
    ESP.restart();
  }

  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  // Announce to Pi
  PiSerial.println("MASTER_READY");
  sendHeartbeat();

  Serial.println("Master Gateway ready");
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
}

// ════════════════════════════════════════════════════════════
//  Main loop
// ════════════════════════════════════════════════════════════

void loop() {
  // ── Heartbeat ──
  static unsigned long lastHB = 0;
  if (millis() - lastHB >= HEARTBEAT_INTERVAL_MS) {
    lastHB = millis();
    sendHeartbeat();
  }


  // ── Read commands from Pi ──
  while (PiSerial.available()) {
    char c = (char)PiSerial.read();
    lastLineActivity = millis();
    if (c == '\n') {
      inputBuf[inputLen] = '\0';
      if (inputLen > 0) processCommand(inputBuf);
      inputLen = 0;
    } else if (c != '\r' && inputLen < SERIAL_INPUT_MAX - 1) {
      inputBuf[inputLen++] = c;
    }
  }
  // Discard stale partial line (e.g. garbage from Pi boot) so PING can be read
  if (inputLen > 0 && (millis() - lastLineActivity > SERIAL_LINE_TIMEOUT_MS)) {
    inputLen = 0;
  }

  delay(1);  // yield
}
