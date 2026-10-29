/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/** Frame stored in the software receive queue. */
typedef struct {
  uint32_t ts_us;      // Timestamp in microseconds (TIM2 counter value)
  uint32_t id;         // CAN identifier (11 or 29 bit)
  uint8_t  dlc;        // Data length code (0..8)
  uint8_t  ext;        // 1 = extended (29 bit) identifier
  uint8_t  rtr;        // 1 = remote transmission request
  uint8_t  data[8];    // Payload
} can_msg_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RXQ_SIZE      512U              // Queue length, must be a power of two
#define RXQ_MASK      (RXQ_SIZE - 1U)
#define STATS_PERIOD  5000U             // Statistics interval [ms]
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan1;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static can_msg_t rxq[RXQ_SIZE];                        // Ring buffer filled in the ISR
static volatile uint16_t rxq_head = 0;                 // Written by the ISR
static volatile uint16_t rxq_tail = 0;                 // Written by the main loop
static volatile uint32_t rx_total = 0;                 // All frames read from the FIFO
static volatile uint32_t rxq_dropped = 0;              // Frames lost: software queue full
static volatile uint32_t fifo_overruns = 0;            // Frames lost: hardware FIFO overrun
static const char HEXCH[] = "0123456789ABCDEF";
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CAN1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/** printf() retargeting (used for the banner and statistics only). */
int __io_putchar(int ch)
{
    if (ch == '\n')
    {
        uint8_t ch2 = '\r';
        HAL_UART_Transmit(&huart2, &ch2, 1, HAL_MAX_DELAY);
    }
    HAL_UART_Transmit(&huart2, (uint8_t*) &ch, 1, HAL_MAX_DELAY);
    return 1;
}


/** Accept-all receive filter (filter bank 0, FIFO0). */
static void CAN_FilterConfig(void)
{
  CAN_FilterTypeDef f = {0};
  f.FilterBank           = 0;
  f.FilterMode           = CAN_FILTERMODE_IDMASK;
  f.FilterScale          = CAN_FILTERSCALE_32BIT;
  f.FilterMaskIdHigh     = 0x0000;      // mask = 0 -> accept all IDs
  f.FilterMaskIdLow      = 0x0000;
  f.FilterFIFOAssignment = CAN_RX_FIFO0;
  f.FilterActivation     = ENABLE;
  if (HAL_CAN_ConfigFilter(&hcan1, &f) != HAL_OK) Error_Handler();
}

/** Write an unsigned number in decimal, return the new write pointer. */
static char *put_dec(char *p, uint32_t v)
{
  char tmp[10];
  uint8_t n = 0;
  do { tmp[n++] = (char)('0' + (v % 10U)); v /= 10U; } while (v != 0U);
  while (n != 0U) *p++ = tmp[--n];
  return p;
}

/** Write 'digits' uppercase hex digits of v, return the new write pointer. */
static char *put_hex(char *p, uint32_t v, uint8_t digits)
{
  for (int8_t i = (int8_t)digits - 1; i >= 0; i--) {
    *p++ = HEXCH[(v >> (4 * i)) & 0xFU];
  }
  return p;
}

/**
  * @brief  Format one frame in candump -L style, e.g. "(1.234567) can0 100#2D0114\n".
  * @note   Own formatter instead of printf: faster and no stack/heap use.
  * @retval Number of characters written (max. about 46).
  */
static uint8_t fmt_frame(char *out, const can_msg_t *m)
{
  char *p = out;
  uint32_t sec = m->ts_us / 1000000U;
  uint32_t us  = m->ts_us % 1000000U;

  *p++ = '(';
  p = put_dec(p, sec);
  *p++ = '.';
  for (uint32_t d = 100000U; d != 0U; d /= 10U) {      // 6 digits, zero padded
    *p++ = (char)('0' + (us / d) % 10U);
  }
  *p++ = ')'; *p++ = ' ';
  *p++ = 'c'; *p++ = 'a'; *p++ = 'n'; *p++ = '0'; *p++ = ' ';

  p = m->ext ? put_hex(p, m->id, 8) : put_hex(p, m->id, 3);
  *p++ = '#';

  if (m->rtr) {
    *p++ = 'R';
  } else {
    for (uint8_t i = 0; i < m->dlc; i++) p = put_hex(p, m->data[i], 2);
  }
  *p++ = '\n';
  return (uint8_t)(p - out);
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
  MX_CAN1_Init();
  MX_USART2_UART_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  setvbuf(stdout, NULL, _IONBF, 0);                      // unbuffered printf

  CAN_FilterConfig();
  HAL_TIM_Base_Start(&htim2);                            // 1 MHz free-running timestamp counter
  if (HAL_CAN_Start(&hcan1) != HAL_OK) Error_Handler();

  // Interrupt on pending frames and on hardware FIFO0 overrun
  if (HAL_CAN_ActivateNotification(&hcan1,
        CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN) != HAL_OK) Error_Handler();

  printf("# L476 CAN sniffer ready: can0 500000 bit/s, candump -L format\n");
  uint32_t last_stats = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  // Print one queued frame per iteration (the whole line is sent in one UART call)
	  if (rxq_tail != rxq_head) {
	    char line[64];
	    uint8_t len = fmt_frame(line, &rxq[rxq_tail]);
	    rxq_tail = (uint16_t)((rxq_tail + 1U) & RXQ_MASK);
	    HAL_UART_Transmit(&huart2, (uint8_t *)line, len, HAL_MAX_DELAY);
	  }

	  // Periodic statistics as a comment line ('#') that log tools can ignore
	  if (HAL_GetTick() - last_stats >= STATS_PERIOD) {
	    last_stats = HAL_GetTick();
	    uint32_t esr = hcan1.Instance->ESR;                  // error counters and last error code
	    printf("# stats: rx=%lu dropped=%lu fifo_ovr=%lu TEC=%lu REC=%lu LEC=%lu\n",
	           (unsigned long)rx_total, (unsigned long)rxq_dropped,
	           (unsigned long)fifo_overruns, (unsigned long)((esr >> 16) & 0xFFU),
	           (unsigned long)(esr >> 24), (unsigned long)((esr >> 4) & 0x7U));
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
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 40;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_CAN1_Init(void)
{

  /* USER CODE BEGIN CAN1_Init 0 */

  /* USER CODE END CAN1_Init 0 */

  /* USER CODE BEGIN CAN1_Init 1 */

  /* USER CODE END CAN1_Init 1 */
  hcan1.Instance = CAN1;
  hcan1.Init.Prescaler = 10;
  hcan1.Init.Mode = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = CAN_BS1_13TQ;
  hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan1.Init.TimeTriggeredMode = DISABLE;
  hcan1.Init.AutoBusOff = DISABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = DISABLE;
  hcan1.Init.ReceiveFifoLocked = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN1_Init 2 */

  /* USER CODE END CAN1_Init 2 */

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

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 79;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

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
  huart2.Init.BaudRate = 921600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

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
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/**
  * @brief  Rx FIFO0 message pending callback (CAN RX0 interrupt).
  *         Timestamps each frame and copies it to the software queue.
  *         Nothing is printed here, UART output is blocking.
  */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef hdr;
  uint8_t d[8];

  while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0U) {
    uint32_t ts = __HAL_TIM_GET_COUNTER(&htim2);       // time of reading the frame
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &hdr, d) != HAL_OK) break;
    rx_total++;

    uint16_t next = (uint16_t)((rxq_head + 1U) & RXQ_MASK);
    if (next == rxq_tail) { rxq_dropped++; continue; } // queue full: drop and count

    can_msg_t *m = &rxq[rxq_head];
    m->ts_us = ts;
    m->ext   = (hdr.IDE == CAN_ID_EXT) ? 1U : 0U;
    m->id    = m->ext ? hdr.ExtId : hdr.StdId;
    m->rtr   = (hdr.RTR == CAN_RTR_REMOTE) ? 1U : 0U;
    m->dlc   = (uint8_t)hdr.DLC;
    for (uint8_t i = 0; i < 8; i++) m->data[i] = d[i];
    rxq_head = next;                                   // publish to the main loop
  }
}

/** CAN error callback: count hardware Rx FIFO0 overruns. */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  if ((hcan->ErrorCode & HAL_CAN_ERROR_RX_FOV0) != 0U) fifo_overruns++;
  HAL_CAN_ResetError(hcan);
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
