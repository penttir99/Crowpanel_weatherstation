#include "weather.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "WEATHER";

#define HTTP_BUF_SIZE 48000
static char *http_buf = NULL;
static int   http_buf_len = 0;

// Odota etta DNS toimii
static void wait_for_dns(void) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int retry = 0;
    while (retry < 20) {
        int err = getaddrinfo("api.openweathermap.org", "80", &hints, &res);
        if (err == 0 && res != NULL) {
            freeaddrinfo(res);
            ESP_LOGI(TAG, "DNS OK");
            return;
        }
        ESP_LOGW(TAG, "DNS odotus... (%d/20)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    ESP_LOGE(TAG, "DNS epaonnistui");
}

// HTTP event handler
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_buf_len + evt->data_len < HTTP_BUF_SIZE - 1) {
                memcpy(http_buf + http_buf_len, evt->data, evt->data_len);
                http_buf_len += evt->data_len;
                http_buf[http_buf_len] = 0;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            break;
        default:
            break;
    }
    return ESP_OK;
}

// HTTP GET -pyynto
static esp_err_t http_get(const char *url) {
    // Varmista buffer on alustettu
    if (!http_buf) {
        http_buf = heap_caps_malloc(HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!http_buf) http_buf = malloc(HTTP_BUF_SIZE);
        if (!http_buf) return ESP_ERR_NO_MEM;
    }
    http_buf_len = 0;
    memset(http_buf, 0, HTTP_BUF_SIZE);

    esp_http_client_config_t config = {
        .url            = url,
        .event_handler  = http_event_handler,
        .timeout_ms     = 10000,
        .buffer_size    = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status: %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET epaonnistui: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

// Saakuvaus suomeksi
const char* weather_translate(const char *desc) {
    if (!desc) return "Tuntematon";
    if (strstr(desc, "thunderstorm with heavy rain")) return "Ukkosta, rankka sade";
    if (strstr(desc, "thunderstorm with rain"))       return "Ukkosta ja sadetta";
    if (strstr(desc, "thunderstorm"))                 return "Ukkosmyrsky";
    if (strstr(desc, "heavy intensity rain"))         return "Rankka sade";
    if (strstr(desc, "moderate rain"))                return "Kohtalainen sade";
    if (strstr(desc, "light rain"))                   return "Kevyt sade";
    if (strstr(desc, "freezing rain"))                return "Jaatava sade";
    if (strstr(desc, "shower rain"))                  return "Sadekuuro";
    if (strstr(desc, "rain"))                         return "Sadetta";
    if (strstr(desc, "heavy snow"))                   return "Runsas lumisade";
    if (strstr(desc, "light snow"))                   return "Kevyt lumisade";
    if (strstr(desc, "sleet"))                        return "Ranta";
    if (strstr(desc, "snow"))                         return "Lumisadetta";
    if (strstr(desc, "mist"))                         return "Sumua";
    if (strstr(desc, "fog"))                          return "Sumua";
    if (strstr(desc, "haze"))                         return "Usvaa";
    if (strstr(desc, "overcast clouds"))              return "Pilvinen";
    if (strstr(desc, "broken clouds"))                return "Melko pilvinen";
    if (strstr(desc, "scattered clouds"))             return "Puolipilvinen";
    if (strstr(desc, "few clouds"))                   return "Melko selkeaa";
    if (strstr(desc, "cloud"))                        return "Pilvista";
    if (strstr(desc, "clear sky"))                    return "Selkeaa";
    if (strstr(desc, "clear"))                        return "Selkeaa";
    return desc;
}

// Tuulen suunta
const char* weather_wind_dir(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg < 22)  return "P";
    if (deg < 67)  return "KP";
    if (deg < 112) return "I";
    if (deg < 157) return "KI";
    if (deg < 202) return "E";
    if (deg < 247) return "LE";
    if (deg < 292) return "L";
    if (deg < 337) return "LP";
    return "P";
}

// Lampomittarin vari (LVGL uint32_t muoto: 0xRRGGBB)
uint32_t weather_temp_color(float temp) {
    if (temp <= -15) return 0x0033CC;  // tumma sininen
    if (temp <= -5)  return 0x0099FF;  // sininen
    if (temp <= 2)   return 0x00CCFF;  // syaani
    if (temp <= 10)  return 0x00CC66;  // vihrea
    if (temp <= 20)  return 0xFFCC00;  // keltainen
    if (temp <= 28)  return 0xFF6600;  // oranssi
    return 0xFF2200;                   // punainen
}

// UV-indeksin arvio pilvista ja vuorokauden ajasta
int weather_uv_estimate(int clouds, long sunrise, long sunset, long now) {
    if (now < sunrise || now > sunset) return 0;
    long day_len = sunset - sunrise;
    long elapsed = now - sunrise;
    float day_pos = (float)elapsed / day_len;  // 0..1
    // Huippuarvo puolenpaivan aikoihin
    float peak = 4.0f * day_pos * (1.0f - day_pos) * 10.0f;
    // Pilvisyys vahentaa UV:ta
    float cloud_factor = 1.0f - (clouds / 100.0f) * 0.75f;
    int uv = (int)(peak * cloud_factor + 0.5f);
    if (uv < 0) uv = 0;
    if (uv > 11) uv = 11;
    return uv;
}

// UV-indeksin teksti
static const char* uv_label_str(int uv) {
    if (uv <= 2)  return "Matala";
    if (uv <= 5)  return "Kohtalainen";
    if (uv <= 7)  return "Korkea";
    if (uv <= 10) return "Erittain korkea";
    return "Aarimmainen";
}

// UNIX-aika -> "HH:MM"
static void unix_to_time_str(long unix_time, char *buf, size_t len) {
    long local = unix_time + GMT_OFFSET_SEC + DST_OFFSET_SEC;
    int h = (local % 86400L) / 3600;
    int m = (local % 3600) / 60;
    snprintf(buf, len, "%02d:%02d", h, m);
}

esp_err_t weather_init(void) {
    return ESP_OK;
}

// Hae nykyinen saa + ennuste
esp_err_t weather_fetch(WeatherData_t *data) {
    // Varmista etta DNS toimii ennen HTTP-hakua
    wait_for_dns();

    char url[256];
    esp_err_t err;

    // --- Nykyinen saa ---
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/weather?id=%s&units=metric&appid=%s",
        OWM_CITY_ID, OWM_API_KEY);

    err = http_get(url);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(http_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error (saa)");
        return ESP_FAIL;
    }

    cJSON *jdt = cJSON_GetObjectItem(root, "dt");
    if (jdt) data->dt = jdt->valueint;

    cJSON *main  = cJSON_GetObjectItem(root, "main");
    cJSON *wind  = cJSON_GetObjectItem(root, "wind");
    cJSON *clouds = cJSON_GetObjectItem(root, "clouds");
    cJSON *sys   = cJSON_GetObjectItem(root, "sys");
    cJSON *weather = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "weather"), 0);
    cJSON *rain  = cJSON_GetObjectItem(root, "rain");
    cJSON *vis   = cJSON_GetObjectItem(root, "visibility");

    if (main) {
        data->temp       = cJSON_GetObjectItem(main, "temp")->valuedouble;
        data->feels_like = cJSON_GetObjectItem(main, "feels_like")->valuedouble;
        data->temp_min   = cJSON_GetObjectItem(main, "temp_min")->valuedouble;
        data->temp_max   = cJSON_GetObjectItem(main, "temp_max")->valuedouble;
        data->humidity   = cJSON_GetObjectItem(main, "humidity")->valueint;
        data->pressure   = cJSON_GetObjectItem(main, "pressure")->valueint;
    }
    if (wind) {
        data->wind_speed = cJSON_GetObjectItem(wind, "speed")->valuedouble;
        data->wind_deg   = cJSON_GetObjectItem(wind, "deg")->valueint;
    }
    if (clouds) {
        data->clouds = cJSON_GetObjectItem(clouds, "all")->valueint;
    }
    if (sys) {
        data->sunrise = cJSON_GetObjectItem(sys, "sunrise")->valueint;
        data->sunset  = cJSON_GetObjectItem(sys, "sunset")->valueint;
    }
    if (weather) {
        cJSON *d = cJSON_GetObjectItem(weather, "description");
        cJSON *i = cJSON_GetObjectItem(weather, "icon");
        if (d) strncpy(data->description, d->valuestring, sizeof(data->description)-1);
        if (i) strncpy(data->icon_code,   i->valuestring, sizeof(data->icon_code)-1);
    }
    cJSON *rain1h = rain ? cJSON_GetObjectItem(rain, "1h") : NULL;
    data->rain_1h = rain1h ? rain1h->valuedouble : 0.0f;
    data->visibility = vis ? vis->valuedouble / 1000.0f : 10.0f;  // km

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Saadata haettu: %.1f°C %s", data->temp, data->description);

    // --- Ennuste --- hae 40 pistetta (5 paivaa x 8 pistetta/pv)
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/forecast?id=%s&units=metric&cnt=40&appid=%s",
        OWM_CITY_ID, OWM_API_KEY);

    err = http_get(url);
    if (err != ESP_OK) {
        data->valid = true;  // nykyinen saa ok, ennuste epaonnistui
        return ESP_OK;
    }

    root = cJSON_Parse(http_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error (ennuste)");
        data->valid = true;
        return ESP_OK;
    }

    cJSON *list = cJSON_GetObjectItem(root, "list");
    int count = cJSON_GetArraySize(list);

    // Hae 3h ennuste (6 pistetta)
    for (int i = 0; i < count && i < FORECAST_COUNT; i++) {
        cJSON *item = cJSON_GetArrayItem(list, i);
        long dt = cJSON_GetObjectItem(item, "dt")->valueint;
        unix_to_time_str(dt, data->forecast[i].time, sizeof(data->forecast[i].time));

        cJSON *fm = cJSON_GetObjectItem(item, "main");
        data->forecast[i].temp = fm ? cJSON_GetObjectItem(fm, "temp")->valuedouble : 0;

        cJSON *fr = cJSON_GetObjectItem(item, "rain");
        cJSON *fr1h = fr ? cJSON_GetObjectItem(fr, "3h") : NULL;
        data->forecast[i].rain = fr1h ? fr1h->valuedouble : 0;

        cJSON *fc = cJSON_GetObjectItem(item, "clouds");
        data->forecast[i].clouds = fc ? cJSON_GetObjectItem(fc, "all")->valueint : 0;

        cJSON *fw = cJSON_GetArrayItem(cJSON_GetObjectItem(item, "weather"), 0);
        cJSON *fd = fw ? cJSON_GetObjectItem(fw, "description") : NULL;
        if (fd) strncpy(data->forecast[i].desc, fd->valuestring, sizeof(data->forecast[i].desc)-1);
    }

    // Laske 5 paivan min/max koko listasta (40 pistetta = 5 paivaa)
    const char *paivat[] = {"Su","Ma","Ti","Ke","To","Pe","La"};
    for (int d = 0; d < DAILY_COUNT; d++) {
        data->daily[d].temp_max = -99.0f;
        data->daily[d].temp_min =  99.0f;
        data->daily[d].day[0]   = 0;
        data->daily[d].desc[0]  = 0;
    }

    int day_idx = -1;
    int last_mday = -1;
    for (int i = 0; i < count; i++) {  // kayda kaikki lapi
        cJSON *item = cJSON_GetArrayItem(list, i);
        long dt = cJSON_GetObjectItem(item, "dt")->valueint;
        // Kayta paikallista aikaa (UTC+2 tai UTC+3 kesalla)
        long local_dt = dt + 7200; // UTC+2, riittaa paivien tunnistukseen
        int mday = (int)(local_dt / 86400);
        int wday = (mday + 4) % 7; // 1.1.1970 = torstai

        if (mday != last_mday) {
            day_idx++;
            last_mday = mday;
            if (day_idx < DAILY_COUNT) {
                strncpy(data->daily[day_idx].day, paivat[wday], 3);
                data->daily[day_idx].day[2] = 0;
                ESP_LOGI(TAG, "Paiva %d: %s (dt=%ld)", day_idx, data->daily[day_idx].day, dt);
            }
        }

        if (day_idx >= 0 && day_idx < DAILY_COUNT) {
            cJSON *fm = cJSON_GetObjectItem(item, "main");
            if (fm) {
                cJSON *jtmax = cJSON_GetObjectItem(fm, "temp_max");
                cJSON *jtmin = cJSON_GetObjectItem(fm, "temp_min");
                cJSON *jtemp = cJSON_GetObjectItem(fm, "temp");
                float tmax = jtmax ? (float)jtmax->valuedouble : (jtemp ? (float)jtemp->valuedouble : -99.0f);
                float tmin = jtmin ? (float)jtmin->valuedouble : (jtemp ? (float)jtemp->valuedouble : 99.0f);
                if (tmax > data->daily[day_idx].temp_max) data->daily[day_idx].temp_max = tmax;
                if (tmin < data->daily[day_idx].temp_min) data->daily[day_idx].temp_min = tmin;
            }
            // Kayta puolipäivän kuvausta
            int hour = (int)((local_dt % 86400) / 3600);
            if (hour >= 11 && hour <= 14 && data->daily[day_idx].desc[0] == 0) {
                cJSON *fw = cJSON_GetArrayItem(cJSON_GetObjectItem(item, "weather"), 0);
                cJSON *fd = fw ? cJSON_GetObjectItem(fw, "description") : NULL;
                if (fd) strncpy(data->daily[day_idx].desc, fd->valuestring,
                               sizeof(data->daily[day_idx].desc)-1);
            }
            // Jos kuvaus puuttuu, kayta ensimmaista saatavilla olevaa
            if (data->daily[day_idx].desc[0] == 0) {
                cJSON *fw = cJSON_GetArrayItem(cJSON_GetObjectItem(item, "weather"), 0);
                cJSON *fd = fw ? cJSON_GetObjectItem(fw, "description") : NULL;
                if (fd) strncpy(data->daily[day_idx].desc, fd->valuestring,
                               sizeof(data->daily[day_idx].desc)-1);
            }
        }
    }
    ESP_LOGI(TAG, "Daily ennuste: %d paivaa laskettu", day_idx + 1);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Ennuste haettu (%d kohtaa)", count);

    data->valid = true;
    time((time_t*)&data->last_update);
    return ESP_OK;
}
