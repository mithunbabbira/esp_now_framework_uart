#include <WiFi.h>
#include <esp_now.h>

// Global buffer for serial input
String inputBuffer = "";

// Function to convert MAC address array to String
String macToString(const uint8_t *macAddr) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", macAddr[0],
           macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
  return String(macStr);
}

// Function to convert String MAC to uint8_t array
void stringToMac(String macStr, uint8_t *macAddr) {
  int values[6];
  if (6 == sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1],
                  &values[2], &values[3], &values[4], &values[5])) {
    for (int i = 0; i < 6; ++i) {
      macAddr[i] = (uint8_t)values[i];
    }
  }
}

// Helper to convert hex char to byte
uint8_t hexCharToByte(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return 0;
}

// Helper to convert hex string to byte array
void hexToBytes(String hex, uint8_t *bytes, int maxLen) {
  for (int i = 0; i < hex.length() && (i / 2) < maxLen; i += 2) {
    bytes[i / 2] = (hexCharToByte(hex[i]) << 4) | hexCharToByte(hex[i + 1]);
  }
}

// Callback when data is received via ESP-NOW (ESP32 Core v3.0 compatible)
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData,
                int len) {
  // Format: "RX:<MAC>:<HEX_DATA>"
  String macStr = macToString(info->src_addr);

  Serial.print("RX:");
  Serial.print(macStr);
  Serial.print(":");

  // Convert ANY incoming payload to Hex (Transparent)
  for (int i = 0; i < len; i++) {
    if (incomingData[i] < 16)
      Serial.print("0");
    Serial.print(incomingData[i], HEX);
  }
  Serial.println();
}

// Callback when data is sent
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("OK:Sent");
  } else {
    Serial.println("ERR:Send Failed");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    delay(10);

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  Serial.println("Master Gateway Started (Transparent Mode)");
}

void loop() {
  // Heartbeat
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 10000) {
    lastHeartbeat = millis();
    Serial.println("HEARTBEAT");
  }

  // Listen for data from Pi
  // Format expected: "TX:<TARGET_MAC>:<DATA>\n"
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processSerialCommand(inputBuffer);
      inputBuffer = "";
    } else if (c != '\r') {
      inputBuffer += c;
    }
  }
}

void processSerialCommand(String cmd) {
  cmd.trim();
  if (!cmd.startsWith("TX:"))
    return;

  // Format: TX:<MAC>:<HEX_DATA>
  int firstColon = cmd.indexOf(':');    // Should be at index 2
  int lastColon = cmd.lastIndexOf(':'); // The one before the HEX payload

  if (lastColon <= firstColon) {
    Serial.println("ERR:Format");
    return;
  }

  String macStr = cmd.substring(firstColon + 1, lastColon);
  String hexPayload = cmd.substring(lastColon + 1);

  uint8_t peerAddr[6];
  stringToMac(macStr, peerAddr);

  if (!esp_now_is_peer_exist(peerAddr)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, peerAddr, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("ERR:PeerAdd");
      return;
    }
  }

  // Convert Hex string back to bytes
  int payloadLen = hexPayload.length() / 2;
  uint8_t buffer[payloadLen];
  hexToBytes(hexPayload, buffer, payloadLen);

  esp_err_t result = esp_now_send(peerAddr, buffer, payloadLen);

  if (result == ESP_OK) {
    Serial.println("OK:Sent");
  } else {
    Serial.println("ERR:Send Failed");
  }
}
