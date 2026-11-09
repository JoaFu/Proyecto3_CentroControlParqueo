# Pinout del centro de control de parqueo

Pines logicos del MCU, no numeros de posicion del conector. Basado en ESP32 y
Display de `dev/joaquin` (`c27a695`) y TFT de `dev/wicho` (`92b4724`).

## ESP32 — controlador principal

| Pin | Conexion / funcion |
|---|---|
| GPIO21 | I2C SDA → PB9 de ambas STM32 |
| GPIO22 | I2C SCL → PB8 de ambas STM32 |
| GND | Tierra comun con las dos STM32 y los neopixeles |

La comunicacion web usa Wi-Fi; no requiere pines adicionales.

## STM32 Display — parqueos P1 a P4 — I2C `0x10`

| Pin | Conexion / funcion |
|---|---|
| PA0 | LDR del parqueo P1 |
| PA1 | LDR del parqueo P2 |
| PC3 | LDR del parqueo P3 |
| PC4 | LDR del parqueo P4 |
| PA15 | DIN del primer neopixel; cadena de 4 LED para P1–P4 |
| PB9 | I2C SDA → GPIO21 de ESP32 |
| PB8 | I2C SCL → GPIO22 de ESP32 |
| PB4 | Display: segmento a |
| PB5 | Display: segmento b |
| PB10 | Display: segmento c |
| PB12 | Display: segmento d |
| PC0 | Display: segmento e |
| PC1 | Display: segmento f |
| PC2 | Display: segmento g |
| PA13 / PA14 | Depuracion SWD: datos / reloj |
| GND | Tierra comun |

El firmware configura display de catodo comun. Usar resistencia por segmento.
PA2/PA3 estan reservados para USART2, PA5 para el LED de placa y PC13 para
el boton de placa. PB3 conserva una configuracion de boton externo, pero
su interrupcion se deshabilita al arrancar y no controla los parqueos.

## STM32 TFT (Wicho) — parqueos P5 a P8 — I2C `0x11`

| Pin | Conexion / funcion |
|---|---|
| PA1 | LDR del parqueo P5 |
| PC0 | LDR del parqueo P6 |
| PC2 | LDR del parqueo P7 |
| PC3 | LDR del parqueo P8 |
| PA0 | DIN del primer neopixel; cadena de 4 LED para P5–P8 |
| PB9 | I2C SDA → GPIO21 de ESP32 |
| PB8 | I2C SCL → GPIO22 de ESP32 |
| PA5 | TFT: reloj SPI (SCK) |
| PA7 | TFT: datos desde STM32 (MOSI / SDI) |
| PA6 | SPI: datos hacia STM32 (MISO / SDO), configurado |
| PB0 | TFT: seleccion (CS) |
| PA4 | TFT: datos/comando (DC) |
| PC1 | TFT: reinicio (RESET) |
| PB6 | Seleccion de tarjeta SD (SD_CS), reservada |
| PA13 / PA14 | Depuracion SWD: datos / reloj |
| GND | Tierra comun |

PA2/PA3 se usan para USART2; PB3 esta reservado para SWO y PC13 para el boton
de placa. El pinout de los LDR se tomo del codigo de inicializacion: el `.ioc`
de Wicho aun tiene los cuatro ranks ADC declarados como canal 1, aunque el
codigo usa canales 1, 10, 12 y 13. Corregir esa discrepancia antes de regenerarlo.

## Conexiones comunes

- I2C compartido: las tres placas comparten SDA, SCL y GND. Pull-ups a 3.3 V.
- Cada STM32 controla su propia cadena de cuatro neopixeles: DOUT → DIN.
- Orden fisico: LED 1 al primer parqueo del grupo, hasta LED 4 al ultimo.
- Verde = libre; rojo = ocupado.
- Neopixeles a 5 V con fuente adecuada, GND comun, adaptador de nivel
  74AHCT125 para datos y resistencia de 330 ohm cerca del primer DIN.
- Los LDR se conectan mediante divisores de 3.3 V. No aplicar 5 V al ADC.
- La alimentacion de la TFT depende del modulo; verificar su especificacion.

Este documento describe la configuracion del firmware; falta validacion fisica
del cableado. Guardado localmente en `hardware`; no publicado automaticamente.
