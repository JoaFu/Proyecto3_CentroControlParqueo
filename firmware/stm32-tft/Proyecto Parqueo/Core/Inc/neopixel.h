#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * STM32F446RE + hasta 4 WS2812B RGB (800 kHz, orden GRB).
 * No compatible con NeoPixels RGBW ni protocolos de 400 kHz.
 * Copiar neopixel.h a Core/Inc y neopixel.c a Core/Src.
 *
 * CONFIGURACION EN CUBEMX:
 * 1. Clock Configuration: APB1 Timer clocks = 80 MHz.
 *    Ejemplo: HCLK=160 MHz, APB1 prescaler=/4, PCLK1=40 MHz,
 *    TIMPRE desactivado (modo por defecto). Comprobar los 80 MHz
 *    en el reloj de timers APB1, no solo en el reloj del bus.
 * 2. TIM2: Clock Source = Internal Clock; Channel1 = PWM Generation CH1.
 *    Prescaler=0; Counter Mode=Up; Counter Period=99;
 *    Clock Division=DIV1; Auto-reload preload=Enable.
 *    CH1: PWM mode 1, Pulse=0, Polarity=High, Fast Mode=Disable.
 *    Reservar TIM2 exclusivamente para esta libreria.
 * 3. Pin PA0 = TIM2_CH1 (AF1), push-pull, sin pull, velocidad Very High.
 *    Conectar PA0 al DIN del primer pixel; encadenar DOUT -> DIN.
 * 4. TIM2 / DMA Settings: agregar solicitud TIM2_CH1:
 *    DMA1 Stream5, Channel3; Memory to Peripheral; Normal;
 *    Peripheral increment=Disable; Memory increment=Enable;
 *    Peripheral data width=Word; Memory data width=Word;
 *    Priority=High; FIFO=Disable. No usar modo Circular.
 * 5. NVIC: habilitar DMA1 Stream5 global interrupt.
 *    Conservar el handler generado que llama a HAL_DMA_IRQHandler().
 *    No hace falta habilitar la interrupcion global TIM2.
 * 6. Project Manager / Advanced Settings: inicializar MX_DMA_Init()
 *    ANTES de MX_TIM2_Init(). Usar HAL y USE_HAL_TIM_REGISTER_CALLBACKS=0.
 *
 * USO (despues de HAL_Init, SystemClock_Config y MX_*_Init):
 *     #include "neopixel.h"
 *     if (neopixel(0, 255, 0, 0) != HAL_OK) { Error_Handler(); }
 *
 * Pixel: 0..3, siendo 0 el mas cercano al STM32. Cambiar NEOPIXEL_COUNT
 * a 1, 2, 3 o 4 segun la cantidad fisica. RGB se limita a 0..255.
 * Cada llamada actualiza un pixel y transmite toda la cadena, conservando
 * los otros colores (inicialmente apagados). No es necesario un show().
 * Funcion bloqueante (~0.8 ms para cuatro pixels); retorna HAL_OK,
 * HAL_ERROR, HAL_BUSY o HAL_TIMEOUT. Invocar desde el bucle principal,
 * con interrupciones y SysTick activos; no desde una ISR ni concurrentemente.
 * Un fallo de envio no confirma el color fisico; la siguiente llamada
 * reenvia todos los colores almacenados, incluido el cambio pendiente.
 *
 * La libreria implementa HAL_TIM_PWM_PulseFinishedCallback(). Si el proyecto
 * ya tiene esa funcion, conservar una sola definicion e incorporar la rama
 * TIM2/CH1 de neopixel.c en la funcion existente.
 *
 * Alimentar los pixels con fuente adecuada, GND comun y adaptar PA0 de
 * 3.3 V a 5 V con un 74AHCT125 si los pixels trabajan a 5 V. Colocar una
 * resistencia de 330 ohm en serie cerca del DIN del primer pixel.
 *
 * Referencias de ST (TIM2, DMA y HAL):
 * https://www.st.com/resource/en/reference_manual/rm0390-stm32f446xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf
 * https://github.com/STMicroelectronics/stm32f4xx-hal-driver/blob/master/Src/stm32f4xx_hal_tim.c
 */
#define NEOPIXEL_COUNT 2U

#if (NEOPIXEL_COUNT < 1U) || (NEOPIXEL_COUNT > 4U)
#error "NEOPIXEL_COUNT debe estar entre 1 y 4"
#endif

HAL_StatusTypeDef neopixel(int Pixel, int R, int G, int B);

#ifdef __cplusplus
}
#endif
#endif
