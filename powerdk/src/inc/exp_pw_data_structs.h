/* ***********************************************************************************************

  This file is provided under a dual BSD/GPLv2 license.  When using or
  redistributing this file, you may do so under either license.

  GPL LICENSE SUMMARY

  Copyright(c) 2011 Intel Corporation. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
  The full GNU General Public License is included in this distribution
  in the file called LICENSE.GPL.

  Contact Information:
  Gautam Upadhyaya <gautam.upadhyaya@intel.com>
  1906 Fox Drive, Champaign, IL - 61820, USA

  BSD LICENSE

  Copyright(c) 2011 Intel Corporation. All rights reserved.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  ***********************************************************************************************
*/

#ifndef _PW_DATA_STRUCTS_H_
#define _PW_DATA_STRUCTS_H_ 1

/*
 * We're the PWR kernel device driver.
 * Flag needs to be set BEFORE
 * including 'pw_ioctl.h'
 */
#define PW_KERNEL_MODULE 1

#include "exp_pw_ioctl.h" // For IOCTL mechanism
#include <linux/fs.h>
#include <linux/bitops.h> // for "test_and_set_bit(...)" atomic functionality


enum{
    EMPTY=0,
    FULL
};

enum{
    IRQ=0,
    TIMER,
    SCHED
};

#define NUM_SEGS_PER_LIST 2

#define SAMPLE_MASK (NUM_SAMPLES_PER_SEG - 1)
#define LIST_MASK (NUM_SEGS_PER_LIST - 1)

/*
 * Output buffer (also called a "segment" or "seg").
 * Driver writes samples into these. "read(...)" function
 * pulls samples out of these.
 */
typedef struct{
    PWCollector_sample_t samples[NUM_SAMPLES_PER_SEG];
    int index;
    atomic_t is_full;
}seg_t;

/*
 * Per-cpu structure (or "list") of output buffers.
 * Each list has "NUM_SEGS_PER_LIST" (== 2) buffers.
 */
typedef struct{
    seg_t segs[NUM_SEGS_PER_LIST];
    int index;
    int flush_index;
}list_t;


#define SAMPLE(s,i) ( (s)->samples[(i)] )
#define C_SAMPLE(s,i) ( (s)->samples[(i)].c_sample )
#define P_SAMPLE(s,i) ( (s)->samples[(i)].p_sample )
#define K_SAMPLE(s,i) ( (s)->samples[(i)].k_sample )
#define M_SAMPLE(s,i) ( (s)->samples[(i)].m_sample )

/*
 * Structure to hold current CMD state
 * of the device driver. Constantly evolving, but
 * that's OK -- this is internal to the driver
 * and is NOT exported.
 */
typedef struct{
    PWCollector_cmd_t cmd; // indicates which command was specified last e.g. START, STOP etc.
    /*
     * Should we write to our per-cpu output buffers?
     * YES if we're actively collecting.
     * NO if we're not.
     */
    bool write_to_buffers;
    /*
     * Should we "drain/flush" the per-cpu output buffers?
     * (See "device_read" for an explanation)
     */
    bool drain_buffers;
    /*
     * Current methodology for generating kernel-space call
     * stacks relies on following frame pointer: has
     * the kernel been compiled with frame pointers?
     */
    bool have_kernel_frame_pointers;
    /*
     * On some archs, C-state residency MSRS do NOT count at TSC frequency.
     * For these, we need to apply a "clock multiplier". Record that
     * here.
     */
    unsigned int residency_count_multiplier;
	/*
	 * Store the bus clock frequency.
	 */
	unsigned int bus_clock_freq_khz;
    /*
     * Core/Pkg MSR residency addresses
     */
    unsigned int coreResidencyMSRAddresses[MAX_MSR_ADDRESSES];
    unsigned int pkgResidencyMSRAddresses[MAX_MSR_ADDRESSES];
    /*
     * What switches should the device driver collect?
     * Note: different from interface spec:
     * We're moving from bitwise OR to bitwise OR of (1 << switch) values.
     * Use the "POWER_XXX_MASK" masks to set/test switch residency.
     */
    int collection_switches;
    /*
     * Total time elapsed for
     * all collections.
     * Aggregated over all collections -- useful
     * in multiple PAUSE/RESUME scenarios
     */
    unsigned long totalCollectionTime;
    /*
     * Start and stop jiffy values for
     * the current collection.
     */
    unsigned long collectionStartJIFF, collectionStopJIFF;
    // Others...
}internal_state_t;

static internal_state_t INTERNAL_STATE;

#define IS_COLLECTING() (INTERNAL_STATE.cmd == START || INTERNAL_STATE.cmd == RESUME)
#define IS_SLEEPING() (INTERNAL_STATE.cmd == PAUSE)
#define IS_SLEEP_MODE() (INTERNAL_STATE.collection_switches & POWER_SLEEP_MASK)
#define IS_FREQ_MODE() (INTERNAL_STATE.collection_switches & POWER_FREQ_MASK)
#define IS_KTIMER_MODE() (INTERNAL_STATE.collection_switches & POWER_KTIMER_MASK)
#define IS_NON_PRECISE_MODE() (INTERNAL_STATE.collection_switches & POWER_SYSTEM_MASK)

/*
 * Per-cpu structure holding MSR residency counts,
 * timer-TSC values etc.
 */
typedef struct per_cpu_struct{
    unsigned int freqState; // 4 bytes
    pid_t last_pid; // 4 bytes
    pid_t last_tid; // 4 bytes
    int last_type; // 4 bytes
    unsigned int prev_state; // 4 bytes
    u64 old_alpha; // 8 bytes
    u64 tsc; // 8 bytes
    u64 residencies[MAX_MSR_ADDRESSES]; // 96 bytes
    u64 prev_msr_vals[MAX_MSR_ADDRESSES]; // 96 bytes
    u64 debug_enters;

    atomic_t is_first;

    u64 last_break[3]; // 24 bytes
    unsigned char is_deferrable; // 1 byte

    void *sched_timer_addr;
}per_cpu_t;


/*
 * Convenience macros for accessing per-cpu residencies
 */
#define RESIDENCY(p,i) ( (p)->residencies[(i)] )
#define PREV_MSR_VAL(p,i) ( (p)->prev_msr_vals[(i)] )

/*
 * Per-cpu structure holding stats information.
 * Eventually, we may want to incorporate these fields within
 * the "per_cpu_t" structure.
 */
typedef struct{
    local_t c_breaks, timer_c_breaks, inters_c_breaks;
    local_t p_trans;
    local_t num_inters, num_timers;
}stats_t;

/*
 * Struct to hold old IA32_FIXED_CTR_CTRL MSR
 * values (to enable restoring
 * after pw driver terminates). These are
 * used to enable/restore/disable CPU_CLK_UNHALTED.REF
 * counting.
 *
 * UPDATE: also store old IA32_PERF_GLOBAL_CTRL values..
 */
typedef struct{
    u32 fixed_data[2], perf_data[2];
}CTRL_values_t;


#endif // _PW_DATA_STRUCTS_H_