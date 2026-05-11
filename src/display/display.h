#pragma once
#include <stdint.h>

void display_init();
void display_set_brightness(uint8_t level);

// Set by SensorLib home-button callback, cleared by main loop after handling
extern volatile bool     g_home_pressed;

// Updated on every touch/button press — used for deep sleep timeout
extern volatile uint32_t g_last_activity;
