/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : calculator.c
  * @brief          : 阶段 4 —— 基本计算器的表达式组织与求值
  *
  *   思路：把用户按键一个字符一个字符地拼成一条表达式字符串（如 "12+3*4"），
  *   按 "=" 时把整条表达式交给库里的 calculator_engine 去算，再显示结果。
  *
  *   真正的四则运算（含乘除优先于加减）由库完成，我们负责：
  *     - 组织输入（哪些字符能接在哪些后面）
  *     - 显示（把 float 结果转成字符串）
  ******************************************************************************
  */
/* USER CODE END Header */

#include "calculator.h"
#include "calculator_engine.h"
#include <string.h>

/* 表达式最长 48 个字符，足够用了 */
#define EXPR_MAX 48

/* 当前表达式（'\0' 结尾） */
static char expr[EXPR_MAX + 1];
static int  expr_len;

/* 光标插入点：0 = 表达式最前面，expr_len = 最后面 */
static int  cursor_pos = 0;

/* 上次的计算结果（Ans），下次求值时 "Ans" 就引用它 */
static calc_complex_t last_answer = {0.0f, 0.0f};

/* ---------- 表达式组织 ---------- */

void calc_reset(void)
{
  expr_len = 0;
  cursor_pos = 0;
  expr[0] = '\0';
}

const char *calc_expr(void)
{
  return expr;
}

int calc_cursor_pos(void)
{
  return cursor_pos;
}

/* 覆盖模式：光标处有字符就替换，光标在末尾就追加；光标后移 */
static void overwrite_char(char c)
{
  if (cursor_pos < expr_len)
  {
    expr[cursor_pos] = c;   /* 替换光标下的字符 */
    cursor_pos++;
  }
  else if (expr_len < EXPR_MAX)
  {
    expr[expr_len++] = c;   /* 末尾则追加 */
    cursor_pos = expr_len;
    expr[expr_len] = '\0';
  }
}

void calc_backspace(void)
{
  int i;
  if (cursor_pos > 0)
  {
    for (i = cursor_pos - 1; i < expr_len - 1; i++)
    {
      expr[i] = expr[i + 1];
    }
    expr_len--;
    cursor_pos--;
    expr[expr_len] = '\0';
  }
}

/* 光标移动 */
void calc_move_left(void)  { if (cursor_pos > 0) cursor_pos--; }
void calc_move_right(void) { if (cursor_pos < expr_len) cursor_pos++; }
void calc_move_start(void) { cursor_pos = 0; }
void calc_move_end(void)   { cursor_pos = expr_len; }

/* 判断一个字符是不是运算符 */
static int is_op(char c)
{
  return c == '+' || c == '-' || c == '*' || c == '/' || c == '^';
}

void calc_input_char(char c)
{
  char left = (cursor_pos > 0) ? expr[cursor_pos - 1] : '\0';

  if (c >= '0' && c <= '9')
  {
    overwrite_char(c);
  }
  else if (c == '.')
  {
    /* 光标左侧这个数字里已有小数点，就不再加（防止 "1.2.3"） */
    int i = cursor_pos - 1;
    while (i >= 0 && expr[i] >= '0' && expr[i] <= '9')
    {
      i--;
    }
    if (i >= 0 && expr[i] == '.')
    {
      return;
    }
    /* 小数点前不是数字就补 0（".5" 变成 "0.5"） */
    if (left == '\0' || is_op(left) || left == '(')
    {
      overwrite_char('0');
    }
    overwrite_char(c);
  }
  else if (is_op(c))
  {
    if (cursor_pos == 0)
    {
      /* 表达式开头只允许负号（表示负数） */
      if (c != '-')
      {
        return;
      }
      overwrite_char(c);
    }
    else if (is_op(left))
    {
      /* 连续运算符：允许 "^-"（负指数），其余替换掉左边那个 */
      if (c == '-' && left == '^')
      {
        overwrite_char(c);
      }
      else
      {
        expr[cursor_pos - 1] = c;
      }
    }
    else
    {
      overwrite_char(c);
    }
  }
  else if (c == '(' || c == ')')
  {
    /* ')' 不能出现在开头，也不能直接跟在运算符或 '(' 后面 */
    if (c == ')' && (left == '\0' || is_op(left) || left == '('))
    {
      return;
    }
    overwrite_char(c);
  }
  /* 其他字符一律忽略 */
}

void calc_input_str(const char *s)
{
  int n = 0, i;
  while (s[n] != '\0')
  {
    n++;
  }
  /* 覆盖模式：光标处已有字符就替换，不够就在末尾追加 */
  for (i = 0; i < n; i++)
  {
    int pos = cursor_pos + i;
    if (pos < expr_len)
    {
      expr[pos] = s[i];
    }
    else if (expr_len < EXPR_MAX)
    {
      expr[expr_len++] = s[i];
    }
    else
    {
      break;
    }
  }
  cursor_pos += n;
  if (cursor_pos > expr_len)
  {
    cursor_pos = expr_len;
  }
  expr[expr_len] = '\0';
}

/* ---------- 结果显示 ---------- */

/**
  * @brief  把 float 转成十进制字符串，最多 6 位小数，自动去掉末尾多余的 0。
  *         不用 printf 是因为这个工程链接的是精简版 C 库（newlib-nano），
  *         默认不带浮点 printf，自己写更省事、更可控。
  */
static void float_to_str(float v, char *out)
{
  char *p = out;
  long ipart;
  long fpart;
  int i;

  if (v < 0.0f)
  {
    *p++ = '-';
    v = -v;
  }

  ipart = (long)v;
  fpart = (long)((v - (float)ipart) * 1000000.0f + 0.5f);
  if (fpart >= 1000000L)          /* 四舍五入后进了位，比如 0.9999999 -> 1.0 */
  {
    fpart = 0;
    ipart++;
  }

  /* 整数部分 */
  if (ipart == 0)
  {
    *p++ = '0';
  }
  else
  {
    char tmp[12];
    int n = 0;
    while (ipart > 0)
    {
      tmp[n++] = (char)('0' + ipart % 10);
      ipart /= 10;
    }
    while (n > 0)
    {
      *p++ = tmp[--n];
    }
  }

  /* 小数部分（最多 6 位，去掉末尾 0） */
  if (fpart > 0)
  {
    char frac[7];
    int j;
    for (i = 0; i < 6; i++)
    {
      frac[6 - 1 - i] = (char)('0' + fpart % 10);
      fpart /= 10;
    }
    frac[6] = '\0';

    i = 6;
    while (i > 0 && frac[i - 1] == '0')
    {
      i--;
    }
    if (i > 0)
    {
      *p++ = '.';
      for (j = 0; j < i; j++)
      {
        *p++ = frac[j];
      }
    }
  }
  *p = '\0';
}

/**
  * @brief  把复数结果格式化成 "a+bi" 形式（系数 1 省略为 "i"，实部 0 省略）。
  */
static void format_complex(calc_complex_t z, char *out)
{
  char a[16], b[16];
  int n = 0;
  int i;

  float_to_str(z.real, a);
  float_to_str(z.imag < 0.0f ? -z.imag : z.imag, b);   /* 虚部取绝对值，符号单独写 */

  /* 实部（非零才写） */
  if (z.real != 0.0f)
  {
    for (i = 0; a[i] != '\0'; i++)
    {
      out[n++] = a[i];
    }
  }

  /* 虚部符号 */
  if (z.imag < 0.0f)
  {
    out[n++] = '-';
  }
  else if (z.real != 0.0f)
  {
    out[n++] = '+';
  }

  /* 虚部系数：是 1 就不写（写 "i" 而不是 "1i"） */
  if (!(b[0] == '1' && b[1] == '\0'))
  {
    for (i = 0; b[i] != '\0'; i++)
    {
      out[n++] = b[i];
    }
  }
  out[n++] = 'i';
  out[n] = '\0';
}

/* ---------- 求值 ---------- */

/**
  * @brief  把"可读"表达式翻译成引擎认识的单字母写法：
  *         sin( -> s(  cos( -> c(  tan( -> t(  ln( -> n(  log( -> l(  pi -> p  Ans -> A
  *         引擎内部用单个字母表示函数（sin='s'、cos='c'、tan='t'、ln='n'、log='l'、Ans='A'）。
  */
static void translate_to_engine(const char *src, char *dst)
{
  while (*src != '\0')
  {
    if      (src[0] == 's' && src[1] == 'i' && src[2] == 'n' && src[3] == '(') { *dst++ = 's'; *dst++ = '('; src += 4; }
    else if (src[0] == 'c' && src[1] == 'o' && src[2] == 's' && src[3] == '(') { *dst++ = 'c'; *dst++ = '('; src += 4; }
    else if (src[0] == 't' && src[1] == 'a' && src[2] == 'n' && src[3] == '(') { *dst++ = 't'; *dst++ = '('; src += 4; }
    else if (src[0] == 'l' && src[1] == 'o' && src[2] == 'g' && src[3] == '(') { *dst++ = 'l'; *dst++ = '('; src += 4; }
    else if (src[0] == 'l' && src[1] == 'n' && src[2] == '(')                   { *dst++ = 'n'; *dst++ = '('; src += 3; }
    else if (src[0] == 'p' && src[1] == 'i')                                    { *dst++ = 'p'; src += 2; }
    else if (src[0] == 'A' && src[1] == 'n' && src[2] == 's')                   { *dst++ = 'A'; src += 3; }
    else                                                                        { *dst++ = *src++; }
  }
  *dst = '\0';
}

int calc_evaluate(char out[16])
{
  calc_complex_t result;
  calc_status_t st;
  char eng[EXPR_MAX + 1];

  if (expr_len == 0)
  {
    out[0] = '\0';
    return 0;
  }

  translate_to_engine(expr, eng);
  /* 第 3 个参数 allow_complex=1：允许复数（虚数单位 i、负数的开方等）
   * 第 4 个参数 last_answer：表达式里的 "Ans" 就是上次的结果 */
  st = calculator_evaluate(eng, CALC_ANGLE_DEG, 1, last_answer, &result);

  if (st == CALC_OK)
  {
    last_answer = result;   /* 记住这次结果，供下次 "Ans" 引用 */
    if (result.imag == 0.0f)
    {
      float_to_str(result.real, out);   /* 纯实数 */
    }
    else
    {
      format_complex(result, out);      /* 复数：a+bi */
    }
    return 1;
  }

  /* 出错：给一句简短提示 */
  if (st == CALC_DIV_ZERO)
  {
    strcpy(out, "Div 0");
  }
  else if (st == CALC_SYNTAX)
  {
    strcpy(out, "Syntax");
  }
  else
  {
    strcpy(out, "Domain");
  }
  return 0;
}
