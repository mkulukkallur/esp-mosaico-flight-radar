#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Set up the speaker and vibration motor (per menuconfig). Safe if either is missing. */
void alert_init(void);

/** Play the "aircraft overhead" chime and buzz. Non-blocking; any task. */
void alert_overhead(void);

#ifdef __cplusplus
}
#endif
