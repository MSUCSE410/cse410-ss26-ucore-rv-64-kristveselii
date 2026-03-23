#include "timer.h"
#include "riscv.h"
#include "sbi.h"

/// read the `mtime` regiser
uint64 get_cycle()
{
	return r_time();
}

/// Enable timer interrupt
void timer_init()
{
	// Enable supervisor timer interrupt
	w_sie(r_sie() | SIE_STIE);
	set_next_timer();
}

/// Set the next timer interrupt
void set_next_timer()
{
	const uint64 timebase = CPU_FREQ / TICKS_PER_SEC;
	set_timer(get_cycle() + timebase);
}

// ============ PROJECT 2: NEW FUNCTION ============
/**
 * get_msec() - Get current time in milliseconds
 * 
 * Project 2: Converts CPU cycle count to milliseconds.
 * Unlike Project 1's formula which gave "ms in current second",
 * this gives total milliseconds since boot.
 * 
 * Formula: (cycles * 1000) / cycles_per_second = milliseconds
 * 
 * Returns: Current time in milliseconds
 */
uint64 get_msec()
{
	uint64 cycle = get_cycle();      // Get current CPU cycle count
	return cycle * 1000 / CPU_FREQ;  // Convert cycles to milliseconds
}
// =================================================