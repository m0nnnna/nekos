#define _POSIX_C_SOURCE 200809L
#include <time.h>

#include "anim.h"

int64_t anim_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void anim_start_open(anim_state_t *st, int64_t duration_ms) {
    st->kind = ANIM_OPENING;
    st->start_ms = anim_now_ms();
    st->duration_ms = duration_ms;
}

void anim_start_close(anim_state_t *st, int64_t duration_ms) {
    st->kind = ANIM_CLOSING;
    st->start_ms = anim_now_ms();
    st->duration_ms = duration_ms;
}

void anim_start_minimize(anim_state_t *st, int64_t duration_ms) {
    st->kind = ANIM_MINIMIZING;
    st->start_ms = anim_now_ms();
    st->duration_ms = duration_ms;
}

double anim_progress(const anim_state_t *st, int64_t now_ms) {
    if (st->kind == ANIM_NONE) return 1.0;

    int64_t elapsed = now_ms - st->start_ms;
    if (elapsed <= 0) return 0.0;
    if (elapsed >= st->duration_ms) return 1.0;
    return (double)elapsed / (double)st->duration_ms;
}

int anim_active(const anim_state_t *st) {
    return st->kind != ANIM_NONE;
}

double ease_linear(double t) {
    return t;
}

double ease_out_cubic(double t) {
    double p = t - 1.0;
    return p * p * p + 1.0;
}

double ease_in_cubic(double t) {
    return t * t * t;
}

double ease_out_back(double t) {
    const double c1 = 1.70158;
    const double c3 = c1 + 1.0;
    double p = t - 1.0;
    return 1.0 + c3 * p * p * p + c1 * p * p;
}
