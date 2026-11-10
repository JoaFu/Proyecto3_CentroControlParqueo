#include "neopixel.h"

extern TIM_HandleTypeDef htim2;

/* 80 MHz / 100 = 800 kHz. T0H=400 ns, T1H=800 ns.
 * 260 periodos a nivel bajo = 325 us de reset antes y despues.
 * Los ceros finales tambien cubren la precarga del registro CCR1.
 */
#define NP_RESET_SLOTS 260U
#define NP_DATA_SLOTS  (24U * NEOPIXEL_COUNT)
#define NP_FRAME_SLOTS (2U * NP_RESET_SLOTS + NP_DATA_SLOTS)
#define NP_T0H         32U
#define NP_T1H         64U
#define NP_TIMEOUT_MS  20U

static uint8_t colors[NEOPIXEL_COUNT][3]; /* GRB */
static uint32_t pwm[NP_FRAME_SLOTS];       /* DMA: Word -> Word */
static volatile uint8_t transfer_done;
static uint8_t busy;

static uint8_t clamp_color(int value)
{
    if (value < 0) { return 0U; }
    if (value > 255) { return 255U; }
    return (uint8_t)value;
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if ((htim == &htim2) &&
        (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1))
    {
        /* El buffer ya lleva mas de 320 us de nivel bajo al terminar.
         * El bucle principal detiene el timer; CCR1 permanece en cero.
         */
        transfer_done = 1U;
    }
}

HAL_StatusTypeDef neopixel(int Pixel, int R, int G, int B)
{
    HAL_StatusTypeDef status;
    uint32_t started;
    uint32_t index = NP_RESET_SLOTS;

    if ((Pixel < 0) || ((uint32_t)Pixel >= NEOPIXEL_COUNT))
    {
        return HAL_ERROR;
    }
    if ((__get_IPSR() != 0U) || (__get_PRIMASK() != 0U))
    {
        return HAL_ERROR;
    }
    if (busy != 0U) { return HAL_BUSY; }
    if ((htim2.Instance != TIM2) ||
        (htim2.hdma[TIM_DMA_ID_CC1] == NULL))
    {
        return HAL_ERROR;
    }
    if ((htim2.Instance->PSC != 0U) ||
        (htim2.Instance->ARR != 104U))
    {
        return HAL_ERROR;
    }
    busy = 1U;
    colors[Pixel][0] = clamp_color(G);
    colors[Pixel][1] = clamp_color(R);
    colors[Pixel][2] = clamp_color(B);

    /* Los extremos del buffer estatico permanecen a cero. */
    for (uint32_t pixel = 0U; pixel < NEOPIXEL_COUNT; ++pixel)
    {
        for (uint32_t component = 0U; component < 3U; ++component)
        {
            uint8_t value = colors[pixel][component];
            for (uint32_t mask = 0x80U; mask != 0U; mask >>= 1U)
            {
                pwm[index++] = ((value & mask) != 0U) ? NP_T1H : NP_T0H;
            }
        }
    }

    __HAL_TIM_DISABLE(&htim2);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0U);
    /* Transferir cero a la precarga y reiniciar contador/prescaler. */
    htim2.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_SET_COUNTER(&htim2, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE | TIM_FLAG_CC1);
    transfer_done = 0U;
    status = HAL_TIM_PWM_Start_DMA(&htim2, TIM_CHANNEL_1,
                                   pwm, (uint16_t)NP_FRAME_SLOTS);
    if (status != HAL_OK)
    {
        busy = 0U;
        return status;
    }

    started = HAL_GetTick();
    while (transfer_done == 0U)
    {
        if ((HAL_GetTick() - started) >= NP_TIMEOUT_MS)
        {
            (void)HAL_TIM_PWM_Stop_DMA(&htim2, TIM_CHANNEL_1);
            __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0U);
            htim2.Instance->EGR = TIM_EGR_UG;
            busy = 0U;
            return HAL_TIMEOUT;
        }
    }
    status = HAL_TIM_PWM_Stop_DMA(&htim2, TIM_CHANNEL_1);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0U);
    htim2.Instance->EGR = TIM_EGR_UG;
    busy = 0U;
    return status;
}
