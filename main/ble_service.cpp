#include "ble_service.h"
#include "esp_log.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLECharacteristic.h"
#include "BLE2902.h"

static const char *TAG = "ble";

// Custom 128-bit UUIDs
#define SERVICE_UUID           "DEAD0001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_STATUS       "DEAD0004-B5A3-F393-E0A9-E50E24DCCA9E"

static BLEServer *s_server = nullptr;
static BLECharacteristic *s_status_char = nullptr;
static bool s_device_connected = false;

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

esp_err_t ble_service_init(void)
{
    BLEDevice::init("DeadEyeShot");

    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());

    BLEService *pService = s_server->createService(SERVICE_UUID);

    // Status TX (Notify)
    s_status_char = pService->createCharacteristic(
        CHAR_UUID_STATUS,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    s_status_char->addDescriptor(new BLE2902());

    pService->start();
    s_server->getAdvertising()->start();

    ESP_LOGI(TAG, "BLE service init OK, advertising as 'DeadEyeShot'");
    return ESP_OK;
}

void ble_notify_status(uint8_t status)
{
    if (!s_device_connected || !s_status_char) return;
    s_status_char->setValue(&status, 1);
    s_status_char->notify();
}
