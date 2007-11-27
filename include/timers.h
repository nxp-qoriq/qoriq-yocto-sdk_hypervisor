#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>
#include <percpu.h>

void guest_timer_init(gcpu_t *gcpu);
void run_deferred_decrementer(void);
void set_tcr(uint32_t val);
void set_tsr(uint32_t val);
uint32_t get_tcr(void);
uint32_t get_tsr(void);

#endif
