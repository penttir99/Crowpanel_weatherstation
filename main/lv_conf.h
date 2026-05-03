/**
 * LVGL konfiguraatio - ohjaa muistiallokaatio PSRAM:iin
 * Kopioi tama tiedosto: C:\P4\Turn_on_the_screen\main\lv_conf.h
 */
#if 1  /* Ota kayttoon muuttamalla 0 -> 1 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include "esp_heap_caps.h"

/* Muistinhallinta - kayta PSRAM:ia */
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM == 0
    #define LV_MEM_SIZE (48U * 1024U)
#else
    #define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
    /* Allokoi aina PSRAM:sta, fallback sisaiseen */
    #define LV_MEM_CUSTOM_ALLOC(size) \
        ({ void *_p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM); \
           if (!_p) _p = malloc(size); _p; })
    #define LV_MEM_CUSTOM_FREE   free
    #define LV_MEM_CUSTOM_REALLOC(p, new_size) \
        ({ void *_p = heap_caps_realloc((p), (new_size), MALLOC_CAP_SPIRAM); \
           if (!_p) _p = realloc((p), (new_size)); _p; })
#endif

/* Muut tarvittavat asetukset */
#define LV_USE_LOG 0
#define LV_COLOR_DEPTH 16
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* Fontit */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1

#endif  /* LV_CONF_H */
#endif  /* Loppumerkki */
