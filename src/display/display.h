#pragma once
#include <LovyanGFX.hpp>
#include <lvgl.h>

// Forward-declare the concrete display class
class LGFX_TDisplayS3Pro;

void display_init();
void display_set_brightness(uint8_t level);  // 0–255
