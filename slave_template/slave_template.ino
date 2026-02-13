#include <WiFi.h>
#include <esp_now.h>

// MASTER MAC ADDRESS: set this to your Master Gateway's MAC (see Master Serial or Pi "RX:" lines).
// Bidirectional: Pi sends TX:<this_slave_MAC>:<HEX> -> Master -> this slave OnDataRecv; slave sends ACK -> Master -> Pi sees RX:<this_slave_MAC>:41434b...
uint8_t masterMAC[] = {0xC0, 0xCD, 0xD6, 0x85, 0x70, 0xCC};

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

// Last command received from Pi (0=OFF, 1=ON) for optional use in loop
volatile uint8_t lastCommand = 2;  // 2 = DATA (no command yet)

// Callback when data is received from Master (Pi -> Master -> Slave). Bidirectional: reply with ACK.
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData,
                int len) {
  Serial.print("\n--- Data Received from Pi (via Master) --- ");
  Serial.print(len);
  Serial.println(" bytes");

  if (len >= 6) {
    // 6-byte ControlPacket: type, command, value (float LE)
    const ControlPacket *p = (const ControlPacket *)incomingData;
    lastCommand = p->command;
    Serial.print("  ControlPacket: type=");
    Serial.print(p->type);
    Serial.print(" command=");
    Serial.print(p->command);
    Serial.print(" value=");
    Serial.println(p->value);
    // Send ACK back to Master (Pi will see RX:<this_slave_MAC>:<hex>)
    uint8_t ack[] = {'A', 'C', 'K', (uint8_t)p->type, (uint8_t)p->command, 0};
    esp_now_send(masterMAC, ack, sizeof(ack));
  } else if (len == 1) {
    // Single-byte command: 0x00 = OFF/STOP, 0x01 = ON/START
    lastCommand = incomingData[0];
    Serial.print("  Command: ");
    Serial.println(incomingData[0] == 1 ? "ON/START (0x01)" : incomingData[0] == 0 ? "OFF/STOP (0x00)" : "?");
    // Send ACK back so Pi sees bidirectional: ACK + echo of command byte
    uint8_t ack[] = {'A', 'C', 'K', incomingData[0]};
    esp_now_send(masterMAC, ack, sizeof(ack));
  } else {
    // Raw hex: print and echo back first byte as ack
    Serial.print("  HEX: ");
    for (int i = 0; i < len && i < 32; i++) {
      if (incomingData[i] < 16) Serial.print("0");
      Serial.print(incomingData[i], HEX);
    }
    Serial.println();
    uint8_t ack[] = {'A', 'C', 'K', len > 0 ? incomingData[0] : 0};
    esp_now_send(masterMAC, ack, sizeof(ack));
  }
  Serial.println("----------------------------\n");
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
