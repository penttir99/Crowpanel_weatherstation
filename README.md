Crowpanel Repo
 https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen

Amazon.de link
https://www.amazon.de/-/en/dp/B0G34ZXVZ6?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1

# CrowPanel Advanced 7" Weather Station — LVGL / ESP-IDF

## Files

- `main.c`    — LVGL user interface and main program
- `weather.c` — Weather data fetching (OpenWeatherMap) and helper functions
- `weather.h` — Data structures and settings

## Installation into Elecrow Lesson07 Project

### 1. Copy the files

Copy `main.c`, `weather.c` and `weather.h` into the project's `main/` directory.
Replace the existing `main.c` (or merge it).

### 2. Edit settings in `weather.h`

```c
#define WIFI_SSID        "YOUR-WIFI"
#define WIFI_PASSWORD    "PASSWORD"
#define OWM_API_KEY      "YOUR-API-KEY"
#define OWM_CITY_ID      "653280"  // Keminmaa
```

### 3. Add `weather.c` to CMakeLists.txt

Open `main/CMakeLists.txt` and add:

```cmake
idf_component_register(
    SRCS
        "main.c"
        "weather.c"          # <-- add this
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_http_client
        json
        esp_sntp
        lvgl
)
```

### 4. sdkconfig — important settings

Make sure these are set in sdkconfig:
CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=1
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=n
CONFIG_HEAP_POISONING_DISABLED=y

### 5. Build and flash

```bash
idf.py build flash monitor
```

## Notes

### WiFi init
If the Lesson07 project already initializes WiFi, **remove** the `wifi_init()` call
from `app_main` and use the existing WiFi code.

### LVGL init
Lesson07 initializes LVGL and the display. Remove any duplicate `lv_init()` and
`esp_lcd_*` calls from `app_main`.

### LVGL fonts
The code uses Montserrat fonts (12, 14, 16, 20, 24, 28, 48).
Make sure they are enabled in the project's `lv_conf.h`:

```c
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_48  1
```

### Memory
The P4 has 32MB PSRAM — more than enough. If you get memory-related build errors:
CONFIG_SPIRAM_USE_MALLOC=y

## Features

- Automatically updates weather data every 15 minutes
- Refresh button triggers an immediate update
- Clock updates every 10 seconds
- UV index is calculated automatically from cloud cover and sun position
- Thermometer changes color based on temperature
- Forecast graph shows the next 6 three-hour data points
- Wind compass displays direction and degrees
