#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <NimBLEAdvertisementData.h>

// ---------- I2C pins & ICM20600 config ----------
#define I2C_SDA 5     // working pins for your Xiao ESP32S3
#define I2C_SCL 6
#define ICM20600_ADDR 0x69

#define RECORDING_PIN D9

#define REG_PWR_MGMT_1   0x6B
#define REG_ACCEL_CONFIG 0x1C
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_XOUT_H 0x3B
#define REG_WHO_AM_I     0x75

// conversions for ±2g accel, ±250 dps gyro
static const float ACC_LSB_PER_G   = 16384.0f; // ±2g
static const float GYR_LSB_PER_DPS = 131.0f;   // ±250 dps

// ---------- BLE UUIDs required by HTML ----------
static const NimBLEUUID SVC_UUID ((uint16_t)0xFF00);
static const NimBLEUUID IMU_UUID ((uint16_t)0xFF01);  // notify
static const NimBLEUUID CTRL_UUID((uint16_t)0xFF02);  // write

// ---------- Globals ----------
NimBLECharacteristic* imuChar = nullptr;
volatile bool streamOn = true;        // toggled by control writes
const uint32_t PERIOD_MS = 25;         // ~40 Hz

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

// ---------- BLE callbacks ----------
class CbServer : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) {
    Serial.println("[BLE] Central connected");
  }
  void onDisconnect(NimBLEServer*) {
    Serial.println("[BLE] Central disconnected");
    NimBLEDevice::startAdvertising();
  }
};

// no 'override' → works with more NimBLE versions
class CbCtrl : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) {
    auto v = c->getValue();
    if (v.size() > 0) {
      streamOn = (v[0] != 0);  // 0x01 = start, 0x00 = stop
      Serial.printf("[CTRL] streamOn = %d\n", streamOn ? 1 : 0);
    }
  }
};

volatile bool isRecording = false;

// Debounce settings
const unsigned long DEBOUNCE_MS = 100;

// For a toggle switch (maintained state) we sample the pin and debounce
int last_switch_state = HIGH;           // INPUT_PULLUP default
unsigned long last_debounce_time = 0;

// (no header-state tracking) we always explicitly send a header on state change

void setup() {
  pinMode(RECORDING_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  for (int i = 0; i < 60 && !Serial; i++) delay(20);
  Serial.println("\n[BOOT] Computer Compatible Tennis Racket");

  // I2C + IMU
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  uint8_t who = 0;
  if (icmInit(who)) {
    Serial.printf("[I2C] ICM20600 OK, WHO_AM_I=0x%02X (addr 0x%02X)\n",
                  who, ICM20600_ADDR);
  } else {
    Serial.println("[I2C] ICM init FAILED");
  }

  // BLE setup
  NimBLEDevice::init("SwingSense");
  NimBLEDevice::setMTU(128);             // IMPORTANT for 36-byte packets
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(false,false,false);

  auto server = NimBLEDevice::createServer();
  server->setCallbacks(new CbServer());

  auto svc = server->createService(SVC_UUID);
  imuChar = svc->createCharacteristic(
      IMU_UUID, NIMBLE_PROPERTY::NOTIFY
  );
  auto ctrl = svc->createCharacteristic(
      CTRL_UUID, NIMBLE_PROPERTY::WRITE
  );
  ctrl->setCallbacks(new CbCtrl());
  svc->start();

  auto adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  NimBLEAdvertisementData scanResp;
  scanResp.setName("SwingSense");
  // Add a short manufacturer data field (two bytes) to make discovery reliable
  // Clients can match this instead of relying on the local name.
  std::string mfg;
  mfg.resize(2);
  mfg[0] = (char)0x12; // arbitrary identifier byte 1
  mfg[1] = (char)0x34; // arbitrary identifier byte 2
  scanResp.setManufacturerData(mfg);
  adv->setScanResponseData(scanResp);
  adv->setName("SwingSense");
  // Also set the manufacturer data on the primary advertisement packet
  adv->setManufacturerData(mfg);
  adv->start();
  Serial.println("[BLE] Advertising as SwingSense with svc 0xFF00");
}

void loop() {
  static uint32_t last = 0;

  if (!streamOn) {
    delay(5);
    return;
  }

  uint32_t now = millis();
  if (now - last < PERIOD_MS) {
    delay(1);
    return;
  }
  last = now;

  float ax, ay, az, gx, gy, gz;
  if (!icmRead(ax, ay, az, gx, gy, gz)) return;

  float vals[6] = { ax, ay, az, gx, gy, gz };

  // --- Read toggle switch (debounced) ---
  int pin_read = digitalRead(RECORDING_PIN);
  if (pin_read != last_switch_state) {
    last_debounce_time = millis();
    last_switch_state = pin_read;
  }

  bool justToggled = false;

  if (millis() - last_debounce_time > DEBOUNCE_MS) {
    bool newRecording = (pin_read == LOW); // active-low
    if (newRecording != isRecording) {
      isRecording = newRecording;
      justToggled = true;
      Serial.printf("[REC] isRecording = %d\n", isRecording ? 1 : 0);

      if (imuChar) {
        uint8_t hdrByte = isRecording ? 0x01 : 0x02;
        imuChar->setValue(&hdrByte, 1);
        imuChar->notify();  // send exactly ONE header per toggle
        Serial.printf("[REC] Sent header: 0x%02X (isRecording=%d)\n",
                      hdrByte, isRecording ? 1 : 0);
      }
    }
  }

  // Only send IMU data when recording and NOT on the exact toggle iteration
  if (isRecording && !justToggled && imuChar) {
    imuChar->setValue((uint8_t*)vals, sizeof(vals));  // 24-byte data packet
    imuChar->notify();
  }
}

