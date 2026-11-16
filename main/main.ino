#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <SPI.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==========================================
// HARDWARE PIN DEFINITIONS
// ==========================================
#define OLED_SDA      4
#define OLED_SCL      5
#define STATUS_LED    27
#define MODE_BUTTON   0  

#define RF_JAM_CE     16
#define RF_JAM_CSN    15
#define RF_SNIFF_CE   22
#define RF_SNIFF_CSN  21

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDR   0x3C

// ==========================================
// GLOBAL OBJECTS & VARIABLES
// ==========================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SPIClass hspi(HSPI);
SPIClass vspi(VSPI);
RF24 radioJam(RF_JAM_CE, RF_JAM_CSN);
RF24 radioSniff(RF_SNIFF_CE, RF_SNIFF_CSN);

enum ScreenMode { MAIN, DEAUTH, JAMMING, SNIFFING };
ScreenMode currentScreen = MAIN;

volatile int deauthCount = 0;
volatile int mgmtCount = 0; 
String lastDeauthSrc = "None";
String lastDeauthDst = "None";

int noisyChannels = 0;
bool isJammed = false;
bool isPacketFlood = false;

int sniffTotalPackets = 0;
String lastSniffData = "Waiting...";
int lastSniffLen = 0;

unsigned long lastEventTime = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastChannelHop = 0;
int currentWifiChannel = 1;

int scanChannel = 1;
unsigned long lastScanTime = 0;
int currentWindowNoisy = 0;

unsigned long lastPacketFloodCheck = 0;
int packetsInLastSecond = 0;

int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// ==========================================
// WIFI PROMISCUOUS CALLBACK
// ==========================================
typedef struct {
  uint16_t frame_ctrl;
  uint16_t duration_id;
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

typedef struct {
  wifi_ieee80211_mac_hdr_t hdr;
  uint8_t payload[0];
} wifi_ieee80211_packet_t;

String macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", 
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void wifi_promiscuous_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  
  mgmtCount++; 
  
  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const wifi_ieee80211_packet_t* ipkt = (wifi_ieee80211_packet_t*)pkt->payload;
  const wifi_ieee80211_mac_hdr_t* hdr = &ipkt->hdr;
  
  // DEBUG: Extract the raw Frame Control bytes
  uint8_t fc1 = hdr->frame_ctrl & 0xFF;
  uint8_t fc2 = (hdr->frame_ctrl >> 8) & 0xFF;
  
  // Print to Serial Monitor so we can see exactly what the LilyGo is sending
  // (Make sure Serial Monitor is open at 115200 baud!)
  Serial.printf("MGMT FRAME: FC=[%02X %02X] ", fc1, fc2);

  // Check for Deauthentication (0x00C0) OR Disassociation (0x00A0)
  if ((hdr->frame_ctrl & 0x00FC) == 0x00C0 || (hdr->frame_ctrl & 0x00FC) == 0x00A0) {
    deauthCount++;
    lastDeauthSrc = macToString(hdr->addr2);
    lastDeauthDst = macToString(hdr->addr1);
    lastEventTime = millis(); 
    Serial.println("-> MATCHED DEAUTH/DISASSOC!");
  } else {
    Serial.println(); // Just a newline for other management frames
  }
}

// ==========================================
// DISPLAY DRAWING FUNCTIONS (Unchanged)
// ==========================================
void drawMainScreen() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 0);
  display.println("                     "); display.println("   ESP32 TRIDENT24   "); display.println("---------------------");
  display.print("Deauth: "); display.print(deauthCount); display.print(" | Ch: "); display.println(currentWifiChannel);
  display.print("Jam: "); display.print(isJammed ? "WARNING" : "SAFE"); display.print(" ("); display.print(noisyChannels); display.println("/100)");
  display.print("NRF24 Pkts: "); display.println(sniffTotalPackets); display.display();
}
void drawDeauthScreen() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 0);
  display.println("                     "); display.println("=== DEAUTH DETECT ===");
  display.println("---------------------");
  display.print("Total Deauths: "); display.println(deauthCount); 
  display.print("Mgmt Frames: "); display.println(mgmtCount);
  display.print("SRC:"); display.println(lastDeauthSrc);
  display.print("DST:"); display.println(lastDeauthDst); display.display();
}
void drawJammingScreen() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 0);
  display.println("                     "); display.println("===  JAMMER SCAN  ===");  display.println("---------------------"); display.print("Live Scan: CH "); display.println(scanChannel);
  display.print("Broadband: "); display.print(noisyChannels); display.println("/100");
  display.print("Pkt Flood: "); display.println(isPacketFlood ? "DETECTED" : "Clear");
  display.print("Status: ");
  display.setTextColor(isJammed ? SSD1306_BLACK : SSD1306_WHITE, isJammed ? SSD1306_WHITE : SSD1306_BLACK);
  display.println(isJammed ? "JAM DETECTED" : "SAFE");
  display.setTextColor(SSD1306_WHITE); display.display();
}
void drawSniffingScreen() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 0);
  display.println("                     "); display.println("=== NRF24 SNIFFER ===");  display.println("---------------------"); display.print("Total Packets: "); display.println(sniffTotalPackets);
  display.print("Last Len: "); display.print(lastSniffLen); display.println(" bytes");
  display.print("Data: "); display.println(lastSniffData); display.display();
}
void updateDisplay() {
  switch (currentScreen) {
    case MAIN: drawMainScreen(); break; case DEAUTH: drawDeauthScreen(); break;
    case JAMMING: drawJammingScreen(); break; case SNIFFING: drawSniffingScreen(); break;
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200); // CRITICAL: Open Serial Monitor at 115200 to see debug output!
  
  pinMode(STATUS_LED, OUTPUT);
  pinMode(MODE_BUTTON, INPUT_PULLUP);
  digitalWrite(STATUS_LED, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDR)) { for (;;); }
  display.ssd1306_command(SSD1306_DISPLAYON); 
  
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0, 25);
  display.println("  Initializing..."); display.display();

  hspi.begin(14, 12, 13, -1); vspi.begin(18, 19, 23, -1);

  radioJam.begin(&hspi); radioJam.setAutoAck(false); radioJam.setCRCLength(RF24_CRC_DISABLED);
  radioJam.setDataRate(RF24_1MBPS); radioJam.powerUp(); radioJam.startListening();

  radioSniff.begin(&vspi); radioSniff.setAutoAck(false); radioSniff.setCRCLength(RF24_CRC_DISABLED);
  radioSniff.setDataRate(RF24_1MBPS); radioSniff.setAddressWidth(1); radioSniff.enableDynamicPayloads();
  radioSniff.openReadingPipe(1, (uint8_t)0x00); radioSniff.startListening();

  // --- WIFI INITIALIZATION FIX ---
  // Changed from WIFI_MODE_NULL to WIFI_STA to ensure the radio is fully powered and clocked
  WiFi.mode(WIFI_STA); 
  WiFi.disconnect();   
  
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous_callback);
  // -------------------------------

  delay(1000);
  updateDisplay();
}

// ==========================================
// LOOP (Unchanged)
// ==========================================
void loop() {
  unsigned long now = millis();

  int reading = digitalRead(MODE_BUTTON);
  if (reading != lastButtonState) lastDebounceTime = now;
  if ((now - lastDebounceTime) > debounceDelay) {
    if (reading == LOW) {
      currentScreen = (ScreenMode)((currentScreen + 1) % 4);
      updateDisplay();
      while (digitalRead(MODE_BUTTON) == LOW) { delay(10); }
    }
  }
  lastButtonState = reading;

  if (now - lastScanTime > 10) {
    lastScanTime = now; radioJam.setChannel(scanChannel); delayMicroseconds(100); 
    if (radioJam.testRPD()) { currentWindowNoisy++; }
    scanChannel++;
    if (scanChannel > 100) {
      noisyChannels = currentWindowNoisy; currentWindowNoisy = 0; scanChannel = 1;
    }
  }

  if (now - lastPacketFloodCheck > 1000) {
    lastPacketFloodCheck = now;
    isPacketFlood = (packetsInLastSecond > 50); 
    packetsInLastSecond = 0;
    isJammed = isPacketFlood || (noisyChannels > 20);
    if (isJammed) lastEventTime = now;
  }

  if (radioSniff.available()) {
    uint8_t len = radioSniff.getDynamicPayloadSize(); if (len > 32) len = 32;
    uint8_t payload[32]; radioSniff.read(payload, len);
    sniffTotalPackets++; packetsInLastSecond++; lastSniffLen = len; lastEventTime = now;
    String hexStr = ""; int showLen = min((int)len, 8);
    for (int i = 0; i < showLen; i++) {
      if (payload[i] < 0x10) hexStr += "0"; hexStr += String(payload[i], HEX); hexStr += " ";
    }
    if (len > 8) hexStr += "..."; lastSniffData = hexStr;
  }

  if (now - lastChannelHop > 1000) {
    lastChannelHop = now; currentWifiChannel = (currentWifiChannel % 13) + 1;
    esp_wifi_set_channel(currentWifiChannel, WIFI_SECOND_CHAN_NONE);
  }

  if (now - lastDisplayUpdate > 250) { lastDisplayUpdate = now; updateDisplay(); }

  if (now - lastEventTime < 1500) { digitalWrite(STATUS_LED, HIGH); } 
  else { digitalWrite(STATUS_LED, LOW); }

  delay(10);
}