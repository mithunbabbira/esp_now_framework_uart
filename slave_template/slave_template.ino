#include <WiFi.h>
#include <esp_now.h>

// MASTER MAC ADDRESS (Given by User)
uint8_t masterMAC[] = {0xF0, 0x24, 0xF9, 0x0D, 0x90, 0xA4};

// Data Structure
typedef struct __attribute__((packed)) {
  uint8_t type;    // 1=Temp, 2=Switch, 3=Motion
  uint8_t command; // 0=OFF, 1=ON, 2=DATA
  float value;     // e.g. 25.4
} ControlPacket;

// Global buffer for serial input
String inputBuffer = "";

// Helper to print MAC
void printMAC(const uint8_t *mac_addr) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac_addr[0],
           mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  Serial.print(macStr);
}

// Callback when data is sent (Core v3 Signature)
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // Serial.print("Last Packet Send Status: ");
  if (status == ESP_NOW_SEND_SUCCESS) {
    // Serial.println("Delivery Success");
  } else {
    Serial.println("Delivery Fail");
  }
}

// Callback when data is received (Core v3 Signature)
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData,
                int len) {
  Serial.print("\n--- Data Received from ");
  printMAC(info->src_addr);
  Serial.println(" ---");
  Serial.print("Content: ");
  for (int i = 0; i < len; i++) {
    Serial.print((char)incomingData[i]);
  }
  Serial.println("\n----------------------------\n");
}

void setup() {
  // Init Serial
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  } // Wait for serial

  // Init WiFi
  WiFi.mode(WIFI_STA);
  delay(500); // Wait for WiFi hardware to initialize
  Serial.println("Slave Node Started");
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register callbacks
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // Register Peer (Master)
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterMAC, 6);
  peerInfo.channel = 0; // 0 = Use current channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  Serial.println("Master Peer Registered.");
  Serial.println("Sending sensor data every 5 seconds...");
}

void sendPacket() {
  ControlPacket packet;
  packet.type = 1;                           // Temp
  packet.command = 2;                        // Data
  packet.value = random(2000, 3000) / 100.0; // 20.00 - 30.00

  esp_err_t result =
      esp_now_send(masterMAC, (uint8_t *)&packet, sizeof(packet));

  if (result == ESP_OK) {
    Serial.print("Sent Packet: Temp=");
    Serial.println(packet.value);
  } else {
    Serial.println("Error sending packet");
  }
}

void loop() {
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 5000) {
    lastSend = millis();
    sendPacket();
  }

  // Keep serial input for manual text commands (optional, mostly for debugging
  // receive)
  while (Serial.available()) {
    char c = Serial.read();
    // Just echo for now or trigger manual send?
    // Let's just ignore manual send for now to avoid struct confusion
  }
}
