/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 阶段 2~6 —— 裸机程序：触摸按键 + 计算器（括号/函数/复数/Ans/秒表/开机动画）
  *
  *   本文件是主程序，替代原来的 FreeRTOS 示例。
  *   结构很简单：
  *     初始化 -> 显示开机语 -> 无限循环{ 扫描按键 -> 计算器处理 -> 显示 }
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"          /* HAL 库 + Error_Handler 声明 */
#include "usb_device.h"    /* MX_USB_DEVICE_Init()：启动 USB 虚拟串口 */
#include "usbd_cdc_if.h"   /* CDC_Transmit_FS()：往串口发数据（调试用） */
#include "lcd1602.h"       /* lcd1602_init() / lcd1602_write_lines()：屏幕 */
#include "ttp229.h"        /* ttp229_read_physical()：自己实现的触摸按键读取（阶段 3） */
#include "calculator.h"    /* calc_reset / calc_input_char / calc_evaluate：计算器（阶段 4） */

/* 开机语改由 boot_animation() 实现（含自定义字符和滚动动画），见下方 */

/* TIM3 句柄：屏幕库 lcd1602.a 依赖它（1MHz 计数） */
TIM_HandleTypeDef htim3;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM3_Init(void);
static void pad16(char dst[16], const char *src);
static void boot_progress_blink(int n);

/* 开机自检用：快速闪 n 次 PC13 的 LED，用来判断程序跑到哪一步
 * （调试用，功能确认后可整段删掉，不影响其它逻辑） */
static void boot_progress_blink(int n)
{
  int i;
  for (i = 0; i < n; ++i)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    HAL_Delay(40);
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    HAL_Delay(40);
  }
  HAL_Delay(150);
}

/* ============ 触摸按键（阶段 3）：读取 + 去抖 + 显示 ============ */

/* 连续多少次（每次约 10ms）读到同一个键，才算"稳定"，用来滤掉抖动 */
#define DEBOUNCE_SCANS 3

/* 当前稳定按下的按键号（0..29），-1 表示没按 */
static int stable_key = -1;

/**
  * @brief  在按键位图里找出被按下的最小键号；没有则返回 -1。
  *         （阶段 3 只处理单个按键，多个同时按时取最小的那个）
  */
static int first_key(uint32_t bitmap)
{
  int i;
  for (i = 0; i < 30; ++i)
  {
    if (bitmap & (1u << i))
    {
      return i;
    }
  }
  return -1;
}

/**
  * @brief  每 10ms 调用一次：读一次触摸按键，做简单去抖，更新 stable_key。
  *         连续 DEBOUNCE_SCANS 次读到同一个键，才认定它稳定。
  */
static void scan_key(void)
{
  static int candidate = -1;   /* 最近在观察的候选键 */
  static uint8_t count = 0;    /* 连续读到 candidate 的次数 */

  int key = first_key(ttp229_read_physical());

  if (key == candidate)
  {
    if (count < 255)
    {
      count++;
    }
  }
  else
  {
    candidate = key;
    count = 1;
  }

  if (count >= DEBOUNCE_SCANS)
  {
    stable_key = candidate;   
  }
}

/* ============ 基本计算器（阶段 4）：键位映射 + 处理 + 显示 ============ */

/* 特殊键的键号（其余是普通字符键） */
#define KEY_AC   14   /* AC：清空 */
#define KEY_DEL  13   /* DEL：退格 */
#define KEY_EQ   29   /* =：求值 */

/* 方向键（光标移动，阶段 6 自选） */
#define KEY_UP     3
#define KEY_DOWN   8
#define KEY_LEFT   7
#define KEY_RIGHT  9

/* 键号(0..29) -> 字符；'\0' 表示特殊键或阶段 4 暂不处理的键。
 * 0=SHIFT、1/2=MODE、3/4/7/8/9=方向键、27=E科学计数、28=Ans。 */
static const char KEY_CHAR[30] = {
    '\0', '\0', '\0', '\0', '\0',     /* 0..4 */
    '(',  ')',  '\0', '\0', '\0',     /* 5..9 */
    '7',  '8',  '9',  '\0', '\0',     /* 10..14（13=DEL、14=AC） */
    '4',  '5',  '6',  '*',  '/',      /* 15..19 */
    '1',  '2',  '3',  '+',  '-',      /* 20..24 */
    '0',  '.',  '\0', '\0', '\0'      /* 25..29（27=E、28=Ans、29==） */
};

/* 最近一次计算结果（或错误提示），显示在屏幕第 2 行 */
static char result_buf[16] = "";

/* SHIFT 键号（阶段 5）：按下后，下一次按键按"特殊函数/常量"解释 */
#define KEY_SHIFT 0

/* SHIFT 是否处于激活状态 */
static int shift_active = 0;

/* SHIFT + 键 -> 要插入的字符串（阶段 5：特殊函数 / 常量）
 * 对应板上每个键的蓝色/上方标注。 */
static const struct { int key; const char *text; } SHIFT_MAP[] = {
  { 10, "pi"   },   /* SHIFT + 7 -> π */
  { 12, "i"    },   /* SHIFT + 9 -> 虚数单位 i */
  { 15, "e"    },   /* SHIFT + 4 -> e */
  { 16, "log(" },   /* SHIFT + 5 -> log */
  { 17, "ln("  },   /* SHIFT + 6 -> ln */
  { 18, "^"    },   /* SHIFT + × -> 幂 */
  { 20, "sin(" },   /* SHIFT + 1 -> sin */
  { 21, "cos(" },   /* SHIFT + 2 -> cos */
  { 22, "tan(" },   /* SHIFT + 3 -> tan */
  { 28, "Ans" },    /* SHIFT + Ans键 -> 上次答案 */
};
#define SHIFT_MAP_N (sizeof(SHIFT_MAP) / sizeof(SHIFT_MAP[0]))

/* ============ 模式：MODE 键在 计算器 / 秒表 / 串口显示 间循环切换 ============ */
#define KEY_MODE 1   /* 用键 1 当 MODE 键 */

typedef enum { MODE_CALC = 0, MODE_STOPWATCH = 1, MODE_SERIAL = 2 } mode_t;
static mode_t mode = MODE_CALC;

static void sw_render(void);        /* 秒表刷新显示 */
static void sw_handle_key(int key); /* 秒表按键处理 */
static void serial_render(void);    /* 串口显示：进入时的提示画面（阶段 2） */

/**
  * @brief  把一行文字通过 USB 串口发到电脑（方便没有屏幕时调试）。
  */
static void send_line(const char *s)
{
  char buf[34];
  int i = 0;
  while (s[i] != '\0' && i < 32)
  {
    buf[i] = s[i];
    i++;
  }
  buf[i++] = '\r';
  buf[i++] = '\n';
  CDC_Transmit_FS((uint8_t *)buf, (uint16_t)i);
}

/**
  * @brief  把表达式和结果刷新到屏幕，并在光标处显示闪烁下划线。
  *         第 1 行 = 表达式（带光标），第 2 行 = 结果或 SHIFT 提示。
  *         表达式超过 16 个字符时，自动滚动窗口让光标始终可见。
  */
static void render_calc(void)
{
  char line1[16], line2[16];
  const char *expr = calc_expr();
  int len = 0, cursor, i, window_start, cursor_col;

  while (expr[len] != '\0')
  {
    len++;
  }
  cursor = calc_cursor_pos();

  /* 计算显示窗口：光标在右边缘时窗口左移，保证光标可见 */
  window_start = (cursor > 15) ? (cursor - 15) : 0;
  cursor_col = cursor - window_start;   /* 0..15 */

  for (i = 0; i < 16; i++)
  {
    int idx = window_start + i;
    line1[i] = (idx < len) ? expr[idx] : ' ';
  }

  /* SHIFT 激活时，第 2 行提示 "SHIFT"，否则显示结果 */
  if (shift_active && result_buf[0] == '\0')
  {
    pad16(line2, "SHIFT");
  }
  else
  {
    pad16(line2, result_buf);
  }

  /* 用 write_frame 显示，打开光标（下划线 + 自动闪烁） */
  lcd1602_write_frame(line1, line2, 1, 0, (uint8_t)cursor_col);
}

/**
  * @brief  处理一次按键：把键号翻译成计算器动作，并刷新屏幕、打印串口。
  */
static void handle_key(int key)
{
  /* MODE 键：在 计算器 / 秒表 之间切换 */
  if (key == KEY_MODE)
  {
    /* 三个模式循环：计算器 -> 秒表 -> 串口显示 -> 计算器 …
     * switch 语句：根据 mode 现在的值，跳到对应的分支执行 */
    switch (mode)
    {
      case MODE_CALC:      mode = MODE_STOPWATCH; break;  /* 计算器 -> 秒表 */
      case MODE_STOPWATCH: mode = MODE_SERIAL;    break;  /* 秒表   -> 串口 */
      case MODE_SERIAL:    mode = MODE_CALC;      break;  /* 串口   -> 计算器 */
    }
    shift_active = 0;   /* 切换模式时取消 SHIFT 状态 */
    if (mode == MODE_CALC)
    {
      render_calc();
    }
    else if (mode == MODE_STOPWATCH)
    {
      sw_render();
    }
    else
    {
      serial_render();
    }
    return;
  }

  /* 秒表模式下交给秒表处理 */
  if (mode == MODE_STOPWATCH)
  {
    sw_handle_key(key);
    return;
  }

  /* 串口显示模式下：只认 MODE 键，其它键一律忽略 */
  if (mode == MODE_SERIAL)
  {
    return;
  }

  /* 下面是计算器模式 */

  /* 方向键：移动光标（不受 SHIFT 影响） */
  if (key == KEY_LEFT)  { shift_active = 0; calc_move_left();  render_calc(); return; }
  if (key == KEY_RIGHT) { shift_active = 0; calc_move_right(); render_calc(); return; }
  if (key == KEY_UP)    { shift_active = 0; calc_move_start(); render_calc(); return; }
  if (key == KEY_DOWN)  { shift_active = 0; calc_move_end();   render_calc(); return; }

  /* SHIFT 键：切换（翻转）shift 状态 */
  if (key == KEY_SHIFT)
  {
    shift_active = !shift_active;
    render_calc();
    return;
  }

  /* 处于 SHIFT 状态时，先查特殊函数映射（一次性，用完就取消） */
  if (shift_active)
  {
    int i;
    shift_active = 0;
    for (i = 0; i < (int)SHIFT_MAP_N; i++)
    {
      if (SHIFT_MAP[i].key == key)
      {
        calc_input_str(SHIFT_MAP[i].text);
        result_buf[0] = '\0';
        render_calc();
        send_line(calc_expr());
        return;
      }
    }
    /* 这个键没有 SHIFT 功能，就掉到下面当普通键处理 */
  }

  if (key == KEY_AC)
  {
    calc_reset();
    result_buf[0] = '\0';
  }
  else if (key == KEY_DEL)
  {
    calc_backspace();
    result_buf[0] = '\0';
  }
  else if (key == KEY_EQ)
  {
    calc_evaluate(result_buf);
  }
  else
  {
    char c = KEY_CHAR[key];
    if (c == '\0')
    {
      return;   /* MODE / 方向键 / E / Ans 等，暂不处理 */
    }
    calc_input_char(c);
    result_buf[0] = '\0';
  }

  render_calc();

  /* 同步打印到串口，方便没有屏幕时也能看到 */
  send_line(calc_expr());
  if (result_buf[0] != '\0')
  {
    send_line(result_buf);
  }
}
/* ================================================================= */

/* ============ LCD 自定义字符（开机 logo，阶段 6 自选）============
 * 1602 有 8 个自定义字符槽（编号 0~7），每个是 5×8 点阵。
 * 屏幕库 lcd1602.a 已经用了 1 号槽（角度符号），我们用 2 号槽画笑脸。
 * 库没提供"写自定义字符"的接口，所以这里按协议直接操作 GPIO 实现。 */

#define LCD_RS  GPIO_PIN_10   /* RS：数据/命令选择 */
#define LCD_E   GPIO_PIN_11   /* E：使能 */
#define LCD_D4  GPIO_PIN_12   /* 数据线 D4~D7 */
#define LCD_D5  GPIO_PIN_13
#define LCD_D6  GPIO_PIN_14
#define LCD_D7  GPIO_PIN_15

/* 微秒延时：用 TIM3 的 1MHz 计数原地等待 */
static void lcd_delay_us(uint32_t us)
{
  uint16_t start = (uint16_t)htim3.Instance->CNT;
  while ((uint16_t)((uint16_t)htim3.Instance->CNT - start) < us)
  {
  }
}

/* 写半个字节（4 位模式）：先放数据线，再给 E 一个脉冲 */
static void lcd_write_nibble(uint8_t nibble)
{
  HAL_GPIO_WritePin(GPIOB, LCD_D4, (nibble & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, LCD_D5, (nibble & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, LCD_D6, (nibble & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, LCD_D7, (nibble & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  lcd_delay_us(1);
  HAL_GPIO_WritePin(GPIOB, LCD_E, GPIO_PIN_SET);
  lcd_delay_us(1);
  HAL_GPIO_WritePin(GPIOB, LCD_E, GPIO_PIN_RESET);
  lcd_delay_us(1);
}

/* 写一个完整字节：rs=0 命令 / rs=1 数据，先高 4 位后低 4 位 */
static void lcd_write_byte(uint8_t byte, uint8_t rs)
{
  HAL_GPIO_WritePin(GPIOB, LCD_RS, rs ? GPIO_PIN_SET : GPIO_PIN_RESET);
  lcd_write_nibble(byte >> 4);
  lcd_write_nibble(byte & 0x0F);
  lcd_delay_us(60);
}

/* 把 8 字节点阵写进 CGRAM 的第 slot 号字符槽（0~7） */
static void lcd_define_char(uint8_t slot, const uint8_t glyph[8])
{
  int i;
  lcd_write_byte((uint8_t)(0x40 | (slot << 3)), 0);   /* 设置 CGRAM 地址 */
  for (i = 0; i < 8; i++)
  {
    lcd_write_byte(glyph[i], 1);                       /* 写点阵数据 */
  }
  lcd_write_byte(0x80, 0);                             /* 回到显示首地址 */
}

/* 5×7 笑脸点阵（每字节低 5 位是一行的 5 个点，1=点亮，用十六进制写） */
static const uint8_t SMILEY[8] = {
  0x0E,   /* .####. */
  0x11,   /* #...# */
  0x15,   /* #.#.#  ← 两只眼睛 */
  0x11,   /* #...# */
  0x11,   /* #...# */
  0x0A,   /* .#.#.  ← 微笑的嘴 */
  0x04,   /* ..#.. */
  0x00    /* ..... */
};
/* ================================================================= */

/* ============ 开机动画（阶段 6 自选）============ */

/* 把一条长消息从右往左滚动显示（前后各补 16 空格，让它滑入又滑出） */
static void boot_scroll(const char *msg)
{
  int len = 0, i, total, off;
  char buf[96];   /* 16 空格 + 消息 + 16 空格，消息最长约 60 字符够用 */
  char line1[16], line2[16];

  while (msg[len] != '\0' && len < 60) len++;

  for (i = 0; i < 16; i++) buf[i] = ' ';
  for (i = 0; i < len; i++) buf[16 + i] = msg[i];
  for (i = 0; i < 16; i++) buf[16 + len + i] = ' ';
  total = 16 + len + 16;
  buf[total] = '\0';

  pad16(line2, "");

  for (off = 0; off + 16 <= total; off++)
  {
    for (i = 0; i < 16; i++) line1[i] = buf[off + i];
    lcd1602_write_lines(line1, line2);
    HAL_Delay(200);   /* 每帧 200ms，滚动速度减半 */
  }
}

/* 开机动画：先显示"笑脸 + 名字"静帧，再滚动欢迎语，然后进入计算器界面 */
static void boot_animation(void)
{
  char line1[16], line2[16];

  /* 第 1 帧：笑脸 logo + 名字，静止显示约 1.5 秒
   * 字节 0x02 = CGRAM 第 2 号槽（我们在 lcd_define_char 里写的笑脸）。
   * 1602 规定：往屏幕写 0x00~0x07 这 8 个字节，就会显示对应槽的自定义字符。 */
  pad16(line1, "\x02 Wzy");
  pad16(line2, "HUST SOEI");
  lcd1602_write_lines(line1, line2);
  HAL_Delay(1500);

  /* 第 2 帧：滚动播放欢迎语 */
  boot_scroll("HUST SOEI Wzy");
}
/* ================================================================= */

/* ============ 秒表（阶段 6 自选：MODE 键切换）============ */

static uint8_t  sw_running = 0;      /* 1 = 正在计时 */
static uint32_t sw_start_ms = 0;     /* 本次开始计时的时刻（毫秒） */
static uint32_t sw_accum_ms = 0;     /* 之前已累计的毫秒数 */

static uint32_t sw_elapsed_ms(void)
{
  uint32_t t = sw_accum_ms;
  if (sw_running)
  {
    t += HAL_GetTick() - sw_start_ms;
  }
  return t;
}

/* 把两位数字写进字符串 */
static void put2(char *p, uint32_t v)
{
  p[0] = (char)('0' + (v / 10) % 10);
  p[1] = (char)('0' + v % 10);
}

static void sw_render(void)
{
  uint32_t ms = sw_elapsed_ms();
  uint32_t mm = ms / 60000u;
  uint32_t ss = (ms / 1000u) % 60u;
  uint32_t cc = (ms / 10u) % 100u;
  char line1[16], line2[16];
  int i;

  pad16(line1, sw_running ? "Stopwatch RUN" : "Stopwatch");

  line2[0] = (char)('0' + (mm / 10) % 10);
  line2[1] = (char)('0' + mm % 10);
  line2[2] = ':';
  put2(&line2[3], ss);
  line2[5] = '.';
  put2(&line2[6], cc);
  for (i = 8; i < 16; i++) line2[i] = ' ';

  lcd1602_write_lines(line1, line2);
}

static void sw_handle_key(int key)
{
  if (key == KEY_EQ)          /* '='：开始 / 暂停 */
  {
    if (sw_running)
    {
      sw_accum_ms += HAL_GetTick() - sw_start_ms;
      sw_running = 0;
    }
    else
    {
      sw_start_ms = HAL_GetTick();
      sw_running = 1;
    }
  }
  else if (key == KEY_AC)     /* AC：清零 */
  {
    sw_running = 0;
    sw_accum_ms = 0;
  }
  sw_render();
}

/* ============ 串口显示（阶段 2：电脑发字 -> 显示到屏幕）============ */

/* 刚进入串口模式时显示的提示画面，告诉用户"在等电脑发数据" */
static void serial_render(void)
{
  char line1[16], line2[16];
  pad16(line1, "Serial RX");
  pad16(line2, "waiting...");
  lcd1602_write_lines(line1, line2);
}
/* ================================================================= */

/**
  * @brief  程序入口。单片机一上电就从这里开始跑。
  */
int main(void)
{
  /* 1. 初始化 HAL 库（含 SysTick/时间基准，之后才能用 HAL_Delay 延时） */
  HAL_Init();

  /* 2. 配置系统主时钟 72MHz + USB 专用时钟 */
  SystemClock_Config();

  /* 关键：打开全局中断！
   * 屏幕库 lcd1602 初始化时要用 HAL_Delay 做毫秒延时，
   * 而 HAL_Delay 是靠 TIM4 的定时器中断来"数毫秒"的。
   * 如果不开中断，程序会卡死在延时里，屏幕就永远不出字。
   */
  __enable_irq();

  /* 3. 初始化引脚（屏幕、触摸按键）和 TIM3 定时器 */
  MX_GPIO_Init();
  MX_TIM3_Init();

  /* 开机自检：闪 1 次表示引脚初始化完成 */
  boot_progress_blink(1);

  /* 4. 启动 TIM3：屏幕库 lcd1602.a 需要它来计时 */
  if (HAL_TIM_Base_Start(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  /* 开机自检：闪 2 次表示 TIM3 已启动 */
  boot_progress_blink(2);

  /* 5. 初始化屏幕，写自定义字符 */
  lcd1602_init();
  lcd_define_char(2, SMILEY);   /* 把笑脸写入 2 号自定义字符槽 */

  /* 开机自检：闪 3 次表示屏幕初始化完成 */
  boot_progress_blink(3);

  /* 6. 屏幕就绪后立刻启动 USB 虚拟串口。
   *    必须赶在主机枚举超时之前完成，所以放在开机动画（约 2 秒）之前；
   *    同时又要放在外设初始化之后，避免影响屏幕初始化。 */
  MX_USB_DEVICE_Init();

  /* 7. 播放开机动画，进入计算器界面 */
  boot_animation();
  render_calc();                /* 进入计算器界面，显示闪烁光标 */

  /* 调试用计数器：让 PC13 的 LED 每 0.5 秒闪一下，确认程序在正常运行 */
  uint32_t tick = 0;

  /* 记录上一次稳定按下的键，用来只在"按下"那一刻触发一次动作 */
  int shown_key = -1;

  /* 7. 主循环：永不结束，反复扫描触摸按键 */
  for (;;)
  {
    /* 阶段 4：扫描按键；稳定键变化时，按下就喂给计算器 */
    scan_key();
    if (stable_key != shown_key)
    {
      shown_key = stable_key;
      if (shown_key >= 0)          /* 按下（>=0）才处理，松开（-1）不处理 */
      {
        handle_key(shown_key);
      }
    }

    /* 秒表正在计时时，每 10ms 刷新一次时间显示 */
    if (mode == MODE_STOPWATCH && sw_running)
    {
      sw_render();
    }

    /* 串口显示模式（阶段 2 的核心）：电脑发来一整行，就把它显示到屏幕。
     * CDC_TakeLine_FS 返回 1 表示"有新的一行"，0 表示"没有"，各自判断即可。 */
    if (mode == MODE_SERIAL)
    {
      char line1[16], line2[16];
      if (CDC_TakeLine_FS(line1, line2))
      {
        lcd1602_write_lines(line1, line2);
      }
    }

    HAL_Delay(10);  /* 每 10 毫秒检查一次 */

    /* 每 50 次循环（= 0.5 秒）翻转一次 LED */
    if (++tick >= 50)
    {
      HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
      tick = 0;
    }
  }
}

/**
  * @brief  把短字符串补空格到正好 16 字符（1602 每行固定 16 字符）
  * @param  dst  输出缓冲区（至少 16 字节）
  * @param  src  普通字符串（以 '\0' 结尾）
  */
static void pad16(char dst[16], const char *src)
{
  int i = 0;
  while (*src != '\0' && i < 16)   /* 先复制字符，直到字符串结束或满 16 */
  {
    dst[i++] = *src++;
  }
  while (i < 16)                   /* 剩下的用空格补满 */
  {
    dst[i++] = ' ';
  }
}

/**
  * @brief 系统时钟配置：外部晶振 -> 倍频到 72MHz，并给 USB 提供 48MHz
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

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
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_PLL_DIV1_5;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM3 初始化：配成 1MHz 计数（屏幕/触摸库都依赖这个频率）
  */
static void MX_TIM3_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 71;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 65535;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO 初始化：屏幕数据线、触摸按键线（这是板子硬件的引脚分配）
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13
                          |GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6|GPIO_PIN_8, GPIO_PIN_SET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PB10 PB11 PB12 PB13 PB14 PB15 PB6 PB8 */
  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13
                          |GPIO_PIN_14|GPIO_PIN_15|GPIO_PIN_6|GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB7 PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_7|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/**
  * @brief 定时器周期回调：TIM4 每次溢出都调用，喂给 HAL 的毫秒计数器
  *        （有了它，HAL_Delay() 才能正常工作）
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM4) {
    HAL_IncTick();
  }
}

/**
  * @brief 出错处理：死循环，方便调试时定位
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
