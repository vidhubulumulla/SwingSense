/*
#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h> 

typedef struct data_packet {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} data_packet;

data_packet data;

void readMacAddress(){
  uint8_t baseMac[6];
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, baseMac);
  if (ret == ESP_OK) {
    Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x\n",
                  baseMac[0], baseMac[1], baseMac[2],
                  baseMac[3], baseMac[4], baseMac[5]);
  } else {
    Serial.println("Failed to read MAC address");
  }
}

void data_received(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
    Serial.print("From: ");
    for (int i = 0; i < 6; ++i) {
        Serial.printf("%02X", mac_addr[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.printf("  len=%d\n", len);

    if (len <= 0) {
        Serial.println("Warning: received packet with non-positive length");
        return;
    }

    // show raw bytes for quick inspection
    Serial.print("Raw: ");
    for (int i = 0; i < len; ++i) {
        Serial.printf("%02X ", incomingData[i]);
    }
    Serial.println();

    // copy only up to our struct size to avoid overflow
    int copyLen = len;
    if (copyLen > (int)sizeof(data)) copyLen = sizeof(data);
    memcpy(&data, incomingData, copyLen);

    Serial.printf("Received: a[%.2f, %.2f, %.2f] g[%.2f, %.2f, %.2f]\n",
                  data.ax, data.ay, data.az,
                  data.gx, data.gy, data.gz);

    if (copyLen != (int)sizeof(data)) {
        Serial.printf("Warning: copied %d bytes but struct is %d bytes\n",
                      copyLen, (int)sizeof(data));
    }
}


void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_STA);
    // Ensure receiver uses the same Wi-Fi channel as the transmitter
    {
      int wifi_channel = 1; // change this on both boards if you want a different channel
      esp_err_t ch_res = esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
      if (ch_res == ESP_OK) {
        Serial.printf("WiFi channel set to %d\n", wifi_channel);
      } else {
        Serial.printf("Failed to set WiFi channel: 0x%02X\n", ch_res);
      }
    }
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }

    Serial.println("MAC Address: ");
    readMacAddress();

    esp_err_t result = esp_now_register_recv_cb(data_received);
    if (result != ESP_OK) {
        Serial.printf("Error registering receive callback: 0x%02X (%s)\n", result, esp_err_to_name(result));
        return;
    }
}

void loop() {
    delay(500);
}
*/