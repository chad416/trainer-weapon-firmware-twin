/* Built-in self-test. SPEC F14. Bit layout matches BIT_RESULT.fault_mask in the DBC. */
#ifndef TRAINER_BIT_H_
#define TRAINER_BIT_H_

#include <stdbool.h>
#include <stdint.h>

#define BIT_F_OUTPUTS        (1U << 0) /* critical */
#define BIT_F_ADC            (1U << 1) /* critical */
#define BIT_F_IMU            (1U << 2) /* critical */
#define BIT_F_TRIGGER_NOT_REST (1U << 3)
#define BIT_F_CFG_DEFAULTED  (1U << 4)
#define BIT_F_CAN            (1U << 5)
#define BIT_F_FLASH          (1U << 6)
#define BIT_F_WATCHDOG       (1U << 7)

#define BIT_CRITICAL_MASK (BIT_F_OUTPUTS | BIT_F_ADC | BIT_F_IMU)

/* Record init results that BIT cannot re-check later. */
void bit_note_outputs(int init_rc);
void bit_note_adc(int init_rc);

/*
 * Run all checks, log them, send BIT_RESULT, and move the weapon into or out of
 * FAULT. Returns the fault mask.
 */
uint16_t bit_run(void);

/* Mask from the most recent run (for STATUS frames). */
uint16_t bit_last_mask(void);

static inline bool bit_is_critical(uint16_t mask)
{
	return (mask & BIT_CRITICAL_MASK) != 0U;
}

#endif /* TRAINER_BIT_H_ */
