#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLE2902.h>
#include <Arduino.h>

// ==================== UUID 설정 ====================
#define SOURCE_MAC          "1C:69:20:E2:6D:2A"
#define SOURCE_SERVICE_UUID "180D"
#define SOURCE_CHAR_UUID    "2A37"

#define RELAY_SERVICE_UUID  "12345678-1234-5678-1234-56789abcdef0"
#define RELAY_CHAR_UUID     "abcdefab-cdef-1234-5678-1234567890ab"

// ==================== BLE 객체 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pRemoteChar = nullptr;
BLECharacteristic* pRelayChar = nullptr;
bool connectedToSource = false;
bool clientConnected = false;

// FreeRTOS 핸들
TaskHandle_t TaskConnectSourceHandle;
TaskHandle_t TaskRelayNotifyHandle;

// 수신된 BPM 값을 안전하게 공유하기 위한 뮤텍스
SemaphoreHandle_t bpmMutex;
uint8_t latestBPM = 0;

// ==================== Notify 콜백 ====================
class MyNotifyCallback {
public:
  void operator()(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length > 0) {
      uint8_t bpmValue = pData[0];
      Serial.printf("📡 Received BPM from source: %d\n", bpmValue);

      // 뮤텍스로 BPM 값 보호
      if (xSemaphoreTake(bpmMutex, portMAX_DELAY) == pdTRUE) {
        latestBPM = bpmValue;
        xSemaphoreGive(bpmMutex);
      }
    }
  }
};

// ==================== BLE 서버 콜백 ====================
class RelayServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    Serial.println("📱 Smartphone connected to relay server");
    clientConnected = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    Serial.println("📴 Smartphone disconnected");
    clientConnected = false;
    pServer->getAdvertising()->start();
  }
};

// ==================== 릴레이 서버 초기화 ====================
void setupRelayServer() {
  BLEDevice::init("ESP32_BPM_Relay");

  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new RelayServerCallbacks());

  BLEService* pService = pServer->createService(RELAY_SERVICE_UUID);
  pRelayChar = pService->createCharacteristic(
                  RELAY_CHAR_UUID,
                  BLECharacteristic::PROPERTY_NOTIFY
                );
  pRelayChar->addDescriptor(new BLE2902());
  pService->start();

  pServer->getAdvertising()->start();
  Serial.println("🚀 Relay BLE Server started (waiting for smartphone)");
}

// ==================== 원본 ESP32 연결 ====================
bool connectToSource() {
  Serial.printf("🔗 Connecting to source: %s\n", SOURCE_MAC);

  BLEAddress srcAddr(SOURCE_MAC);
  pClient = BLEDevice::createClient();

  if (!pClient->connect(srcAddr)) {
    Serial.println("❌ Failed to connect to source server");
    return false;
  }

  Serial.println("✅ Connected to source server");
  BLERemoteService* pService = pClient->getService(SOURCE_SERVICE_UUID);
  if (!pService) {
    Serial.println("❌ Source service not found");
    pClient->disconnect();
    return false;
  }

  pRemoteChar = pService->getCharacteristic(SOURCE_CHAR_UUID);
  if (!pRemoteChar) {
    Serial.println("❌ Source characteristic not found");
    pClient->disconnect();
    return false;
  }

  if (pRemoteChar->canNotify()) {
    pRemoteChar->registerForNotify(MyNotifyCallback());
    Serial.println("🔔 Notify registered from source");
  }

  connectedToSource = true;
  return true;
}

// ==================== Task: 원본 ESP32 연결 및 유지 ====================
void TaskConnectSource(void* pvParameters) {
  for (;;) {
    if (!connectedToSource) {
      if (connectToSource()) {
        Serial.println("📡 Source reconnected");
      } else {
        Serial.println("🔄 Retrying connection to source...");
      }
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS); // 5초마다 재시도
  }
}

// ==================== Task: 릴레이 Notify 전송 ====================
void TaskRelayNotify(void* pvParameters) {
  uint8_t bpmCopy = 0;
  for (;;) {
    if (clientConnected && pRelayChar) {
      if (xSemaphoreTake(bpmMutex, 0) == pdTRUE) {
        bpmCopy = latestBPM;
        xSemaphoreGive(bpmMutex);
      }
      if (bpmCopy > 0) {
        pRelayChar->setValue(&bpmCopy, 1);
        pRelayChar->notify();
        Serial.printf("📤 Relayed BPM: %d\n", bpmCopy);
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); // 1초마다 전송
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32 BLE Relay with FreeRTOS ===");

  bpmMutex = xSemaphoreCreateMutex();
  setupRelayServer();

  // BLE Client(소스 연결) Task → Core 0
  xTaskCreatePinnedToCore(TaskConnectSource, "TaskConnectSource", 4096, NULL, 1, &TaskConnectSourceHandle, 0);

  // BLE Notify 전송 Task → Core 1
  xTaskCreatePinnedToCore(TaskRelayNotify, "TaskRelayNotify", 4096, NULL, 1, &TaskRelayNotifyHandle, 1);
}

void loop() {
  vTaskDelay(100 / portTICK_PERIOD_MS);
}
