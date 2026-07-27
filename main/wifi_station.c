#include "wifi_station.h"
#include "buzzer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi";
static const char *NVS_NAMESPACE = "wifi_cfg";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";

static bool s_connected = false;
static bool s_started = false;
static bool s_reconfiguring = false;
static bool s_has_credentials = false;
static char s_ssid[33] = {0};
static char s_password[65] = {0};
static char s_status[64] = "not_configured";
static wifi_status_callback_t s_status_callback = NULL;

static void set_status(const char *status)
{
    if (strncmp(s_status, status, sizeof(s_status)) == 0) {
        return;
    }
    strncpy(s_status, status, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    if (s_status_callback) {
        s_status_callback(s_status);
    }
}

static esp_err_t load_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        s_has_credentials = false;
        set_status("not_configured");
        return err;
    }

    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_password);
    err = nvs_get_str(nvs, NVS_KEY_SSID, s_ssid, &ssid_len);
    if (err == ESP_OK) {
        esp_err_t pass_err = nvs_get_str(nvs, NVS_KEY_PASS, s_password, &pass_len);
        if (pass_err != ESP_OK) {
            s_password[0] = '\0';
        }
    }
    nvs_close(nvs);

    s_has_credentials = (err == ESP_OK && s_ssid[0] != '\0');
    set_status(s_has_credentials ? "loaded" : "not_configured");
    return err;
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t apply_credentials(void)
{
    if (!s_has_credentials) {
        ESP_LOGW(TAG, "WiFi credentials are not configured");
        set_status("not_configured");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);

    bool restart_wifi = s_started;
    if (restart_wifi) {
        s_reconfiguring = true;
        s_connected = false;
        ESP_LOGI(TAG, "Stopping current WiFi connection before applying new credentials");
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK) {
            s_reconfiguring = false;
            set_status("stop_failed");
            return stop_err;
        }
        s_started = false;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        s_reconfiguring = false;
        set_status("config_failed");
        return err;
    }

    if (restart_wifi) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            s_reconfiguring = false;
            set_status("start_failed");
            return err;
        }
    }
    set_status("connecting");
    ESP_LOGI(TAG, "Connecting to %s...", s_ssid);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_started = true;
        s_reconfiguring = false;
        if (s_has_credentials) {
            esp_wifi_connect();
            set_status("connecting");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        s_started = false;
        s_connected = false;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_reconfiguring) {
            ESP_LOGI(TAG, "WiFi disconnected for credential update");
            return;
        }
        ESP_LOGW(TAG, "WiFi disconnected%s", s_has_credentials ? ", reconnecting..." : "");
        s_connected = false;
        set_status(s_has_credentials ? "disconnected" : "not_configured");
        if (s_has_credentials) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        bool newly_connected = !s_connected;
        s_connected = true;
        if (newly_connected) {
            buzzer_play_async(BUZZER_MELODY_WIFI_CONNECTED);
        }
        char status[64];
        snprintf(status, sizeof(status), "connected:" IPSTR, IP2STR(&event->ip_info.ip));
        set_status(status);
    }
}

esp_err_t wifi_init(void)
{
    load_credentials();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_has_credentials) {
        ESP_ERROR_CHECK(apply_credentials());
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (!s_has_credentials) {
        ESP_LOGW(TAG, "WiFi credentials missing; use BLE provisioning");
    }
    return ESP_OK;
}

esp_err_t wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        set_status("invalid_credentials");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_password, password ? password : "", sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';
    s_has_credentials = true;

    esp_err_t err = save_credentials(s_ssid, s_password);
    if (err != ESP_OK) {
        set_status("save_failed");
        return err;
    }
    return apply_credentials();
}

bool wifi_is_connected(void)
{
    return s_connected;
}

bool wifi_has_credentials(void)
{
    return s_has_credentials;
}

const char *wifi_get_status_text(void)
{
    return s_status;
}

void wifi_set_status_callback(wifi_status_callback_t callback)
{
    s_status_callback = callback;
}
