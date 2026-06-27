#include "ble_service.h"
#include "wifi_station.h"
#include "esp_log.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLECharacteristic.h"
#include "BLE2902.h"
#include <string>
#include <cstring>

static const char *TAG = "ble";

#define SERVICE_UUID      "DEAD0001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_SSID    "DEAD0002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_PASS    "DEAD0003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_APPLY   "DEAD0004-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_STATUS  "DEAD0005-B5A3-F393-E0A9-E50E24DCCA9E"

static BLEServer *s_server = nullptr;
static BLECharacteristic *s_status_char = nullptr;
static bool s_device_connected = false;
static std::string s_pending_ssid;
static std::string s_pending_pass;

static void update_status(const char *status)
{
    if (!s_status_char) {
        return;
    }
    s_status_char->setValue((uint8_t *)status, strlen(status));
    if (s_device_connected) {
        s_status_char->notify();
    }
}

static void on_wifi_status_changed(const char *status)
{
    update_status(status);
}

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        s_device_connected = true;
        update_status(wifi_get_status_text());
        ESP_LOGI(TAG, "BLE provisioning client connected");
    }

    void onDisconnect(BLEServer *pServer) override {
        s_device_connected = false;
        ESP_LOGI(TAG, "BLE provisioning client disconnected, restarting advertising");
        pServer->startAdvertising();
    }
};

class SsidCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        String value = characteristic->getValue();
        s_pending_ssid = value.c_str();
        ESP_LOGI(TAG, "BLE provisioning SSID received (%u bytes)", (unsigned)s_pending_ssid.size());
        update_status("ssid_received");
    }
};

class PasswordCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        String value = characteristic->getValue();
        s_pending_pass = value.c_str();
        ESP_LOGI(TAG, "BLE provisioning password received (%u bytes)", (unsigned)s_pending_pass.size());
        update_status("password_received");
    }
};

class ApplyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        if (s_pending_ssid.empty()) {
            update_status("missing_ssid");
            ESP_LOGW(TAG, "BLE provisioning apply ignored: missing SSID");
            return;
        }

        esp_err_t err = wifi_set_credentials(s_pending_ssid.c_str(), s_pending_pass.c_str());
        if (err == ESP_OK) {
            update_status("connecting");
            ESP_LOGI(TAG, "BLE provisioning credentials saved");
        } else {
            update_status("provision_failed");
            ESP_LOGE(TAG, "BLE provisioning failed: 0x%x", err);
        }
    }
};

esp_err_t ble_service_init(void)
{
    BLEDevice::init("DeadEyeShot-Setup");

    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new ServerCallbacks());

    BLEService *service = s_server->createService(SERVICE_UUID);

    BLECharacteristic *ssid_char = service->createCharacteristic(
        CHAR_UUID_SSID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    ssid_char->setCallbacks(new SsidCallbacks());

    BLECharacteristic *pass_char = service->createCharacteristic(
        CHAR_UUID_PASS,
        BLECharacteristic::PROPERTY_WRITE
    );
    pass_char->setCallbacks(new PasswordCallbacks());

    BLECharacteristic *apply_char = service->createCharacteristic(
        CHAR_UUID_APPLY,
        BLECharacteristic::PROPERTY_WRITE
    );
    apply_char->setCallbacks(new ApplyCallbacks());

    s_status_char = service->createCharacteristic(
        CHAR_UUID_STATUS,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    s_status_char->addDescriptor(new BLE2902());
    update_status(wifi_get_status_text());

    service->start();
    s_server->getAdvertising()->addServiceUUID(SERVICE_UUID);
    s_server->getAdvertising()->start();
    wifi_set_status_callback(on_wifi_status_changed);

    ESP_LOGI(TAG, "BLE provisioning service ready as 'DeadEyeShot-Setup'");
    return ESP_OK;
}
