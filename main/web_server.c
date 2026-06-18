#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web";

#define SHOT_QUEUE_CAPACITY 8

static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_shot_mutex = NULL;

typedef struct {
    uint32_t id;
    uint8_t *jpeg;
    int jpeg_len;
    int width;
    int height;
} shot_entry_t;

// Shared shot data protected by mutex
static shot_entry_t s_shots[SHOT_QUEUE_CAPACITY];
static int s_shot_start = 0;
static int s_shot_count = 0;
static uint32_t s_next_shot_id = 1;

// HTML page
static const char HTML_PAGE[] =
"<!DOCTYPE html><html><head>"
"<title>DeadEyeShot</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:sans-serif;text-align:center;background:#1a1a2e;color:#eee}"
"h1{color:#e94560}"
".s{font-size:2em;color:#0f3460;background:#e94560;padding:10px 20px;"
"border-radius:8px;display:inline-block;font-weight:bold}"
".w{position:relative;display:inline-block;border:2px solid #e94560;"
"border-radius:8px;margin:10px}"
".w img{display:block;border-radius:6px}"
".lb{color:#aaa;font-size:0.8em;margin-top:4px}"
"</style></head><body>"
"<h1>DeadEyeShot</h1>"
"<div id='sc' class='s'>WAITING</div>"
"<br>"
"<div class='w' id='wrap' style='display:none'>"
"<img id='pic' src='' width='320' height='240'>"
"</div>"
"<p id='info'></p>"
"<script>"
"function u(){"
"var r=new XMLHttpRequest();"
"r.onload=function(){"
"var d=JSON.parse(r.responseText);"
"if(d.has_shot){"
"document.getElementById('sc').textContent='CAPTURED';"
"document.getElementById('pic').src='/shot/'+d.id+'.jpg?t='+Date.now();"
"document.getElementById('wrap').style.display='inline-block';"
"document.getElementById('info').textContent="
"'#'+d.id+' '+d.w+'x'+d.h+' '+d.jpeg_len+' bytes';"
"}else{document.getElementById('sc').textContent='WAITING';}"
"}};"
"r.open('GET','/api/shot');r.send();"
"}"
"setInterval(u,2000);u();"
"</script></body></html>";

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static void free_shot_entry(shot_entry_t *entry)
{
    if (entry->jpeg) {
        free(entry->jpeg);
    }
    memset(entry, 0, sizeof(*entry));
}

static shot_entry_t *get_latest_shot(void)
{
    if (s_shot_count <= 0) {
        return NULL;
    }
    int index = (s_shot_start + s_shot_count - 1) % SHOT_QUEUE_CAPACITY;
    return &s_shots[index];
}

static shot_entry_t *find_shot_by_id(uint32_t id)
{
    for (int i = 0; i < s_shot_count; i++) {
        int index = (s_shot_start + i) % SHOT_QUEUE_CAPACITY;
        if (s_shots[index].id == id) {
            return &s_shots[index];
        }
    }
    return NULL;
}

static uint32_t get_oldest_id(void)
{
    if (s_shot_count <= 0) {
        return 0;
    }
    return s_shots[s_shot_start].id;
}

static uint32_t get_latest_id(void)
{
    shot_entry_t *latest = get_latest_shot();
    return latest ? latest->id : 0;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

static esp_err_t shot_jpg_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    shot_entry_t *shot = get_latest_shot();
    if (!shot || !shot->jpeg) {
        xSemaphoreGive(s_shot_mutex);
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    set_cors_headers(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, (const char *)shot->jpeg, shot->jpeg_len);
    xSemaphoreGive(s_shot_mutex);
    return err;
}

static esp_err_t shot_id_jpg_handler(httpd_req_t *req)
{
    unsigned long parsed_id = 0;
    if (sscanf(req->uri, "/shot/%lu.jpg", &parsed_id) != 1 || parsed_id == 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    uint32_t id = (uint32_t)parsed_id;

    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    shot_entry_t *shot = find_shot_by_id(id);
    if (!shot || !shot->jpeg) {
        xSemaphoreGive(s_shot_mutex);
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    set_cors_headers(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, (const char *)shot->jpeg, shot->jpeg_len);
    xSemaphoreGive(s_shot_mutex);
    return err;
}

static esp_err_t api_shot_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    char json[256];
    shot_entry_t *shot = get_latest_shot();
    if (shot) {
        snprintf(json, sizeof(json),
                 "{\"has_shot\":true,\"id\":%lu,\"latest_id\":%lu,\"w\":%d,\"h\":%d,\"jpeg_len\":%d}",
                 (unsigned long)shot->id, (unsigned long)shot->id,
                 shot->width, shot->height, shot->jpeg_len);
    } else {
        snprintf(json, sizeof(json), "{\"has_shot\":false,\"latest_id\":0}");
    }
    xSemaphoreGive(s_shot_mutex);

    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static uint32_t parse_after_query(httpd_req_t *req)
{
    char query[64];
    char value[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return 0;
    }
    if (httpd_query_key_value(query, "after", value, sizeof(value)) != ESP_OK) {
        return 0;
    }
    return (uint32_t)strtoul(value, NULL, 10);
}

static esp_err_t api_shot_events_handler(httpd_req_t *req)
{
    uint32_t after = parse_after_query(req);
    char json[1024];
    int offset = 0;

    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);

    uint32_t oldest_id = get_oldest_id();
    uint32_t latest_id = get_latest_id();
    bool overflow = (s_shot_count > 0 && after + 1 < oldest_id);

    offset += snprintf(json + offset, sizeof(json) - offset,
                       "{\"latest_id\":%lu,\"oldest_id\":%lu,\"events\":[",
                       (unsigned long)latest_id, (unsigned long)oldest_id);

    bool first = true;
    for (int i = 0; i < s_shot_count; i++) {
        int index = (s_shot_start + i) % SHOT_QUEUE_CAPACITY;
        shot_entry_t *shot = &s_shots[index];
        if (shot->id <= after) {
            continue;
        }
        offset += snprintf(json + offset, sizeof(json) - offset,
                           "%s{\"id\":%lu,\"w\":%d,\"h\":%d,\"jpeg_len\":%d}",
                           first ? "" : ",",
                           (unsigned long)shot->id, shot->width,
                           shot->height, shot->jpeg_len);
        first = false;
        if (offset >= (int)sizeof(json) - 128) {
            break;
        }
    }

    snprintf(json + offset, sizeof(json) - offset,
             "],\"overflow\":%s}", overflow ? "true" : "false");

    xSemaphoreGive(s_shot_mutex);

    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

esp_err_t web_server_init(void)
{
    s_shot_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 5;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ESP_FAIL;
    }

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t jpg_uri  = { .uri = "/shot.jpg", .method = HTTP_GET, .handler = shot_jpg_handler };
    httpd_uri_t id_jpg_uri  = { .uri = "/shot/*.jpg", .method = HTTP_GET, .handler = shot_id_jpg_handler };
    httpd_uri_t events_uri  = { .uri = "/api/shot/events", .method = HTTP_GET, .handler = api_shot_events_handler };
    httpd_uri_t api_uri  = { .uri = "/api/shot", .method = HTTP_GET, .handler = api_shot_handler };

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &jpg_uri);
    httpd_register_uri_handler(s_server, &id_jpg_uri);
    httpd_register_uri_handler(s_server, &events_uri);
    httpd_register_uri_handler(s_server, &api_uri);

    ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

void web_server_update_shot(const uint8_t *jpeg_data, int jpeg_len,
                            int width, int height)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);

    if (jpeg_data && jpeg_len > 0) {
        if (s_shot_count == SHOT_QUEUE_CAPACITY) {
            free_shot_entry(&s_shots[s_shot_start]);
            s_shot_start = (s_shot_start + 1) % SHOT_QUEUE_CAPACITY;
            s_shot_count--;
        }

        int index = (s_shot_start + s_shot_count) % SHOT_QUEUE_CAPACITY;
        shot_entry_t *shot = &s_shots[index];
        memset(shot, 0, sizeof(*shot));
        shot->jpeg = (uint8_t *)malloc(jpeg_len);
        if (shot->jpeg) {
            memcpy(shot->jpeg, jpeg_data, jpeg_len);
            shot->id = s_next_shot_id++;
            shot->jpeg_len = jpeg_len;
            shot->width = width;
            shot->height = height;
            s_shot_count++;
            ESP_LOGI(TAG, "Queued shot #%lu (%d/%d)",
                     (unsigned long)shot->id, s_shot_count, SHOT_QUEUE_CAPACITY);
        } else {
            ESP_LOGE(TAG, "Failed to allocate queued JPEG (%d bytes)", jpeg_len);
        }
    } else {
        for (int i = 0; i < SHOT_QUEUE_CAPACITY; i++) {
            free_shot_entry(&s_shots[i]);
        }
        s_shot_start = 0;
        s_shot_count = 0;
    }

    xSemaphoreGive(s_shot_mutex);
}
