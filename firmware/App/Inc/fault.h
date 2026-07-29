#ifndef UNOQBLINKY_FAULT_H
#define UNOQBLINKY_FAULT_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FAULT_REASON_NONE = 0,
    FAULT_REASON_RPC_FAULT,     /* rpc_fault(): a static/impossible protocol condition */
    FAULT_REASON_HARDFAULT,
    FAULT_REASON_MEMMANAGE,
    FAULT_REASON_BUSFAULT,
    FAULT_REASON_USAGEFAULT,
    FAULT_REASON_NMI,
    FAULT_REASON_ERROR_HANDLER, /* generic CubeMX-generated Error_Handler() */
} fault_reason_t;

/* Call once at boot, as early as possible (right after the PWR peripheral
   clock is enabled) -- before anything else that could fault. Enables
   backup-domain register access, then reads and clears whatever the
   previous boot left behind so fault_get_last_reset_reason()/
   fault_was_watchdog_reset() reflect the *previous* run, not this one. */
void fault_init(void);

/* Reason the previous boot deliberately reset itself via
   fault_record_and_reset(), or FAULT_REASON_NONE if the previous reset
   wasn't one of ours (power-on, pin reset, debugger reset, etc). */
fault_reason_t fault_get_last_reset_reason(void);

/* True if the hardware IWDG reset flag was set at boot -- i.e. the
   watchdog actually fired. This is independent of (and a useful
   cross-check against) fault_get_last_reset_reason(): a bare watchdog
   timeout means the device was hung badly enough that no fault handler
   ever ran to record a reason at all. */
bool fault_was_watchdog_reset(void);

/* Writes `reason` to a backup register (survives a warm reset) and resets
   the MCU via NVIC_SystemReset() -- does not return in that case. If a
   debugger is attached (checked via CoreDebug->DHCSR), breaks instead so
   the existing attach-and-inspect workflow during development still
   works; execution can resume from there under debugger control. */
void fault_record_and_reset(fault_reason_t reason);

#endif //UNOQBLINKY_FAULT_H
