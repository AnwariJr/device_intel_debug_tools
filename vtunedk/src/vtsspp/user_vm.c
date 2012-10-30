/*
  Copyright (C) 2010-2012 Intel Corporation.  All Rights Reserved.

  This file is part of SEP Development Kit

  SEP Development Kit is free software; you can redistribute it
  and/or modify it under the terms of the GNU General Public License
  version 2 as published by the Free Software Foundation.

  SEP Development Kit is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with SEP Development Kit; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

  As a special exception, you may use this file as part of a free software
  library without restriction.  Specifically, if other files instantiate
  templates or use macros or inline functions from this file, or you compile
  this file and link it with other files to produce an executable, this
  file does not by itself cause the resulting executable to be covered by
  the GNU General Public License.  This exception does not however
  invalidate any other reasons why the executable file might be covered by
  the GNU General Public License.
*/
#include "vtss_config.h"
#include "user_vm.h"

#include <linux/slab.h>
#include <linux/vmstat.h>
#include <linux/highmem.h>      /* for kmap()/kunmap() */
#include <linux/pagemap.h>      /* for page_cache_release() */
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <asm/fixmap.h>         /* VSYSCALL_START */
#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/uaccess.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)

typedef int (gup_huge_pmd_t) (pmd_t pmd, unsigned long addr, unsigned long end, int write, struct page **pages, int *nr);
static gup_huge_pmd_t* gup_huge_pmd = NULL;

typedef int (gup_huge_pud_t) (pud_t pud, unsigned long addr, unsigned long end, int write, struct page **pages, int *nr);
static gup_huge_pud_t* gup_huge_pud = NULL;

typedef int (gup_pte_range_t) (pmd_t pmd, unsigned long addr, unsigned long end, int write, struct page **pages, int *nr);
static gup_pte_range_t* gup_pte_range = NULL;

static struct kprobe _kp_gup_huge_pmd = {
    .pre_handler = NULL,
    .post_handler = NULL,
    .fault_handler = NULL,
#ifdef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
    .symbol_name = "gup_huge_pmd",
#endif
    .addr = (kprobe_opcode_t*)NULL
};

static struct kprobe _kp_gup_huge_pud = {
    .pre_handler = NULL,
    .post_handler = NULL,
    .fault_handler = NULL,
#ifdef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
    .symbol_name = "gup_huge_pud",
#endif
    .addr = (kprobe_opcode_t*)NULL
};

static struct kprobe _kp_gup_pte_range = {
    .pre_handler = NULL,
    .post_handler = NULL,
    .fault_handler = NULL,
#ifdef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
    .symbol_name = "gup_pte_range",
#endif
    .addr = (kprobe_opcode_t*)NULL
};

static int gup_pmd_range(pud_t pud, unsigned long addr, unsigned long end, int write, struct page **pages, int *nr)
{
    unsigned long next;
    pmd_t *pmdp;

    pmdp = pmd_offset(&pud, addr);
    do {
        pmd_t pmd = *pmdp;

        next = pmd_addr_end(addr, end);
        if (pmd_none(pmd))
            return 0;
        if (unlikely(pmd_large(pmd))) {
            if (!gup_huge_pmd(pmd, addr, next, write, pages, nr))
                return 0;
        } else {
            if (!gup_pte_range(pmd, addr, next, write, pages, nr))
                return 0;
        }
    } while (pmdp++, addr = next, addr != end);

    return 1;
}

static int gup_pud_range(pgd_t pgd, unsigned long addr, unsigned long end, int write, struct page **pages, int *nr)
{
    unsigned long next;
    pud_t *pudp;

    pudp = pud_offset(&pgd, addr);
    do {
        pud_t pud = *pudp;

        next = pud_addr_end(addr, end);
        if (pud_none(pud))
            return 0;
        if (unlikely(pud_large(pud))) {
            if (!gup_huge_pud(pud, addr, next, write, pages, nr))
                return 0;
        } else {
            if (!gup_pmd_range(pud, addr, next, write, pages, nr))
                return 0;
        }
    } while (pudp++, addr = next, addr != end);

    return 1;
}

int vtss_get_user_pages_fast(unsigned long start, int nr_pages, int write, struct page **pages)
{
    struct mm_struct *mm = current->mm;
    unsigned long addr, len, end;
    unsigned long next;
    unsigned long flags;
    pgd_t *pgdp;
    int nr = 0;

    start &= PAGE_MASK;
    addr = start;
    len = (unsigned long) nr_pages << PAGE_SHIFT;
    end = start + len;
    if (!access_ok(write ? VERIFY_WRITE : VERIFY_READ, (void __user *)start, len))
        return 0;

    local_irq_save(flags);
    pgdp = pgd_offset(mm, addr);
    do {
        pgd_t pgd = *pgdp;

        next = pgd_addr_end(addr, end);
        if (pgd_none(pgd))
            break;
        if (!gup_pud_range(pgd, addr, next, write, pages, &nr))
            break;
    } while (pgdp++, addr = next, addr != end);
    local_irq_restore(flags);

    return nr;
}

#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)

typedef int (vtss_get_user_pages_fast_t) (unsigned long start, int nr_pages, int write, struct page **pages);
static vtss_get_user_pages_fast_t* vtss_get_user_pages_fast = NULL;

static struct kprobe _kp_dummy = {
    .pre_handler = NULL,
    .post_handler = NULL,
    .fault_handler = NULL,
#ifdef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
    .symbol_name = "__get_user_pages_fast",
#endif
    .addr = (kprobe_opcode_t*)NULL
};

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38) */
#define vtss_get_user_pages_fast __get_user_pages_fast
#endif

#ifndef VTSS_AUTOCONF_KMAP_ATOMIC_ONE_ARG
#ifndef KM_NMI
#define KM_NMI KM_IRQ0
#endif

#ifndef in_nmi
static inline int in_nmi(void) __attribute__ ((always_inline));
static inline int in_nmi(void)
{
    return 0;
}
#endif /* in_nmi */
#endif /* VTSS_AUTOCONF_KMAP_ATOMIC_ONE_ARG */

static int vtss_user_vm_page_pin(struct user_vm_accessor* this, unsigned long addr)
{
    int rc;

    if (this->m_irq) {
        rc = vtss_get_user_pages_fast(addr, 1, 0, &this->m_page);
        if (rc != 1) {
            this->m_page  = NULL;
            this->m_maddr = NULL;
            this->m_page_id = (unsigned long)-1;
            return 1;
        }
#ifdef VTSS_AUTOCONF_KMAP_ATOMIC_ONE_ARG
        this->m_maddr = kmap_atomic(this->m_page);
#else
        this->m_maddr = kmap_atomic(this->m_page, in_nmi() ? KM_NMI : KM_IRQ0);
#endif
    } else {
        rc = get_user_pages(this->m_task, this->m_mm, addr, 1, 0, 1, &this->m_page, &this->m_vma);
        if (rc != 1) {
            this->m_page  = NULL;
            this->m_maddr = NULL;
            this->m_page_id = (unsigned long)-1;
            return 1;
        }
        this->m_maddr = kmap(this->m_page);
    }
    this->m_page_id = (addr & PAGE_MASK) >> PAGE_SHIFT;
    rc = (this->m_maddr != NULL) ? 0 : 2;
#ifdef VTSS_VMA_CACHE
    if (!rc) {
        if (this->m_irq) {
            mm_segment_t old_fs = get_fs();
            set_fs(KERNEL_DS);
            pagefault_disable();
            VTSS_PROFILE(cpy, rc = __copy_from_user_inatomic(this->m_buffer, this->m_maddr, PAGE_SIZE));
            pagefault_enable();
            set_fs(old_fs);
        } else {
            VTSS_PROFILE(cpy, copy_from_user_page(this->m_vma, this->m_page, addr & PAGE_MASK, this->m_buffer, this->m_maddr, PAGE_SIZE));
        }
    } else {
        memset(this->m_buffer, 0, PAGE_SIZE);
    }
#endif
    return rc;
}

static int vtss_user_vm_page_unpin(struct user_vm_accessor* this)
{
    if (this->m_irq) {
        if (this->m_maddr != NULL)
#ifdef VTSS_AUTOCONF_KMAP_ATOMIC_ONE_ARG
            kunmap_atomic(this->m_maddr);
#else
            kunmap_atomic(this->m_maddr, in_nmi() ? KM_NMI : KM_IRQ0);
#endif
        this->m_maddr = NULL;
        if (this->m_page != NULL)
            put_page(this->m_page);
        this->m_page = NULL;
    } else {
        if (this->m_maddr != NULL)
            kunmap(this->m_maddr);
        this->m_maddr = NULL;
        if (this->m_page != NULL)
            page_cache_release(this->m_page); /* put_page(this->m_page); */
        this->m_page = NULL;
    }
    this->m_page_id = (unsigned long)-1;
    return 0;
}

static int vtss_user_vm_trylock(struct user_vm_accessor* this, struct task_struct* task)
{
    this->m_task = NULL;
    this->m_mm   = NULL;

    if (task == NULL || task->mm == NULL)
        return 1;

    if (!this->m_irq) {
        struct mm_struct* mm = get_task_mm(task);
        if (!down_read_trylock(&mm->mmap_sem)) {
            mmput(mm);
            return 2;
        }
        this->m_mm = mm;
    } else {
        this->m_mm = task->mm;
    }
    this->m_task = task;
    return 0;
}

static int vtss_user_vm_unlock(struct user_vm_accessor* this)
{
    vtss_user_vm_page_unpin(this);
    if (this->m_mm != NULL) {
        if (!this->m_irq) {
            up_read(&this->m_mm->mmap_sem);
            mmput(this->m_mm);
        }
        this->m_mm = NULL;
    }
    this->m_task = NULL;
    return 0;
}

static size_t vtss_user_vm_read(struct user_vm_accessor* this, void* from, void* to, size_t size)
{
    size_t i, cpsize, bytes = 0;
    unsigned long offset, addr = (unsigned long)from;
#ifdef DEBUG_PROFILE
    cycles_t start_vma_time = get_cycles();
#endif
#ifndef VTSS_VMA_CACHE
    mm_segment_t old_fs = get_fs();

    if (this->m_irq) {
        set_fs(KERNEL_DS);
        pagefault_disable();
    }
#endif

    do {
        unsigned long page_id = (addr & PAGE_MASK) >> PAGE_SHIFT;

        offset = addr & (PAGE_SIZE - 1);
        cpsize = min((size_t)(PAGE_SIZE - offset), size - bytes);
        TRACE("addr=0x%p(0x%lx) size=%zu (page=0x%lx, offset=0x%lx)", from, addr, cpsize, page_id, offset);
        if (!access_ok(VERIFY_READ, addr, cpsize))
            break; /* Don't have a read access */
        if (page_id != this->m_page_id) {
#ifdef DEBUG_PROFILE
            cycles_t start_pgp_time = get_cycles();
#endif
            TRACE("not in cache 0x%lx", this->m_page_id);
            vtss_user_vm_page_unpin(this);
            if (vtss_user_vm_page_pin(this, addr)) {
                vtss_user_vm_page_unpin(this);
#ifdef DEBUG_PROFILE
                vtss_profile_cnt_pgp++;
                vtss_profile_clk_pgp += get_cycles() - start_pgp_time;
#endif
                TRACE("page lock FAIL");
                break; /* Cannot get a page for an access */
            }
#ifdef DEBUG_PROFILE
            vtss_profile_cnt_pgp++;
            vtss_profile_clk_pgp += get_cycles() - start_pgp_time;
#endif
        }
#ifdef VTSS_VMA_CACHE
        memcpy(to, this->m_buffer + offset, cpsize);
#else
        if (this->m_irq) {
            long rc = 0;
            VTSS_PROFILE(cpy, rc = __copy_from_user_inatomic(to, this->m_maddr + offset, cpsize));
            if (rc)
                break;
        } else {
            VTSS_PROFILE(cpy, copy_from_user_page(this->m_vma, this->m_page, addr, to, this->m_maddr + offset, cpsize));
        }
#endif
#ifdef DEBUG_VMA
        printk("vtss_user_vm_read: [");
        for (i = 0; i < cpsize; i++) {
            printk("%02x", ((unsigned char*)to)[i]);
        }
        printk("]\n");
#endif
        bytes += cpsize;
        to    += cpsize;
        addr  += cpsize;
    } while (bytes < size);

#ifndef VTSS_VMA_CACHE
    if (this->m_irq) {
        pagefault_enable();
        set_fs(old_fs);
    }
#endif
#ifdef DEBUG_PROFILE
    vtss_profile_cnt_vma++;
    vtss_profile_clk_vma += get_cycles() - start_vma_time;
#endif
    return bytes;
}

static int vtss_user_vm_validate(struct user_vm_accessor* this, unsigned long ip)
{
#ifdef CONFIG_X86_64
    unsigned long kaddr = (unsigned long)__START_KERNEL_map;
#else
    unsigned long kaddr = (unsigned long)PAGE_OFFSET;
#endif
    kaddr += (CONFIG_PHYSICAL_START + (CONFIG_PHYSICAL_ALIGN - 1)) & ~(CONFIG_PHYSICAL_ALIGN - 1);

#ifdef CONFIG_X86_64
    if ((ip >= VSYSCALL_START) && (ip < VSYSCALL_END))
        return 1; /* [vsyscall] */
    else
#endif
    if (ip < kaddr) {
        struct vm_area_struct* vma = this->m_mm ? find_vma(this->m_mm, ip) : NULL;
        return ((vma != NULL) && (vma->vm_flags & VM_EXEC) && (ip >= vma->vm_start) && (ip < vma->vm_end)) ? 1 : 0;
    } else
        return (ip < PAGE_OFFSET) ? 1 : 0; /* in kernel? */
}

user_vm_accessor_t* vtss_user_vm_accessor_init(int in_irq)
{
    user_vm_accessor_t* acc;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
    if (gup_huge_pmd == NULL || gup_huge_pud == NULL || gup_pte_range == NULL)
        return NULL;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
    if (vtss_get_user_pages_fast == NULL)
        return NULL;
#endif
    acc = (user_vm_accessor_t*)kmalloc(sizeof(user_vm_accessor_t), (in_irq ? GFP_ATOMIC : GFP_KERNEL) | __GFP_ZERO);
    if (acc != NULL) {
        acc->m_page_id = (unsigned long)-1;
        acc->m_irq     = in_irq;
        acc->trylock   = vtss_user_vm_trylock;
        acc->unlock    = vtss_user_vm_unlock;
        acc->read      = vtss_user_vm_read;
        acc->validate  = vtss_user_vm_validate;
    } else {
        ERROR("No memory for accessor");
    }
    return acc;
}

void vtss_user_vm_accessor_fini(user_vm_accessor_t* acc)
{
    if (acc != NULL) {
        acc->unlock(acc);
        kfree(acc);
    }
}

int vtss_user_vm_init(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
    if (gup_huge_pmd == NULL || gup_huge_pud == NULL || gup_pte_range == NULL) {
#ifndef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
        gup_huge_pmd  =  (gup_huge_pmd_t*)kallsyms_lookup_name("gup_huge_pmd");
        gup_huge_pud  =  (gup_huge_pud_t*)kallsyms_lookup_name("gup_huge_pud");
        gup_pte_range = (gup_pte_range_t*)kallsyms_lookup_name("gup_pte_range");
#else  /* VTSS_AUTOCONF_KPROBE_SYMBOL_NAME */
        if (!register_kprobe(&_kp_gup_huge_pmd)) {
            gup_huge_pmd = (gup_huge_pmd_t*)_kp_gup_huge_pmd.addr;
            TRACE("gup_huge_pmd=0x%p", gup_huge_pmd);
            unregister_kprobe(&_kp_gup_huge_pmd);
        }
        if (!register_kprobe(&_kp_gup_huge_pud)) {
            gup_huge_pud = (gup_huge_pud_t*)_kp_gup_huge_pud.addr;
            TRACE("gup_huge_pud=0x%p", gup_huge_pud);
            unregister_kprobe(&_kp_gup_huge_pud);
        }
        if (!register_kprobe(&_kp_gup_pte_range)) {
            gup_pte_range = (gup_pte_range_t*)_kp_gup_pte_range.addr;
            TRACE("gup_pte_range=0x%p", gup_pte_range);
            unregister_kprobe(&_kp_gup_pte_range);
        }
#endif /* VTSS_AUTOCONF_KPROBE_SYMBOL_NAME */
        if (gup_huge_pmd == NULL) {
            ERROR("Cannot find 'gup_huge_pmd' symbol");
        }
        if (gup_huge_pud == NULL) {
            ERROR("Cannot find 'gup_huge_pud' symbol");
        }
        if (gup_pte_range == NULL) {
            ERROR("Cannot find 'gup_pte_range' symbol");
        }
        if (gup_huge_pmd == NULL || gup_huge_pud == NULL || gup_pte_range == NULL) {
            return -1;
        }
    }
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
    if (vtss_get_user_pages_fast == NULL) {
#ifndef VTSS_AUTOCONF_KPROBE_SYMBOL_NAME
        vtss_get_user_pages_fast = (vtss_get_user_pages_fast_t*)kallsyms_lookup_name("__get_user_pages_fast");
#else  /* VTSS_AUTOCONF_KPROBE_SYMBOL_NAME */
        if (!register_kprobe(&_kp_dummy)) {
            vtss_get_user_pages_fast = (vtss_get_user_pages_fast_t*)_kp_dummy.addr;
            TRACE("__get_user_pages_fast=0x%p", vtss_get_user_pages_fast);
            unregister_kprobe(&_kp_dummy);
        }
#endif /* VTSS_AUTOCONF_KPROBE_SYMBOL_NAME */
        if (vtss_get_user_pages_fast == NULL) {
            ERROR("Cannot find '__get_user_pages_fast' symbol");
            return -1;
        }
    }
#endif
    return 0;
}

void vtss_user_vm_fini(void)
{
}
