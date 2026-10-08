#ifndef EDGETOP_TERM_H
#define EDGETOP_TERM_H

int term_enter(void);
void term_leave(void);
/* Async-signal-safe: only write() and tcsetattr(). */
void term_restore_now(void);
void term_reenter_now(void);
void term_size(int *rows, int *cols);

#endif
