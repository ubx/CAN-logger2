#pragma once

#include "esp_http_server.h"

// Initializes WiFi as SoftAP
void wifi_init_softap();

// Starts webserver (returns handle)
httpd_handle_t start_webserver();

// Mount SD for WiFi file browsing
bool mount_sd_for_wifi();

// Unmount SD after WiFi phase
void unmount_sd_after_wifi();

// For idle timeout tracking
unsigned long get_last_web_activity();

void reset_web_activity();
