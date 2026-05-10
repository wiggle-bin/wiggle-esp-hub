#include <WiFi.h>
#include <esp_now.h>
#include "esp_wifi.h"
#include <HTTPClient.h>
#include "env.h"

// === CONFIG ===
const char *serverBaseUrl = "http://homeassistant.local:8123/api/wiggle/upload";

// Set the WiFi channel for ESP-NOW (must match sender)
#define RECEIVER_WIFI_CHANNEL 9

// Image parameters
#define DOWNSAMPLED_WIDTH 80
#define DOWNSAMPLED_HEIGHT 60
#define DOWNSAMPLED_IMAGE_SIZE (DOWNSAMPLED_WIDTH * DOWNSAMPLED_HEIGHT)
#define DOWNSAMPLED_PACKED_SIZE (DOWNSAMPLED_IMAGE_SIZE / 2)

// Buffer for incoming image
uint8_t packedImage[DOWNSAMPLED_PACKED_SIZE];
bool chunkReceived[128] = {0}; // Enough for 2400/150 = 16 chunks, but allow more
uint16_t totalChunks = 0;
uint16_t chunksReceived = 0;
bool receivingImage = false;
volatile bool imageReadyToUpload = false;

// Unpack 4-bit grayscale to 8-bit
void unpack_4bit(const uint8_t *src, uint8_t *dst, int num_pixels) {
  for (int i = 0; i < num_pixels / 2; ++i) {
    uint8_t b = src[i];
    dst[i * 2]     = (b >> 4) * 17; // 0-15 -> 0-255
    dst[i * 2 + 1] = (b & 0x0F) * 17;
  }
}

// ESP-NOW receive callback
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  Serial.println("ESP-NOW packet received");

  if (len < 4) return;
  uint16_t chunk = incomingData[0] | (incomingData[1] << 8);
  uint16_t tChunks = incomingData[2] | (incomingData[3] << 8);
  if (tChunks > 128) return; // Sanity check

  if (!receivingImage || tChunks != totalChunks) {
    // New image
    memset(chunkReceived, 0, sizeof(chunkReceived));
    totalChunks = tChunks;
    chunksReceived = 0;
    receivingImage = true;
    Serial.printf("Receiving new image: %d chunks\n", totalChunks);
  }

  if (!chunkReceived[chunk]) {
    chunkReceived[chunk] = true;
    ++chunksReceived;
    size_t offset = chunk * (len - 4);
    size_t copyLen = len - 4;
    if (offset + copyLen > DOWNSAMPLED_PACKED_SIZE) copyLen = DOWNSAMPLED_PACKED_SIZE - offset;
    memcpy(packedImage + offset, incomingData + 4, copyLen);
    Serial.printf("Chunk %d/%d received\n", chunk + 1, totalChunks);
  }

  if (chunksReceived == totalChunks) {
    Serial.println("All chunks received, ready to upload.");
    imageReadyToUpload = true;
    receivingImage = false;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP-NOW receiver starting...");

  // WiFi STA mode for ESP-NOW only
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Set WiFi channel for ESP-NOW (must match sender)
  esp_wifi_set_channel(RECEIVER_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // ESP-NOW init
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);

  // Optionally: add peer (sender MAC)
  // uint8_t senderMac[] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
  // esp_now_peer_info_t peerInfo = {};
  // memcpy(peerInfo.peer_addr, senderMac, 6);
  // peerInfo.channel = 0;
  // peerInfo.encrypt = false;
  // esp_now_add_peer(&peerInfo);
}

void loop() {
  // Check if image is ready to upload (set by onDataRecv)
  if (imageReadyToUpload) {
    imageReadyToUpload = false;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("Connecting to WiFi SSID: %s\n", ssid);
    WiFi.begin(ssid, password);
    unsigned long start = millis();
    wl_status_t lastStatus = WiFi.status();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      wl_status_t s = WiFi.status();
      if (s != lastStatus) {
        Serial.printf("WiFi status: %d\n", s);
        lastStatus = s;
      }
      delay(200);
      Serial.print(".");
    }
    wl_status_t finalStatus = WiFi.status();
    Serial.printf("\nFinal WiFi status: %d\n", finalStatus);
    if (finalStatus == WL_CONNECTED) {
      Serial.println("WiFi connected, uploading image...");
      WiFiClient client;
      HTTPClient http;
      http.begin(client, serverBaseUrl);
      http.addHeader("Content-Type", "application/octet-stream");
      int httpResponseCode = http.POST(packedImage, DOWNSAMPLED_PACKED_SIZE);
      if (httpResponseCode > 0) {
        Serial.printf("Image sent! Response: %d\n", httpResponseCode);
      } else {
        Serial.printf("Send failed: %s\n", http.errorToString(httpResponseCode).c_str());
      }
      http.end();
    } else {
      Serial.println("WiFi connect failed, image not sent.");
    }

    // --- Disconnect WiFi and reset ESP-NOW channel (best practice) ---
    WiFi.disconnect(); // Do NOT use true argument
    delay(100);
    esp_wifi_set_channel(RECEIVER_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // Reset image reception state for next image
    memset(chunkReceived, 0, sizeof(chunkReceived));
    totalChunks = 0;
    chunksReceived = 0;
    receivingImage = false;
  }
}