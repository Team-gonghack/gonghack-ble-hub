#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLE2902.h>
#include <Arduino.h>

#define SOURCE_MAC "1C:69:20:E2:6D:2A"
#define SOURCE_SERVICE_UUID "180D"
#define SOURCE_CHAR_UUID "2A37"
#define LED_CTRL_UUID "2A56"

#define RELAY_SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define RELAY_CHAR_UUID "abcdefab-cdef-1234-5678-1234567890ab"

BLEClient *pClient = nullptr;
BLERemoteCharacteristic *pRemoteBpmChar = nullptr;
BLERemoteCharacteristic *pRemoteLedChar = nullptr;
BLECharacteristic *pRelayChar = nullptr;

bool connectedToWearable = false;
bool smartphoneConnected = false;
SemaphoreHandle_t bpmMutex;

uint8_t latestBPM = 0;
int latestValue = 0;

class MyNotifyCallback {
public:
  void operator()(BLERemoteCharacteristic *pChar, uint8_t *pData, size_t len, bool isNotify) {
    if (len > 0) {
      uint8_t bpm = pData[0];
      if (xSemaphoreTake(bpmMutex, portMAX_DELAY)) {
        latestBPM = bpm;
        xSemaphoreGive(bpmMutex);
      }
      Serial.printf("💓 BPM from wearable: %d\n", bpm);
    }
  }
};

class RelayServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    smartphoneConnected = true;
    Serial.println("📱 Smartphone connected");
  }
  void onDisconnect(BLEServer *pServer) override {
    smartphoneConnected = false;
    pServer->getAdvertising()->start();
    Serial.println("📴 Smartphone disconnected");
  }
};

bool connectToWearable() {
  BLEAddress srcAddr(SOURCE_MAC);
  pClient = BLEDevice::createClient();
  if (!pClient->connect(srcAddr)) return false;

  BLERemoteService *pService = pClient->getService(SOURCE_SERVICE_UUID);
  if (!pService) return false;

  pRemoteBpmChar = pService->getCharacteristic(SOURCE_CHAR_UUID);
  pRemoteLedChar = pService->getCharacteristic(LED_CTRL_UUID);
  if (pRemoteBpmChar && pRemoteBpmChar->canNotify())
    pRemoteBpmChar->registerForNotify(MyNotifyCallback());
  connectedToWearable = true;
  Serial.println("✅ Connected to wearable");
  return true;
}

void setupRelayServer() {
  BLEDevice::init("ESP32_BPM_Relay");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new RelayServerCallbacks());
  BLEService *pService = pServer->createService(RELAY_SERVICE_UUID);

  pRelayChar = pService->createCharacteristic(RELAY_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pRelayChar->addDescriptor(new BLE2902());
  pService->start();
  pServer->getAdvertising()->start();
}

void setup() {
  Serial.begin(115200);
  bpmMutex = xSemaphoreCreateMutex();
  setupRelayServer();
  connectToWearable();
}

void loop() {
  // ① TX/RX로부터 값 수신
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    latestValue = input.toInt();
    Serial.printf("📨 Received integer via UART: %d\n", latestValue);

    // ② 웨어러블로 전송
    if (connectedToWearable && pRemoteLedChar) {
      uint8_t v = latestValue;
      pRemoteLedChar->writeValue(&v, 1);
      Serial.printf("➡️ Sent integer to wearable: %d\n", v);
    }
  }

  // ③ 스마트폰으로 BPM + 정수 함께 전송
  if (smartphoneConnected && pRelayChar) {
    uint8_t bpmCopy = 0;
    if (xSemaphoreTake(bpmMutex, 0) == pdTRUE) {
      bpmCopy = latestBPM;
      xSemaphoreGive(bpmMutex);
    }

    uint8_t data[2] = { bpmCopy, (uint8_t)latestValue };
    pRelayChar->setValue(data, 2);
    pRelayChar->notify();
    Serial.printf("📤 Relayed [BPM:%d | VAL:%d] to phone\n", bpmCopy, latestValue);
  }

  delay(1000);
}
