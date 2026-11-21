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
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
#include "OLED.h"
#include <stdio.h>
#include <string.h>

// 1. 定义数据包结构体 (严格按 6 字节解析)
typedef struct __attribute__((packed)) {
  uint8_t header;       // 0xAA
  uint8_t run;          // 00暂停 / 01运行
  uint8_t dir;          // 00正向 / 01反向
  uint16_t interval_ms; // 间隔时间 (低字节 STEP_L 在前，高字节 STEP_H 在后)
  uint8_t checksum;     // 校验和
} RxPacket;

RxPacket rx_packet;             // 接收缓冲对象
volatile uint8_t rx_ready = 0;  // 接收完成标志

// 2. 系统运行状态 (中断与主循环共享，必须加 volatile)
volatile uint8_t system_run = 1;     // 默认 1 运行
volatile uint8_t system_dir = 0;     // 默认 0 正向
volatile uint16_t system_step = 300; // 默认 300ms
volatile uint8_t current_led = 0;    // 当前LED编号 (0,1,2 对应LED1,2,3)
volatile uint8_t update_flag = 1;    // 状态更新标志位 (触发OLED刷新和串口发送)


// 3. 按键消抖时间戳
volatile uint32_t key1_tick = 0;
volatile uint32_t key2_tick = 0;

// 4. 串口发送缓冲区 (必须全局，因为中断发送是异步的)
char tx_buf[64];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  OLED_Init(); // 初始化 OLED 屏幕

  // 开启串口中断接收，期待接收 sizeof(rx_packet) 即 6 个字节
  HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_packet, sizeof(rx_packet));

  // 启动时先给电脑发一次初始状态反馈
  int len = snprintf(tx_buf, sizeof(tx_buf), "System Ready! STATE=RUN,DIR=FWD,STEP=300,LED=1\r\n");
  HAL_UART_Transmit_IT(&huart1, (uint8_t*)tx_buf, len);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t last_led_tick = 0;

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    // ----- 任务1：处理串口接收到的数据包 -----
    if (rx_ready) {
      rx_ready = 0;

      // 计算校验和 (强转uint8_t自动截断保留低8位)
      uint8_t expected = (uint8_t)(rx_packet.header + rx_packet.run + rx_packet.dir
                        + (rx_packet.interval_ms & 0xFF) + (rx_packet.interval_ms >> 8));

      // 校验帧头、校验和、参数范围
      if (rx_packet.header == 0xAA && rx_packet.checksum == expected
          && rx_packet.run <= 1 && rx_packet.dir <= 1
          && rx_packet.interval_ms >= 100 && rx_packet.interval_ms <= 2000) {

        // 更新全局状态
        system_run = rx_packet.run;
        system_dir = rx_packet.dir;
        system_step = rx_packet.interval_ms;
        update_flag = 1; // 触发屏幕刷新和串口反馈
          }

      // 处理完毕，重新开启接收，等待下一包
      HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_packet, sizeof(rx_packet));
    }

    // ----- 任务2：非阻塞流水灯控制 -----
    if (system_run && (HAL_GetTick() - last_led_tick >= system_step)) {
      last_led_tick = HAL_GetTick(); // 记录本次切灯时间

      // 先关掉所有LED
      HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2, GPIO_PIN_SET);

      // 计算下一个LED编号
      if (system_dir == 0) { // 正向
        current_led = (current_led + 1) % 3;
      } else { // 反向
        current_led = (current_led == 0) ? 2 : (current_led - 1);
      }

      // 点亮当前LED
      if (current_led == 0) HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
      if (current_led == 1) HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
      if (current_led == 2) HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);

      update_flag = 1; // 切灯了，OLED的编号需要更新
    }

    // ----- 任务3：OLED显示与串口状态反馈 -----
    if (update_flag) {
      update_flag = 0;

      // 1. 刷新 OLED
      char line[17];
      snprintf(line, sizeof(line), "STATE: %s", system_run ? "RUN " : "STOP");
      OLED_ShowString(1, 1, line);

      snprintf(line, sizeof(line), "DIR: %s", system_dir ? "REV " : "FWD ");
      OLED_ShowString(2, 1, line);

      snprintf(line, sizeof(line), "STEP: %dms   ", system_step);
      OLED_ShowString(3, 1, line);

      snprintf(line, sizeof(line), "LED: %d      ", current_led + 1);
      OLED_ShowString(4, 1, line);

      // 2. 发送串口反馈 (中断发送)
      int len = snprintf(tx_buf, sizeof(tx_buf),
         "STATE=%s,DIR=%s,STEP=%d,LED=%d\r\n",
          system_run ? "RUN" : "STOP",
          system_dir ? "REV" : "FWD",
          system_step, current_led + 1);
      HAL_UART_Transmit_IT(&huart1, (uint8_t*)tx_buf, len);
      //只有收到按键或串口指令时，才发送一次反馈

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

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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

/* USER CODE BEGIN 4 */
// 【按键外部中断回调】
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
  uint32_t now = HAL_GetTick(); // 获取当前系统毫秒数

  if (GPIO_Pin == GPIO_PIN_11) {
    // 按键1：切换方向 (软件消抖：两次触发间隔需大于200ms)
    if (now - key1_tick > 200) {
      key1_tick = now;
      system_dir = !system_dir; // 翻转方向 (0变1，1变0)
      update_flag = 1;          // 通知主循环：状态变了，快刷新屏幕和发串口
    }
  }
  if (GPIO_Pin == GPIO_PIN_12) {
    // 按键2：切换暂停/运行
    if (now - key2_tick > 200) {
      key2_tick = now;
      system_run = !system_run;
      update_flag = 1;
    }
  }
}

// 【串口接收完成中断回调】
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    rx_ready = 1; // 绝不能在中断里做复杂的解析，只置位标志，交给主循环
  }
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
