#include "ble_service.h"
#include "esp_log.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLECharacteristic.h"
#include "BLE2902.h"

static const char *TAG = "ble";

// Custom 128-bit UUIDs
#define SERVICE_UUID           "DEAD0001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TARGET_TYPE  "DEAD0002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_SCORE        "DEAD0003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_STATUS       "DEAD0004-B5A3-F393-E0A9-E50E24DCCA9E"

static BLEServer *s_server = nullptr;
static BLECharacteristic *s_score_char = nullptr;
static BLECharacteristic *s_status_char = nullptr;
static bool s_device_connected = false;
static target_type_t s_target_type = TARGET_TYPE_PISTOL;

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        s_device_connected = true;
        ESP_LOGI(TAG, "BLE client connected");
    }
    void onDisconnect(BLEServer *pServer) override {
        s_device_connected = false;
        ESP_LOGI(TAG, "BLE client disconnected, restarting advertising");
        pServer->startAdvertising();
    }
};

class TargetTypeCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        std::string val = pChar->getValue();
        if (val.length() == 1) {
            s_target_type = (target_type_t)val[0];
            ESP_LOGI(TAG, "Target type set: %s",
                     s_target_type == TARGET_TYPE_PISTOL ? "Pistol" : "Rifle");
        }
    }
};

esp_err_t ble_service_init(void)
{
    BLEDevice::init("DeadEyeShot");

    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());

    BLEService *pService = s_server->createService(SERVICE_UUID);

    // Score TX (Notify)
    s_score_char = pService->createCharacteristic(
        CHAR_UUID_SCORE,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    s_score_char->addDescriptor(new BLE2902());

    // Status TX (Notify)
    s_status_char = pService->createCharacteristic(
        CHAR_UUID_STATUS,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    s_status_char->addDescriptor(new BLE2902());

    // Target Type RX (Write)
    BLECharacteristic *target_char = pService->createCharacteristic(
        CHAR_UUID_TARGET_TYPE,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    target_char->setCallbacks(new TargetTypeCallbacks());

    pService->start();
    s_server->getAdvertising()->start();

    ESP_LOGI(TAG, "BLE service init OK, advertising as 'DeadEyeShot'");
    return ESP_OK;
}

void ble_notify_score(float score)
{
    if (!s_device_connected || !s_score_char) return;
    s_score_char->setValue((uint8_t *)&score, sizeof(score));
    s_score_char->notify();
}

void ble_notify_status(uint8_t status)
{
    if (!s_device_connected || !s_status_char) return;
    s_status_char->setValue(&status, 1);
    s_status_char->notify();
}

target_type_t ble_get_target_type(void)
{
    return s_target_type;
}
