#include "ble_service.h"
#include "wifi_station.h"
#include "esp_log.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEAdvertising.h"
#include "BLECharacteristic.h"
#include "BLE2902.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include <cstring>

static const char *TAG = "ble";

#define SERVICE_UUID      "DEAD0001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_SSID    "DEAD0002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_PASS    "DEAD0003-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_APPLY   "DEAD0004-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_STATUS  "DEAD0005-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_NAME    "DEAD0006-B5A3-F393-E0A9-E50E24DCCA9E"
#define DEVICE_NAME_MAX_BYTES 24

static const char *NVS_NAMESPACE = "ble_cfg";
static const char *NVS_KEY_DEVICE_NAME = "device_name";

static BLEServer *s_server = nullptr;
static BLECharacteristic *s_status_char = nullptr;
static bool s_device_connected = false;
static std::string s_pending_ssid;
static std::string s_pending_pass;
static std::string s_device_name;
static TaskHandle_t s_advertising_task = nullptr;
static TaskHandle_t s_status_notify_task = nullptr;

static void restart_advertising_task(void *arg)
{
    BLEServer *server = static_cast<BLEServer *>(arg);
    vTaskDelay(pdMS_TO_TICKS(500));

    BLEAdvertising *advertising = server ? server->getAdvertising() : nullptr;
    bool stopped = advertising && advertising->stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    bool started = advertising && advertising->start();
    ESP_LOGI(TAG, "BLE advertising reset after disconnect: stop=%s start=%s",
             stopped ? "ok" : "failed", started ? "requested" : "failed");

    s_advertising_task = nullptr;
    vTaskDelete(nullptr);
}

static std::string default_device_name(void)
{
    uint8_t mac[6] = {};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));

    uint32_t hash = 2166136261u;
    for (uint8_t byte : mac) {
        hash ^= byte;
        hash *= 16777619u;
    }

    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char suffix[7] = {};
    for (int i = 5; i >= 0; --i) {
        suffix[i] = alphabet[hash & 31u];
        hash >>= 5;
    }
    return std::string("DeadEyeShot-") + suffix;
}

static std::string load_device_name(void)
{
    char value[DEVICE_NAME_MAX_BYTES + 1] = {};
    size_t length = sizeof(value);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, NVS_KEY_DEVICE_NAME, value, &length);
        nvs_close(nvs);
        if (err == ESP_OK && value[0] != '\0') {
            return value;
        }
    }
    return default_device_name();
}

static esp_err_t save_device_name(const std::string &name)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, NVS_KEY_DEVICE_NAME, name.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool valid_device_name(const std::string &name)
{
    if (name.empty() || name.size() > DEVICE_NAME_MAX_BYTES) {
        return false;
    }
    for (unsigned char byte : name) {
        if (byte < 0x20 || byte == 0x7f) {
            return false;
        }
    }
    return true;
}

static void restart_after_name_change(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void update_status(const char *status);

static void notify_current_status_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    if (s_device_connected) {
        update_status(wifi_get_status_text());
    }
    s_status_notify_task = nullptr;
    vTaskDelete(nullptr);
}

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
        if (!s_status_notify_task) {
            xTaskCreate(notify_current_status_task, "ble_status_notify", 2048,
                        nullptr, 3, &s_status_notify_task);
        }
        ESP_LOGI(TAG, "BLE provisioning client connected");
    }

    void onDisconnect(BLEServer *pServer) override {
        s_device_connected = false;
        ESP_LOGI(TAG, "BLE provisioning client disconnected, scheduling advertising reset");
        if (!s_advertising_task) {
            xTaskCreate(restart_advertising_task, "ble_adv_reset", 3072,
                        pServer, 3, &s_advertising_task);
        }
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

class DeviceNameCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        String value = characteristic->getValue();
        std::string name = value.c_str();
        if (!valid_device_name(name)) {
            update_status("device_name_invalid");
            ESP_LOGW(TAG, "BLE device name rejected (%u bytes)", (unsigned)name.size());
            return;
        }

        esp_err_t err = save_device_name(name);
        if (err != ESP_OK) {
            update_status("device_name_write_failed");
            ESP_LOGE(TAG, "BLE device name save failed: 0x%x", err);
            return;
        }

        s_device_name = name;
        characteristic->setValue((uint8_t *)s_device_name.data(), s_device_name.size());
        update_status("device_name_saved");
        ESP_LOGI(TAG, "BLE device name changed to '%s', restarting", s_device_name.c_str());
        xTaskCreate(restart_after_name_change, "ble_name_restart", 2048, nullptr, 3, nullptr);
    }
};

esp_err_t ble_service_init(void)
{
    s_device_name = load_device_name();
    BLEDevice::init(s_device_name.c_str());

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

    BLECharacteristic *name_char = service->createCharacteristic(
        CHAR_UUID_NAME,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    name_char->setValue((uint8_t *)s_device_name.data(), s_device_name.size());
    name_char->setCallbacks(new DeviceNameCallbacks());

    service->start();
    s_server->getAdvertising()->addServiceUUID(SERVICE_UUID);
    s_server->getAdvertising()->start();
    wifi_set_status_callback(on_wifi_status_changed);

    ESP_LOGI(TAG, "BLE provisioning service ready as '%s'", s_device_name.c_str());
    return ESP_OK;
}
