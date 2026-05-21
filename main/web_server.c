#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_shot_mutex = NULL;

// Shared shot data protected by mutex
static struct {
    uint8_t *jpeg;
    int jpeg_len;
    int width;
    int height;
    float score;
    int target_cx;
    int target_cy;
    float black_radius;
    int aim_x;
    int aim_y;
    bool has_shot;
} s_shot;

// Shared mask data protected by same mutex
static struct {
    uint8_t *data;
    int len;
    int width;
    int height;
    bool has_mask;
} s_mask;

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
".w canvas{display:block;border-radius:6px}"
".lb{color:#aaa;font-size:0.8em;margin-top:4px}"
"</style></head><body>"
"<h1>DeadEyeShot</h1>"
"<div id='sc' class='s'>---</div>"
"<br>"
"<div class='w' id='wrap' style='display:none'>"
"<img id='pic' src='' width='320' height='240'>"
"<canvas id='ov' width='320' height='240' "
"style='position:absolute;top:0;left:0;border-radius:6px'></canvas>"
"</div>"
"<br>"
"<div class='w' id='mwrap' style='display:none'>"
"<canvas id='msk' width='320' height='240'></canvas>"
"<div class='lb'>Dark Component Mask</div>"
"</div>"
"<p id='info'></p>"
"<script>"
"var sd=null;"
"function draw(){"
"if(!sd||!sd.has_shot)return;"
"var c=document.getElementById('ov'),x=c.getContext('2d');"
"x.clearRect(0,0,c.width,c.height);"
"var sx=c.width/sd.w,sy=c.height/sd.h;"
"x.beginPath();"
"x.arc(sd.cx*sx,sd.cy*sy,sd.r*Math.max(sx,sy),0,2*Math.PI);"
"x.strokeStyle='red';x.lineWidth=2;x.stroke();"
"x.beginPath();"
"x.arc(sd.cx*sx,sd.cy*sy,4,0,2*Math.PI);"
"x.fillStyle='red';x.fill();"
"x.beginPath();"
"x.arc(sd.ax*sx,sd.ay*sy,4,0,2*Math.PI);"
"x.fillStyle='#4488ff';x.fill();"
"}"
"function lm(){"
"var r=new XMLHttpRequest();"
"r.responseType='arraybuffer';"
"r.onload=function(){"
"if(r.status!=200)return;"
"var d=new Uint8Array(r.response);"
"var c=document.getElementById('msk');"
"var ctx=c.getContext('2d');"
"var img=ctx.createImageData(c.width,c.height);"
"for(var i=0;i<d.length;i++){"
"img.data[i*4]=0;"
"img.data[i*4+1]=d[i];"
"img.data[i*4+2]=0;"
"img.data[i*4+3]=255;"
"}"
"ctx.putImageData(img,0,0);"
"document.getElementById('mwrap').style.display='inline-block';"
"};"
"r.open('GET','/mask.bin');r.send();"
"}"
"function u(){"
"var r=new XMLHttpRequest();"
"r.onload=function(){"
"var d=JSON.parse(r.responseText);"
"if(d.has_shot){"
"document.getElementById('sc').textContent=d.score.toFixed(1);"
"document.getElementById('pic').src='/shot.jpg?t='+Date.now();"
"document.getElementById('wrap').style.display='inline-block';"
"sd=d;"
"document.getElementById('pic').onload=draw;"
"document.getElementById('info').textContent="
"'Target:('+d.cx+','+d.cy+') Aim:('+d.ax+','+d.ay+')';"
"lm();"
"}};"
"r.open('GET','/api/shot');r.send();"
"}"
"setInterval(u,2000);u();"
"</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

static esp_err_t shot_jpg_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    if (!s_shot.has_shot || !s_shot.jpeg) {
        xSemaphoreGive(s_shot_mutex);
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, (const char *)s_shot.jpeg, s_shot.jpeg_len);
    xSemaphoreGive(s_shot_mutex);
    return err;
}

static esp_err_t api_shot_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    char json[256];
    if (s_shot.has_shot) {
        snprintf(json, sizeof(json),
                 "{\"has_shot\":true,\"score\":%.1f,\"w\":%d,\"h\":%d,"
                 "\"cx\":%d,\"cy\":%d,\"r\":%.1f,\"ax\":%d,\"ay\":%d}",
                 s_shot.score, s_shot.width, s_shot.height,
                 s_shot.target_cx, s_shot.target_cy, s_shot.black_radius,
                 s_shot.aim_x, s_shot.aim_y);
    } else {
        snprintf(json, sizeof(json), "{\"has_shot\":false}");
    }
    xSemaphoreGive(s_shot_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t mask_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    if (!s_mask.has_mask || !s_mask.data) {
        xSemaphoreGive(s_shot_mutex);
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, (const char *)s_mask.data, s_mask.len);
    xSemaphoreGive(s_shot_mutex);
    return err;
}

esp_err_t web_server_init(void)
{
    s_shot_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 5;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ESP_FAIL;
    }

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t jpg_uri  = { .uri = "/shot.jpg", .method = HTTP_GET, .handler = shot_jpg_handler };
    httpd_uri_t api_uri  = { .uri = "/api/shot", .method = HTTP_GET, .handler = api_shot_handler };
    httpd_uri_t mask_uri = { .uri = "/mask.bin", .method = HTTP_GET, .handler = mask_handler };

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &jpg_uri);
    httpd_register_uri_handler(s_server, &api_uri);
    httpd_register_uri_handler(s_server, &mask_uri);

    ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

void web_server_update_shot(const uint8_t *jpeg_data, int jpeg_len,
                            int width, int height, float score,
                            int target_cx, int target_cy,
                            float black_radius,
                            int aim_x, int aim_y)
{
    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);

    // Free old buffer
    if (s_shot.jpeg) {
        free(s_shot.jpeg);
        s_shot.jpeg = NULL;
    }

    // Allocate and copy new JPEG data
    if (jpeg_data && jpeg_len > 0) {
        s_shot.jpeg = (uint8_t *)malloc(jpeg_len);
        if (s_shot.jpeg) {
            memcpy(s_shot.jpeg, jpeg_data, jpeg_len);
            s_shot.jpeg_len = jpeg_len;
        }
    }

    s_shot.width = width;
    s_shot.height = height;
    s_shot.score = score;
    s_shot.target_cx = target_cx;
    s_shot.target_cy = target_cy;
    s_shot.black_radius = black_radius;
    s_shot.aim_x = aim_x;
    s_shot.aim_y = aim_y;
    s_shot.has_shot = true;

    xSemaphoreGive(s_shot_mutex);
}

void web_server_update_mask(const uint8_t *mask, int width, int height)
{
    int len = width * height;

    xSemaphoreTake(s_shot_mutex, portMAX_DELAY);

    if (s_mask.data) {
        free(s_mask.data);
        s_mask.data = NULL;
    }

    s_mask.data = (uint8_t *)malloc(len);
    if (s_mask.data) {
        memcpy(s_mask.data, mask, len);
        s_mask.len = len;
        s_mask.width = width;
        s_mask.height = height;
        s_mask.has_mask = true;
    }

    xSemaphoreGive(s_shot_mutex);
}
