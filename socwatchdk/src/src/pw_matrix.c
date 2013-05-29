/* ***********************************************************************************************

   This file is provided under a dual BSD/GPLv2 license.  When using or 
   redistributing this file, you may do so under either license.

   GPL LICENSE SUMMARY

   Copyright(c) 2013 Intel Corporation. All rights reserved.

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
   SOCWatch Developer Team <socwatchdevelopers@intel.com>

   BSD LICENSE 

   Copyright(c) 2013 Intel Corporation. All rights reserved.
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/version.h>
#if DO_ANDROID
    #include <asm/intel_mid_rpmsg.h>
    #include <asm/intel_scu_pmic.h>	/* Needed for 3.4 kernel port */
    #include <asm/intel-mid.h>
#endif // DO_ANDROID
#include <linux/kdev_t.h>
#include <asm/paravirt.h>

#include "matrix.h"

// #define NAME "matrix"
#define NAME "socwatch"
//#define DRIVER_VERSION "1.0"

#define MT_SUCCESS 0
#define MT_ERROR 1

#define MCR_WRITE_OPCODE    0x11
#define BIT_POS_OPCODE      24

#define PW_NUM_SOCHAP_COUNTERS 9
extern void LWPMUDRV_VISA_Read_Data (void *data_buffer);

static int matrix_major_number;
static bool instantiated;
static bool mem_alloc_status;
static u8 *ptr_lut_ops;
static u8 *ptr_xhg_buf_ops;
static unsigned long io_pm_status_reg;
static unsigned long io_pm_lower_status;
static unsigned long io_pm_upper_status;
static unsigned int io_base_pwr_address;
static dev_t matrix_dev;
static struct cdev *matrix_cdev;
static struct class *matrix_class;
static struct timeval matrix_time;
static struct device *matrix_device;
static struct xchange_buffer_all *ptr_xc_buff_all;
static struct lookup_table *ptr_lut;
static struct mtx_size_info lut_info;
static struct mtx_size_info xhg_buf_info;


/**
 * Matrix Driver works in such a way that only one thread
 * and one instance of driver can occur at a time.
 * At the time of opening the driver file, driver checks driver
 * status whether its already instantiated or not.. if it is, it
 * will not allow to open new instance..
 */

#define MATRIX_GET_TIME_STAMP(time_stamp) \
    do { \
        do_gettimeofday(&matrix_time); \
        time_stamp = \
        (((u64)matrix_time.tv_sec * 1000000) \
         + (u64)matrix_time.tv_usec); \
    } while (0)

#define MATRIX_GET_TSC(tsc) rdtscll(tsc)

#define MATRIX_COPY_TO_USER_BUFFER(state, type, label) \
    do { \
        if (copy_to_user(xbuf_all.xhg_buf_##state.ptr_##type##_buff , \
                    ptr_xc_buff_all->xhg_buf_##state.ptr_##type##_buff , \
                    xhg_buf_info.state##_##type##_size) > 0) { \
            dev_dbg(matrix_device, \
                    "file : %s ,function : %s ,line %i\n", \
                    __FILE__, __func__, __LINE__); \
            goto label; \
        } \
    } while (0)

#define MATRIX_INCREMENT_MEMORY(cu, cl, buffer, type, lut) \
    do { \
        if (lut) { \
            buffer##_info.init_##type##_size = \
            sizeof(cu cl) * ptr_lut->type##_##init_length; \
            buffer##_info.term_##type##_size = \
            sizeof(cu cl) * ptr_lut->type##_##term_length; \
            buffer##_info.poll_##type##_size = \
            sizeof(cu cl) * ptr_lut->type##_##poll_length; \
            lut_info.total_mem_bytes_req += \
            buffer##_info.init_##type##_size + \
            buffer##_info.term_##type##_size + \
            buffer##_info.poll_##type##_size; \
        } \
        else { \
            buffer##_info.init_##type##_size = \
            sizeof(cu cl) * ptr_lut->type##_##init_wb; \
            buffer##_info.term_##type##_size = \
            sizeof(cu cl) * ptr_lut->type##_##term_wb; \
            buffer##_info.poll_##type##_size = \
            sizeof(cu cl) * ptr_lut->type##_##poll_wb \
            * ptr_lut->records; \
            xhg_buf_info.total_mem_bytes_req += \
            buffer##_info.init_##type##_size + \
            buffer##_info.term_##type##_size + \
            buffer##_info.poll_##type##_size; \
        } \
    } while (0)

#define MATRIX_IO_REMAP_MEMORY(state) \
    do { \
        unsigned long count; \
        for (count = 0; \
                count < ptr_lut->mem_##state##_length; count++) { \
            if (ptr_lut->mmap_##state[count].ctrl_addr) { \
                ptr_lut->mmap_##state[count].ctrl_remap_address = \
                ioremap_nocache(ptr_lut-> \
                        mmap_##state[count].ctrl_addr, \
                        sizeof(unsigned long)); \
            } \
            if (ptr_lut->mmap_##state[count].data_addr) { \
                ptr_lut->mmap_##state[count].data_remap_address = \
                ioremap_nocache(ptr_lut-> \
                        mmap_##state[count].data_addr, \
                        (ptr_lut-> \
                         mmap_##state[count].data_size) \
                        * sizeof(unsigned long));\
            }  \
        } \
    } while (0)

#define MATRIX_IOUNMAP_MEMORY(state) \
    do { \
        unsigned long count; \
        for (count = 0; \
                count < ptr_lut->mem_##state##_length; count++) { \
            if (ptr_lut->mmap_##state[count].ctrl_remap_address) { \
                iounmap(ptr_lut->mmap_##state[count]. \
                        ctrl_remap_address); \
                ptr_lut->mmap_##state[count]. \
                ctrl_remap_address = NULL; \
            } \
            if (ptr_lut->mmap_##state[count].data_remap_address) { \
                iounmap(ptr_lut->mmap_##state[count]. \
                        data_remap_address); \
                ptr_lut->mmap_##state[count]. \
                data_remap_address = NULL; \
            } \
        } \
    } while (0)

#define MATRIX_BOOK_MARK_LUT(state, type, struct_init, structure, member, mem, label) \
    do { \
        if (lut_info.state##_##type##_size) { \
            if (copy_from_user \
                    (&ptr_lut_ops[offset], ptr_lut->member##_##state, \
                     lut_info.state##_##type##_size) > 0) { \
                dev_dbg(matrix_device, \
                        "file : %s ,function : %s ,line %i\n", \
                        __FILE__, __func__, __LINE__); \
                goto label; \
            } \
            ptr_lut->member##_##state =  \
            (struct_init structure*)&ptr_lut_ops[offset]; \
            offset += lut_info.state##_##type##_size; \
            if (mem) \
            MATRIX_IO_REMAP_MEMORY(state); \
        } else \
        ptr_lut->member##_##state = NULL; \
    } while (0)

#define MATRIX_BOOK_MARK_EXCHANGE_BUFFER(state, type, cu, cl, mem) \
    do { \
        ptr_xc_buff_all->xhg_buf_##state.type##_length = \
        ptr_lut->type##_##state##_wb; \
        if (xhg_buf_info.state##_##type##_size) { \
            ptr_xc_buff_all->xhg_buf_##state.ptr_##type##_buff = \
            (cu cl*)&ptr_xhg_buf_ops[offset]; \
            if (mem) \
            memset(&ptr_xhg_buf_ops[offset], \
                    0, \
                    xhg_buf_info.state##_##type##_size); \
            offset += xhg_buf_info.state##_##type##_size; \
        } else \
        ptr_xc_buff_all-> \
        xhg_buf_##state.ptr_##type##_buff = NULL; \
    } while (0)

#define MATRIX_SCAN_MSR(state, label) \
    do { \
        unsigned long msr_loop = 0; \
        for (lut_loop = 0; lut_loop < max_msr_loop; \
                lut_loop++) { \
            unsigned int cpu;   \
            u32 *lo_rd, *high_rd, lo_wr, high_wr; \
            u32 msr_no; \
            cpu = (unsigned int) ptr_lut->msrs_##state[lut_loop]\
            .n_cpu; \
            msr_no = ptr_lut->msrs_##state[lut_loop]. \
            ecx_address;  \
            lo_rd = (u32 *)&(ptr_xc_buff_all->xhg_buf_##state. \
                    ptr_msr_buff[msr_loop].eax_LSB); \
            high_rd = (u32 *) &(ptr_xc_buff_all->xhg_buf_##state. \
                    ptr_msr_buff[msr_loop].edx_MSB);   \
            lo_wr = (ptr_lut->msrs_##state[lut_loop].eax_LSB); \
            high_wr = (ptr_lut->msrs_##state[lut_loop].edx_MSB);   \
            switch (ptr_lut->msrs_##state[lut_loop].operation) { \
                case READ_OP:   \
                                { \
                                    rdmsr_on_cpu(cpu, msr_no, lo_rd, high_rd);    \
                                    msr_loop++; \
                                    break;  \
                                } \
                case WRITE_OP:  \
                                {	\
                                    wrmsr_on_cpu(cpu, msr_no, lo_wr, high_wr); \
                                    break;  \
                                }	\
                case SET_BITS_OP:   \
                                    { \
                                        u32 eax_LSB, edx_MSB; \
                                        rdmsr_on_cpu(cpu, msr_no, \
                                                &eax_LSB, &edx_MSB); \
                                        wrmsr_on_cpu(cpu, msr_no, \
                                                (eax_LSB | lo_wr), \
                                                (edx_MSB | high_wr)); \
                                        break;  \
                                    } \
                case RESET_BITS_OP:   \
                                      { \
                                          u32 eax_LSB, edx_MSB; \
                                          rdmsr_on_cpu(cpu, msr_no, \
                                                  &eax_LSB, &edx_MSB); \
                                          wrmsr_on_cpu(cpu, msr_no, \
                                                  (eax_LSB & ~(lo_wr)), \
                                                  (edx_MSB & ~(high_wr))); \
                                          break;  \
                                      } \
                default: \
                         dev_dbg(matrix_device, \
                                 "Error in MSR_OP value..\n"); \
                goto label; \
            } \
        } \
    } while (0)

#if DO_ANDROID
#define MATRIX_SCAN_MMAP(state, label) \
    do { \
        unsigned long mem_loop = 0; \
        unsigned long scu_sub_cmd; \
        unsigned long scu_cmd; \
        for (lut_loop = 0; lut_loop < ptr_lut->mem_##state##_length; \
                lut_loop++) { \
            /* If ctrl_addr != NULL, we do scu IPC command  \
             * else it is a case of mmio read and data_remap \
             * _address should point to the mmio address from which \
             * we need to read  \
             */ \
            if (ptr_lut->mmap_##state[lut_loop].ctrl_addr) {    \
                scu_cmd = ptr_lut->mmap_##state[lut_loop]. \
                ctrl_data & 0xFF; \
                scu_sub_cmd = (ptr_lut->mmap_##state[lut_loop]. \
                        ctrl_data >> 12) & 0xF; \
                if ((rpmsg_send_generic_simple_command \
                            (scu_cmd, scu_sub_cmd)) != 0) { \
                    dev_dbg(matrix_device, \
                            "Unable to get SCU data..\n"); \
                    goto label; \
                } \
            }   \
            if (ptr_lut->mmap_##state[lut_loop]. \
                    data_size != 0) { \
                memcpy(&ptr_xc_buff_all->xhg_buf_##state. \
                        ptr_mem_buff[mem_loop], \
                        ptr_lut->mmap_##state[lut_loop]. \
                        data_remap_address, \
                        ptr_lut->mmap_##state[lut_loop]. \
                        data_size * sizeof(unsigned long)); \
                mem_loop += ptr_lut->mmap_##state[lut_loop]. \
                data_size; \
                if (mem_loop > max_mem_loop) { \
                    dev_dbg(matrix_device, \
                            "A(%04d) [0x%40lu]of [0x%40lu]\n", \
                            __LINE__, mem_loop, max_mem_loop); \
                    goto label; \
                } \
            } \
        } \
    } while (0)

#define MATRIX_SCAN_DATA(state, label) \
    do { \
        unsigned long max_msr_loop; \
        unsigned long max_mem_loop; \
        unsigned long max_cfg_db_loop; \
        unsigned long lut_loop; \
        max_msr_loop = ptr_lut->msr_##state##_length; \
        max_mem_loop = ptr_xc_buff_all->xhg_buf_##state.mem_length; \
        max_cfg_db_loop = ptr_lut->cfg_db_##state##_length; \
        MATRIX_GET_TIME_STAMP(ptr_xc_buff_all->state##_time_stamp); \
        MATRIX_GET_TSC(ptr_xc_buff_all->state##_tsc);   \
        if (NULL != ptr_lut->msrs_##state) \
        MATRIX_SCAN_MSR(state, label); \
        if (NULL != ptr_lut->mmap_##state) \
        MATRIX_SCAN_MMAP(state, label); \
        for (lut_loop = 0; lut_loop < max_cfg_db_loop; lut_loop++) { \
            ptr_xc_buff_all->xhg_buf_##state. \
            ptr_cfg_db_buff[lut_loop] = \
            mt_platform_pci_read32(ptr_lut-> \
                    cfg_db_##state[lut_loop]); \
        } \
    } while (0)

#else // !DO_ANDROID
#define MATRIX_SCAN_DATA(state, label) \
    do { \
        unsigned long max_msr_loop; \
        unsigned long max_mem_loop; \
        unsigned long max_cfg_db_loop; \
        unsigned long lut_loop; \
        max_msr_loop = ptr_lut->msr_##state##_length; \
        max_mem_loop = ptr_xc_buff_all->xhg_buf_##state.mem_length; \
        max_cfg_db_loop = ptr_lut->cfg_db_##state##_length; \
        MATRIX_GET_TIME_STAMP(ptr_xc_buff_all->state##_time_stamp); \
        MATRIX_GET_TSC(ptr_xc_buff_all->state##_tsc); \
        if (NULL != ptr_lut->msrs_##state) \
        MATRIX_SCAN_MSR(state, label); \
        for (lut_loop = 0; lut_loop < max_cfg_db_loop; lut_loop++) { \
            ptr_xc_buff_all->xhg_buf_##state. \
            ptr_cfg_db_buff[lut_loop] = \
            mt_platform_pci_read32(ptr_lut-> \
                    cfg_db_##state[lut_loop]); \
        } \
    } while (0)

#endif // DO_ANDROID

#define MATRIX_VMALLOC(ptr, size, label) \
    do { \
        if (size > 0) { \
            ptr = vmalloc(size); \
            if (ptr == NULL) { \
                dev_dbg(matrix_device, "file : %s line %i\n", \
                        __FILE__, __LINE__); \
                goto label; \
            } \
        } \
    } while (0)

static int matrix_open(struct inode *in, struct file *filp)
{
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	if (instantiated) {
		module_put(THIS_MODULE);
		return -EBUSY;
	} else {
		instantiated = true;
		return 0;
	}
}

/**
 * platform_pci_read32 - for reading PCI space through config registers
 * of the platform.
 * @address : an address in the pci space
 */
static unsigned long mt_platform_pci_read32(unsigned long address)
{
	u32 read_value = 0;
	struct pci_dev *pci_root = pci_get_bus_and_slot(0, PCI_DEVFN(0, 0));
	if (!pci_root)
		return 0; /* Application will verify the data */
	pci_write_config_dword(pci_root, MTX_PCI_MSG_CTRL_REG, address);
	pci_read_config_dword(pci_root, MTX_PCI_MSG_DATA_REG, &read_value);
	return read_value;
}

/**
 * platform_pci_write32 - for writing into PCI space through config registers
 * of the platform.
 * @address : an address in the pci space
 * @data : data that has to wrriten
 */
static void mt_platform_pci_write32(unsigned long address, unsigned long data)
{
	struct pci_dev *pci_root = pci_get_bus_and_slot(0, PCI_DEVFN(0, 0));
	if (pci_root) {
		pci_write_config_dword(pci_root, MTX_PCI_MSG_DATA_REG, data);
		pci_write_config_dword(pci_root, MTX_PCI_MSG_CTRL_REG, address);
	}
}

/**
 * calculate_memory_requirements - determine the amount of memory required based * on data passed in from the user space
 */
static void mt_calculate_memory_requirements(void)
{
	lut_info.total_mem_bytes_req = 0;
	xhg_buf_info.total_mem_bytes_req = 0;

	/* Find out memory required for Lookup table */
	MATRIX_INCREMENT_MEMORY(struct, mtx_msr, lut, msr, 1);
	MATRIX_INCREMENT_MEMORY(struct, memory_map, lut, mem, 1);
	MATRIX_INCREMENT_MEMORY(struct, mtx_pci_ops, lut, pci_ops, 1);
//	MATRIX_INCREMENT_MEMORY(unsigned, long, lut, cfg_db, 1);
	MATRIX_INCREMENT_MEMORY(unsigned, int, lut, cfg_db, 1);
	MATRIX_INCREMENT_MEMORY(struct, mtx_visa, lut, visa, 1);
	lut_info.poll_scu_drv_size =
	    ptr_lut->scu_poll.length * ptr_lut->scu_poll_length;
	lut_info.total_mem_bytes_req += lut_info.poll_scu_drv_size;

	/* calculate memory required for buffer to be copied to user space */
	MATRIX_INCREMENT_MEMORY(struct, msr_buffer, xhg_buf, msr, 0);
	MATRIX_INCREMENT_MEMORY(unsigned, int, xhg_buf, mem, 0);
	MATRIX_INCREMENT_MEMORY(unsigned, int, xhg_buf, pci_ops, 0);
	MATRIX_INCREMENT_MEMORY(unsigned, int, xhg_buf, cfg_db, 0);
	MATRIX_INCREMENT_MEMORY(struct, visa_buffer, xhg_buf, visa, 0);
//	MATRIX_INCREMENT_MEMORY(unsigned, long, xhg_buf, mem, 0);
//	MATRIX_INCREMENT_MEMORY(unsigned, long, xhg_buf, pci_ops, 0);
//	MATRIX_INCREMENT_MEMORY(unsigned, long, xhg_buf, cfg_db, 0);
//	MATRIX_INCREMENT_MEMORY(struct, visa_buffer, xhg_buf, visa, 0);
}

/**
 * bookmark_lookup_table - bookmark memory locations of structures within the
 * chunk of memory allocated earlier
 */
static int mt_bookmark_lookup_table(void)
{
	unsigned long offset = 0;

	/* msr part of the lookup table */
	MATRIX_BOOK_MARK_LUT(init, msr, struct, mtx_msr, msrs, 0, COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(poll, msr, struct, mtx_msr, msrs, 0, COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(term, msr, struct, mtx_msr, msrs, 0, COPY_FAIL);

	/* mem part of the lookup table */
	MATRIX_BOOK_MARK_LUT(init, mem, struct, memory_map, mmap, 1, COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(poll, mem, struct, memory_map, mmap, 1, COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(term, mem, struct, memory_map, mmap, 1, COPY_FAIL);

	/* pci part of the lookup table */
	MATRIX_BOOK_MARK_LUT(init, pci_ops, struct, mtx_pci_ops, pci_ops, 0,
			     COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(poll, pci_ops, struct, mtx_pci_ops, pci_ops, 0,
			     COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(term, pci_ops, struct, mtx_pci_ops, pci_ops, 0,
			     COPY_FAIL);

	/* config_db part of the lookup table */
	MATRIX_BOOK_MARK_LUT(init, cfg_db, unsigned, long, cfg_db, 0,
			     COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(poll, cfg_db, unsigned, long, cfg_db, 0,
			     COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(term, cfg_db, unsigned, long, cfg_db, 0,
			     COPY_FAIL);

	/* scu part of the lookup table */
	ptr_lut->scu_poll.drv_data = (unsigned char *)&ptr_lut_ops[offset];

	/* visa part of the lookup table */
	MATRIX_BOOK_MARK_LUT(init, visa, struct, mtx_visa, visa, 0,
			     COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(poll, visa, struct, mtx_visa, visa, 0,
			     COPY_FAIL);
	MATRIX_BOOK_MARK_LUT(term, visa, struct, mtx_visa, visa, 0,
			     COPY_FAIL);
	return 0;
COPY_FAIL:
	return -EFAULT;
}

/**
 * bookmark_xchange_buffer - bookmark memory locations of structures that hold
 * all of the data collected
 */
static void mt_bookmark_xchange_buffer(void)
{
	unsigned long offset = 0;

	/* bookmark memory location used in the first run */
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(init, msr, struct, msr_buffer, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(init, mem, unsigned, long, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(init, pci_ops, unsigned, long, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(init, cfg_db, unsigned, long, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(init, visa, struct, visa_buffer, 0);

	/* bookmark memory location used while polling */
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(poll, msr, struct, msr_buffer, 1);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(poll, mem, unsigned, long, 1);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(poll, pci_ops, unsigned, long, 1);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(poll, cfg_db, unsigned, long, 1);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(poll, visa, struct, visa_buffer, 1);

	/* bookmark memory location used in the last run */
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(term, msr, struct, msr_buffer, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(term, mem, unsigned, long, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(term, pci_ops, unsigned, long, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(term, cfg_db, unsigned, long, 0);
	MATRIX_BOOK_MARK_EXCHANGE_BUFFER(term, visa, struct, visa_buffer, 0);
}

/**
 * free_memory - frees up all of the memory obtained
 */
static int mt_free_memory(void)
{
	/*Freeing Exchange Buffer Memory */
	vfree(ptr_xhg_buf_ops);
	ptr_xhg_buf_ops = NULL;

	if (ptr_xc_buff_all) {
		vfree(ptr_xc_buff_all->poll_time_stamp);
		ptr_xc_buff_all->poll_time_stamp = NULL;
		vfree(ptr_xc_buff_all->poll_tsc);
		ptr_xc_buff_all->poll_tsc = NULL;
	}

	/* Freeing IOREMAP Memory */
	if (ptr_lut) {
		MATRIX_IOUNMAP_MEMORY(init);
		MATRIX_IOUNMAP_MEMORY(term);
		MATRIX_IOUNMAP_MEMORY(poll);
		vfree(ptr_lut);
		ptr_lut = NULL;
	}
	/*Freeing LUT Memory */
	vfree(ptr_lut_ops);
	ptr_lut_ops = NULL;

	vfree(ptr_xc_buff_all);
	ptr_xc_buff_all = NULL;
	mem_alloc_status = false;
	return 0;
}

/**
 * initialize_memory - initializes all of the required memory as requested
 * @ptr_data : gets the address of the lookup table that has all the info
 */
static int mt_initialize_memory(unsigned long ptr_data)
{
	if (mem_alloc_status) {
		dev_dbg(matrix_device,
			"Initialization of Memory is already done..\n");
                // printk(KERN_INFO "Initialization of Memory is already done..\n");
		return -EPERM;
	}
	/* pointer to kernel buffer that stores captured info */
	MATRIX_VMALLOC(ptr_xc_buff_all, sizeof(struct xchange_buffer_all),
		       ERROR);

	/* get information about lookup table from user space */
	MATRIX_VMALLOC(ptr_lut, sizeof(struct lookup_table), ERROR);

	if (copy_from_user(ptr_lut,
			   (struct lookup_table *)ptr_data,
			   sizeof(struct lookup_table)) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		// printk(KERN_INFO "file : %s ,function : %s ,line %i\n", __FILE__, __func__, __LINE__);
		goto ERROR;
	}

	/* allocate memory for time stamps off all polls */
	MATRIX_VMALLOC(ptr_xc_buff_all->poll_time_stamp,
		       sizeof(u64) * ptr_lut->records, ERROR);

	/* allocate memory for tsc counter for all polls */
	MATRIX_VMALLOC(ptr_xc_buff_all->poll_tsc,
		       sizeof(u64) * ptr_lut->records, ERROR);

	mt_calculate_memory_requirements();

	/* allocate once and for all memory required for lookup table */
	MATRIX_VMALLOC(ptr_lut_ops, lut_info.total_mem_bytes_req, ERROR);

	if (mt_bookmark_lookup_table() < 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		// printk(KERN_INFO "file : %s ,function : %s ,line %i\n", __FILE__, __func__, __LINE__);
		goto ERROR;
	}

	/* allocate once and for all memory required for exchange buffer */
	MATRIX_VMALLOC(ptr_xhg_buf_ops, xhg_buf_info.total_mem_bytes_req,
		       ERROR);

	mt_bookmark_xchange_buffer();
	io_pm_status_reg =
	    (mt_platform_pci_read32(ptr_lut->pci_ops_poll->port) &
	     PWR_MGMT_BASE_ADDR_MASK);
	io_base_pwr_address =
	    (mt_platform_pci_read32(ptr_lut->pci_ops_poll->port_island) &
	     PWR_MGMT_BASE_ADDR_MASK);
	mem_alloc_status = true;

	return 0;
ERROR:
        printk(KERN_INFO "Memory Initialization Error!\n");
	mt_free_memory();
	return -EFAULT;
}

/**
 * initial_scan - function that is run once before polling at regular intervals
 * sets the msr's and other variables to their default values
 */
static int mt_data_scan(unsigned long ioctl_request)
{
	if (ptr_lut == NULL || ptr_xc_buff_all == NULL)
		goto ERROR;
	if (ioctl_request == IOCTL_INIT_SCAN)
		MATRIX_SCAN_DATA(init, ERROR);
	else if (ioctl_request == IOCTL_TERM_SCAN)
		MATRIX_SCAN_DATA(term, ERROR);
	else
		goto ERROR;
	return 0;
ERROR:
	mt_free_memory();
	return -EFAULT;
}

/**
 * poll_scan - function that is called at each iteration of the poll.
 * at each poll observations are made and stored in kernel buffer.
 * @poll_loop : specifies the current iteration of polling
 */
static int mt_poll_scan(unsigned long poll_loop)
{
	unsigned long msr_loop = 0;
	unsigned long mem_loop = 0;
	unsigned long lut_loop;
	unsigned long max_msr_loop;
	unsigned long max_mem_loop;
	unsigned long msr_base_addr;
	unsigned long mem_base_addr;
	unsigned long max_msr_read;
	unsigned long max_cfg_db_loop;
	unsigned long cfg_db_base_addr;
	unsigned long delta_time;

	if (ptr_lut == NULL || ptr_xc_buff_all == NULL)
		goto ERROR;

        //printk(KERN_INFO "poll_loop = %lu\n", poll_loop);

#if DO_ANDROID
	max_msr_loop = ptr_lut->msr_poll_length;
	max_msr_read = ptr_lut->msr_poll_wb;
	max_mem_loop = ptr_xc_buff_all->xhg_buf_poll.mem_length;
	max_cfg_db_loop = ptr_lut->cfg_db_poll_length;
	msr_base_addr = (poll_loop * max_msr_read);
	mem_base_addr = (poll_loop * max_mem_loop);
	cfg_db_base_addr = (poll_loop * max_cfg_db_loop);
#endif
	MATRIX_GET_TIME_STAMP(ptr_xc_buff_all->poll_time_stamp[poll_loop]);
	MATRIX_GET_TSC(ptr_xc_buff_all->poll_tsc[poll_loop]);
	delta_time =
	    (poll_loop >
	     0) ? (ptr_xc_buff_all->poll_time_stamp[poll_loop] -
		   ptr_xc_buff_all->poll_time_stamp[poll_loop - 1]) : 0;

#if DO_ANDROID
	if (NULL != ptr_lut->msrs_poll) {
		for (lut_loop = 0; lut_loop < max_msr_loop; lut_loop++) {
			if (ptr_lut->msrs_poll[lut_loop].operation == READ_OP) {
				rdmsr_on_cpu(ptr_lut->msrs_poll[lut_loop].n_cpu,
					     ptr_lut->msrs_poll[lut_loop].
					     ecx_address,
					     (u32 *) & (ptr_xc_buff_all->
							xhg_buf_poll.ptr_msr_buff
							[msr_base_addr +
							 msr_loop].eax_LSB),
					     (u32 *) & (ptr_xc_buff_all->
							xhg_buf_poll.ptr_msr_buff
							[msr_base_addr +
							 msr_loop].edx_MSB));
				msr_loop++;
			} else if (ptr_lut->msrs_poll[lut_loop].operation ==
				   WRITE_OP) {
				wrmsr_on_cpu(ptr_lut->msrs_poll[lut_loop].n_cpu,
					     ptr_lut->msrs_poll[lut_loop].
					     ecx_address,
					     ptr_lut->msrs_poll[lut_loop].
					     eax_LSB,
					     ptr_lut->msrs_poll[lut_loop].
					     edx_MSB);
			} else {
				dev_dbg(matrix_device,
					"Error in MSR_OP value..\n");
				goto ERROR;
			}
		}
	}

	if (NULL != ptr_lut->mmap_poll) {
		for (lut_loop = 0; lut_loop < max_mem_loop; lut_loop++) {
			/*
			 * If ctrl_remap_adr = NULL, then the interface does
			 * mmio read
			 */
			if (ptr_lut->mmap_poll[lut_loop].ctrl_remap_address)
				writel(ptr_lut->mmap_poll[lut_loop].ctrl_data,
						ptr_lut->mmap_poll[lut_loop].ctrl_remap_address);
			if (ptr_lut->mmap_poll[lut_loop].data_size != 0) {
				memcpy(&ptr_xc_buff_all->
				       xhg_buf_poll.ptr_mem_buff[mem_base_addr +
								 mem_loop],
				       ptr_lut->
				       mmap_poll[lut_loop].data_remap_address,
				       ptr_lut->mmap_poll[lut_loop].data_size *
				       sizeof(unsigned long));
				mem_loop +=
				    ptr_lut->mmap_poll[lut_loop].data_size;
				if (mem_loop > max_mem_loop) {
					dev_dbg(matrix_device,
						"A(%04d) [0x%40lu]of [0x%40lu]\n",
						__LINE__, mem_loop,
						max_mem_loop);
					goto ERROR;
				}
			}
		}
	}

	/* Get the status of power islands in the North Complex */
	io_pm_lower_status = inl(io_pm_status_reg + PWR_STS_NORTH_CMPLX_LOWER);
	io_pm_upper_status =
	    inl(io_base_pwr_address + PWR_STS_NORTH_CMPLX_UPPER);
	memcpy(&ptr_xc_buff_all->xhg_buf_poll.ptr_pci_ops_buff[2 * poll_loop],
	       &io_pm_lower_status, sizeof(unsigned long));
	memcpy(&ptr_xc_buff_all->xhg_buf_poll.
	       ptr_pci_ops_buff[2 * poll_loop + 1], &io_pm_upper_status,
	       sizeof(unsigned long));

	/* SCU IO */
	if (0 != ptr_lut->scu_poll.length) {
		int status;
		unsigned long offset = (ptr_lut->scu_poll.length * poll_loop);
		for (lut_loop = 0; lut_loop < ptr_lut->scu_poll.length;
		     lut_loop++) {
			status =
			    intel_scu_ipc_ioread8(ptr_lut->scu_poll.address
						  [lut_loop],
						  &ptr_lut->scu_poll.drv_data
						  [offset + lut_loop]);
			if (status == 0) {
				dev_dbg(matrix_device,
					"IPC failed for reg: %lu addr: %c ..\n",
					ptr_lut->scu_poll.address[lut_loop],
					ptr_lut->scu_poll.drv_data[offset +
								   lut_loop]);
				goto ERROR;
			}
		}
	}
	cfg_db_base_addr = (poll_loop * max_cfg_db_loop);
	for (lut_loop = 0; lut_loop < max_cfg_db_loop; lut_loop++) {
		ptr_xc_buff_all->xhg_buf_poll.ptr_cfg_db_buff[cfg_db_base_addr +
							      lut_loop] =
		    platform_pci_read32(ptr_lut->cfg_db_poll[lut_loop]);
	}
#endif // DO_ANDROID

	/* Get the VISA counter values */
        if (NULL != ptr_lut->visa_poll) {
            if (ptr_lut->visa_poll[0].operation == READ_OP) {
                 //int i = 0;
                 u64 __visa_buffer[PW_NUM_SOCHAP_COUNTERS];

                 memset(__visa_buffer, 0, sizeof(__visa_buffer));

                 LWPMUDRV_VISA_Read_Data(__visa_buffer);

                 //for (i=0; i<sizeof(__visa_buffer) / sizeof(u64); ++i) {
                 //    printk(KERN_INFO "\t[SOCWATCH VISA]: VISA[%d] = %llu\n", i, __visa_buffer[i]);
                 //}

                 memcpy(&ptr_xc_buff_all->xhg_buf_poll.ptr_visa_buff[poll_loop], __visa_buffer,
                     sizeof(__visa_buffer));
             } else {
                 dev_dbg(matrix_device, "VISA operation is NOT read!\n");
             }
        } else {
            dev_dbg(matrix_device, "VISA poll is NULL!\n");
        }

	return 0;
ERROR:
	mt_free_memory();
	return -EFAULT;
}

/**
 * transfer_data - transfers all the recorded info to user space for profiling
 * @ptr_data : gets the address of the user buffer that has to be populated
 */
static int mt_transfer_data(unsigned long ptr_data)
{

	struct xchange_buffer_all xbuf_all;
	if (!mem_alloc_status) {
		dev_dbg(matrix_device, "Memory allocation is not done..\n");
		return -EFAULT;
	}
	if ((struct xchange_buffer_all *)ptr_data == NULL) {
		dev_dbg(matrix_device,
			"Data Transfer can not be done as user buffer is NULL..\n");
		goto ERROR;
	}
	if (copy_from_user(&xbuf_all, (struct xchange_buffer_all *)
			   ptr_data, sizeof(struct xchange_buffer_all)) > 0) {
		dev_dbg(matrix_device, "Transferring data had issues..\n");
		goto ERROR;
	}

	/*Buffers for Init transferred */
	MATRIX_COPY_TO_USER_BUFFER(init, msr, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(init, mem, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(init, pci_ops, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(init, cfg_db, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(init, visa, ERROR);

	/*Buffers for Poll transferred */
	MATRIX_COPY_TO_USER_BUFFER(poll, msr, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(poll, mem, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(poll, pci_ops, ERROR);
	if (copy_to_user
	    (xbuf_all.poll_time_stamp, ptr_xc_buff_all->poll_time_stamp,
	     (sizeof(u64) * ptr_lut->records)) > 0) {
		dev_dbg(matrix_device, "Transfer_data Copy had issues..\n");
		goto ERROR;
	}
	if (copy_to_user
	    (xbuf_all.poll_tsc, ptr_xc_buff_all->poll_tsc,
	     (sizeof(u64) * ptr_lut->records)) > 0) {
		dev_dbg(matrix_device, "Transfer_tsc_data to ring3 failed..\n");
		goto ERROR;
	}
	if (0 != ptr_lut->scu_poll_length)
		if (copy_to_user
		    (ptr_lut->scu_poll.usr_data, ptr_lut->scu_poll.drv_data,
		     (ptr_lut->scu_poll_length * ptr_lut->scu_poll.length)) >
		    0) {
			dev_dbg(matrix_device,
				"Transfer_data Copy had issues..\n");
			goto ERROR;
		}
	MATRIX_COPY_TO_USER_BUFFER(poll, cfg_db, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(poll, visa, ERROR);

	/*Buffers for Term transferred */
	MATRIX_COPY_TO_USER_BUFFER(term, msr, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(term, mem, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(term, pci_ops, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(term, cfg_db, ERROR);
	MATRIX_COPY_TO_USER_BUFFER(term, visa, ERROR);

	xbuf_all.init_time_stamp = ptr_xc_buff_all->init_time_stamp;
	xbuf_all.term_time_stamp = ptr_xc_buff_all->term_time_stamp;
	xbuf_all.init_tsc = ptr_xc_buff_all->init_tsc;
	xbuf_all.term_tsc = ptr_xc_buff_all->term_tsc;
	if (copy_to_user((struct xchange_buffer_all *)
			 ptr_data, &xbuf_all,
			 sizeof(struct xchange_buffer_all)) > 0) {
		dev_dbg(matrix_device, "Transferring data had issues..\n");
		goto ERROR;
	}
	return 0;
ERROR:
	mt_free_memory();
	return -EFAULT;
}

/**
 * ioctl_mtx_msr - mtx_msr_container refers to structure designed to hold data related
 * to MSR( Model Specific Registers).
 * @ptr_data : gets the address of the user buffer that has to be populated
 */
static int IOCTL_mtx_msr(unsigned long ptr_data)
{
	struct mtx_msr_container mtx_msr_drv;
	unsigned long *buffer = NULL;
	int err = 0;

	if ((struct mtx_msr_container *)ptr_data == NULL) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	if (copy_from_user
	    (&mtx_msr_drv, (struct mtx_msr_container *)ptr_data,
	     sizeof(mtx_msr_drv)) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	if (mtx_msr_drv.length > 0) {
		MATRIX_VMALLOC(buffer,
			       sizeof(unsigned long) * mtx_msr_drv.length,
			       ERROR);
		if (copy_from_user
		    (buffer, mtx_msr_drv.buffer,
		     (sizeof(unsigned long) * mtx_msr_drv.length)) > 0) {
			dev_dbg(matrix_device,
				"file : %s ,function : %s ,line %i\n", __FILE__,
				__func__, __LINE__);
			goto ERROR;
		}
	}
	switch (mtx_msr_drv.msrType1.operation) {
	case WRITE_OP:
		err = wrmsr_on_cpu(mtx_msr_drv.msrType1.n_cpu,
				   mtx_msr_drv.msrType1.ecx_address,
				   mtx_msr_drv.msrType1.eax_LSB,
				   mtx_msr_drv.msrType1.edx_MSB);
		break;
	case READ_OP:
		err = rdmsr_on_cpu(mtx_msr_drv.msrType1.n_cpu,
				   mtx_msr_drv.msrType1.ecx_address,
				   (u32 *) & mtx_msr_drv.msrType1.eax_LSB,
				   (u32 *) & mtx_msr_drv.msrType1.edx_MSB);
		break;
	case ENABLE_OP:
		wrmsrl(mtx_msr_drv.msrType1.ecx_address,
		       (unsigned long)&buffer[0]);
		wrmsr(mtx_msr_drv.msrType1.ebx_value, 0x01, 0x00);
		vfree(buffer);
		return 0;
	default:
		dev_dbg(matrix_device,
			"There is a problem in MSR Operation..\n");
		goto ERROR;
	}
	if (err != 0)
		goto ERROR;
	if (copy_to_user
	    ((struct mtx_msr_container *)ptr_data, &mtx_msr_drv,
	     sizeof(mtx_msr_drv)) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		goto ERROR;
	}
	vfree(buffer);
	return 0;
ERROR:
	vfree(buffer);
	return -EFAULT;
}

/**
 * ioctl_sram - memory map refers to a structure designed to hold data related
 * to SRAM (Shared RAM).
 * @ptr_data : gets the address of the user buffer that has to be populated
 */
static int IOCTL_sram(unsigned long ptr_data)
{
#if DO_ANDROID
	struct memory_map mem_map_drv;
	char *buffer = NULL;
	if ((struct memory_map *)ptr_data == NULL) {
		dev_dbg(matrix_device,
			"Data Transfer can not be done as user buffer is NULL..\n");
		return -EFAULT;
	}
	if (copy_from_user
	    (&mem_map_drv,
	     (struct memory_map *)ptr_data, sizeof(mem_map_drv)) > 0) {
		dev_dbg(matrix_device, "Transferring data had issues..\n");
		return -EFAULT;
	}
	if (mem_map_drv.ctrl_addr != 0) {
		void *remap_addr = ioremap_nocache
		    (mem_map_drv.ctrl_addr, sizeof(unsigned long));
		if (remap_addr == NULL) {
			dev_dbg(matrix_device, "IOREMAP has issue..\n");
			return -ENOMEM;
		}
		writel(mem_map_drv.ctrl_data, remap_addr);
		iounmap(remap_addr);
	} else {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	MATRIX_VMALLOC(buffer, mem_map_drv.data_size, ERROR);
	mem_map_drv.data_remap_address =
	    ioremap_nocache(mem_map_drv.data_addr, mem_map_drv.data_size);
	if (mem_map_drv.data_remap_address == NULL) {
		dev_dbg(matrix_device, "IOREMAP has issue..\n");
		goto ERROR;
	}
	memcpy(buffer, mem_map_drv.data_remap_address, mem_map_drv.data_size);
	if (copy_to_user
	    (mem_map_drv.ptr_data_usr, buffer, mem_map_drv.data_size) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		iounmap(mem_map_drv.data_remap_address);
		goto ERROR;
	}
	iounmap(mem_map_drv.data_remap_address);
	vfree(buffer);
        return 0;
ERROR:
	vfree(buffer);
	return -EFAULT;
#endif // DO_ANDROID
        return 0;
}

/**
 * read_config - procedure to read the config db registers (very generic)
 * @ptr_data : gets the address of the user buffer that has to be populated
 */
static int mt_read_config(unsigned long *ptr_data)
{
#if DO_ANDROID
	unsigned long buf, data;

	if (copy_from_user(&buf, (u32 *) ptr_data, sizeof(unsigned long)) > 0) {
		dev_err(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	data = mt_platform_pci_read32(buf);
	/* Write back to the same user buffer */
	if (copy_to_user
	    ((unsigned long *)ptr_data, &data, sizeof(unsigned long)) > 0) {
		dev_err(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
#endif // DO_ANDROID
	return 0;
}

/**
 * write_config - proceduer to write the config db registers
 * @ptr_data : user buffer address that contains information like
 * mcr (port) and mdr (data) used for writing config DB registers.
 */
static inline int mt_write_config(unsigned long *ptr_data)
{
#if DO_ANDROID
	unsigned long addr, val;
	struct mtx_pci_ops pci_data;
	if (copy_from_user
	    (&pci_data,
	     (struct mtx_pci_ops *)ptr_data, sizeof(struct mtx_pci_ops)) > 0) {
		dev_err(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	addr = pci_data.port | (MCR_WRITE_OPCODE << BIT_POS_OPCODE);
	val = pci_data.data;
	mt_platform_pci_write32(addr, val);
#endif // DO_ANDROID
	return 0;
}

/**
 * read_pci_config - procedure to read the pci configuration space
 * @ptr_data : gets the pci configuration info like bus, device,
 * function and the offset in the config space. Also, returns
 * the read data in "data" field of the structure
 */
static inline int mt_read_pci_config(unsigned long *ptr_data)
{
	int ret = 0;
#if DO_ANDROID
	struct pci_config pci_config_data;
	struct pci_dev *pdev = NULL;
	if (copy_from_user
	    (&pci_config_data,
	     (struct pci_config *)ptr_data, sizeof(struct pci_config)) > 0) {
		dev_err(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	pdev = pci_get_bus_and_slot(pci_config_data.bus,
			PCI_DEVFN(pci_config_data.device,
				pci_config_data.function));
	if (!pdev) {
		ret = -EINVAL;
		goto exit;
	}
	ret = pci_read_config_dword(pdev, pci_config_data.offset,
			(u32 *)&pci_config_data.data);
	/* Write back to the same user buffer */
	if (copy_to_user
	    ((unsigned long *)ptr_data, &pci_config_data,
		 sizeof(struct pci_config)) > 0) {
		dev_err(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
#endif // DO_ANDROID
exit:
	return ret;
}

/**
 * read_gmch_gen_pur_regs -  use this function to retrieve the complete set of
 * general purpose gmch registers
 * @data : gets the address of the user buffer that has to be populated
 * @read_mask : read_mask applies mask corresponding to the platform
 */
static void mt_read_gmch_gen_pur_regs(unsigned long *data, unsigned long *clks,
				      unsigned long read_mask)
{
#if DO_ANDROID
	if (data && clks) {
		data[0] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR0_L | read_mask);
		data[1] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR0_H | read_mask);
		data[2] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR1_L | read_mask);
		data[3] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR1_H | read_mask);
		data[4] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR2_L | read_mask);
		data[5] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR2_H | read_mask);
		data[6] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR3_L | read_mask);
		data[7] = mt_platform_pci_read32(MTX_GMCH_PMON_GP_CTR3_H | read_mask);
		clks[0] = mt_platform_pci_read32(MTX_GMCH_PMON_FIXED_CTR0 | read_mask);
	}
#endif // DO_ANDROID
}

/**
 * gmch_gen_pur_regs_trigger_enable -  use this function to trigger global flag
 * that enables/disables gmch counters.
 * @enable : enable is boolean.
 * @write_mask : write_mask applies mask corresponding to the platform
 */
static void mt_gmch_gen_pur_regs_trigger_enable(bool enable,
						unsigned long write_mask)
{
#if DO_ANDROID
	if (enable)
		mt_platform_pci_write32((MTX_GMCH_PMON_GLOBAL_CTRL |
					 write_mask),
					MTX_GMCH_PMON_GLOBAL_CTRL_ENABLE);
	else
		mt_platform_pci_write32((MTX_GMCH_PMON_GLOBAL_CTRL |
					 write_mask),
					MTX_GMCH_PMON_GLOBAL_CTRL_DISABLE);
#endif // DO_ANDROID
}

/**
 * write_mt_gmch_gen_pur_regs -  use this function to write to the complete set of
 * general purpose gmch registers
 * @write_mask : write_mask applies mask corresponding to the platform
 */
static void mt_write_gmch_gen_pur_regs(unsigned long data,
				       unsigned long write_mask)
{
#if DO_ANDROID
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR0_L | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR0_H | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR1_L | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR1_H | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR2_L | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR2_H | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR3_L | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_GP_CTR3_H | write_mask), data);
	mt_platform_pci_write32((MTX_GMCH_PMON_FIXED_CTR_CTRL | write_mask),
				data);
	mt_platform_pci_write32((MTX_GMCH_PMON_FIXED_CTR_CTRL | write_mask),
				DATA_ENABLE);
#endif // DO_ANDROID
}

/**
 * mt_reset_gmch_gen_pur_regs -  use this function to reset all of the gmch
 * peformance counters
 * @event : event points to the first of the event passed in from the
 * application space.
 * @mcr1 : config register 1 for perf event selection
 * @mcr2 : config register 2 for perf event selection
 * @mcr3 : config register 3 if ,for perf event selection depends on platform
 */
static void mt_reset_gmch_gen_pur_regs(unsigned long
				       *event, unsigned long
				       *mcr1, unsigned long
				       *mcr2, unsigned long
				       *mcr3, unsigned long write_mask)
{
	unsigned long count = 0;
#if DO_ANDROID
	if (event == NULL || mcr1 == NULL || mcr2 == NULL || mcr3 == NULL)
		return;

	/*disable  gmch general purpose counter */
	mt_gmch_gen_pur_regs_trigger_enable(false, write_mask);

	/*re-initialize gmch general purpose counter */
	mt_write_gmch_gen_pur_regs(0x00000000, write_mask);

	/*trigger performance counters */
	for (count = 0; count < 4; count++) {
		if (mcr1[count])
			mt_platform_pci_write32(mcr1[count], event[count]);
		if (mcr2[count])
			mt_platform_pci_write32(mcr2[count], event[count]);
		if (mcr3[count])
			mt_platform_pci_write32(mcr3[count], event[count]);
	}

	/*enable gmch general purpose counter */
	mt_gmch_gen_pur_regs_trigger_enable(true, write_mask);
#endif // DO_ANDROID
}

/**
 * ioctl_gmch - gmch_container refers to a struct designed to hold data related
 * to GMCH( The Greater Memeory Controller Hub, giving access to all Emons
 * @ptr_data : gets the address of the user buffer that has to be populated
 */
static int IOCTL_gmch(unsigned long ioctl_request, unsigned long ptr_data)
{
#if DO_ANDROID
	struct gmch_container gmch_drv;
	if ((struct gmch_container *)ptr_data == NULL) {
		dev_dbg(matrix_device,
			"Data Transfer can not be done as user buffer is NULL..\n");
		return -EFAULT;
	}
	if (copy_from_user
	    (&gmch_drv,
	     (struct gmch_container *)ptr_data,
	     sizeof(struct gmch_container)) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}

	/* read gmch counters */
	mt_read_gmch_gen_pur_regs(gmch_drv.data, &gmch_drv.core_clks,
				  gmch_drv.read_mask);
	MATRIX_GET_TIME_STAMP(gmch_drv.time_stamp);

	/* reset gmch counters */
	if (ioctl_request == IOCTL_GMCH_RESET) {
		mt_reset_gmch_gen_pur_regs(gmch_drv.event,
					   gmch_drv.mcr1,
					   gmch_drv.mcr2,
					   gmch_drv.mcr3, gmch_drv.write_mask);
	}
	if (copy_to_user
	    ((struct gmch_container *)ptr_data,
	     &gmch_drv, sizeof(struct gmch_container)) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
#endif // DO_ANDROID
	return 0;
}

/*
 * The following function reads/writes to an MSR with
 * inputs given by the user. The two primary use cases of
 * this  function are: a) Request to read IA32_PERF_STATUS MSR from ring3.
 * b) Debugging from user space. There could be other users of this in the
 * future.
 */
static int mt_operate_on_msr(unsigned long ptr_data)
{
	struct mtx_msr data_msr;
	if (copy_from_user
	    (&data_msr, (struct mtx_msr *)ptr_data,
	     sizeof(struct mtx_msr)) > 0) {
		dev_dbg(matrix_device, "file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	if (data_msr.operation == READ_OP)
		rdmsr(data_msr.ecx_address, data_msr.eax_LSB, data_msr.edx_MSB);
	else if (data_msr.operation == WRITE_OP)
		wrmsr(data_msr.ecx_address, data_msr.eax_LSB, data_msr.edx_MSB);
	else
		return -EFAULT;
	if (copy_to_user((struct mtx_msr *)ptr_data, &data_msr,
			 sizeof(struct mtx_msr)) > 0) {
		dev_dbg(matrix_device,
			"file : %s ,function : %s ,line %i\n",
			__FILE__, __func__, __LINE__);
		return -EFAULT;
	}
	return 0;
}

static long matrix_ioctl(struct file
			 *filp, unsigned int request, unsigned long ptr_data)
{
    // printk(KERN_INFO "Received MATRIX IOCTL: %lu\n", request);
    switch (request) {
        case IOCTL_VERSION_INFO:
            // printk(KERN_INFO "IOCTL_VERSION_INFO received!\n");
            if (copy_to_user((char *)ptr_data, PW_DRV_VERSION_STRING,
                        strlen(PW_DRV_VERSION_STRING) + 1) > 0) {
                dev_dbg(matrix_device,
                        "file : %s ,function : %s ,line %i\n",
                        __FILE__, __func__, __LINE__);
                return -EFAULT;
            }
            break;
        case IOCTL_INIT_MEMORY:
            //printk(KERN_INFO "IOCTL_INIT_MEMORY received!\n");
            return mt_initialize_memory(ptr_data);
        case IOCTL_FREE_MEMORY:
            //printk(KERN_INFO "IOCTL_FREE_MEMORY received!\n");
            return mt_free_memory();
        case IOCTL_OPERATE_ON_MSR:
            // printk(KERN_INFO "IOCTL_OPERATE_ON_MSR received!\n");
            return mt_operate_on_msr(ptr_data);
        case IOCTL_INIT_SCAN:
            //printk(KERN_INFO "IOCTL_INIT_SCAN received!\n");
            return mt_data_scan(request);
        case IOCTL_TERM_SCAN:
            //printk(KERN_INFO "IOCTL_TERM_SCAN received!\n");
            return mt_data_scan(request);
        case IOCTL_POLL_SCAN:
            //printk(KERN_INFO "IOCTL_POLL_SCAN received!\n");
            return mt_poll_scan(ptr_data);
        case IOCTL_COPY_TO_USER:
            //printk(KERN_INFO "IOCTL_COPY_TO_USER received!\n");
            return mt_transfer_data(ptr_data);
            /* MSR based ioctl's */
        case IOCTL_MSR:
            // printk(KERN_INFO "IOCTL_MSR received!\n");
            return IOCTL_mtx_msr(ptr_data);
            /* SRAM based ioctl's */
        case IOCTL_SRAM:
            // printk(KERN_INFO "IOCTL_SRAM received!\n");
            return IOCTL_sram(ptr_data);
            // return -1;
            /* GMCH based ioctl's */
        case IOCTL_GMCH:
            // printk(KERN_INFO "IOCTL_GMCH received!\n");
            return IOCTL_gmch(request, ptr_data);
        case IOCTL_GMCH_RESET:
            // printk(KERN_INFO "IOCTL_GMCH_REQUEST received!\n");
            return IOCTL_gmch(request, ptr_data);
        case IOCTL_READ_CONFIG_DB:
            // printk(KERN_INFO "IOCTL_READ_CONFIG_DB received!\n");
            return mt_read_config((unsigned long *)ptr_data);
            // return -1;
        case IOCTL_WRITE_CONFIG_DB:
            // printk(KERN_INFO "IOCTL_WRITE_CONFIG_DB received!\n");
            return mt_write_config((unsigned long *)ptr_data);
            // return -1;
        case IOCTL_READ_PCI_CONFIG:
            return mt_read_pci_config((unsigned long *)ptr_data);
        default:
            // printk(KERN_INFO "INVALID IOCTL = %lu received!\n", request);
            dev_dbg(matrix_device,
                    "file : %s ,function : %s ,line %i\n", __FILE__,
                    __func__, __LINE__);
            return -EFAULT;
    }
    return 0;
}

static int matrix_release(struct inode *in, struct file *filp)
{
	if (instantiated) {
		mt_free_memory();
		instantiated = false;
	}
	module_put(THIS_MODULE);
	return 0;
}

static const struct file_operations matrix_fops = {
	.owner = THIS_MODULE,
	.open = matrix_open,
	.unlocked_ioctl = matrix_ioctl,
	.release = matrix_release
};

int mt_register_dev(void)
{
	int error;
	error = alloc_chrdev_region(&matrix_dev, 0, 1, NAME);
	if (error < 0) {
		pr_err("Matrix : Could not allocate char dev region");
		// return 1;
		return error;
	}
	matrix_major_number = MAJOR(matrix_dev);
	matrix_class = class_create(THIS_MODULE, NAME);
	if (IS_ERR(matrix_class)) {
		pr_err("Matrix :Error registering class\n");
		// return 1;
		return -MT_ERROR;
	}
	device_create(matrix_class, NULL, matrix_dev, NULL, NAME);

	/*Device Registration */
	matrix_cdev = cdev_alloc();
	if (!matrix_cdev) {
		pr_err("Matrix :Could not create device");
		return -ENOMEM;
	}
	matrix_cdev->owner = THIS_MODULE;
	matrix_cdev->ops = &matrix_fops;
	matrix_device = (struct device *)matrix_cdev->dev;
	if (cdev_add(matrix_cdev, matrix_dev, 1) < 0) {
		pr_err("Error registering device driver\n");
		// return error;
		return -MT_ERROR;
	}
	pr_info("Matrix Registered Successfully with major no.[%d]\n",
		matrix_major_number);
	return MT_SUCCESS;
}

void mt_unregister_dev(void)
{
	pr_info("Matrix De-Registered Successfully...\n");
	unregister_chrdev(matrix_major_number, NAME);
	device_destroy(matrix_class, matrix_dev);
	class_destroy(matrix_class);
	unregister_chrdev_region(matrix_dev, 1);
	cdev_del(matrix_cdev);
};