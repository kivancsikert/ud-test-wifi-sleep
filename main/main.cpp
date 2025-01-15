#include <stdio.h>

#include <atomic>
#include <chrono>

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

using namespace std::chrono;

std::atomic<uint64_t> sleepDurationInUs { 0 };
std::atomic<uint32_t> sleepCount { 0 };

esp_pm_lock_handle_t noSleep;

static const gpio_num_t ledGpio = GPIO_NUM_23;

extern "C" void app_main() {
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

    auto startTime = high_resolution_clock::now();
    while (true) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        auto endTime = high_resolution_clock::now();
        auto delayDurationInUs = duration_cast<microseconds>(endTime - startTime).count();
        ESP_LOGI("main", "Awake %.3f%% (%lu cycles)",
            (1.0 - ((double) sleepDurationInUs.exchange(0)) / ((double) delayDurationInUs)) * 100.0,
            sleepCount.exchange(0));
        fflush(stdout);
        startTime = endTime;
    }
}
