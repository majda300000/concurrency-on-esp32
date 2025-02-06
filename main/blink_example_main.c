

#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "led_strip.h"

#define WIFI_SSID "🗝️💖"
#define WIFI_PASS "jelena123"

#define BUTTON_GPIO 0
#define LED_GPIO 32

static const char *TAG = "MAIN";
static QueueHandle_t button_queue;
static EventGroupHandle_t wifi_event_group;
static SemaphoreHandle_t wifi_connected_semaphore;
static led_strip_handle_t led_strip;

const int BUTTON_PRESSED_BIT = BIT0;
const int BITS_TO_WAIT = 7; // 7 => 0b111

volatile int press_number = 0;

// ISR funkcija za prekid tipke
void IRAM_ATTR button_isr_handler(void *arg)
{
    int button_state = 1;
    xQueueSendFromISR(button_queue, &button_state, NULL);

    xEventGroupSetBits(wifi_event_group, BUTTON_PRESSED_BIT << (press_number));
    press_number = (press_number + 1) % 3;
}


void led_set_state(int state)
{
    if (state)
    {
        led_strip_set_pixel(led_strip, 0, 16, 16, 16);
        led_strip_refresh(led_strip);
    }
    else
    {
        led_strip_clear(led_strip);
    }
}

// Wi-Fi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        xEventGroupWaitBits(wifi_event_group, BITS_TO_WAIT, false, true, portMAX_DELAY);
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(wifi_connected_semaphore);
    }
}

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected!");
        esp_mqtt_client_subscribe(client, "/led", 0);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT Received: %.*s", event->data_len, event->data);
        if (strncmp(event->data, "ON", 2) == 0)
        {
            led_set_state(1);
        }
        else if (strncmp(event->data, "OFF", 3) == 0)
        {
            led_set_state(0);
        }
        break;
    default:
        break;
    }
}

void button_task(void *pvParameters)
{
    int button_state;
    while (1)
    {
        if (xQueueReceive(button_queue, &button_state, portMAX_DELAY))
        {
            ESP_LOGI(TAG, "Button Pressed!");

            for (int i = 0; i < 10; i++)
            {

                led_strip_set_pixel(led_strip, 0, 16 * (i % 1), 16 * (i % 2), 16 * (i % 3));
                led_strip_refresh(led_strip);

                vTaskDelay(pdMS_TO_TICKS(100));

                led_strip_clear(led_strip);
            }
        }
    }
}

void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_event_group = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS},
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}

void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://192.168.237.87:1883"};
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}


void app_main(void)
{

    wifi_connected_semaphore = xSemaphoreCreateBinary();

    nvs_flash_init();
    wifi_init();

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE};

    gpio_config(&io_conf);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);

    button_queue = xQueueCreate(10, sizeof(int));
    xTaskCreate(button_task, "Button Task", 2048, NULL, 10, NULL);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    xSemaphoreTake(wifi_connected_semaphore, portMAX_DELAY);
    mqtt_init();
}