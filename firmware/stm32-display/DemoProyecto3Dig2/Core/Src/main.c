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
#define NUM_PARQUEOS_TOTALES       8U
#define DISPLAY_ANODO_COMUN        0U
#define PERIODO_LECTURA_MS         20U
#define CONFIRMACION_CAMBIO_MS     150U
#define MUESTRAS_PROMEDIO          8U
#define ADC_TIMEOUT_MS            5U
#define ADC_MAXIMO                4095U
#define LUZ_PARA_LIBERAR_PCT       60U
#define LUZ_PARA_OCUPAR_PCT        40U
#define I2C_DIRECCION_7BIT          0x10U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_tim2_ch1;

/* USER CODE BEGIN PV */
static uint8_t neopixels_inicializados;
static uint8_t ocupacion_neopixels;
static uint32_t neopixels_ultimo_intento;
volatile uint32_t neopixels_errores;
static Parqueo_t parqueos[NUM_PARQUEOS_LOCALES];
static uint8_t espacios_disponibles = NUM_PARQUEOS_TOTALES;
static uint8_t actualizar_display = 1U;
static uint32_t ultima_lectura_ms;
volatile uint16_t lecturas_ldr[NUM_PARQUEOS_LOCALES];
volatile uint8_t luminosidad_pct[NUM_PARQUEOS_LOCALES];
volatile uint8_t lectura_adc_valida;
volatile uint32_t errores_adc;

/* Main publica un byte atomico; la ISR conserva su propia copia estable. */
static volatile uint8_t ocupacion_publicada;
static uint8_t i2c_tx;
static volatile uint8_t i2c_recuperar;
volatile uint32_t i2c_ultimo_error;
volatile uint32_t i2c_errores;
volatile uint32_t i2c_lecturas;

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

static volatile uint8_t mascara_global_recibida;
static volatile uint8_t mascara_global_valida;
static uint8_t i2c_rx_global[3];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);


static void MX_DMA_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */
void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);
static void Neopixels_Servicio(void);
static void Parqueos_Init(void);
static void Parqueos_ActualizarTotalGlobal(void);
static void I2C_Servicio(void);
static void I2C_PedirRecuperacion(void);
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
    uint8_t mascara = 0U;

    for (uint32_t sensor = 0U;
         sensor < NUM_PARQUEOS_LOCALES;
         ++sensor)
    {
        if (parqueos[sensor].estado == ESTADO_OCUPADO)
        {
            mascara |= (uint8_t)(1U << sensor);
        }
    }

    // Mantener la publicación local para el ESP32.
    ocupacion_publicada = mascara;
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

static void Parqueos_ActualizarTotalGlobal(void)
{
    if (mascara_global_valida == 0U)
        return;

    uint8_t mascara = mascara_global_recibida;
    uint8_t ocupados = 0U;

    for (uint8_t i = 0U; i < 8U; ++i)
    {
        ocupados += (mascara >> i) & 1U;
    }

    uint8_t disponibles = 8U - ocupados;

    if (disponibles != espacios_disponibles)
    {
        espacios_disponibles = disponibles;
        actualizar_display = 1U;
    }
}

/* Una sola trama para los cuatro LEDs, desde el mismo byte que lee ESP32.
 * Confirmar el envio solo cuando DMA termina; reintentar fallos en 50 ms. */
static void Neopixels_Servicio(void)
{
    uint8_t mascara = ocupacion_publicada & 0x0FU;
    uint32_t ahora = HAL_GetTick();
    if (neopixels_inicializados && mascara == ocupacion_neopixels) { return; }
    if ((uint32_t)(ahora - neopixels_ultimo_intento) < 50U) { return; }
    neopixels_ultimo_intento = ahora;
    if (neopixel_ocupacion(mascara) == HAL_OK) {
        ocupacion_neopixels = mascara;
        neopixels_inicializados = 1U;
    } else {
        ++neopixels_errores;
    }
}
/* USER CODE END 0 */

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
  htim2.Init.Period = 104; /* 84 MHz / 105 = 800 kHz */
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
  MX_DMA_Init();
  MX_TIM2_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  HAL_NVIC_DisableIRQ(EXTI3_IRQn);
  Parqueos_Init();
  Parqueos_CalcularDisponibles();
  neopixels_ultimo_intento = HAL_GetTick() - 50U;
  Neopixels_Servicio();
  Display_MostrarNumero(espacios_disponibles);
  actualizar_display = 0U;
  if (HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK)
  {
      I2C_PedirRecuperacion();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    I2C_Servicio();
    Parqueos_ProcesarLdr();
    Parqueos_ActualizarFSM();
    Neopixels_Servicio();
    Parqueos_ActualizarTotalGlobal();
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
  hadc1.Init.DiscontinuousConvMode = ENABLE;
  hadc1.Init.NbrOfDiscConversion = 1U;
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
  sConfig.Channel = ADC_CHANNEL_0; /* LDR 1: PA0 */
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1; /* LDR 2: PA1 */
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_13; /* LDR 3: PC3 */
  sConfig.Rank = 3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_14; /* LDR 4: PC4 */
  sConfig.Rank = 4;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */
  /* Secuencia PA0, PA1, PC3, PC4: un rank por disparo, sin DMA. */
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
  hi2c1.Init.OwnAddress1 = 32;
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
static void I2C_PedirRecuperacion(void)
{
    i2c_recuperar = 1U;
    __HAL_I2C_DISABLE_IT(&hi2c1, I2C_IT_EVT | I2C_IT_BUF | I2C_IT_ERR);
    __HAL_I2C_DISABLE(&hi2c1);
}

static void I2C_Servicio(void)
{
    static uint32_t ultimo_intento;
    if (i2c_recuperar == 0U ||
        (uint32_t)(HAL_GetTick() - ultimo_intento) < 10U) { return; }
    ultimo_intento = HAL_GetTick();

    /* Recuperacion fuera de ISR. HAL F4 Init/DeInit no esperan al maestro.
     * Seccion acotada: impide carreras con EV/ER y restaura PRIMASK. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    HAL_StatusTypeDef resultado = HAL_I2C_DeInit(&hi2c1);
    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();
    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    if (resultado == HAL_OK) { resultado = HAL_I2C_Init(&hi2c1); }
    if (resultado == HAL_OK) { resultado = HAL_I2C_EnableListen_IT(&hi2c1); }
    if (resultado == HAL_OK) { i2c_recuperar = 0U; }
    else { I2C_PedirRecuperacion(); }
    __set_PRIMASK(primask);
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c,
                          uint8_t TransferDirection,
                          uint16_t AddrMatchCode)
{
    (void)AddrMatchCode;

    if (hi2c->Instance != I2C1 || i2c_recuperar != 0U)
        return;

    HAL_StatusTypeDef resultado;

    if (TransferDirection == I2C_DIRECTION_RECEIVE)
    {
        // ESP32 solicita la ocupacion local P1-P4
        i2c_tx = ocupacion_publicada;

        resultado = HAL_I2C_Slave_Seq_Transmit_IT(
            hi2c,
            &i2c_tx,
            1U,
            I2C_FIRST_AND_LAST_FRAME
        );

        if (resultado == HAL_OK)
            ++i2c_lecturas;
    }
    else
    {
        // ESP32 envia comando 0xA0 + mascara global
        resultado = HAL_I2C_Slave_Seq_Receive_IT(
            hi2c,
            i2c_rx_global,
            2U,
            I2C_FIRST_AND_LAST_FRAME
        );
    }

    if (resultado != HAL_OK)
        I2C_PedirRecuperacion();
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1)
        return;

    // Validar identificador de sincronizacion
    if (!i2c_recuperar && hi2c->XferCount == 0U && i2c_rx_global[0] == 0xA0)
    {
        mascara_global_recibida = i2c_rx_global[1];
        mascara_global_valida = 1U;
    }
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1 || i2c_recuperar != 0U) { return; }
    if (HAL_I2C_EnableListen_IT(hi2c) != HAL_OK) { I2C_PedirRecuperacion(); }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) { return; }
    i2c_ultimo_error = HAL_I2C_GetError(hi2c);
    ++i2c_errores;
    /* No rearmar aqui: I2C_ITError de F4 aun modifica interrupciones al salir.
     * El NACK normal tras leer un byte va a ListenCplt en esta HAL. */
    I2C_PedirRecuperacion();
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
