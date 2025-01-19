# Ugly Duckling Test: WiFi with light sleep

A simple test project to figure out the most efficient mode to stay connected to WiFi while going to light sleep as much as possible.

## Setup

Create an `sdkconfig.local.defaults` with:

```properties
CONFIG_ESP_WIFI_SSID="SSID"
CONFIG_ESP_WIFI_PASSWORD="PASSWORD"
```

Then run:

```shell
idf.py build flash monitor
```
