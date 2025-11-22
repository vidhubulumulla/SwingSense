/*
#include <Arduino.h>
#include <Wire.h>
//#include <NimBLEDevice.h>
//#include <NimBLEAdvertisementData.h> // use ESP NOW to send btwn 2 ESP32s
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ---------- I2C pins & ICM20600 config ----------
#define I2C_SDA 5     // working pins for your Xiao ESP32S3
#define I2C_SCL 6
#define ICM20600_ADDR 0x69

#define REG_PWR_MGMT_1   0x6B
#define REG_ACCEL_CONFIG 0x1C
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_XOUT_H 0x3B
#define REG_WHO_AM_I     0x75

// conversions for ±2g accel, ±250 dps gyro
static const float ACC_LSB_PER_G   = 16384.0f; // ±2g
static const float GYR_LSB_PER_DPS = 131.0f;   // ±250 dps


// ---------- Globals ----------
const uint32_t PERIOD_MS = 200;         // ~40 Hz

// ---------- I2C helpers ----------
static bool i2cWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission((uint8_t)ICM20600_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool i2cReadBytes(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission((uint8_t)ICM20600_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  // force the uint8_t,uint8_t overload to avoid ambiguity
  if (Wire.requestFrom((uint8_t)ICM20600_ADDR, (uint8_t)len) != (int)len) return false;
  for (size_t i = 0; i < len; ++i) {
    buf[i] = Wire.read();
  }
  return true;
}

static bool icmInit(uint8_t& who) {
  who = 0;
  if (!i2cWrite(REG_PWR_MGMT_1, 0x01)) return false;  // wake, PLL
  delay(50);
  if (!i2cWrite(REG_ACCEL_CONFIG, 0x00)) return false; // ±2g
  if (!i2cWrite(REG_GYRO_CONFIG,  0x00)) return false; // ±250 dps
  delay(10);
  return i2cReadBytes(REG_WHO_AM_I, &who, 1);
}

static bool icmRead(float& ax,float& ay,float& az,
                    float& gx,float& gy,float& gz) {
  uint8_t raw[14];
  if (!i2cReadBytes(REG_ACCEL_XOUT_H, raw, sizeof(raw))) return false;

  auto s16 = [&](int i)->int16_t {
    return (int16_t)((raw[i] << 8) | raw[i+1]);
  };

  ax = s16(0)  / ACC_LSB_PER_G;
  ay = s16(2)  / ACC_LSB_PER_G;
  az = s16(4)  / ACC_LSB_PER_G;

  gx = s16(8)  / GYR_LSB_PER_DPS;
  gy = s16(10) / GYR_LSB_PER_DPS;
  gz = s16(12) / GYR_LSB_PER_DPS;

  return true;
}

typedef struct data_packet {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
} data_packet;

data_packet data = {};

esp_now_peer_info_t receiver; 

uint8_t rxMACaddr[] = {0xB8,0xF8,0x62,0xF9,0xEF,0x64}; // put rx MAC address: b8:f8:62:f9:ef:64

// simple thread-safe handoff for send callback printing
volatile bool send_event_pending = false;
uint8_t send_event_mac[6];
volatile esp_now_send_status_t send_event_status;

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

// send callback: do minimal work and stash result for the main loop to print
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // copy mac and status to globals and mark event pending
  for (int i = 0; i < 6; ++i) send_event_mac[i] = mac_addr[i];
  send_event_status = status;
  send_event_pending = true;
}

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA, I2C_SCL, 400000);
    uint8_t who = 0;
    if (icmInit(who)) {
        Serial.printf("[I2C] ICM20600 OK, WHO_AM_I=0x%02X (addr 0x%02X)\n",
                  who, ICM20600_ADDR);
    } else {
        Serial.println("[I2C] ICM init FAILED");
    }

    WiFi.mode(WIFI_STA);
    // Ensure transmitter uses same Wi-Fi channel as receiver
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

    memcpy(receiver.peer_addr, rxMACaddr, ESP_NOW_ETH_ALEN);
    receiver.channel = 1; // match the esp_wifi_set_channel value above
    receiver.encrypt = false;

    esp_now_init(); 

    if (esp_now_add_peer(&receiver) != ESP_OK){
        Serial.println("Failed to add peer");
        return;
    }

    // register send callback to get delivery status
    esp_err_t reg = esp_now_register_send_cb(OnDataSent);
    if (reg != ESP_OK) {
      //Serial.printf("Failed to register send callback: 0x%02X\n", reg);
    } else {
      //Serial.println("Send callback registered");
    }
}

void loop() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < PERIOD_MS) {
        delay(1);
        return;
    }
    last = now;

    float ax, ay, az, gx, gy, gz;
    if (!icmRead(ax, ay, az, gx, gy, gz)) return;

    data.ax += ax * PERIOD_MS * 0.001f; // simple integration to get rough velocity/angle
    data.ay += ay * PERIOD_MS * 0.001f;
    data.az += az * PERIOD_MS * 0.001f;
    data.gx += gx * PERIOD_MS * 0.001f;
    data.gy += gy * PERIOD_MS * 0.001f;
    data.gz += gz * PERIOD_MS * 0.001f;
    
    esp_err_t result = esp_now_send(rxMACaddr, (uint8_t *) &data, sizeof(data));
    //Serial.printf("esp_now_send result: 0x%02X (%s)\n", result, esp_err_to_name(result));
    if (result != ESP_OK) {
      // Serial.println("failed to send data");
    } else {
      Serial.printf("Sent: a[%.2f, %.2f, %.2f] g[%.2f, %.2f, %.2f]\n",
              data.ax, data.ay, data.az,
              data.gx, data.gy, data.gz);
    }
    // print any pending send callback info (stashed from callback)
    if (send_event_pending) {
      // copy flag under assumption of single-writer (callback) single-reader (loop)
      uint8_t mac[6];
      for (int i = 0; i < 6; ++i) mac[i] = send_event_mac[i];
      esp_now_send_status_t st = send_event_status;
      send_event_pending = false;
    
      //Serial.print("Send callback - To: ");
      //for (int i = 0; i < 6; ++i) {
        //Serial.printf("%02X", mac[i]);
        //if (i < 5) Serial.print(":");
      //}
      //Serial.print("  ");
      if (st == ESP_NOW_SEND_SUCCESS) {
        //Serial.println("Status: ESP_NOW_SEND_SUCCESS");
      } else {
        //Serial.println("Status: ESP_NOW_SEND_FAIL");
      }
    }
    //Serial.printf("Sent: a[%.2f, %.2f, %.2f] g[%.2f, %.2f, %.2f]\n",
    //              data.ax, data.ay, data.az,
    //              data.gx, data.gy, data.gz);
    
}

*/