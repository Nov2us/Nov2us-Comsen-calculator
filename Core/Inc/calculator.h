#ifndef CALCULATOR_H
#define CALCULATOR_H

/* 阶段 4：基本计算器（不带括号的四则运算 + AC）。
 * 这里只负责"表达式怎么组织"，真正的四则运算交给库里的 calculator_engine。 */

/* 清空当前表达式（对应 AC 键） */
void calc_reset(void);

/* 删掉光标前一个字符（对应 DEL 键） */
void calc_backspace(void);

/* 光标：插入点位置（0 = 最前面，表达式长度 = 最后面） */
int  calc_cursor_pos(void);
void calc_move_left(void);   /* 光标左移一格 */
void calc_move_right(void);  /* 光标右移一格 */
void calc_move_start(void);  /* 跳到行首 */
void calc_move_end(void);    /* 跳到行尾 */

/* 在光标处替换一个字符（覆盖模式，末尾则追加），会自动做简单合法性检查 */
void calc_input_char(char c);

/* 在光标处替换一整段字符串（覆盖模式；用于特殊函数，如 "sin("、"pi"） */
void calc_input_str(const char *s);

/* 求值当前表达式，把结果显示字符串写进 out（最多 15 个字符）。
 * 返回 1 表示成功，0 表示出错（out 里是错误提示，如 "Div 0"）。 */
int  calc_evaluate(char out[16]);

/* 返回当前表达式字符串（只读，以 '\0' 结尾） */
const char *calc_expr(void);

#endif /* CALCULATOR_H */
