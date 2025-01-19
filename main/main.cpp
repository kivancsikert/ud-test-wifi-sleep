#include <stdio.h>

#include <atomic>
#include <chrono>

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "mqtt_client.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

using namespace std::chrono;

std::atomic<uint64_t> sleepDurationInUs { 0 };
std::atomic<uint32_t> sleepCount { 0 };

esp_pm_lock_handle_t noSleep;

/* FreeRTOS event group to signal when we are connected*/
EventGroupHandle_t wifiEventGroup;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char* TAG = "wifi station";

static int retryNum = 0;

static const gpio_num_t ledGpio = GPIO_NUM_23;

static void event_handler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
        if (retryNum < 3) {
            esp_wifi_connect();
            retryNum++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifiEventGroup, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) eventData;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        retryNum = 0;
        xEventGroupSetBits(wifiEventGroup, WIFI_CONNECTED_BIT);
    } else {
        ESP_LOGI(TAG, "wifi event: %ld", eventId);
    }
}

void connectWifi() {
    wifiEventGroup = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    esp_sleep_enable_wifi_wakeup();
    esp_sleep_enable_wifi_beacon_wakeup();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &event_handler,
        NULL,
        &instance_got_ip));

    wifi_config_t wifiConfig = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .listen_interval = 50,
            .threshold = {
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK,
            .sae_h2e_identifier = "",
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifiEventGroup,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s",
            CONFIG_ESP_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s",
            CONFIG_ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

const char* wifiStatus(EventBits_t bits) {
    if (bits & WIFI_CONNECTED_BIT) {
        return "connected";
    } else if (bits & WIFI_FAIL_BIT) {
        return "failed";
    } else {
        return "disconnected";
    }
}

static void log_error_if_nonzero(const char* message, int error_code) {
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, "topic/qos1", "data_3", 0, 1, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "topic/qos0", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "topic/qos1", 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_unsubscribe(client, "topic/qos1");
            ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "topic/qos0", "data", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

void connectMqtt() {
    esp_mqtt_client_config_t mqtt_config = {
        .broker {
            .address {
                .uri = CONFIG_ESP_MQTT_BROKER_URI,
            } }
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_config);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

extern "C" void app_main() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Set up GPIO for the LED
    gpio_config_t ledConfig = {
        .pin_bit_mask = 1ULL << ledGpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ledConfig);
    gpio_set_direction(ledGpio, GPIO_MODE_OUTPUT);
    gpio_sleep_set_direction(ledGpio, GPIO_MODE_OUTPUT);

    esp_pm_sleep_cbs_register_config_t cbs_conf = {
        .enter_cb = [](int64_t timeToSleepInUs, void* arg) {
            gpio_set_level(ledGpio, 1);
            return ESP_OK; },
        .exit_cb = [](int64_t timeSleptInUs, void* arg) {
            gpio_set_level(ledGpio, 0);
            sleepDurationInUs += timeSleptInUs;
            sleepCount++;
            return ESP_OK; },
    };
    ESP_ERROR_CHECK(esp_pm_light_sleep_register_cbs(&cbs_conf));

    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "no-sleep", &noSleep);

    ESP_LOGI(TAG, "Connecting...");
    connectWifi();
    ESP_LOGI(TAG, "Connected");

    connectMqtt();

    auto startTime = high_resolution_clock::now();
    auto lastPing = startTime;
    while (true) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        // auto endTime = high_resolution_clock::now();
        // auto delayDurationInUs = duration_cast<microseconds>(endTime - startTime).count();
        // ESP_LOGI(TAG, "Awake %.3f%% (%lu cycles), wifi %s",
        //     (1.0 - ((double) sleepDurationInUs.exchange(0)) / ((double) delayDurationInUs)) * 100.0,
        //     sleepCount.exchange(0),
        //     wifiStatus(xEventGroupGetBits(wifiEventGroup)));
        // fflush(stdout);
        // startTime = endTime;

        // if (endTime - lastPing > seconds(5)) {
        //     ESP_LOGI(TAG, "Pinging...");
        //     const struct addrinfo hints = {
        //         .ai_family = AF_INET,
        //         .ai_socktype = SOCK_STREAM,
        //     };
        //     struct addrinfo* res;
        //     struct in_addr* addr;
        //     int s, r;
        //     char recv_buf[64];

        //     while (1) {
        //         int err = getaddrinfo("444.hu", WEB_PORT, &hints, &res);
        //     }
        // }
    }
}
