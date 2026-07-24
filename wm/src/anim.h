#ifndef NEKOS_ANIM_H
#define NEKOS_ANIM_H

#include <stdint.h>

typedef enum {
    ANIM_NONE,
    ANIM_OPENING,
    ANIM_CLOSING,
    /* Same fade+shrink curve as closing, but the window survives: when it
     * finishes the compositor hides the frame instead of tearing it down
     * (restore replays the open animation via compositor_show). */
    ANIM_MINIMIZING,
} anim_kind_t;

typedef struct {
    anim_kind_t kind;
    int64_t start_ms;
    int64_t duration_ms;
} anim_state_t;

/* Current monotonic time in milliseconds. */
int64_t anim_now_ms(void);

void anim_start_open(anim_state_t *st, int64_t duration_ms);
void anim_start_close(anim_state_t *st, int64_t duration_ms);
void anim_start_minimize(anim_state_t *st, int64_t duration_ms);

/* Linear progress in [0,1] since `st` started animating; 1.0 once its
 * duration has fully elapsed. Doesn't mutate `st` -- callers decide what
 * "finished" means (settle to steady state for opens, tear down resources
 * for closes), since that's WM policy, not animation math. */
double anim_progress(const anim_state_t *st, int64_t now_ms);

/* True if `st` is actively animating (still needs future ticks). */
int anim_active(const anim_state_t *st);

/* Easing curves, applied to a progress value in [0,1]. */
double ease_linear(double t);
double ease_out_cubic(double t);
double ease_in_cubic(double t);
double ease_out_back(double t); /* overshoots past 1.0 before settling -- the "bouncy" open */

#endif
