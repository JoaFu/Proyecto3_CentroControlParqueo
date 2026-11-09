#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include "stm32f4xx_hal.h"

/* Adaptado de dev/wicho (92b4724): WS2812B RGB, orden GRB.
 * PA15 AF1 / TIM2_CH1, DMA1 Stream5 Channel3, Word -> Word.
 * Reloj TIM2 84 MHz, PSC=0, ARR=104: 800 kHz.
 * CCR=34/67: alto 405/798 ns; reset de 325 us antes y despues.
 * Una trama actualiza los cuatro pixels juntos (~770 us).
 * Invocar solo desde main, con IRQ habilitadas. Timeout: 20 ms.
 * Bit 0 = primer LED (P1); 0 libre/verde, 1 ocupado/rojo.
 * HAL_OK confirma transferencia DMA, no realimentacion del LED fisico.
 */
#define NEOPIXEL_COUNT       4U
#define NEOPIXEL_BRIGHTNESS  180U

HAL_StatusTypeDef neopixel_ocupacion(uint8_t mascara);

#endif
