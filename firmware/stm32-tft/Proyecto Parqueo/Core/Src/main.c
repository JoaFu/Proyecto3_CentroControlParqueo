/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2024 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ili9341.h"
#include "bitmaps.h"
#include "neopixel.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum
{
    ESTADO_DISPONIBLE = 0,
    ESTADO_OCUPADO
} EstadoParqueo_t;

typedef struct
{
    EstadoParqueo_t estado;
    uint8_t flag_ocupado;
    uint8_t candidato_ocupado;
    uint8_t cambio_pendiente;
    uint32_t inicio_cambio_ms;
} Parqueo_t;

typedef struct
{
    uint16_t adc_con_luz;
    uint16_t adc_sin_luz;
    uint8_t luz_para_liberar_pct;
    uint8_t luz_para_ocupar_pct;
} ConfiguracionLdr_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define NUM_PARQUEOS_LOCALES       4U
#define PERIODO_LECTURA_MS         20U
#define CONFIRMACION_CAMBIO_MS     150U
#define MUESTRAS_PROMEDIO          8U
#define ADC_TIMEOUT_MS            5U
#define ADC_MAXIMO                4095U
#define I2C_COMANDO_GLOBAL        0xA0U
#define GLOBAL_CADUCIDAD_MS       2000U
#define LUZ_PARA_LIBERAR_PCT       60U
#define LUZ_PARA_OCUPAR_PCT        40U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_tim2_ch1;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
extern const uint16_t fondo[];

static Parqueo_t parqueos[NUM_PARQUEOS_LOCALES];
static uint32_t ultima_lectura_ms;
volatile uint16_t lecturas_ldr[NUM_PARQUEOS_LOCALES];
volatile uint8_t luminosidad_pct[NUM_PARQUEOS_LOCALES];
volatile uint8_t lectura_adc_valida;
volatile uint32_t errores_adc;
/* Divisor: 3.3 V -> resistencia fija -> ADC -> LDR -> GND.
 * Referencias iniciales de escala completa, no mediciones calibradas.
 * Ajustar adc_con_luz/adc_sin_luz por sensor con las lecturas reales.
 * Con el divisor invertido, intercambiar las dos referencias. */
static const ConfiguracionLdr_t configuracion_ldr[NUM_PARQUEOS_LOCALES] =
{
    {0U, ADC_MAXIMO, LUZ_PARA_LIBERAR_PCT, LUZ_PARA_OCUPAR_PCT},
    {0U, ADC_MAXIMO, LUZ_PARA_LIBERAR_PCT, LUZ_PARA_OCUPAR_PCT},
    {0U, ADC_MAXIMO, LUZ_PARA_LIBERAR_PCT, LUZ_PARA_OCUPAR_PCT},
    {0U, ADC_MAXIMO, LUZ_PARA_LIBERAR_PCT, LUZ_PARA_OCUPAR_PCT}
};


/* STM32 #2: unicamente bits 0..3 = parqueos 5..8.
 * Cero inicial sigue el estado DISPONIBLE de arranque de la FSM original.
 * El protocolo existente no tiene indicador de validez.
 */
static volatile uint8_t ocupacion_publicada;
static uint8_t i2c_tx;
/* Dos bytes de protocolo; tercero solo como margen defensivo HAL F4. */
static uint8_t i2c_rx_global[3];
static uint8_t i2c_rx_global_activa;
/* ISR publica estos tres campos; main toma una copia coherente breve. */
static volatile uint8_t mascara_global_recibida;
static volatile uint8_t mascara_global_valida;
static volatile uint32_t ultima_mascara_ms;
/* Caducidad solo diagnostica; no se inventan nuevos estados ni avisos TFT. */
volatile uint8_t mascara_global_caducada = 1U;
static uint8_t mascara_global_dibujada;
static uint8_t mascara_global_inicializada;
static volatile uint8_t i2c_recuperar;
static volatile uint32_t i2c_ultimo_error;
static volatile uint32_t i2c_errores;
static uint32_t i2c_ultimo_intento;
static uint8_t neopixels_inicializados;
static uint8_t ocupacion_neopixels;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM2_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */
static void Actualizar_carros(uint16_t espacio, uint16_t disponible);
static void Actualizar_numero(uint16_t parqueos);
static void Parqueos_Init(void);
static HAL_StatusTypeDef ADC_LeerLdr(uint16_t valores[]);
static void Parqueos_ProcesarLdr(void);
static void Parqueos_ActualizarFSM(void);
static void Parqueos_CalcularDisponibles(void);
static void I2C_Escuchar(void);
static void I2C_Servicio(void);
static void TFT_ServicioGlobal(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void Parqueos_Init(void)
{
    for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
    {
        parqueos[sensor].estado = ESTADO_DISPONIBLE;
        parqueos[sensor].flag_ocupado = 0U;
        parqueos[sensor].cambio_pendiente = 0U;
        const ConfiguracionLdr_t *config = &configuracion_ldr[sensor];
        if (config->adc_con_luz == config->adc_sin_luz ||
            config->adc_con_luz > ADC_MAXIMO ||
            config->adc_sin_luz > ADC_MAXIMO ||
            config->luz_para_liberar_pct > 100U ||
            config->luz_para_ocupar_pct >= config->luz_para_liberar_pct)
        {
            Error_Handler();
        }
    }
    ultima_lectura_ms = HAL_GetTick() - PERIODO_LECTURA_MS;
}

static HAL_StatusTypeDef ADC_LeerLdr(uint16_t valores[])
{
    uint32_t sumas[NUM_PARQUEOS_LOCALES] = {0};
    HAL_StatusTypeDef resultado = HAL_OK;

    /* Un disparo por rank; detener solo al terminar la secuencia completa. */
    for (uint32_t muestra = 0U; muestra <= MUESTRAS_PROMEDIO; ++muestra)
    {
        for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
        {
            resultado = HAL_ADC_Start(&hadc1);
            if (resultado != HAL_OK) { goto finalizar; }
            if (HAL_ADC_GetError(&hadc1) != HAL_ADC_ERROR_NONE)
            {
                resultado = HAL_ERROR;
                goto finalizar;
            }
            resultado = HAL_ADC_PollForConversion(&hadc1, ADC_TIMEOUT_MS);
            if (resultado != HAL_OK) { goto finalizar; }
            if (__HAL_ADC_GET_FLAG(&hadc1, ADC_FLAG_OVR) != RESET)
            {
                resultado = HAL_ERROR;
                goto finalizar;
            }
            uint32_t lectura = HAL_ADC_GetValue(&hadc1);
            if (lectura > ADC_MAXIMO)
            {
                resultado = HAL_ERROR;
                goto finalizar;
            }
            if (muestra != 0U) { sumas[sensor] += lectura; }
        }
        if (HAL_ADC_Stop(&hadc1) != HAL_OK ||
            (hadc1.Instance->CR2 & ADC_CR2_ADON) != 0U)
        {
            Error_Handler();
        }
    }
    for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
    {
        valores[sensor] = (uint16_t)((sumas[sensor] + MUESTRAS_PROMEDIO / 2U)
                                    / MUESTRAS_PROMEDIO);
    }
    return HAL_OK;

finalizar:
    if (HAL_ADC_Stop(&hadc1) != HAL_OK ||
        (hadc1.Instance->CR2 & ADC_CR2_ADON) != 0U)
    {
        Error_Handler();
    }
    return resultado;
}

static void Parqueos_ProcesarLdr(void)
{
    uint32_t ahora = HAL_GetTick();
    if ((uint32_t)(ahora - ultima_lectura_ms) < PERIODO_LECTURA_MS) { return; }
    ultima_lectura_ms = ahora;
    uint16_t valores[NUM_PARQUEOS_LOCALES];

    if (ADC_LeerLdr(valores) != HAL_OK)
    {
        lectura_adc_valida = 0U;
        if (errores_adc < UINT32_MAX) { ++errores_adc; }
        for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
        {
            parqueos[sensor].cambio_pendiente = 0U;
        }
        return; /* Conservar el ultimo numero y reintentar la lectura. */
    }
    lectura_adc_valida = 1U;
    ahora = HAL_GetTick();

    for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
    {
        const ConfiguracionLdr_t *config = &configuracion_ldr[sensor];
        Parqueo_t *parqueo = &parqueos[sensor];
        lecturas_ldr[sensor] = valores[sensor];
        int32_t luz = (100 * ((int32_t)valores[sensor] - config->adc_sin_luz))
                    / ((int32_t)config->adc_con_luz - config->adc_sin_luz);
        if (luz < 0) { luz = 0; }
        if (luz > 100) { luz = 100; }
        luminosidad_pct[sensor] = (uint8_t)luz;

        uint8_t ocupado;
        if (luz >= config->luz_para_liberar_pct) { ocupado = 0U; }
        else if (luz <= config->luz_para_ocupar_pct) { ocupado = 1U; }
        else
        {
            parqueo->cambio_pendiente = 0U;
            continue;
        }

        if (ocupado == parqueo->flag_ocupado)
        {
            parqueo->cambio_pendiente = 0U;
        }
        else if (parqueo->cambio_pendiente == 0U ||
                 ocupado != parqueo->candidato_ocupado)
        {
            parqueo->candidato_ocupado = ocupado;
            parqueo->inicio_cambio_ms = ahora;
            parqueo->cambio_pendiente = 1U;
        }
        else if ((uint32_t)(ahora - parqueo->inicio_cambio_ms)
                 >= CONFIRMACION_CAMBIO_MS)
        {
            parqueo->flag_ocupado = ocupado;
            parqueo->cambio_pendiente = 0U;
        }
    }
}

static void Parqueos_ActualizarFSM(void)
{
    for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
    {
        Parqueo_t *parqueo = &parqueos[sensor];
        switch (parqueo->estado)
        {
            case ESTADO_DISPONIBLE:
                if (parqueo->flag_ocupado != 0U)
                    parqueo->estado = ESTADO_OCUPADO;
                break;
            case ESTADO_OCUPADO:
                if (parqueo->flag_ocupado == 0U)
                    parqueo->estado = ESTADO_DISPONIBLE;
                break;
            default:
                parqueo->estado = parqueo->flag_ocupado != 0U
                                ? ESTADO_OCUPADO : ESTADO_DISPONIBLE;
                break;
        }
    }
    Parqueos_CalcularDisponibles();
}

static void Parqueos_CalcularDisponibles(void)
{
    uint8_t mascara = 0U;
    for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
    {
        if (parqueos[sensor].estado == ESTADO_OCUPADO)
        {
            mascara |= (uint8_t)(1U << sensor);
        }
    }
    /* Publicacion local para I2C y NeoPixels, despues de toda la FSM.
     * La TFT se actualiza exclusivamente desde la mascara global del ESP. */
    Parqueo_PublicarConfirmados(mascara);
}



/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_SPI1_Init();
  MX_TIM2_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
    I2C_Escuchar();
	LCD_Init();
	LCD_Bitmap(0, 0, 320, 240, fondo);
	LCD_Print("Global", 260, 50, 1, 0xffffff, 0x0000);
	LCD_Print("Disp. :", 260, 70, 1, 0xffffff, 0x0000);
//	FillRect(0, 0, 319, 206, 0x421b);

	//LCD_Sprite(int x, int y, int width, int height, unsigned char bitmap[],int columns, int index, char flip, char offset);

	//LCD_Bitmap(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned char bitmap[]);
	//LCD_Bitmap(0, 0, 320, 240, fondo);
    /* Mismo estado inicial disponible que STM32 #1; no es calibracion.
     * Las lecturas siguientes confirman la ocupacion con 150 ms de filtro. */
    Parqueos_Init();
    Parqueos_CalcularDisponibles();
    /* No mostrar 0..8 como medicion global hasta recibir el primer A0. */
    LCD_Print("--", 280, 100, 1, 0xffffff, 0x0000);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1) {
        I2C_Servicio();
        Parqueos_ProcesarLdr();
        Parqueos_ActualizarFSM();
        TFT_ServicioGlobal();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	}
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 4;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_12;
  sConfig.Rank = 3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_13;
  sConfig.Rank = 4;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 34;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 104;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LCD_CS_Pin|SD_CS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_RESET_Pin */
  GPIO_InitStruct.Pin = LCD_RESET_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(LCD_RESET_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_DC_Pin */
  GPIO_InitStruct.Pin = LCD_DC_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(LCD_DC_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LCD_CS_Pin SD_CS_Pin */
  GPIO_InitStruct.Pin = LCD_CS_Pin|SD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* Solo desde main: conservar independientes sensores y NeoPixels locales.
 * La mascara global nunca sobrescribe ocupacion_publicada. */
void Parqueo_PublicarConfirmados(uint8_t mascara)
{
    uint8_t nueva = mascara & 0x0FU;
    uint8_t anterior = ocupacion_neopixels;
    ocupacion_publicada = nueva;
    for (uint32_t i = 0U; i < 4U; ++i) {
        if (!neopixels_inicializados || ((anterior ^ nueva) & (1U << i))) {
            uint8_t ocupado = (nueva >> i) & 1U;
            (void)neopixel((int)i, ocupado ? 180 : 0, ocupado ? 0 : 180, 0);
        }
    }
    ocupacion_neopixels = nueva;
    neopixels_inicializados = 1U;
}

static void TFT_ServicioGlobal(void)
{
    uint8_t mascara;
    uint8_t valida;
    uint32_t instante;
    /* Solo copiar datos compartidos: nunca dibujar con IRQ deshabilitadas.
     * Restaurar PRIMASK conserva el estado previo y evita mezclar paquetes. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    mascara = mascara_global_recibida;
    valida = mascara_global_valida;
    instante = ultima_mascara_ms;
    __set_PRIMASK(primask);

    mascara_global_caducada = (uint8_t)(!valida ||
        (uint32_t)(HAL_GetTick() - instante) >= GLOBAL_CADUCIDAD_MS);
    if (!valida) { return; }
    if (mascara_global_inicializada && mascara == mascara_global_dibujada) {
        return;
    }
    uint8_t cambios = mascara_global_inicializada
                    ? (uint8_t)(mascara ^ mascara_global_dibujada) : 0xFFU;
    uint16_t disponibles = 0U;
    for (uint32_t i = 0U; i < 8U; ++i) {
        uint8_t ocupado = (mascara >> i) & 1U;
        disponibles += (uint16_t)(1U - ocupado);
        if ((cambios & (1U << i)) != 0U) {
            Actualizar_carros((uint16_t)(i + 1U), (uint16_t)(1U - ocupado));
        }
    }
    Actualizar_numero(disponibles);
    mascara_global_dibujada = mascara;
    mascara_global_inicializada = 1U;
    /* Ante caducidad se conserva la ultima imagen. Indicacion visual de
     * perdida de comunicacion pendiente de definir, segun especificacion. */
}

static void I2C_Escuchar(void)
{
    if (HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK) {
        i2c_recuperar = 1U;
    }
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c,
                        uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    HAL_StatusTypeDef resultado;
    (void)AddrMatchCode;
    if (hi2c->Instance != I2C1 || i2c_recuperar) { return; }
    i2c_rx_global_activa = 0U;
    if (TransferDirection == I2C_DIRECTION_RECEIVE) {
        i2c_tx = ocupacion_publicada & 0x0FU; /* Buffer estable hasta STOP. */
        resultado = HAL_I2C_Slave_Seq_Transmit_IT(hi2c, &i2c_tx, 1U,
                                               I2C_FIRST_AND_LAST_FRAME);
    } else {
        i2c_rx_global[0] = 0U;
        i2c_rx_global[1] = 0U;
        i2c_rx_global_activa = 1U;
        resultado = HAL_I2C_Slave_Seq_Receive_IT(hi2c, i2c_rx_global, 2U,
                                              I2C_FIRST_AND_LAST_FRAME);
    }
    if (resultado != HAL_OK) {
        i2c_rx_global_activa = 0U;
        i2c_recuperar = 1U;
    }
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1 || i2c_recuperar || !i2c_rx_global_activa) {
        return;
    }
    i2c_rx_global_activa = 0U;
    /* No publicar paquetes truncados ni identificadores desconocidos. */
    if (hi2c->XferCount == 0U && i2c_rx_global[0] == I2C_COMANDO_GLOBAL) {
        mascara_global_recibida = i2c_rx_global[1];
        ultima_mascara_ms = HAL_GetTick();
        mascara_global_valida = 1U;
    }
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) { i2c_rx_global_activa = 0U; }
    if (hi2c->Instance == I2C1 && !i2c_recuperar) { I2C_Escuchar(); }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) { return; }
    i2c_rx_global_activa = 0U;
    i2c_ultimo_error = HAL_I2C_GetError(hi2c);
    /* El NACK final normal de una lectura de 1 byte lo gestiona HAL.
     * Si HAL llega a este callback (incluso AF), recuperar fuera de ISR. */
    ++i2c_errores;
    i2c_recuperar = 1U;
}

static void I2C_Servicio(void)
{
    uint32_t ahora = HAL_GetTick();
    if (!i2c_recuperar || (uint32_t)(ahora - i2c_ultimo_intento) < 50U) {
        return;
    }
    i2c_ultimo_intento = ahora;
    /* Aislar SOLO I2C1; SysTick, TIM2 y DMA siguen activos.
     * No generar pulsos de bus: tambien esta conectada STM32 #1. */
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    i2c_rx_global_activa = 0U;
    (void)HAL_I2C_DeInit(&hi2c1);
    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();
    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    /* MSP habilita las IRQ al reinicializar; CR2 aun no tiene IT activas. */
    if (HAL_I2C_Init(&hi2c1) == HAL_OK) {
        i2c_recuperar = 0U;
        I2C_Escuchar();
    }
    /* Ante fallo se reintenta en 50 ms sin Error_Handler ni HAL_Delay.
     * Un SDA/SCL retenido externamente requiere liberar la causa fisica. */
}


void Actualizar_carros(uint16_t espacio, uint16_t disponible)
{
    uint16_t x;
    uint16_t y;

    if (espacio < 1 || espacio > 8) {
        return;
    }

    // Coordenadas X: 4, 69, 134 y 199
    if (espacio == 1 || espacio == 5){
    	x = 4;
    }

    else if (espacio == 2 || espacio == 6){
    	x = 69;
    }
    else if (espacio == 3 || espacio == 7){
    	x = 134;
    }
    else if (espacio == 4 || espacio == 8){
    	x = 197;
    }
    // Espacios 1-4 arriba; espacios 5-8 abajo
    if (espacio <= 4) {
        y = 3;
    } else {
        y = 145;
    }

    if (disponible == 0) {
        // Espacio ocupado
        LCD_BitmapTransparent(x, y, 58, 92, carro, 0x0000);
    } else {
        // Espacio disponible
        LCD_BitmapTransparent(x, y, 58, 92, borrar_carro, 0x0000);
    }
}

void Actualizar_numero(uint16_t parqueos){
	LCD_Sprite(280,100,16,16,numeros,9,parqueos,0,0);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
	}
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
