#include "fault.h"
#include "main.h"

/* Backup registers (TAMP->BKPxR) survive a warm reset (software reset,
   IWDG timeout) but not a power loss, which is exactly the boundary we
   want: "why did we just reset ourselves" should survive the reset that
   answers the question, but doesn't need to survive unplugging the
   board. */
#define FAULT_MAGIC 0x4641554cu /* 'FAUL' -- distinguishes "we wrote this"
                                    from power-on-reset garbage/zero */

static fault_reason_t cached_reason = FAULT_REASON_NONE;
static bool cached_watchdog_reset = false;

void fault_init(void) {
    LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_RTCAPB);
    LL_PWR_EnableBkUpAccess();

    if (TAMP->BKP0R == FAULT_MAGIC) {
        cached_reason = (fault_reason_t)TAMP->BKP1R;
    }
    TAMP->BKP0R = 0;
    TAMP->BKP1R = 0;

    cached_watchdog_reset = (LL_RCC_IsActiveFlag_IWDGRST() != 0);
    LL_RCC_ClearResetFlags();
}

fault_reason_t fault_get_last_reset_reason(void) {
    return cached_reason;
}

bool fault_was_watchdog_reset(void) {
    return cached_watchdog_reset;
}

void fault_record_and_reset(fault_reason_t reason) {
    __disable_irq();

    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0) {
        __BKPT(0);
        __enable_irq();
        return;
    }

    TAMP->BKP0R = FAULT_MAGIC;
    TAMP->BKP1R = (uint32_t)reason;
    NVIC_SystemReset();
    for (;;) { } /* NVIC_SystemReset() does not return; unreachable */
}
