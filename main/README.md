# CrowPanel Advanced 7" Sääasema — LVGL / ESP-IDF

## Tiedostot

- `main.c`    — LVGL-käyttöliittymä ja pääohjelma
- `weather.c` — Säädata haku (OpenWeatherMap) ja apufunktiot
- `weather.h` — Datarakenteet ja asetukset

## Asennus Elecrown Lesson07-projektiin

### 1. Kopioi tiedostot

Kopioi `main.c`, `weather.c` ja `weather.h` projektin `main/`-kansioon.
Korvaa olemassa oleva `main.c` (tai yhdistä se).

### 2. Muuta asetukset `weather.h`:ssa

```c
#define WIFI_SSID        "SINUN-WIFI"
#define WIFI_PASSWORD    "SALASANA"
#define OWM_API_KEY      "OMA-API-AVAIN"
#define OWM_CITY_ID      "653280"  // Keminmaa
```

### 3. Lisää `weather.c` CMakeLists.txt:ään

Avaa `main/CMakeLists.txt` ja lisää:

```cmake
idf_component_register(
    SRCS
        "main.c"
        "weather.c"          # <-- lisää tämä
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

### 4. sdkconfig — tärkeät asetukset

Varmista että nämä ovat sdkconfig:issa:

```
CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=1
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=n
CONFIG_HEAP_POISONING_DISABLED=y
```

### 5. Käännä ja lataa

```bash
idf.py build flash monitor
```

## Huomiot

### WiFi init
Jos Lesson07-projektissa on jo WiFi-alustus, **poista** `wifi_init()`-kutsu
`app_main`:sta ja käytä olemassa olevaa WiFi-koodia.

### LVGL init
Lesson07 alustaa LVGL:n ja näytön valmiiksi. Poista `app_main`:sta
mahdolliset päällekkäiset `lv_init()` ja `esp_lcd_*` kutsut.

### LVGL fontit
Koodissa käytetään Montserrat-fontteja (12, 14, 16, 20, 24, 28, 48).
Varmista että ne on käytössä projektin `lv_conf.h`:ssa:

```c
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_48  1
```

### Muisti
P4:ssä on 32MB PSRAM — riittää hyvin. Jos käännösvirheitä muistista:
```
CONFIG_SPIRAM_USE_MALLOC=y
```

## Toiminnallisuus

- Päivittää säätiedot automaattisesti 15 min välein
- Päivitä-nappi päivittää heti
- Kello päivittyy joka 10 sekunnin välein
- UV-indeksi lasketaan automaattisesti pilvistä ja auringon asennosta
- Lämpömittari muuttaa väriä lämpötilan mukaan
- Ennustegraafi näyttää 6 seuraavaa 3h pistettä
- Tuulikompassi näyttää suunnan ja asteluvun
