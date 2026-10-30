/* USER CODE BEGIN Header */
/**
 * @file main.c
 * @brief Parqueos: cuatro LDR, FSM y display de 7 segmentos.
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdint.h>
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
#define NUM_PARQUEOS_TOTALES       8U
#define DISPLAY_ANODO_COMUN        0U
#define PERIODO_LECTURA_MS         20U
#define CONFIRMACION_CAMBIO_MS     150U
#define MUESTRAS_PROMEDIO          8U
#define ADC_TIMEOUT_MS            5U
#define ADC_MAXIMO                4095U
#define LUZ_PARA_LIBERAR_PCT       60U
#define LUZ_PARA_OCUPAR_PCT        40U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

/* USER CODE BEGIN PV */
static Parqueo_t parqueos[NUM_PARQUEOS_LOCALES];
static uint8_t espacios_disponibles = NUM_PARQUEOS_TOTALES;
static uint8_t actualizar_display = 1U;
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

static const uint8_t tabla_7segmentos[9] =
{
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F
};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
/* USER CODE BEGIN PFP */
static void Parqueos_Init(void);
static void Parqueos_ProcesarLdr(void);
static void Parqueos_ActualizarFSM(void);
static void Parqueos_CalcularDisponibles(void);
static HAL_StatusTypeDef ADC_LeerLdr(uint16_t valores[]);
static void Display_MostrarNumero(uint8_t numero);
static void Display_EscribirSegmento(GPIO_TypeDef *puerto, uint16_t pin,
                                    uint8_t encender);
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
    espacios_disponibles = NUM_PARQUEOS_TOTALES;
    actualizar_display = 1U;
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
    uint8_t ocupados = 0U;
    for (uint32_t sensor = 0U; sensor < NUM_PARQUEOS_LOCALES; ++sensor)
    {
        if (parqueos[sensor].estado == ESTADO_OCUPADO) { ++ocupados; }
    }
    uint8_t nuevo_total = NUM_PARQUEOS_TOTALES - ocupados;
    if (nuevo_total != espacios_disponibles)
    {
        espacios_disponibles = nuevo_total;
        actualizar_display = 1U;
    }
}

static void Display_EscribirSegmento(
    GPIO_TypeDef *puerto,
    uint16_t pin,
    uint8_t encender
)
{
    GPIO_PinState nivel;

#if DISPLAY_ANODO_COMUN

    nivel = (encender != 0U)
          ? GPIO_PIN_RESET
          : GPIO_PIN_SET;

#else

    nivel = (encender != 0U)
          ? GPIO_PIN_SET
          : GPIO_PIN_RESET;

#endif

    HAL_GPIO_WritePin(puerto, pin, nivel);
}


static void Display_MostrarNumero(uint8_t numero)
{
    if (numero > 8U)
    {
        return;
    }

    uint8_t patron = tabla_7segmentos[numero];

    Display_EscribirSegmento(
        GPIOB, GPIO_PIN_4, (patron >> 0) & 0x01U
    );

    Display_EscribirSegmento(
        GPIOB, GPIO_PIN_5, (patron >> 1) & 0x01U
    );

    Display_EscribirSegmento(
        GPIOB, GPIO_PIN_10, (patron >> 2) & 0x01U
    );

    Display_EscribirSegmento(
        GPIOB, GPIO_PIN_12, (patron >> 3) & 0x01U
    );

    Display_EscribirSegmento(
        GPIOC, GPIO_PIN_0, (patron >> 4) & 0x01U
    );

    Display_EscribirSegmento(
        GPIOC, GPIO_PIN_1, (patron >> 5) & 0x01U
    );

    Display_EscribirSegmento(
        GPIOC, GPIO_PIN_2, (patron >> 6) & 0x01U
    );
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
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  HAL_NVIC_DisableIRQ(EXTI3_IRQn);
  Parqueos_Init();
  Display_MostrarNumero(espacios_disponibles);
  actualizar_display = 0U;
/* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Parqueos_ProcesarLdr();
    Parqueos_ActualizarFSM();
    if (actualizar_display != 0U)
    {
        Display_MostrarNumero(espacios_disponibles);
        actualizar_display = 0U;
    }
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
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_4;
  sConfig.Rank = 3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_8;
  sConfig.Rank = 4;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */
  /* Mantener los cuatro ranks; avanzar uno por disparo para leer sin DMA. */
  hadc1.Init.DiscontinuousConvMode = ENABLE;
  hadc1.Init.NbrOfDiscConversion = 1U;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
      Error_Handler();
  }
/* USER CODE END ADC1_Init 2 */

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
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10|GPIO_PIN_12|GPIO_PIN_4|GPIO_PIN_5, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PC0 PC1 PC2 */
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : USART_TX_Pin USART_RX_Pin */
  GPIO_InitStruct.Pin = USART_TX_Pin|USART_RX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PB10 PB12 PB4 PB5 */
  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_12|GPIO_PIN_4|GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : BTN_P4_Pin */
  GPIO_InitStruct.Pin = BTN_P4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BTN_P4_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

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
  while (1)
  {
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
