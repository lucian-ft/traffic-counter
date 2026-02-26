#include <WiFi.h>
#include <esp_wifi.h>
#include "ThingSpeak.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// --- 1. CREDENTIALE REALE ---
const char* WIFI_SSID = "nume retea";    
const char* WIFI_PASSWORD = "parola retea"; 

unsigned long THINGSPEAK_CHANNEL_ID = 1234567; 
const char* THINGSPEAK_API_KEY = "write API key"; 

// --- 2. SETARI TIMP & LIMITE ---
#define REPORT_INTERVAL_SEC 60      // 1 Minute (60 secunde) intre trimiteri
#define SLICE_INTERVAL_MS 5000       // 5 secunde pentru fiecare protocol
#define CHANNEL_HOP_INTERVAL_MS 500  // Wi-Fi: Schimbam canalul la 0.5s
#define MAX_DEVICES 200              // Maxim dispozitive unice per protocol

WiFiClient client;
BLEScan* pBLEScan;

// --- 3. STRUCTURI DE DATE ---
String wifiPietoni[MAX_DEVICES];
int wifiCount = 0;

String blePietoni[MAX_DEVICES];
int bleCount = 0;

unsigned long lastReportTime = 0;

// --- CALLBACK Wi-Fi (Ruleaza doar cand Promiscuous e activ) ---
void IRAM_ATTR sniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  
  const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t *payload = pkt->payload;
  uint8_t frameControl = payload[0];
  
  // 0x40 = Probe Request
  if (frameControl == 0x40) {
    String macAddress = "";
    for (int i = 10; i < 16; i++) {
      if (payload[i] < 0x10) macAddress += "0";
      macAddress += String(payload[i], HEX);
      if (i < 15) macAddress += ":";
    }
    macAddress.toUpperCase();

    // Verificare duplicate Wi-Fi
    bool isNew = true;
    for (int i = 0; i < wifiCount; i++) {
      if (wifiPietoni[i] == macAddress) {
        isNew = false; break;
      }
    }
    
    if (isNew && wifiCount < MAX_DEVICES) {
      wifiPietoni[wifiCount] = macAddress;
      wifiCount++;
    }
  }
}

// --- CALLBACK BLE (Se activeaza automat cand BLE scaneaza) ---
class MyBLECallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      String macAddress = advertisedDevice.getAddress().toString().c_str();
      macAddress.toUpperCase();

      // Verificare duplicate BLE
      bool isNew = true;
      for (int i = 0; i < bleCount; i++) {
        if (blePietoni[i] == macAddress) {
          isNew = false; break;
        }
      }
      
      if (isNew && bleCount < MAX_DEVICES) {
        blePietoni[bleCount] = macAddress;
        bleCount++;
      }
    }
};

void setup() {
  Serial.begin(115200);
  
  // Initializare WiFi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous_rx_cb(&sniffer);
  
  // Initializare BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyBLECallbacks());
  pBLEScan->setActiveScan(true); // Active scan extrage mai multe date din telefoane
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99); 
  
  ThingSpeak.begin(client);
  
  Serial.println("\n--- SISTEM HIBRID (Wi-Fi + BLE) PORNIT ---");
  Serial.println("Algoritm: 5 secunde asculta Wi-Fi -> 5 secunde asculta BLE.");
  
  lastReportTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // Verificam daca a venit timpul pentru upload
  if (currentMillis - lastReportTime >= (REPORT_INTERVAL_SEC * 1000)) {
    // --- FAZA DE UPLOAD ---
    Serial.println("\n------------------------------------------------");
    Serial.print("[SISTEM] 5 min incheiate. Pietoni Wi-Fi: ");
    Serial.print(wifiCount);
    Serial.print(" | Pietoni BLE: ");
    Serial.println(bleCount);
    
    Serial.println("[SISTEM] Conectare la router pentru upload...");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500); Serial.print("."); retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WIFI] Conectat! Trimitere catre ThingSpeak...");
      
      ThingSpeak.setField(1, wifiCount); // Campul 1 = Wi-Fi
      ThingSpeak.setField(2, bleCount);  // Campul 2 = BLE
      
      int httpCode = ThingSpeak.writeFields(THINGSPEAK_CHANNEL_ID, THINGSPEAK_API_KEY);
      
      if (httpCode == 200) {
        Serial.println("[SUCCESS] Date trimise!");
      } else {
        Serial.println("[EROARE] ThingSpeak Error: " + String(httpCode));
      }
    } else {
      Serial.println("\n[EROARE] Esec la conectare.");
    }

    // Resetari pentru noul ciclu
    WiFi.disconnect();
    wifiCount = 0;
    bleCount = 0;
    lastReportTime = millis();
    Serial.println("------------------------------------------------\n");
    
  } else {
    // --- FAZA DE SCANARE (Alternanta 5s / 5s) ---

    // 1. SCANARE Wi-Fi (5 Secunde)
    // Serial.println("-> Ascultare Wi-Fi..."); // Optional, poti decomenta pentru a vedea ciclurile
    esp_wifi_set_promiscuous(true);
    unsigned long startWifi = millis();
    int currentChannel = 1;
    
    while (millis() - startWifi < SLICE_INTERVAL_MS) {
      // Rotim canalele la fiecare jumatate de secunda in interiorul celor 5s
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
      currentChannel++;
      if (currentChannel > 13) currentChannel = 1;
      delay(CHANNEL_HOP_INTERVAL_MS); 
    }
    esp_wifi_set_promiscuous(false); // Oprim urechea de Wi-Fi

    // 2. SCANARE BLE (5 Secunde)
    // Serial.println("-> Ascultare BLE..."); // Optional
    // Functia start() blocheaza codul fix numarul de secunde mentionat in paranteza
    pBLEScan->start(5, false); 
    pBLEScan->clearResults(); // Curatam memoria scanerului BLE
  }
}
