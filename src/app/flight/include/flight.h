#pragma once

#include <stdbool.h>
#include <stdint.h>

void flight_init(void);
void flight_tick(float dt_s);
void flight_end_session(void);
bool flight_session_active(void);
float flight_phase_deg(void);
int32_t flight_rx_age_ms(void);
uint32_t flight_rejected_frames(void);
