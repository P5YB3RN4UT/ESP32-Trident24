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
#define OLED_SDA 4
#define OLED_SCL 5
#define STATUS_LED 27
#define MODE_BUTTON 0
#define RF_JAM_CE 16
#define RF_JAM_CSN 15
#define RF_SNIFF_CE 22
#define RF_SNIFF_CSN 21

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDR 0x3C

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

// Original button variables
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// Startup Menu Variables
enum StartupState { STARTUP_LOGO, STARTUP_MENU, RUNNING };
StartupState startupState = STARTUP_LOGO;
unsigned long logoStartTime = 0;
int selectedOption = 1; // 1 = Web UI, 2 = Standalone
bool webUIMode = false;

// Menu button variables
int lastButtonStateMenu = HIGH;
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

unsigned long lastSerialUpdate = 0;

// --- SIGNAL ANALYZER ---
volatile int channelActivity[14] = {0}; // Index 1 to 13 for WiFi channels

// --- FALSE POSITIVE SUPPRESSION ---
#define MAX_IGNORED_MACS 10
uint8_t ignoredMacs[MAX_IGNORED_MACS][6];
int ignoredMacCount = 0;
String serialCmdBuffer = "";

// ==========================================
// TRIDENT LOGO DRAWING FUNCTION
// ==========================================
void drawTridentLogo() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  const int cx = 64;
  const int cy = 47;

  // --- TOP HALF: THREE-PRONGED TRIDENT (REBUILT FOR PERFECT SYMMETRY & CURVES) ---
  
  // 1. Center prong (Sharp, perfectly centered triangle)
  // Tip at x=64, base width 12px at y=26
  display.fillTriangle(64, 2, 58, 26, 70, 26, SSD1306_WHITE);
  
  // 2. Left prong (Curved outwards using 3 overlapping triangles for a smooth blade look)
  // Outer edge curves from tip (40,6) out to (36,18) and back in to (40,26)
  display.fillTriangle(40, 6, 36, 18, 42, 18, SSD1306_WHITE);
  display.fillTriangle(36, 18, 42, 18, 40, 26, SSD1306_WHITE);
  display.fillTriangle(42, 18, 40, 26, 52, 26, SSD1306_WHITE); // Connects to crossguard
  
  // 3. Right prong (Perfect mathematical mirror of the left prong)
  // Outer edge curves from tip (88,6) out to (92,18) and back in to (88,26)
  display.fillTriangle(88, 6, 92, 18, 86, 18, SSD1306_WHITE);
  display.fillTriangle(92, 18, 86, 18, 88, 26, SSD1306_WHITE);
  display.fillTriangle(86, 18, 88, 26, 76, 26, SSD1306_WHITE); // Connects to crossguard

  // 4. Crossguard (Connects the three prongs seamlessly at their base)
  // Spans from x=52 to x=76, perfectly bridging the inner bases of the side prongs
  display.fillRect(52, 26, 24, 4, SSD1306_WHITE);
  
  // 5. Center shaft (Connects the crossguard down to the handle)
  // Starts just below the crossguard (y=30) and ends exactly at the handle bar (y=45)
  display.fillRect(61, 30, 6, 15, SSD1306_WHITE);

  // --- BOTTOM HALF: HANDLE / BASE (Unchanged, connects perfectly to the new shaft) ---
  display.drawCircle(cx, cy, 15, SSD1306_WHITE);
  display.drawCircle(cx, cy, 14, SSD1306_WHITE);
  display.fillCircle(cx, cy, 11, SSD1306_BLACK);
  
  // Horizontal bar 
  display.fillRect(31, 45, 66, 5, SSD1306_WHITE);
  
  display.fillCircle(cx, cy, 9, SSD1306_BLACK);
  display.fillCircle(61, 44, 3, SSD1306_WHITE);
  display.fillCircle(67, 44, 3, SSD1306_WHITE);
  
  display.drawCircle(64, 54, 5, SSD1306_WHITE);
  display.drawCircle(64, 54, 3, SSD1306_WHITE);
  display.fillCircle(64, 54, 1, SSD1306_WHITE);

  display.display();
}

void drawStartupMenu(int selectedOption) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("                     ");
  display.println("   ESP32 TRIDENT24   ");
  display.println("                     ");
  if (selectedOption == 1) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.println(" 1. Web UI (USB)");
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    display.println(" 2. Standalone  ");
  } else {
    display.println(" 1. Web UI (USB)");
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.println(" 2. Standalone  ");
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }
  display.println("");
  display.println("Quick: Scroll");
  display.println("Long: Select");
  display.display();
}

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
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool isMacIgnored(const uint8_t* mac) {
  for (int i = 0; i < ignoredMacCount; i++) {
    if (memcmp(ignoredMacs[i], mac, 6) == 0) return true;
  }
  return false;
}

bool parseMacString(String macStr, uint8_t* mac) {
  macStr.trim();
  if (macStr.length() != 17) return false; // Ensure format XX:XX:XX:XX:XX:XX
  for (int i = 0; i < 6; i++) {
    String byteStr = macStr.substring(i * 3, i * 3 + 2);
    mac[i] = strtoul(byteStr.c_str(), NULL, 16);
  }
  return true;
}

void wifi_promiscuous_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  // FIX: Accept WIFI_PKT_MISC as well as WIFI_PKT_MGMT. 
  // Raw injected frames (like those from Evil-M5Project) are often classified as MISC 
  // by the ESP32 hardware, causing them to be silently dropped by a strict MGMT check.
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_MISC) return;
  
  // Only increment mgmtCount for actual MGMT frames to keep stats accurate
  if (type == WIFI_PKT_MGMT) {
    mgmtCount++;
  }

    // Track per-channel activity for the Web UI signal analyzer
  if (currentWifiChannel >= 1 && currentWifiChannel <= 13) {
    channelActivity[currentWifiChannel]++;
  }
  
  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const wifi_ieee80211_packet_t* ipkt = (wifi_ieee80211_packet_t*)pkt->payload;
  const wifi_ieee80211_mac_hdr_t* hdr = &ipkt->hdr;
  
  // 0x00C0 = Deauthentication (Type 0, Subtype 12)
  // 0x00A0 = Disassociation (Type 0, Subtype 10)
  // 0x00FC mask isolates Type and Subtype bits, ignoring To DS / From DS flags
  if ((hdr->frame_ctrl & 0x00FC) == 0x00C0 || (hdr->frame_ctrl & 0x00FC) == 0x00A0) {
    
    // --- SUPPRESSION CHECK ---
    // If either the Source or Destination is in our ignore list, drop it silently
    if (isMacIgnored(hdr->addr1) || isMacIgnored(hdr->addr2)) {
      return; 
    }

    deauthCount++;
    lastDeauthSrc = macToString(hdr->addr2);
    lastDeauthDst = macToString(hdr->addr1);
    lastEventTime = millis();
  }
}

// ==========================================
// DISPLAY DRAWING FUNCTIONS
// ==========================================
void drawMainScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("                     ");
  display.println("   ESP32 TRIDENT24   ");
  display.println("---------------------");
  display.print("Deauth: "); display.print(deauthCount); display.print(" | Ch: "); display.println(currentWifiChannel);
  display.print("Jam: "); display.print(isJammed ? "WARNING" : "SAFE"); display.print(" ("); display.print(noisyChannels); display.println("/100)");
  display.print("NRF24 Pkts: "); display.println(sniffTotalPackets);
  display.display();
}

void drawDeauthScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("                     ");
  display.println("=== DEAUTH DETECT ===");
  display.println("---------------------");
  display.print("Total Deauths: "); display.println(deauthCount);
  display.print("Mgmt Frames: "); display.println(mgmtCount);
  display.print("SRC: "); display.println(lastDeauthSrc);
  display.print("DST: "); display.println(lastDeauthDst);
  display.display();
}

void drawJammingScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("                     ");
  display.println("===  JAMMER SCAN  ===");
  display.println("---------------------");
  display.print("Live Scan: CH "); display.println(scanChannel);
  display.print("Broadband: "); display.print(noisyChannels); display.println("/100");
  display.print("Pkt Flood: "); display.println(isPacketFlood ? "DETECTED" : "Clear");
  display.print("Status: ");
  display.setTextColor(isJammed ? SSD1306_BLACK : SSD1306_WHITE, isJammed ? SSD1306_WHITE : SSD1306_BLACK);
  display.println(isJammed ? "JAM DETECTED" : "SAFE");
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void drawSniffingScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("                     ");
  display.println("=== NRF24 SNIFFER ===");
  display.println("---------------------");
  display.print("Total Packets: "); display.println(sniffTotalPackets);
  display.print("Last Len: "); display.print(lastSniffLen); display.println(" bytes");
  display.print("Data: "); display.println(lastSniffData);
  display.display();
}

void updateDisplay() {
  switch (currentScreen) {
    case MAIN: drawMainScreen(); break;
    case DEAUTH: drawDeauthScreen(); break;
    case JAMMING: drawJammingScreen(); break;
    case SNIFFING: drawSniffingScreen(); break;
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(MODE_BUTTON, INPUT_PULLUP);
  digitalWrite(STATUS_LED, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDR)) {
    for (;;);
  }
  display.ssd1306_command(SSD1306_DISPLAYON);

  drawTridentLogo();
  logoStartTime = millis();

  hspi.begin(14, 12, 13, -1);
  vspi.begin(18, 19, 23, -1);

  radioJam.begin(&hspi);
  radioJam.setAutoAck(false);
  radioJam.setCRCLength(RF24_CRC_DISABLED);
  radioJam.setDataRate(RF24_1MBPS);
  radioJam.powerUp();
  radioJam.startListening();

  radioSniff.begin(&vspi);
  radioSniff.setAutoAck(false);
  radioSniff.setCRCLength(RF24_CRC_DISABLED);
  radioSniff.setDataRate(RF24_1MBPS);
  radioSniff.setAddressWidth(1);
  radioSniff.enableDynamicPayloads();
  radioSniff.openReadingPipe(1, (uint8_t)0x00);
  radioSniff.startListening();

  // WiFi setup for promiscuous mode (does not connect to any network)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous_callback);
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  unsigned long now = millis();
    // --- SERIAL TELEMETRY (Web UI Mode Only) ---
    if (webUIMode && now - lastSerialUpdate > 1000) {
      lastSerialUpdate = now;
      
      // Local copies to prevent concurrent modification issues during string building
      int l_deauthCount = deauthCount;
      int l_mgmtCount = mgmtCount;
      String l_lastDeauthSrc = lastDeauthSrc;
      String l_lastDeauthDst = lastDeauthDst;
      int l_noisyChannels = noisyChannels;
      bool l_isJammed = isJammed;
      bool l_isPacketFlood = isPacketFlood;
      int l_sniffTotalPackets = sniffTotalPackets;
      int l_lastSniffLen = lastSniffLen;
      String l_lastSniffData = lastSniffData;
      int l_currentWifiChannel = currentWifiChannel;

      Serial.print("{\"uptime\":"); Serial.print(now);
      Serial.print(",\"wifiCh\":"); Serial.print(l_currentWifiChannel);
      Serial.print(",\"deauthCount\":"); Serial.print(l_deauthCount);
      
      // NEW: Send true if deauthCount is greater than 0
      Serial.print(",\"isDeauthDetected\":"); Serial.print((l_deauthCount > 0) ? "true" : "false"); 
      
      Serial.print(",\"mgmtCount\":"); Serial.print(l_mgmtCount);
      Serial.print(",\"lastDeauthSrc\":\""); Serial.print(l_lastDeauthSrc); Serial.print("\"");
      Serial.print(",\"lastDeauthDst\":\""); Serial.print(l_lastDeauthDst); Serial.print("\"");
      Serial.print(",\"noisyChannels\":"); Serial.print(l_noisyChannels);
      // NEW: Add jamming intensity percentage (0-100%)
      Serial.print(",\"jamIntensity\":"); Serial.print(min(100, (l_noisyChannels * 5))); 
      Serial.print(",\"isJammed\":"); Serial.print(l_isJammed ? "true" : "false");
      Serial.print(",\"isPacketFlood\":"); Serial.print(l_isPacketFlood ? "true" : "false");
      Serial.print(",\"sniffTotalPackets\":"); Serial.print(l_sniffTotalPackets);
      Serial.print(",\"lastSniffLen\":"); Serial.print(l_lastSniffLen);
      Serial.print(",\"lastSniffData\":\""); Serial.print(l_lastSniffData); Serial.print("\"");
      
      // --- ADD SIGNAL ANALYZER DATA ---
      Serial.print(",\"chActivity\":[");
      for(int i = 1; i <= 13; i++) {
          Serial.print(channelActivity[i]);
          if(i < 13) Serial.print(",");
          channelActivity[i] = 0; // Reset for the next 1-second window
      }
      Serial.print("]");
      
      Serial.print("}\n");
    }

  // --- STATE MACHINE ---
  if (startupState == STARTUP_LOGO) {
    if (now - logoStartTime >= 3000) {
      startupState = STARTUP_MENU;
      drawStartupMenu(selectedOption);
    }
  } 
  else if (startupState == STARTUP_MENU) {
    int reading = digitalRead(MODE_BUTTON);
    
    if (reading == LOW && lastButtonStateMenu == HIGH) {
      buttonPressTime = now;
      buttonPressed = true;
    }
    
    if (reading == HIGH && lastButtonStateMenu == LOW && buttonPressed) {
      unsigned long duration = now - buttonPressTime;
      if (duration < 800) { 
        // Quick press: Scroll options
        selectedOption = (selectedOption == 1) ? 2 : 1;
        drawStartupMenu(selectedOption);
      } else { 
        // Long press: Select option
        webUIMode = (selectedOption == 1);
        startupState = RUNNING;
        
        if (webUIMode) {
          drawTridentLogo(); // Static logo as requested
          Serial.println("{\"status\":\"Web UI Mode Active\"}");
        } else {
          currentScreen = MAIN;
          updateDisplay();
        }
      }
      buttonPressed = false;
    }
    lastButtonStateMenu = reading;
  } 
  else if (startupState == RUNNING) {

        // --- LISTEN FOR WEBUI COMMANDS ---
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (serialCmdBuffer.length() > 0) {
          if (serialCmdBuffer.startsWith("IGNORE:")) {
            String macStr = serialCmdBuffer.substring(7);
            uint8_t mac[6];
            if (parseMacString(macStr, mac)) {
              if (ignoredMacCount < MAX_IGNORED_MACS) {
                memcpy(ignoredMacs[ignoredMacCount], mac, 6);
                ignoredMacCount++;
              }
            }
          }
          serialCmdBuffer = "";
        }
      } else {
        serialCmdBuffer += c;
      }
    }

    // --- BUTTON HANDLING (Standalone Mode Only) ---
    if (!webUIMode) {
      int reading = digitalRead(MODE_BUTTON);
      if (reading != lastButtonState) {
        lastDebounceTime = now;
      }
      if ((now - lastDebounceTime) > debounceDelay) {
        if (reading == LOW) {
          currentScreen = (ScreenMode)((currentScreen + 1) % 4);
          updateDisplay();
          while (digitalRead(MODE_BUTTON) == LOW) { delay(10); }
        }
      }
      lastButtonState = reading;
    }

    // --- PASSIVE RF CHANNEL SCANNING ---
    if (now - lastScanTime > 10) {
      lastScanTime = now;
      radioJam.setChannel(scanChannel);
      delayMicroseconds(100);
      if (radioJam.testRPD()) {
        currentWindowNoisy++;
      }
      scanChannel++;
      if (scanChannel > 100) {
        noisyChannels = currentWindowNoisy;
        currentWindowNoisy = 0;
        scanChannel = 1;
      }
    }

// --- PACKET FLOOD CHECK ---
if (now - lastPacketFloodCheck > 1000) {
  lastPacketFloodCheck = now;
  isPacketFlood = (packetsInLastSecond > 50);
  // 1. UI/State threshold changed to >=5
  isJammed = isPacketFlood || (noisyChannels > 20);
  // 2. LED trigger threshold remains > 1
  if (isJammed || (noisyChannels > 1)) {
    lastEventTime = now;
  }
  
  // Reset the counter for the next 1-second window
  packetsInLastSecond = 0; 
}

    // --- NRF24 PACKET RECEPTION ---
    if (radioSniff.available()) {
      uint8_t len = radioSniff.getDynamicPayloadSize();
      if (len > 32) len = 32;
      uint8_t payload[32];
      radioSniff.read(payload, len);
      sniffTotalPackets++;
      packetsInLastSecond++;
      lastSniffLen = len;
      // lastEventTime = now; // <-- Disabled so sniffer packets don't trigger the LED
      
      String hexStr = "";
      for (int i = 0; i < len; i++) {       // <--- LOOP THROUGH FULL LENGTH
        if (payload[i] < 0x10) hexStr += "0";
        hexStr += String(payload[i], HEX);
        hexStr += " ";
      }
      lastSniffData = hexStr;               // <--- NO TRUNCATION
    }

    // --- WIFI CHANNEL HOPPING ---
    if (now - lastChannelHop > 200) {
      lastChannelHop = now;
      currentWifiChannel = (currentWifiChannel % 13) + 1;
      esp_wifi_set_channel(currentWifiChannel, WIFI_SECOND_CHAN_NONE);
    }

    // --- DISPLAY UPDATE (Standalone Only) ---
    if (!webUIMode && now - lastDisplayUpdate > 250) {
      lastDisplayUpdate = now;
      updateDisplay();
    }

    // --- STATUS LED ---
    if (now - lastEventTime < 1500) {
      digitalWrite(STATUS_LED, HIGH);
    } else {
      digitalWrite(STATUS_LED, LOW);
    }
    
    delay(10);
  }
}
