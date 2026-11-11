#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include "stm32f4xx_hal.h"
#define NEOPIXEL_COUNT       4U
#define NEOPIXEL_BRIGHTNESS  180U

HAL_StatusTypeDef neopixel(int Pixel, int R, int G, int B);

#endif
