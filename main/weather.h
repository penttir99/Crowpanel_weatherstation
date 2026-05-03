#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// === MUOKKAA NÄMÄ ===
#define WIFI_SSID        "SINUN-WIFI"
#define WIFI_PASSWORD    "SALASANA"
#define OWM_API_KEY      "OMA-API-AVAIN"
#define OWM_CITY_ID      "653280"
#define UPDATE_INTERVAL_SEC  900   // 15 minuuttia

// Aikavyöhyke
#define GMT_OFFSET_SEC   7200      // UTC+2
#define DST_OFFSET_SEC   3600      // +1h kesäaika

#define FORECAST_COUNT   6
#define DAILY_COUNT      5

typedef struct {
    char day[4];        // "Ma", "Ti" jne
    float temp_max;
    float temp_min;
    char  desc[32];
} DailyForecast_t;

typedef struct {
    char time[6];       // "HH:MM"
    float temp;
    float rain;
    int   clouds;
    char  desc[32];
} ForecastSlot_t;

typedef struct {
    // Lämpötila
    float temp;
    float feels_like;
    float temp_min;
    float temp_max;
    // Ilmasto
    int   humidity;
    int   pressure;
    int   clouds;
    float visibility;   // metreissä
    // Tuuli
    float wind_speed;
    int   wind_deg;
    // Sade/lumi
    float rain_1h;
    // Aurinko
    long  sunrise;
    long  sunset;
    // Aikaleima
    long  dt;
    // Kuvaus
    char  description[64];
    char  icon_code[8];
    // Ennuste
    ForecastSlot_t forecast[FORECAST_COUNT];
    DailyForecast_t daily[DAILY_COUNT];
    // Status
    bool  valid;
    long  last_update;
} WeatherData_t;

// Funktiot
esp_err_t weather_init(void);
esp_err_t weather_fetch(WeatherData_t *data);
const char* weather_translate(const char *desc);
const char* weather_wind_dir(int deg);
uint32_t weather_temp_color(float temp);
int weather_uv_estimate(int clouds, long sunrise, long sunset, long now);
