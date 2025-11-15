#ifndef WIFI_H
#define WIFI_H

#include "freertos/event_groups.h"

#define WIFI_SSID      "Loveshack"
#define WIFI_PASS      "Babyloveshack"
// #define WIFI_SSID      "jolene"
// #define WIFI_PASS      "donttake"
#define INIT_WIFI_MAXIMUM_RETRY  5

/* Event group bits for WiFi connection status */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

void Wifi__Start(void);

/* Get the WiFi event group handle for waiting on connection status */
EventGroupHandle_t Wifi__GetEventGroup(void);

#endif