#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "led_strip.h"
#include "lvgl.h"

// ============================================================================
// MAPEAMIENTO DE PINES Y CONFIGURACIÓN HARDWARE (ESP32-C3)
// ============================================================================
#define PIN_TFT_SCL GPIO_NUM_4
#define PIN_TFT_SDA GPIO_NUM_6
#define PIN_TFT_RES GPIO_NUM_8
#define PIN_TFT_DC GPIO_NUM_9
#define PIN_TFT_CS GPIO_NUM_7
#define PIN_TFT_BLK GPIO_NUM_10

#define PIN_ENC_A GPIO_NUM_2
#define PIN_ENC_B GPIO_NUM_3
#define PIN_PUSH GPIO_NUM_5
#define PIN_KO GPIO_NUM_1
#define PIN_WS2815_DATA GPIO_NUM_11

#define LCD_H_RES 240
#define LCD_V_RES 320
#define WS2815_LED_COUNT 240
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_INPUT_PERIOD_MS 5
#define LVGL_HANDLER_PERIOD_MS 10

static const char *TAG = "LVGL_APP";

// ============================================================================
// MANEJADORES GLOBALES DE LVGL Y HARDWARE
// ============================================================================
static int encoder_position = 0;
static uint8_t last_encoder_state = 0;
static lv_indev_t *indev_encoder = NULL;
static lv_group_t *g_main_group = NULL;
static SemaphoreHandle_t lvgl_mutex = NULL;
static led_strip_handle_t ws2815_strip = NULL;

static lv_obj_t *starting_screen = NULL;
static lv_obj_t *main_menu_screen = NULL;

// Declaraciones previas
static void create_main_menu(void);

static void ws2815_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint32_t index = 0; index < WS2815_LED_COUNT; index++)
    {
        ESP_ERROR_CHECK(led_strip_set_pixel(ws2815_strip, index, red, green, blue));
    }
    ESP_ERROR_CHECK(led_strip_refresh(ws2815_strip));
}

static void init_ws2815(void)
{
    ESP_LOGI(TAG, "Inicializando WS2815 en GPIO11 (%d LEDs)", WS2815_LED_COUNT);
    gpio_reset_pin(PIN_WS2815_DATA);
    ESP_ERROR_CHECK(gpio_set_direction(PIN_WS2815_DATA, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(PIN_WS2815_DATA, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_WS2815_DATA,
        .max_leds = WS2815_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &ws2815_strip));
    ESP_ERROR_CHECK(led_strip_clear(ws2815_strip));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "WS2815 inicializado y apagado");
}

// ============================================================================
// 1. MANEJADOR DE TIEMPO DE LVGL (Tick Interface)
// ============================================================================
static void lvgl_increase_tick(void *arg)
{
    lv_tick_inc(2); // Incrementa el reloj interno de LVGL en 2 ms
}

// ============================================================================
// 2. CALLBACKS DE LA PANTALLA (ESP_LCD -> LVGL)
// ============================================================================
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

// ============================================================================
// 3. INICIALIZACIÓN DE PANTALLA TFT VIA SPI (esp_lcd)
// ============================================================================
static void init_lcd_display(void)
{
    ESP_LOGI(TAG, "Inicializando retroiluminacion...");
    gpio_reset_pin(PIN_TFT_BLK);
    gpio_set_direction(PIN_TFT_BLK, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_TFT_BLK, 1);

    ESP_LOGI(TAG, "Inicializando Bus SPI...");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_TFT_SCL,
        .mosi_io_num = PIN_TFT_SDA,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Instalando IO del panel LCD...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_TFT_DC,
        .cs_gpio_num = PIN_TFT_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Instalando driver de panel ST7789/ILI9341...");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_TFT_RES,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Inicializar LVGL y registrar buffers
    lv_init();

    static lv_disp_draw_buf_t disp_buf;
    lv_color_t *buf1 = heap_caps_malloc(LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 != NULL);
    assert(buf2 != NULL);

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;

    io_config.on_color_trans_done = notify_lvgl_flush_ready;
    esp_lcd_panel_io_register_event_callbacks(io_handle, &(esp_lcd_panel_io_callbacks_t){
                                                             .on_color_trans_done = notify_lvgl_flush_ready,
                                                         },
                                              &disp_drv);

    lv_disp_drv_register(&disp_drv);
}

// ============================================================================
// 4. CALLBACK DE LECTURA DE ENTRADAS PARA LVGL (Encoder + Botón PUSH)
// ============================================================================
static void encoder_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static const int8_t transition_table[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0};
    uint8_t current_state = (uint8_t)((gpio_get_level(PIN_ENC_A) << 1) |
                                      gpio_get_level(PIN_ENC_B));

    encoder_position += transition_table[(last_encoder_state << 2) | current_state];
    last_encoder_state = current_state;
    data->enc_diff = (int16_t)(encoder_position / 4);
    encoder_position %= 4;

    if (gpio_get_level(PIN_PUSH) == 0)
    {
        data->state = LV_INDEV_STATE_PR;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ============================================================================
// 5. TAREA DE MONITOREO PARA BOTÓN KO (Atrás / Volver)
// ============================================================================
static void ko_button_task(void *pvParameters)
{
    while (1)
    {
        if (gpio_get_level(PIN_KO) == 0)
        {
            ESP_LOGI(TAG, "Botón KO presionado -> Volviendo al menú principal");

            xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
            if (lv_scr_act() != main_menu_screen && main_menu_screen != NULL)
            {
                lv_scr_load_anim(main_menu_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
            }
            xSemaphoreGive(lvgl_mutex);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// 6. EVENT HANDLER DE LOS BOTONES DEL MENÚ
// ============================================================================
static void menu_btn_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = lv_event_get_target(e);

    if (code == LV_EVENT_CLICKED)
    {
        uint32_t option = lv_obj_get_index(btn);
        switch (option)
        {
        case 0:
            ws2815_set_color(255, 0, 0);
            ESP_LOGI(TAG, "WS2815: rojo");
            break;
        case 1:
            ws2815_set_color(0, 255, 0);
            ESP_LOGI(TAG, "WS2815: verde");
            break;
        case 2:
            ws2815_set_color(0, 0, 255);
            ESP_LOGI(TAG, "WS2815: azul");
            break;
        case 3:
            ESP_ERROR_CHECK(led_strip_clear(ws2815_strip));
            ESP_LOGI(TAG, "WS2815: apagado");
            break;
        default:
            ESP_LOGW(TAG, "Opción de menú desconocida: %" PRIu32, option);
            break;
        }
    }
}

// ============================================================================
// 7. CONSTRUCCIÓN DEL MENÚ PRINCIPAL
// ============================================================================
static void create_main_menu(void)
{
    main_menu_screen = lv_obj_create(NULL);
    g_main_group = lv_group_create();
    lv_group_set_default(g_main_group);
    lv_indev_set_group(indev_encoder, g_main_group);

    lv_obj_t *title = lv_label_create(main_menu_screen);
    lv_label_set_text(title, "Menú Principal");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t *list = lv_list_create(main_menu_screen);
    lv_obj_set_size(list, 200, 220);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);

    const char *options[] = {
        "1. WS2815 Rojo",
        "2. WS2815 Verde",
        "3. WS2815 Azul",
        "4. WS2815 Apagar"};

    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *btn = lv_list_add_btn(list, LV_SYMBOL_SETTINGS, options[i]);
        lv_obj_add_event_cb(btn, menu_btn_event_handler, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(g_main_group, btn);
    }
}

// ============================================================================
// 8. PANTALLA DE INICIO (Splash) Y CALLBACK DE TEMPORIZADOR
// ============================================================================
static void timer_callback_pantalla_inicio(void *arg)
{
    xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    create_main_menu();
    lv_scr_load_anim(main_menu_screen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
    xSemaphoreGive(lvgl_mutex);
}

static void create_starting_screen(void)
{
    starting_screen = lv_scr_act();

    lv_obj_t *lbl_title = lv_label_create(starting_screen);
    lv_label_set_text(lbl_title, "Starting...");
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *bar = lv_bar_create(starting_screen);
    lv_obj_set_size(bar, 180, 15);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 10);
    lv_bar_set_value(bar, 100, LV_ANIM_ON);

    esp_timer_create_args_t timer_args = {
        .callback = &timer_callback_pantalla_inicio,
        .arg = NULL,
        .name = "splash_timer"};

    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);
    esp_timer_start_once(timer, 2500000);
}

// ============================================================================
// 9. INICIALIZACIÓN DE ENTRADAS HARDWARE (GPIOs)
// ============================================================================
static void init_inputs(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B) |
                        (1ULL << PIN_PUSH) | (1ULL << PIN_KO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    last_encoder_state = (uint8_t)((gpio_get_level(PIN_ENC_A) << 1) |
                                   gpio_get_level(PIN_ENC_B));

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_ENCODER;
    indev_drv.read_cb = encoder_read_cb;
    indev_encoder = lv_indev_drv_register(&indev_drv);
    lv_timer_set_period(indev_encoder->driver->read_timer, LVGL_INPUT_PERIOD_MS);

    xTaskCreate(ko_button_task, "ko_task", 2048, NULL, 5, NULL);
}

// ============================================================================
// APLICACIÓN PRINCIPAL (app_main)
// ============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando hardware");
    // 1. Inicializar pantalla y LVGL
    init_lcd_display();
    init_ws2815();
    lvgl_mutex = xSemaphoreCreateMutex();
    assert(lvgl_mutex != NULL);

    // 2. Configurar temporizador del Tick de LVGL
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_increase_tick,
        .name = "lvgl_tick"};
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // 3. Inicializar hardware de entradas (Encoder + PUSH + KO)
    init_inputs();

    // 4. Crear la pantalla de inicio
    create_starting_screen();

    ESP_LOGI(TAG, "LVGL, pantalla y WS2815 iniciados correctamente.");

    // 5. Bucle principal de ejecución de LVGL
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(LVGL_HANDLER_PERIOD_MS));
        xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
        lv_timer_handler();
        xSemaphoreGive(lvgl_mutex);
    }
}