#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_ldo_regulator.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "driver/jpeg_decode.h"
#include "lwip/netdb.h"
#include "bsp_illuminate.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/ip_addr.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "weather.h"
#include <string.h>
#include <time.h>
#include <math.h>

static const char *TAG = "SAASEMA";

// === WiFi (Lesson17 pohja) ===
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     10

static EventGroupHandle_t wifi_events;
static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_num++;
        ESP_LOGW(TAG, "WiFi katkesi, yhdistetaan uudelleen... (%d)", s_retry_num);
        vTaskDelay(pdMS_TO_TICKS(3000)); // odota 3s ennen yritystä
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi yhdistetty, IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init(void) {
    wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wc = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Odotetaan WiFi-yhteytta...");
    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi OK");
        return true;
    }
    ESP_LOGE(TAG, "WiFi epaonnistui");
    return false;
}

// === Aika ===
static void time_init(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();

    time_t now = 0;
    struct tm ti = {0};
    int retry = 0;
    while (ti.tm_year < (2020 - 1900) && retry++ < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &ti);
    }
    ESP_LOGI(TAG, "Aika synkronoitu");
}

// === LVGL globaalit ===
static WeatherData_t g_weather = {0};
static lv_obj_t *g_scr = NULL;

// Ylapalkki
static lv_obj_t *lbl_time    = NULL;
static lv_obj_t *lbl_date    = NULL;
static lv_obj_t *lbl_city    = NULL;
static lv_obj_t *lbl_update  = NULL;
static lv_obj_t *lbl_wifi    = NULL;

// Paakortti
static lv_obj_t *lbl_temp      = NULL;
static lv_obj_t *lbl_feels     = NULL;
static lv_obj_t *lbl_desc      = NULL;
static lv_obj_t *lbl_minmax    = NULL;
static lv_obj_t *canvas_icon   = NULL;  // graafinen saaikoni
#define ICON_SIZE 80
static lv_color_t *icon_buf = NULL;  // allokoidaan dynaamisesti PSRAM:iin

// Eteenpain-deklaraatio
static void draw_weather_icon(const char *desc);

// Tietokortit
static lv_obj_t *lbl_humidity  = NULL;
static lv_obj_t *lbl_wind      = NULL;
static lv_obj_t *lbl_pressure  = NULL;
static lv_obj_t *lbl_clouds    = NULL;
static lv_obj_t *lbl_uv        = NULL;
static lv_obj_t *lbl_uv_txt    = NULL;
static lv_obj_t *bar_uv        = NULL;
static lv_obj_t *lbl_rain      = NULL;
static lv_obj_t *lbl_sunrise   = NULL;
static lv_obj_t *lbl_visibility = NULL;

// Kuun vaihe
static lv_obj_t *lbl_thermo = NULL;

// Ennustegraafi
static lv_obj_t *chart_forecast = NULL;
static lv_chart_series_t *ser_forecast = NULL;
static lv_obj_t *lbl_fc[FORECAST_COUNT] = {NULL};

// 5 paivan ennuste
static lv_obj_t *lbl_daily_day[DAILY_COUNT]  = {NULL};
static lv_obj_t *lbl_daily_max[DAILY_COUNT]  = {NULL};
static lv_obj_t *lbl_daily_min[DAILY_COUNT]  = {NULL};

// Kompassi
static lv_obj_t *lbl_compass   = NULL;
static lv_obj_t *lbl_wind_deg  = NULL;
static lv_obj_t *compass_arrow = NULL;   // nuolen varsi
static lv_obj_t *compass_arrow_l = NULL; // vasen siipi
static lv_obj_t *compass_arrow_r = NULL; // oikea siipi
static lv_point_t arrow_points[2];
static lv_point_t arrow_l[2];
static lv_point_t arrow_r[2];

// Status
static lv_obj_t *lbl_status    = NULL;

// Kamerakuva
static lv_obj_t *cam_img       = NULL;
static lv_obj_t *lbl_cam_status = NULL;
static uint8_t  *cam_buf       = NULL;
static lv_img_dsc_t cam_img_dsc = {0};
static lv_img_dsc_t icon_img_dsc = {0};  // erillinen ikonille

// Tyylit
static lv_style_t style_card;

// Varit
#define COL_BG      lv_color_hex(0x0D1117)
#define COL_CARD    lv_color_hex(0x161B22)
#define COL_BORDER  lv_color_hex(0x30363D)
#define COL_TEXT    lv_color_hex(0xE6EDF3)
#define COL_MUTED   lv_color_hex(0x8B949E)
#define COL_BLUE    lv_color_hex(0x58A6FF)
#define COL_GREEN   lv_color_hex(0x3FB950)
#define COL_YELLOW  lv_color_hex(0xF0C040)
#define COL_RED     lv_color_hex(0xFF4444)
#define COL_ORANGE  lv_color_hex(0xFF6600)

// Apufunktio: luo kortti
static lv_obj_t* create_card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// Apufunktio: luo otsikkoteksti kortissa
static lv_obj_t* create_label_title(lv_obj_t *parent, const char *txt, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_color(lbl, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    return lbl;
}

// Apufunktio: luo arvolabel
static lv_obj_t* create_label_value(lv_obj_t *parent, const char *txt, int x, int y,
                                    const lv_font_t *font, lv_color_t color) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

// Paivita UI datasta
static void ui_update(void) {
    if (!g_weather.valid) return;

    char buf[64];
    WeatherData_t *w = &g_weather;

    // --- Lampotila ---
    snprintf(buf, sizeof(buf), "%.1f°", w->temp);
    lv_label_set_text(lbl_temp, buf);
    lv_color_t tc = lv_color_hex(weather_temp_color(w->temp));
    lv_obj_set_style_text_color(lbl_temp, tc, 0);

    // --- Graafinen saaikoni ---
    draw_weather_icon(w->description);

    snprintf(buf, sizeof(buf), "Tuntuu %.1f°C", w->feels_like);
    lv_label_set_text(lbl_feels, buf);

    lv_label_set_text(lbl_desc, weather_translate(w->description));

    snprintf(buf, sizeof(buf), "Max %.0f°  Min %.0f°", w->temp_max, w->temp_min);
    lv_label_set_text(lbl_minmax, buf);

    // --- Tietokortit ---
    snprintf(buf, sizeof(buf), "%d%%", w->humidity);
    lv_label_set_text(lbl_humidity, buf);

    snprintf(buf, sizeof(buf), "%.1f m/s %s", w->wind_speed, weather_wind_dir(w->wind_deg));
    lv_label_set_text(lbl_wind, buf);

    snprintf(buf, sizeof(buf), "%d hPa", w->pressure);
    lv_label_set_text(lbl_pressure, buf);

    snprintf(buf, sizeof(buf), "%d%%", w->clouds);
    lv_label_set_text(lbl_clouds, buf);

    // UV-indeksi (arvioitu)
    time_t now;
    time(&now);
    int uv = weather_uv_estimate(w->clouds, w->sunrise, w->sunset, (long)now);
    snprintf(buf, sizeof(buf), "%d", uv);
    lv_label_set_text(lbl_uv, buf);

    // UV vari
    lv_color_t uv_col = (uv <= 2) ? COL_GREEN :
                        (uv <= 5) ? COL_YELLOW :
                        (uv <= 7) ? COL_ORANGE : COL_RED;
    lv_obj_set_style_text_color(lbl_uv, uv_col, 0);

    // UV-teksti
    const char *uv_txt = (uv <= 2) ? "Matala" :
                         (uv <= 5) ? "Kohtalainen" :
                         (uv <= 7) ? "Korkea" :
                         (uv <= 10) ? "Erittain korkea" : "Aarimmainen";
    lv_label_set_text(lbl_uv_txt, uv_txt);
    lv_obj_set_style_text_color(lbl_uv_txt, uv_col, 0);

    // UV palkki
    lv_bar_set_value(bar_uv, uv, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_uv, uv_col, LV_PART_INDICATOR);

    // Sade
    snprintf(buf, sizeof(buf), "%.1f mm", w->rain_1h);
    lv_label_set_text(lbl_rain, buf);

    // Aurinko
    char sr[8], ss[8];
    long sr_local = w->sunrise + GMT_OFFSET_SEC + DST_OFFSET_SEC;
    long ss_local = w->sunset  + GMT_OFFSET_SEC + DST_OFFSET_SEC;
    snprintf(sr, sizeof(sr), "%02d:%02d", (int)((sr_local % 86400) / 3600), (int)((sr_local % 3600) / 60));
    snprintf(ss, sizeof(ss), "%02d:%02d", (int)((ss_local % 86400) / 3600), (int)((ss_local % 3600) / 60));
    snprintf(buf, sizeof(buf), LV_SYMBOL_UP " %s\n" LV_SYMBOL_DOWN " %s", sr, ss);
    lv_label_set_text(lbl_sunrise, buf);

    // Nakyvyys
    snprintf(buf, sizeof(buf), "%.0f km", w->visibility);
    lv_label_set_text(lbl_visibility, buf);

    // --- Kuun vaihe ---
    // Lasketaan kuun vaihe Unix-aikaleimasta
    // Tunnettu uusikuu: 2000-01-06 18:14 UTC = Unix 947182440
    // Kuun kiertoaika: 29.53059 paivaa
    {
        double synodic = 29.53059;
        double known_new = 947182440.0;
        double elapsed = ((double)w->dt - known_new) / 86400.0;
        double phase = fmod(elapsed, synodic);
        if (phase < 0) phase += synodic;
        double p = phase / synodic; // 0.0 - 1.0

        const char *moon_name;
        if      (p < 0.033) { moon_name = "Uusikuu"; }
        else if (p < 0.233) { moon_name = "Kasvava sirppi"; }
        else if (p < 0.283) { moon_name = "Puolikuu (kasv)"; }
        else if (p < 0.483) { moon_name = "Kasvava kuu"; }
        else if (p < 0.533) { moon_name = "Taysikuu"; }
        else if (p < 0.733) { moon_name = "Vaheneva kuu"; }
        else if (p < 0.783) { moon_name = "Puolikuu (vah)"; }
        else if (p < 0.967) { moon_name = "Vaheneva sirppi"; }
        else                { moon_name = "Uusikuu"; }

        // Vaiheprosentti
        int moon_pct = (int)(p * 100);
        snprintf(buf, sizeof(buf), "%s\n%d%%", moon_name, moon_pct);

        lv_label_set_text(lbl_thermo, buf);
    }

    // --- Tuulikompassi ---
    const char *dirs[] = {"P","KP","I","KI","E","LE","L","LP"};
    int di = ((w->wind_deg + 22) % 360) / 45;
    if (di >= 8) di = 0;
    snprintf(buf, sizeof(buf), "%s\n%d\xC2\xB0", dirs[di], w->wind_deg);
    lv_label_set_text(lbl_compass, buf);
    snprintf(buf, sizeof(buf), "%.1f m/s", w->wind_speed);
    lv_label_set_text(lbl_wind_deg, buf);
    ESP_LOGI(TAG, "Tuuli: %d ast -> %s (di=%d)", w->wind_deg, dirs[di], di);

    // Paivita kompassinuoli
    float rad = (w->wind_deg - 90.0f) * (3.14159f / 180.0f);
    int cx_a = 38, cy_a = 38;
    float r = 26.0f, rw = 10.0f;

    // Varsi: keskelta karkeen
    arrow_points[0].x = cx_a;
    arrow_points[0].y = cy_a;
    arrow_points[1].x = cx_a + (int)(r * cosf(rad));
    arrow_points[1].y = cy_a + (int)(r * sinf(rad));
    lv_line_set_points(compass_arrow, arrow_points, 2);
    lv_obj_set_style_line_color(compass_arrow, COL_BLUE, 0);

    // Vasen siipi
    arrow_l[0].x = arrow_points[1].x;
    arrow_l[0].y = arrow_points[1].y;
    arrow_l[1].x = arrow_points[1].x + (int)(rw * cosf(rad + 3.14159f * 0.75f));
    arrow_l[1].y = arrow_points[1].y + (int)(rw * sinf(rad + 3.14159f * 0.75f));
    lv_line_set_points(compass_arrow_l, arrow_l, 2);
    lv_obj_set_style_line_color(compass_arrow_l, COL_BLUE, 0);

    // Oikea siipi
    arrow_r[0].x = arrow_points[1].x;
    arrow_r[0].y = arrow_points[1].y;
    arrow_r[1].x = arrow_points[1].x + (int)(rw * cosf(rad - 3.14159f * 0.75f));
    arrow_r[1].y = arrow_points[1].y + (int)(rw * sinf(rad - 3.14159f * 0.75f));
    lv_line_set_points(compass_arrow_r, arrow_r, 2);
    lv_obj_set_style_line_color(compass_arrow_r, COL_BLUE, 0);

    // --- Ennustegraafi ---
    // Laske dynaaminnen min/max ennusteesta
    float fc_min = w->forecast[0].temp, fc_max = w->forecast[0].temp;
    for (int i = 1; i < FORECAST_COUNT; i++) {
        if (w->forecast[i].temp < fc_min) fc_min = w->forecast[i].temp;
        if (w->forecast[i].temp > fc_max) fc_max = w->forecast[i].temp;
    }
    int axis_min = (int)fc_min - 3;
    int axis_max = (int)fc_max + 3;
    lv_chart_set_range(chart_forecast, LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
    for (int i = 0; i < FORECAST_COUNT; i++) {
        lv_chart_set_value_by_id(chart_forecast, ser_forecast, i,
                                 (lv_coord_t)(w->forecast[i].temp));
    }
    lv_chart_refresh(chart_forecast);

    // Ennuste aika + lampo labelit
    for (int i = 0; i < FORECAST_COUNT; i++) {
        snprintf(buf, sizeof(buf), "%s\n%.0f°", w->forecast[i].time, w->forecast[i].temp);
        lv_label_set_text(lbl_fc[i], buf);
    }

    // --- 5 paivan ennuste ---
    for (int i = 0; i < DAILY_COUNT; i++) {
        if (w->daily[i].day[0] == 0) continue;
        lv_label_set_text(lbl_daily_day[i], w->daily[i].day);

        snprintf(buf, sizeof(buf), "%.0f°", w->daily[i].temp_max);
        lv_label_set_text(lbl_daily_max[i], buf);

        snprintf(buf, sizeof(buf), "%.0f°", w->daily[i].temp_min);
        lv_label_set_text(lbl_daily_min[i], buf);
    }

    // --- Paivitysaika ---
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(buf, sizeof(buf), "Paiv. %02d:%02d", ti.tm_hour, ti.tm_min);
    lv_label_set_text(lbl_update, buf);

    ESP_LOGI(TAG, "UI paivitetty");
}

// Paivita kello joka minuutti
static void clock_timer_cb(lv_timer_t *timer) {
    char buf[32];
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    const char *paivat[] = {"Su","Ma","Ti","Ke","To","Pe","La"};
    snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    lv_label_set_text(lbl_time, buf);

    snprintf(buf, sizeof(buf), "%s %d.%d.%d",
             paivat[ti.tm_wday], ti.tm_mday, ti.tm_mon+1, ti.tm_year+1900);
    lv_label_set_text(lbl_date, buf);
}

// === KAMERAKUVA ===
#define CAM_BUF_MAX   (300 * 1024)  // 300KB JPEG buffer
#define CAM_W         285
#define CAM_H         155
// Thumbnail URL - paljon pienempi kuva
#define CAM_URL "https://weathercam.digitraffic.fi/C1452002.jpg?thumbnail=true"

static int cam_http_len = 0;

static esp_err_t cam_http_event(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (cam_buf && cam_http_len + (int)evt->data_len < CAM_BUF_MAX) {
                memcpy(cam_buf + cam_http_len, evt->data, evt->data_len);
                cam_http_len += evt->data_len;
            }
            break;
        default: break;
    }
    return ESP_OK;
}

static void camera_task(void *pv) {
    // JPEG raw buffer
    cam_buf = heap_caps_malloc(CAM_BUF_MAX, MALLOC_CAP_SPIRAM);
    if (!cam_buf) {
        ESP_LOGE(TAG, "Kamera JPEG buffer epaonnistui");
        vTaskDelete(NULL);
        return;
    }

    // RGB565 dekoodattu buffer nayttolle
    size_t rgb_size = CAM_W * CAM_H * 2;
    uint8_t *rgb_buf = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        ESP_LOGE(TAG, "Kamera RGB buffer epaonnistui");
        free(cam_buf);
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(10000)); // Odota WiFi + DNS valmiiksi

    // Odota etta DNS toimii
    {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        int retry = 0;
        while (retry < 20) {
            int err = getaddrinfo("weathercam.digitraffic.fi", "443", &hints, &res);
            if (err == 0 && res != NULL) {
                freeaddrinfo(res);
                ESP_LOGI(TAG, "Kamera DNS OK");
                break;
            }
            ESP_LOGW(TAG, "Kamera DNS odotus... (%d/20)", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            retry++;
        }
    }

    while (1) {
        ESP_LOGI(TAG, "Haetaan kelikuvaa...");
        if (lvgl_port_lock(0)) {
            lv_label_set_text(lbl_cam_status, "Haetaan...");
            lvgl_port_unlock();
        }

        // Hae JPEG
        cam_http_len = 0;
        esp_http_client_config_t cfg = {
            .url               = CAM_URL,
            .event_handler     = cam_http_event,
            .timeout_ms        = 20000,
            .buffer_size       = 8192,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200 || cam_http_len < 1000) {
            ESP_LOGW(TAG, "Kelikuva haku epaonnistui: err=%d status=%d len=%d",
                     err, status, cam_http_len);
            if (lvgl_port_lock(0)) {
                lv_label_set_text(lbl_cam_status, "Ei kuvaa");
                lvgl_port_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        ESP_LOGI(TAG, "JPEG haettu: %d tavua", cam_http_len);

        // Dekoodaa JPEG -> RGB565 kayttaen P4 hardware JPEG dekooder
        jpeg_decode_engine_cfg_t engine_cfg = {
            .intr_priority = 0,
            .timeout_ms    = 5000,  // 5 sekuntia timeout
        };
        jpeg_decode_cfg_t dec_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        };

        jpeg_decode_picture_info_t pic_info;
        jpeg_decoder_handle_t decoder = NULL;

        esp_err_t ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "JPEG decoder init epaonnistui: %d", ret);
            goto next;
        }

        ret = jpeg_decoder_get_info((const uint8_t *)cam_buf, cam_http_len, &pic_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "JPEG info luku epaonnistui: %d", ret);
            jpeg_del_decoder_engine(decoder);
            goto next;
        }
        ESP_LOGI(TAG, "JPEG koko: %lux%lu", (unsigned long)pic_info.width, (unsigned long)pic_info.height);

        // Laske output buffer koko 16-tavun tasauksella (hardware JPEG vaatii)
        uint32_t aligned_w = (pic_info.width  + 15) & ~15;
        uint32_t aligned_h = (pic_info.height + 15) & ~15;
        uint32_t out_size = aligned_w * aligned_h * 2; // RGB565
        size_t allocated_size = 0;
        uint8_t *out_buf = (uint8_t *)jpeg_alloc_decoder_mem(out_size, &(jpeg_decode_memory_alloc_cfg_t){
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        }, &allocated_size);
        if (!out_buf) {
            ESP_LOGE(TAG, "JPEG output buffer epaonnistui");
            jpeg_del_decoder_engine(decoder);
            goto next;
        }
        ESP_LOGI(TAG, "Output buffer: %u tavua (aligned %lux%lu)",
                 (unsigned)allocated_size, (unsigned long)aligned_w, (unsigned long)aligned_h);

        uint32_t decoded_size = 0;
        ret = jpeg_decoder_process(decoder, &dec_cfg, (const uint8_t *)cam_buf,
                                   cam_http_len, out_buf, out_size, &decoded_size);
        jpeg_del_decoder_engine(decoder);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "JPEG dekoodaus epaonnistui: %d", ret);
            free(out_buf);
            goto next;
        }
        ESP_LOGI(TAG, "JPEG dekoodattu: %lu tavua (%lux%lu)",
                 (unsigned long)decoded_size, (unsigned long)pic_info.width,
                 (unsigned long)pic_info.height);

        // Skaalaa kuva 285x155 pikseliin nearest-neighbor
        {
            uint16_t *src = (uint16_t *)out_buf;
            uint16_t *dst = (uint16_t *)rgb_buf;
            int sw = (int)aligned_w;  // aligned leveys
            int sh = (int)aligned_h;  // aligned korkeus
            for (int y = 0; y < CAM_H; y++) {
                int sy = y * sh / CAM_H;
                for (int x = 0; x < CAM_W; x++) {
                    int sx = x * sw / CAM_W;
                    dst[y * CAM_W + x] = src[sy * sw + sx];
                }
            }
            free(out_buf);
        }

        // Nayta LVGL:ssa
        cam_img_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
        cam_img_dsc.header.always_zero = 0;
        cam_img_dsc.header.w           = CAM_W;
        cam_img_dsc.header.h           = CAM_H;
        cam_img_dsc.data_size          = rgb_size;
        cam_img_dsc.data               = rgb_buf;

        if (lvgl_port_lock(0)) {
            lv_img_set_src(cam_img, &cam_img_dsc);
            lv_label_set_text(lbl_cam_status, "");
            lvgl_port_unlock();
        }
        ESP_LOGI(TAG, "Kelikuva naytossa");

next:
        vTaskDelay(pdMS_TO_TICKS(900000)); // 15 min
    }
}

// Saapaivitys taustalla
static void weather_task(void *pv) {
    // Pieni odotus etta verkkostack on valmis
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        ESP_LOGI(TAG, "Haetaan saadataa...");
        lv_label_set_text(lbl_status, "Haetaan saatietoja...");

        esp_err_t err = weather_fetch(&g_weather);
        if (err == ESP_OK) {
            lv_label_set_text(lbl_status, "");
            ui_update();
        } else {
            lv_label_set_text(lbl_status, "Virhe: tarkista verkko");
            ESP_LOGE(TAG, "Saahaku epaonnistui");
            // Yrita uudelleen 30s kuluttua jos epaonnistui
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL_SEC * 1000));
    }
}

// Paivita-napin callback
static void btn_refresh_cb(lv_event_t *e) {
    lv_label_set_text(lbl_status, "Paivitetaan...");
    esp_err_t err = weather_fetch(&g_weather);
    if (err == ESP_OK) {
        lv_label_set_text(lbl_status, "");
        ui_update();
    } else {
        lv_label_set_text(lbl_status, "Virhe: tarkista verkko");
    }
}

// === GRAAFISET SAAIKONIT ===
#define IC_BG     lv_color_hex(0x0D1117)  // tausta
#define IC_SUN    lv_color_hex(0xF0C040)  // aurinko
#define IC_CLOUD  lv_color_hex(0x8B949E)  // pilvi
#define IC_RAIN   lv_color_hex(0x58A6FF)  // sade
#define IC_SNOW   lv_color_hex(0xCCEEFF)  // lumi
#define IC_BOLT   lv_color_hex(0xFFDD00)  // salama

static void icon_clear(void) {
    // Taytta tumma tausta suoraan muistiin
    lv_color_t bg = IC_BG;
    lv_color_t *buf = icon_buf;
    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) buf[i] = bg;
}

static void icon_set_pixel(int x, int y, lv_color_t c) {
    if (x >= 0 && x < ICON_SIZE && y >= 0 && y < ICON_SIZE)
        icon_buf[y * ICON_SIZE + x] = c;
}

static void icon_fill_circle(int cx, int cy, int r, lv_color_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x*x + y*y <= r*r)
                icon_set_pixel(cx+x, cy+y, c);
}

static void icon_fill_rect_px(int x, int y, int w, int h, lv_color_t c) {
    for (int row = y; row < y+h && row < ICON_SIZE; row++)
        for (int col = x; col < x+w && col < ICON_SIZE; col++)
            icon_set_pixel(col, row, c);
}

static void icon_draw_line_px(int x0, int y0, int x1, int y1, lv_color_t c, int w) {
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = x0<x1?1:-1, sy = y0<y1?1:-1, err = dx-dy;
    while (1) {
        for (int ow = -w/2; ow <= w/2; ow++) {
            icon_set_pixel(x0+ow, y0, c);
            icon_set_pixel(x0, y0+ow, c);
        }
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void icon_draw_sun(void) {
    icon_clear();
    icon_fill_circle(40, 40, 18, IC_SUN);
    int rays[][4] = {{40,2,40,14},{40,66,40,78},{2,40,14,40},{66,40,78,40},
                     {13,13,21,21},{59,59,67,67},{67,13,59,21},{21,59,13,67}};
    for (int i = 0; i < 8; i++)
        icon_draw_line_px(rays[i][0],rays[i][1],rays[i][2],rays[i][3],IC_SUN,3);
}

static void icon_draw_cloud(lv_color_t col) {
    icon_fill_circle(25, 42, 14, col);
    icon_fill_circle(40, 34, 16, col);
    icon_fill_circle(55, 40, 12, col);
    icon_fill_rect_px(12, 40, 56, 22, col);
}

static void icon_draw_rain_drops(int count, lv_color_t col) {
    int spacing = 60 / (count + 1);
    for (int i = 0; i < count; i++) {
        int x = 10 + spacing * (i + 1);
        icon_draw_line_px(x, 68, x-4, 78, col, 2);
    }
}

static void icon_draw_snow_dots(void) {
    int xs[] = {15, 35, 55, 25, 45};
    int ys[] = {70, 74, 70, 80, 80};
    for (int i = 0; i < 5; i++)
        icon_fill_circle(xs[i], ys[i], 4, IC_SNOW);
}

static void icon_draw_bolt(void) {
    icon_draw_line_px(45, 63, 35, 73, IC_BOLT, 3);
    icon_draw_line_px(35, 73, 42, 73, IC_BOLT, 3);
    icon_draw_line_px(42, 73, 32, 83, IC_BOLT, 3);
}

static void draw_weather_icon(const char *desc) {
    if (!canvas_icon || !icon_buf) return;
    icon_clear();

    char d[64] = {0};
    strncpy(d, desc, sizeof(d)-1);
    for (int i = 0; d[i]; i++) if (d[i] >= 'A' && d[i] <= 'Z') d[i] += 32;

    if (strstr(d, "thunder")) {
        icon_draw_cloud(IC_CLOUD); icon_draw_bolt(); icon_draw_rain_drops(2, IC_RAIN);
    } else if (strstr(d, "snow") || strstr(d, "sleet")) {
        icon_draw_cloud(IC_CLOUD); icon_draw_snow_dots();
    } else if (strstr(d, "rain") || strstr(d, "drizzle")) {
        icon_draw_cloud(IC_CLOUD); icon_draw_rain_drops(4, IC_RAIN);
    } else if (strstr(d, "fog") || strstr(d, "mist") || strstr(d, "haze")) {
        icon_draw_cloud(lv_color_hex(0x555555)); icon_draw_rain_drops(3, lv_color_hex(0x666666));
    } else if (strstr(d, "overcast") || strstr(d, "broken")) {
        icon_draw_cloud(IC_CLOUD);
    } else if (strstr(d, "cloud")) {
        icon_draw_sun(); icon_draw_cloud(lv_color_hex(0x6A7280));
    } else {
        icon_draw_sun();
    }
    // Pakota LVGL paivittamaan
    lv_obj_invalidate(canvas_icon);
}

// Rakennetaan UI
static void ui_build(void) {
    g_scr = lv_scr_act();
    lv_obj_set_style_bg_color(g_scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Tyylit
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COL_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, COL_BORDER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 10);
    lv_style_set_pad_all(&style_card, 0);

    // === YLAPALKKI ===
    lv_obj_t *topbar = lv_obj_create(g_scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, 1024, 70);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_time = create_label_value(topbar, "--:--", 12, 10, &lv_font_montserrat_36, COL_TEXT);
    lbl_date = create_label_value(topbar, "", 148, 22, &lv_font_montserrat_14, COL_MUTED);
    lbl_city = create_label_value(topbar, "Keminmaa", 430, 22, &lv_font_montserrat_16, COL_BLUE);
    lbl_update = create_label_value(topbar, "", 680, 22, &lv_font_montserrat_14, COL_MUTED);
    lbl_wifi = create_label_value(topbar, LV_SYMBOL_WIFI, 986, 22, &lv_font_montserrat_16, COL_GREEN);
    lbl_status = create_label_value(g_scr, "", 300, 578, &lv_font_montserrat_14, COL_MUTED);

    // === ISO SAAKORTTI (vasen) ===
    lv_obj_t *card_main = create_card(g_scr, 10, 80, 285, 260);

    // Graafinen saaikoni - allokoi buffer PSRAM:iin
    icon_buf = heap_caps_malloc(ICON_SIZE * ICON_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!icon_buf) {
        ESP_LOGE(TAG, "icon_buf allokaatio epaonnistui!");
        icon_buf = heap_caps_malloc(ICON_SIZE * ICON_SIZE * sizeof(lv_color_t), MALLOC_CAP_DEFAULT);
    }
    // Tayta taustalla
    memset(icon_buf, 0x0D, ICON_SIZE * ICON_SIZE * sizeof(lv_color_t));

    // Kayta lv_img PSRAM-bufferista suoraan (ei LVGL-heapia)
    icon_img_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    icon_img_dsc.header.always_zero = 0;
    icon_img_dsc.header.w           = ICON_SIZE;
    icon_img_dsc.header.h           = ICON_SIZE;
    icon_img_dsc.data_size          = ICON_SIZE * ICON_SIZE * sizeof(lv_color_t);
    icon_img_dsc.data               = (const uint8_t *)icon_buf;

    canvas_icon = lv_img_create(card_main);
    lv_img_set_src(canvas_icon, &icon_img_dsc);
    lv_obj_set_pos(canvas_icon, 10, 10);

    lbl_temp = create_label_value(card_main, "--°", 100, 10, &lv_font_montserrat_48, COL_TEXT);
    lbl_feels = create_label_value(card_main, "", 100, 65, &lv_font_montserrat_14, COL_MUTED);
    lbl_desc = create_label_value(card_main, "", 10, 98, &lv_font_montserrat_16, COL_BLUE);
    lbl_minmax = create_label_value(card_main, "", 10, 120, &lv_font_montserrat_14, COL_MUTED);

    // Kuun vaihe
    create_label_title(card_main, "Kuun vaihe", 20, 175);
    lbl_thermo = create_label_value(card_main, "🌑", 20, 195, &lv_font_montserrat_16, COL_TEXT);

    // === TIETOKORTIT 2x4 ruudukko ===
    const int cx = 305, cy1 = 80, cy2 = 186, cw = 172, ch = 95, gap = 5;

    // Korttirakenne: [otsikko, arvolbl, x, y]
    // Kosteus
    lv_obj_t *c1 = create_card(g_scr, cx, cy1, cw, ch);
    create_label_title(c1, "Kosteus", 15, 12);
    lbl_humidity = create_label_value(c1, "--%", 15, 38, &lv_font_montserrat_48, COL_BLUE);

    // Tuuli
    lv_obj_t *c2 = create_card(g_scr, cx+cw+gap, cy1, cw, ch);
    create_label_title(c2, "Tuuli", 15, 12);
    lbl_wind = create_label_value(c2, "-- m/s", 15, 38, &lv_font_montserrat_16, COL_TEXT);

    // Paine
    lv_obj_t *c3 = create_card(g_scr, cx+(cw+gap)*2, cy1, cw, ch);
    create_label_title(c3, "Paine", 15, 12);
    lbl_pressure = create_label_value(c3, "-- hPa", 15, 38, &lv_font_montserrat_16, COL_TEXT);

    // Pilvisyys
    lv_obj_t *c4 = create_card(g_scr, cx+(cw+gap)*3, cy1, cw, ch);
    create_label_title(c4, "Pilvisyys", 15, 12);
    lbl_clouds = create_label_value(c4, "--%", 15, 38, &lv_font_montserrat_48, COL_TEXT);

    // Rivi 2
    // UV-indeksi
    lv_obj_t *c5 = create_card(g_scr, cx, cy2, cw, ch);
    create_label_title(c5, "UV-indeksi", 15, 12);
    lbl_uv = create_label_value(c5, "-", 15, 30, &lv_font_montserrat_36, COL_GREEN);
    lbl_uv_txt = create_label_value(c5, "", 55, 36, &lv_font_montserrat_14, COL_GREEN);
    bar_uv = lv_bar_create(c5);
    lv_obj_set_pos(bar_uv, 15, 78);
    lv_obj_set_size(bar_uv, 140, 10);
    lv_bar_set_range(bar_uv, 0, 11);
    lv_bar_set_value(bar_uv, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_uv, lv_color_hex(0x21262D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_uv, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_uv, 5, 0);
    lv_obj_set_style_radius(bar_uv, 5, LV_PART_INDICATOR);

    // Sade
    lv_obj_t *c6 = create_card(g_scr, cx+cw+gap, cy2, cw, ch);
    create_label_title(c6, "Sade (1h)", 15, 12);
    lbl_rain = create_label_value(c6, "-- mm", 15, 38, &lv_font_montserrat_16, COL_BLUE);

    // Aurinko
    lv_obj_t *c7 = create_card(g_scr, cx+(cw+gap)*2, cy2, cw, ch);
    create_label_title(c7, "Aurinko", 15, 12);
    lbl_sunrise = create_label_value(c7, "", 15, 32, &lv_font_montserrat_16, COL_YELLOW);

    // Nakyvyys
    lv_obj_t *c8 = create_card(g_scr, cx+(cw+gap)*3, cy2, cw, ch);
    create_label_title(c8, "Nakyvyys", 15, 12);
    lbl_visibility = create_label_value(c8, "-- km", 15, 38, &lv_font_montserrat_16, COL_TEXT);

    // === TUULIKOMPASSI (iso kortti) ===
    lv_obj_t *card_compass = create_card(g_scr, 305, 290, 172, 182);
    create_label_title(card_compass, "Tuulen suunta", 15, 12);

    // Suunta iso teksti vasemmalla
    lbl_compass = lv_label_create(card_compass);
    lv_label_set_text(lbl_compass, "P\n0");
    lv_obj_set_pos(lbl_compass, 10, 30);
    lv_obj_set_style_text_color(lbl_compass, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_compass, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(lbl_compass, LV_TEXT_ALIGN_CENTER, 0);

    // Kompassiympyra oikealla - isompi
    lv_obj_t *compass_ring = lv_obj_create(card_compass);
    lv_obj_set_pos(compass_ring, 88, 28);
    lv_obj_set_size(compass_ring, 76, 76);
    lv_obj_set_style_bg_color(compass_ring, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_border_color(compass_ring, COL_BORDER, 0);
    lv_obj_set_style_border_width(compass_ring, 1, 0);
    lv_obj_set_style_radius(compass_ring, 38, 0);
    lv_obj_clear_flag(compass_ring, LV_OBJ_FLAG_SCROLLABLE);

    // Ilmansuunnat selkeasti
    lv_obj_t *lbl_n = lv_label_create(compass_ring);
    lv_label_set_text(lbl_n, "P");
    lv_obj_align(lbl_n, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_text_color(lbl_n, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_n, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl_s = lv_label_create(compass_ring);
    lv_label_set_text(lbl_s, "E");
    lv_obj_align(lbl_s, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_color(lbl_s, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_s, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl_w = lv_label_create(compass_ring);
    lv_label_set_text(lbl_w, "L");
    lv_obj_align(lbl_w, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_text_color(lbl_w, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_w, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl_e = lv_label_create(compass_ring);
    lv_label_set_text(lbl_e, "I");
    lv_obj_align(lbl_e, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_text_color(lbl_e, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_e, &lv_font_montserrat_12, 0);

    // Nuoli - aluksi pohjoiseen (keskipiste 38,38 sade 26)
    arrow_points[0].x = 38; arrow_points[0].y = 12;
    arrow_points[1].x = 38; arrow_points[1].y = 38;
    compass_arrow = lv_line_create(compass_ring);
    lv_line_set_points(compass_arrow, arrow_points, 2);
    lv_obj_set_style_line_color(compass_arrow, COL_BLUE, 0);
    lv_obj_set_style_line_width(compass_arrow, 3, 0);
    lv_obj_set_style_line_rounded(compass_arrow, true, 0);

    arrow_l[0].x = 38; arrow_l[0].y = 12;
    arrow_l[1].x = 30; arrow_l[1].y = 20;
    compass_arrow_l = lv_line_create(compass_ring);
    lv_line_set_points(compass_arrow_l, arrow_l, 2);
    lv_obj_set_style_line_color(compass_arrow_l, COL_BLUE, 0);
    lv_obj_set_style_line_width(compass_arrow_l, 3, 0);
    lv_obj_set_style_line_rounded(compass_arrow_l, true, 0);

    arrow_r[0].x = 38; arrow_r[0].y = 12;
    arrow_r[1].x = 46; arrow_r[1].y = 20;
    compass_arrow_r = lv_line_create(compass_ring);
    lv_line_set_points(compass_arrow_r, arrow_r, 2);
    lv_obj_set_style_line_color(compass_arrow_r, COL_BLUE, 0);
    lv_obj_set_style_line_width(compass_arrow_r, 3, 0);
    lv_obj_set_style_line_rounded(compass_arrow_r, true, 0);

    // Tuulinopeus
    lbl_wind_deg = create_label_value(card_compass, "-- m/s", 15, 152, &lv_font_montserrat_14, COL_MUTED);

    // === ENNUSTEGRAAFI ===
    lv_obj_t *card_fc = create_card(g_scr, 485, 290, 529, 140);
    create_label_title(card_fc, "Ennuste (3h)", 15, 8);

    chart_forecast = lv_chart_create(card_fc);
    lv_obj_set_pos(chart_forecast, 35, 22);
    lv_obj_set_size(chart_forecast, 480, 80);
    lv_chart_set_type(chart_forecast, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_forecast, FORECAST_COUNT);
    lv_chart_set_range(chart_forecast, LV_CHART_AXIS_PRIMARY_Y, -10, 10);
    lv_obj_set_style_bg_color(chart_forecast, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_border_width(chart_forecast, 0, 0);
    lv_obj_set_style_line_color(chart_forecast, lv_color_hex(0x21262D), LV_PART_MAIN);
    lv_chart_set_axis_tick(chart_forecast, LV_CHART_AXIS_PRIMARY_Y, 4, 2, 3, 1, true, 28);
    lv_obj_set_style_text_color(chart_forecast, COL_MUTED, LV_PART_TICKS);
    lv_obj_set_style_text_font(chart_forecast, &lv_font_montserrat_12, LV_PART_TICKS);
    ser_forecast = lv_chart_add_series(chart_forecast, COL_BLUE, LV_CHART_AXIS_PRIMARY_Y);

    for (int i = 0; i < FORECAST_COUNT; i++) {
        lbl_fc[i] = lv_label_create(card_fc);
        lv_label_set_text(lbl_fc[i], "--:--\n--°");
        lv_obj_set_pos(lbl_fc[i], 35 + i * 80, 108);
        lv_obj_set_style_text_color(lbl_fc[i], COL_MUTED, 0);
        lv_obj_set_style_text_font(lbl_fc[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(lbl_fc[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // === PAINIKKEET ===
    lv_obj_t *btn_refresh = lv_btn_create(g_scr);
    lv_obj_set_pos(btn_refresh, 10, 350);
    lv_obj_set_size(btn_refresh, 285, 46);
    lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(0x1F6FEB), 0);
    lv_obj_set_style_radius(btn_refresh, 8, 0);
    lv_obj_add_event_cb(btn_refresh, btn_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_btn = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_btn, LV_SYMBOL_REFRESH "  Paivita");
    lv_obj_center(lbl_btn);
    lv_obj_set_style_text_color(lbl_btn, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_16, 0);

    // === 5 PAIVAN ENNUSTE ===
    lv_obj_t *card_daily_hdr = create_card(g_scr, 485, 438, 529, 152);
    create_label_title(card_daily_hdr, "5 paivan ennuste", 15, 8);

    int dw = 529 / DAILY_COUNT;  // yhden paivan leveys ~105px
    for (int i = 0; i < DAILY_COUNT; i++) {
        int dx = i * dw;

        // Pystyviiva paivien valissa
        if (i > 0) {
            lv_obj_t *sep = lv_obj_create(card_daily_hdr);
            lv_obj_set_pos(sep, dx, 25);
            lv_obj_set_size(sep, 1, 155);
            lv_obj_set_style_bg_color(sep, COL_BORDER, 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
        }

        // Paivan nimi
        lbl_daily_day[i] = create_label_value(card_daily_hdr, "--", dx + 4, 22,
                                               &lv_font_montserrat_14, COL_TEXT);

        // Max lampotila
        lbl_daily_max[i] = create_label_value(card_daily_hdr, "--", dx + 4, 72,
                                               &lv_font_montserrat_16, lv_color_hex(0xFF6B6B));

        // Min lampotila
        lbl_daily_min[i] = create_label_value(card_daily_hdr, "--", dx + 4, 100,
                                               &lv_font_montserrat_14, lv_color_hex(0x58A6FF));
    }

    // === KAMERAKUVA ===
    // Kelikamera C1452002 (Keminmaa -> Kemi)
    lv_obj_t *card_cam = create_card(g_scr, 10, 405, 285, 185);
    create_label_title(card_cam, "Kelikamera Keminmaa", 10, 8);
    cam_img = lv_img_create(card_cam);
    lv_obj_set_pos(cam_img, 0, 25);
    lv_obj_set_size(cam_img, 285, 155);
    lv_obj_set_style_bg_color(card_cam, lv_color_hex(0x0D1117), 0);
    lbl_cam_status = create_label_value(card_cam, "Haetaan...", 10, 85, &lv_font_montserrat_14, COL_MUTED);

    // Ajastimet
    lv_timer_create(clock_timer_cb, 10000, NULL);
    clock_timer_cb(NULL);  // kutsu heti

    ESP_LOGI(TAG, "UI rakennettu");
}

// === LVGL KOSKETUS INDEV ===
static lv_indev_t *touch_indev = NULL;

// LVGL kutsuu tata funktiota saadakseen kosketustilan
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (touch_read() == ESP_OK) {
        uint16_t x, y;
        bool pressed;
        get_coor(&x, &y, &pressed);
        if (pressed && x != 0xffff && y != 0xffff) {
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGD(TAG, "Touch: X=%d Y=%d", x, y);
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Rekisteroi LVGL indev kosketukselle
static void touch_indev_init(void) {
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    touch_indev = lv_indev_drv_register(&indev_drv);
    ESP_LOGI(TAG, "LVGL touch indev rekisteroity");
}

// === APP MAIN ===
void app_main(void) {
    ESP_LOGI(TAG, "=== Saaasema ESP32-P4 kaynnistyy ===");

    // 1. LDO-jannitteet (Lesson07:n system_init)
    static esp_ldo_channel_handle_t ldo3 = NULL;
    static esp_ldo_channel_handle_t ldo4 = NULL;
    esp_ldo_channel_config_t ldo3_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    esp_ldo_channel_config_t ldo4_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    esp_ldo_acquire_channel(&ldo3_cfg, &ldo3);
    esp_ldo_acquire_channel(&ldo4_cfg, &ldo4);

    // 2. Nayto + LVGL
    esp_err_t err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD init epaonnistui: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "LCD alustettu");

    // 3. Taustavalo paalle
    set_lcd_blight(100);
    ESP_LOGI(TAG, "Taustavalo paalla");

    // 4. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 5. I2C + Kosketusnaytto + LVGL indev
    if (i2c_init() == ESP_OK) {
        if (touch_init() == ESP_OK) {
            ESP_LOGI(TAG, "Kosketusnaytto alustettu");
            // Rekisteroi LVGL indev LVGL-lukolla
            if (lvgl_port_lock(0)) {
                touch_indev_init();
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGW(TAG, "Kosketusnaytto init epaonnistui");
        }
    } else {
        ESP_LOGW(TAG, "I2C init epaonnistui");
    }

    // 6. Odota ESP-Hosted (C6) valmiiksi ennen WiFi-alustusta
    ESP_LOGI(TAG, "Odotetaan ESP-Hosted alustusta...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 7. WiFi
    bool wifi_ok = wifi_init();
    if (!wifi_ok) {
        ESP_LOGW(TAG, "WiFi epaonnistui - jatketaan ilman verkkoa");
    }

    // 8. Aika
    time_init();

    // 9. Rakenna UI LVGL-lukolla
    if (lvgl_port_lock(0)) {
        ui_build();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock epaonnistui");
    }

    // 10. Aloita saapaivitys taustalla
    xTaskCreatePinnedToCore(weather_task, "weather", 16384, NULL, 5, NULL, 1);

    // 11. Aloita kamerakuvan haku taustalla
    xTaskCreatePinnedToCore(camera_task, "camera", 16384, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "Kaikki alustettu!");
}
