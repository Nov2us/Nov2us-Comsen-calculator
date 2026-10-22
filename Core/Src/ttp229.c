/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : ttp229.c
  * @brief          : 阶段 3 —— 我们自己实现的 TTP229 触摸按键读取
  *
  *   这块板上有两颗 TTP229 电容触摸芯片，每颗 16 个通道，加起来 32 个，
  *   其中 30 个接到了按键上（另外 2 个空着）。
  *
  *   TTP229 不是标准的 I2C，而是一个很简单的"两线时序"：
  *     - 单片机在一条"时钟线"SCL 上输出方波；
  *     - 每翻一次时钟，芯片就在"数据线"SDO 上放出一个按键的状态。
  *   所以我们用普通 GPIO（翻转电平 + 读电平）就能读，不需要 I2C 外设。
  *
  *   本文件对应公开头文件 ttp229.h 里的 ttp229_read_physical()，
  *   是替代 libttp229.a 的、我们自己的实现。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "ttp229.h"     /* 公开接口：uint32_t ttp229_read_physical(void) */
#include "main.h"       /* HAL 库；TIM3 句柄 htim3 用它做微秒延时 */

/* ============ 两线引脚分配（都在 GPIOB，main.c 里已经初始化好） ============ */
#define SCL_A   GPIO_PIN_6    /* 芯片 A 的时钟线：PB6（单片机输出） */
#define SDO_A   GPIO_PIN_7    /* 芯片 A 的数据线：PB7（单片机读入） */
#define SCL_B   GPIO_PIN_8    /* 芯片 B 的时钟线：PB8（单片机输出） */
#define SDO_B   GPIO_PIN_9    /* 芯片 B 的数据线：PB9（单片机读入） */
/* =========================================================================== */

/* htim3 定义在 main.c 里，这里声明一下就能直接用 */
extern TIM_HandleTypeDef htim3;

/**
  * @brief  微秒级延时：原地空转，等到 TIM3 的计数器走过 us 个计数。
  *         TIM3 已经被配成 1MHz（1 个计数 = 1 微秒），所以数 us 个计数即可。
  *         用 16 位相减，即使计数器溢出回绕也能正确算出"经过了多久"。
  */
static void delay_us(uint32_t us)
{
  uint16_t start = (uint16_t)htim3.Instance->CNT;
  while ((uint16_t)((uint16_t)htim3.Instance->CNT - start) < us)
  {
    /* 原地空转，什么也不做，只等计数走够 */
  }
}

/**
  * @brief  读一颗 TTP229 的 16 个通道，返回 16 位结果：bit i = 1 表示通道 i 被按下。
  *
  *   时序（重复 16 次，第 i 次对应通道 i）：
  *     1. 时钟 SCL 拉低；
  *     2. 等约 5µs，让芯片把第 i 个按键的状态放到数据线 SDO 上；
  *     3. 读 SDO：读到"低电平"就表示这个键被按下（结果里把 bit i 置 1）；
  *     4. 时钟 SCL 拉高；
  *     5. 再等约 5µs，然后进入下一轮。
  */
static uint16_t read_chip(uint16_t scl, uint16_t sdo)
{
  uint16_t result = 0;
  int i;

  for (i = 0; i < 16; ++i)
  {
    HAL_GPIO_WritePin(GPIOB, scl, GPIO_PIN_RESET);   /* 时钟拉低 */
    delay_us(5);

    if (HAL_GPIO_ReadPin(GPIOB, sdo) == GPIO_PIN_RESET)
    {
      result |= (uint16_t)(1u << i);                 /* 数据线低 = 按下 */
    }

    HAL_GPIO_WritePin(GPIOB, scl, GPIO_PIN_SET);     /* 时钟拉高 */
    delay_us(5);
  }

  return result;
}

/* ============ 通道号 → 按键编号 的映射表 ============
 * 两颗芯片的通道在 PCB 上不是按 0..15 顺序接的，所以需要这两张表
 * 把"芯片的第 i 个通道"翻译成"板子上的第几个按键（0..29）"。
 * 0xFF 表示这个通道没接按键（32 个通道里只用了 30 个）。
 */
static const uint8_t a_channel_to_key[16] =
  { 7, 11, 6, 1, 0, 5, 10, 2, 0xFF, 14, 9, 4, 3, 8, 13, 12 };

static const uint8_t b_channel_to_key[16] =
  { 27, 15, 20, 25, 16, 21, 26, 17, 22, 28, 23, 18, 29, 24, 19, 0xFF };
/* ================================================= */

/**
  * @brief  读全部 30 个按键，返回一个 32 位位图：bit k = 1 表示按键 k 被按下。
  *
  *   做法：把两颗芯片各读一遍，再把"通道被按下"的位，按映射表搬到
  *   "按键编号"对应的位上。
  */
uint32_t ttp229_read_physical(void)
{
  uint16_t a = read_chip(SCL_A, SDO_A);   /* 芯片 A 的 16 位原始状态 */
  uint16_t b = read_chip(SCL_B, SDO_B);   /* 芯片 B 的 16 位原始状态 */
  uint32_t result = 0;
  int i;

  for (i = 0; i < 16; ++i)
  {
    if (a & (1u << i))                    /* 芯片 A 的通道 i 被按下 */
    {
      uint8_t key = a_channel_to_key[i];
      if (key != 0xFF)
      {
        result |= (1u << key);            /* 把按键 key 的位置成 1 */
      }
    }

    if (b & (1u << i))                    /* 芯片 B 的通道 i 被按下 */
    {
      uint8_t key = b_channel_to_key[i];
      if (key != 0xFF)
      {
        result |= (1u << key);
      }
    }
  }

  return result;
}
