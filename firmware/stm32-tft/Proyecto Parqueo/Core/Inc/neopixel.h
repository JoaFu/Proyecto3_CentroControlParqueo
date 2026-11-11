#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif
#define NEOPIXEL_COUNT 4U

HAL_StatusTypeDef neopixel(int Pixel, int R, int G, int B);

#ifdef __cplusplus
}
#endif
#endif
