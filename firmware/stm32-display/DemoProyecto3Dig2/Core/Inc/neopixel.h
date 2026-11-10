#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include "stm32f4xx_hal.h"

/* Adaptado de la version funcional compartida por Wicho: WS2812B RGB, GRB.
 * PA15 AF1 / TIM2_CH1, DMA1 Stream5 Channel3, Word -> Word.
 * Reloj TIM2 84 MHz, PSC=0, ARR=104: 800 kHz.
 * CCR=32/64: alto 381/762 ns; reset de 325 us antes y despues.
 * Cada llamada cambia un pixel y transmite los cuatro (~770 us),
 * conservando los colores restantes. Pixel=0..3; RGB limitado a 0..255.
 * Invocar solo desde main, con IRQ habilitadas. Timeout: 20 ms.
 * HAL_OK confirma transferencia DMA, no realimentacion del LED fisico.
 */
#define NEOPIXEL_COUNT       4U
#define NEOPIXEL_BRIGHTNESS  180U

HAL_StatusTypeDef neopixel(int Pixel, int R, int G, int B);

#endif
