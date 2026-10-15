/*
 *  PowerPC MMU, TLB and BAT emulation helpers for QEMU.
 *
 *  Copyright (c) 2003-2007 Jocelyn Mayer
 *  Copyright (c) 2013 David Gibson, IBM Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "system/kvm.h"
#include "kvm_ppc.h"
#include "internal.h"
#include "mmu-hash32.h"
#include "mmu-books.h"
#include "exec/log.h"
#include "exec/tcg-mmu-stats.h"

/* #define DEBUG_BATS */

#ifdef DEBUG_BATS
#  define LOG_BATS(...) qemu_log_mask(CPU_LOG_MMU, __VA_ARGS__)
#else
#  define LOG_BATS(...) do { } while (0)
#endif

static target_ulong hash32_bat_size(int mmu_idx,
                                    target_ulong batu, target_ulong batl)
{
    if ((mmuidx_pr(mmu_idx) && !(batu & BATU32_VP))
        || (!mmuidx_pr(mmu_idx) && !(batu & BATU32_VS))) {
        return 0;
    }

    return BATU32_BEPI & ~((batu & BATU32_BL) << 15);
}

static hwaddr ppc_hash32_bat_lookup(PowerPCCPU *cpu, target_ulong ea,
                                    MMUAccessType access_type, int *prot,
                                    int mmu_idx)
{
    CPUPPCState *env = &cpu->env;
    target_ulong *BATlt, *BATut;
    bool ifetch = access_type == MMU_INST_FETCH;
    int i;

    LOG_BATS("%s: %cBAT v " TARGET_FMT_lx "\n", __func__,
             ifetch ? 'I' : 'D', ea);
    if (ifetch) {
        BATlt = env->IBAT[1];
        BATut = env->IBAT[0];
    } else {
        BATlt = env->DBAT[1];
        BATut = env->DBAT[0];
    }
    for (i = 0; i < env->nb_BATs; i++) {
        target_ulong batu = BATut[i];
        target_ulong batl = BATlt[i];
        target_ulong mask;

        mask = hash32_bat_size(mmu_idx, batu, batl);
        LOG_BATS("%s: %cBAT%d v " TARGET_FMT_lx " BATu " TARGET_FMT_lx
                 " BATl " TARGET_FMT_lx "\n", __func__,
                 ifetch ? 'I' : 'D', i, ea, batu, batl);

        if (mask && ((ea & mask) == (batu & BATU32_BEPI))) {
            hwaddr raddr = (batl & mask) | (ea & ~mask);

            *prot = ppc_hash32_bat_prot(batu, batl);

            return raddr & TARGET_PAGE_MASK;
        }
    }

    /* No hit */
#if defined(DEBUG_BATS)
    if (qemu_log_enabled()) {
        target_ulong *BATu, *BATl;
        target_ulong BEPIl, BEPIu, bl;

        LOG_BATS("no BAT match for " TARGET_FMT_lx ":\n", ea);
        for (i = 0; i < 4; i++) {
            BATu = &BATut[i];
            BATl = &BATlt[i];
            BEPIu = *BATu & BATU32_BEPIU;
            BEPIl = *BATu & BATU32_BEPIL;
            bl = (*BATu & 0x00001FFC) << 15;
            LOG_BATS("%s: %cBAT%d v " TARGET_FMT_lx " BATu " TARGET_FMT_lx
                     " BATl " TARGET_FMT_lx "\n\t" TARGET_FMT_lx " "
                     TARGET_FMT_lx " " TARGET_FMT_lx "\n",
                     __func__, ifetch ? 'I' : 'D', i, ea,
                     *BATu, *BATl, BEPIu, BEPIl, bl);
        }
    }
#endif

    return -1;
}

static bool ppc_hash32_direct_store(PowerPCCPU *cpu, target_ulong sr,
                                    target_ulong eaddr,
                                    MMUAccessType access_type,
                                    hwaddr *raddr, int *prot, int mmu_idx,
                                    bool guest_visible)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;

    qemu_log_mask(CPU_LOG_MMU, "direct store...\n");

    if (access_type == MMU_INST_FETCH) {
        /* No code fetch is allowed in direct-store areas */
        if (guest_visible) {
            cs->exception_index = POWERPC_EXCP_ISI;
            env->error_code = 0x10000000;
        }
        return false;
    }

    /*
     * From ppc_cpu_get_phys_addr_debug, env->access_type is not set.
     * Assume ACCESS_INT for that case.
     */
    switch (guest_visible ? env->access_type : ACCESS_INT) {
    case ACCESS_INT:
        /* Integer load/store : only access allowed */
        break;
    case ACCESS_FLOAT:
        /* Floating point load/store */
        cs->exception_index = POWERPC_EXCP_ALIGN;
        env->error_code = POWERPC_EXCP_ALIGN_FP;
        env->spr[SPR_DAR] = eaddr;
        return false;
    case ACCESS_RES:
        /* lwarx, ldarx or srwcx. */
        env->error_code = 0;
        env->spr[SPR_DAR] = eaddr;
        if (access_type == MMU_DATA_STORE) {
            env->spr[SPR_DSISR] = 0x06000000;
        } else {
            env->spr[SPR_DSISR] = 0x04000000;
        }
        return false;
    case ACCESS_CACHE:
        /*
         * dcba, dcbt, dcbtst, dcbf, dcbi, dcbst, dcbz, or icbi
         *
         * Should make the instruction do no-op.  As it already do
         * no-op, it's quite easy :-)
         */
        *raddr = eaddr;
        return true;
    case ACCESS_EXT:
        /* eciwx or ecowx */
        cs->exception_index = POWERPC_EXCP_DSI;
        env->error_code = 0;
        env->spr[SPR_DAR] = eaddr;
        if (access_type == MMU_DATA_STORE) {
            env->spr[SPR_DSISR] = 0x06100000;
        } else {
            env->spr[SPR_DSISR] = 0x04100000;
        }
        return false;
    default:
        cpu_abort(cs, "ERROR: insn should not need address translation\n");
    }

    if (ppc_hash32_key(mmuidx_pr(mmu_idx), sr)) {
        *prot = PAGE_READ | PAGE_WRITE;
    } else {
        *prot = PAGE_READ;
    }
    if (check_prot_access_type(*prot, access_type)) {
        *raddr = eaddr;
        return true;
    }

    if (guest_visible) {
        cs->exception_index = POWERPC_EXCP_DSI;
        env->error_code = 0;
        env->spr[SPR_DAR] = eaddr;
        if (access_type == MMU_DATA_STORE) {
            env->spr[SPR_DSISR] = 0x0a000000;
        } else {
            env->spr[SPR_DSISR] = 0x08000000;
        }
    }
    return false;
}

static target_ulong ppc_hash32_load_hpte0(PowerPCCPU *cpu, hwaddr pte_offset)
{
    target_ulong base = ppc_hash32_hpt_base(cpu);

    return ldl_phys(CPU(cpu)->as, base + pte_offset);
}

static target_ulong ppc_hash32_load_hpte1(PowerPCCPU *cpu, hwaddr pte_offset)
{
    target_ulong base = ppc_hash32_hpt_base(cpu);

    return ldl_phys(CPU(cpu)->as, base + pte_offset + HASH_PTE_SIZE_32 / 2);
}

/*
 * Map a whole PTEG -- 8 PTEs, 64 bytes -- with one address_space_map(),
 * the way ppc_hash64_map_hptes() does, so a search reads the PTEs straight
 * out of the hash table's RAM block instead of paying a flatview lookup per
 * ldl_phys().  Returns NULL if the table is not mapped contiguously (it can
 * sit anywhere in the guest's address space, MMIO included), in which case
 * the caller falls back to the per-PTE loads.
 */
static const uint32_t *ppc_hash32_map_pteg(PowerPCCPU *cpu, hwaddr pteg_off)
{
    hwaddr base = ppc_hash32_hpt_base(cpu);
    hwaddr plen = HASH_PTEG_SIZE_32;
    void *pteg;

    pteg = address_space_map(CPU(cpu)->as, base + pteg_off, &plen, false,
                             MEMTXATTRS_UNSPECIFIED);
    if (pteg && plen < HASH_PTEG_SIZE_32) {
        address_space_unmap(CPU(cpu)->as, pteg, plen, false, 0);
        return NULL;
    }
    return pteg;
}

static void ppc_hash32_unmap_pteg(PowerPCCPU *cpu, const uint32_t *pteg)
{
    address_space_unmap(CPU(cpu)->as, (void *)pteg, HASH_PTEG_SIZE_32,
                        false, HASH_PTEG_SIZE_32);
}

static inline bool ppc_hash32_pte_match(uint32_t pte0, bool secondary,
                                        target_ulong ptem)
{
    return (pte0 & HPTE32_V_VALID)
        && (secondary == !!(pte0 & HPTE32_V_SECONDARY))
        && HPTE32_V_COMPARE(pte0, ptem);
}

static hwaddr ppc_hash32_pteg_search(PowerPCCPU *cpu, hwaddr pteg_off,
                                     bool secondary, target_ulong ptem,
                                     ppc_hash_pte32_t *pte)
{
    TCGMMUStats *st = tcg_mmu_slot(CPU(cpu)->cpu_index);
    const uint32_t *pteg;
    hwaddr pte_offset = pteg_off;
    target_ulong pte0, pte1;
    int i;

    st->hash32_pteg_probe++;

    pteg = ppc_hash32_map_pteg(cpu, pteg_off);
    if (pteg) {
        for (i = 0; i < HPTES_PER_GROUP; i++) {
            st->hash32_pte_slot++;
            pte0 = ldl_p(&pteg[2 * i]);
            /*
             * pte0 contains the valid bit and must be read before pte1,
             * otherwise we might see an old pte1 with a new valid bit and
             * thus an inconsistent hpte value
             */
            smp_rmb();
            pte1 = ldl_p(&pteg[2 * i + 1]);

            if (ppc_hash32_pte_match(pte0, secondary, ptem)) {
                pte->pte0 = pte0;
                pte->pte1 = pte1;
                ppc_hash32_unmap_pteg(cpu, pteg);
                return pteg_off + i * HASH_PTE_SIZE_32;
            }
        }
        ppc_hash32_unmap_pteg(cpu, pteg);
        return -1;
    }

    st->hash32_pteg_map_fail++;
    for (i = 0; i < HPTES_PER_GROUP; i++) {
        st->hash32_pte_slot++;
        pte0 = ppc_hash32_load_hpte0(cpu, pte_offset);
        /* see above */
        smp_rmb();
        pte1 = ppc_hash32_load_hpte1(cpu, pte_offset);

        if (ppc_hash32_pte_match(pte0, secondary, ptem)) {
            pte->pte0 = pte0;
            pte->pte1 = pte1;
            return pte_offset;
        }

        pte_offset += HASH_PTE_SIZE_32;
    }

    return -1;
}

static void ppc_hash32_set_r(PowerPCCPU *cpu, hwaddr pte_offset, uint32_t pte1)
{
    target_ulong base = ppc_hash32_hpt_base(cpu);
    hwaddr offset = pte_offset + 6;

    /* The HW performs a non-atomic byte update */
    stb_phys(CPU(cpu)->as, base + offset, ((pte1 >> 8) & 0xff) | 0x01);
}

static void ppc_hash32_set_c(PowerPCCPU *cpu, hwaddr pte_offset, uint64_t pte1)
{
    target_ulong base = ppc_hash32_hpt_base(cpu);
    hwaddr offset = pte_offset + 7;

    /* The HW performs a non-atomic byte update */
    stb_phys(CPU(cpu)->as, base + offset, (pte1 & 0xff) | 0x80);
}

static hwaddr ppc_hash32_htab_lookup(PowerPCCPU *cpu,
                                     target_ulong sr, target_ulong eaddr,
                                     ppc_hash_pte32_t *pte)
{
    hwaddr pteg_off, pte_offset;
    hwaddr hash;
    uint32_t vsid, pgidx, ptem;

    vsid = sr & SR32_VSID;
    pgidx = (eaddr & ~SEGMENT_MASK_256M) >> TARGET_PAGE_BITS;
    hash = vsid ^ pgidx;
    ptem = (vsid << 7) | (pgidx >> 10);

    /* Page address translation */
    qemu_log_mask(CPU_LOG_MMU, "htab_base " HWADDR_FMT_plx
            " htab_mask " HWADDR_FMT_plx
            " hash " HWADDR_FMT_plx "\n",
            ppc_hash32_hpt_base(cpu), ppc_hash32_hpt_mask(cpu), hash);

    /* Primary PTEG lookup */
    qemu_log_mask(CPU_LOG_MMU, "0 htab=" HWADDR_FMT_plx "/" HWADDR_FMT_plx
            " vsid=%" PRIx32 " ptem=%" PRIx32
            " hash=" HWADDR_FMT_plx "\n",
            ppc_hash32_hpt_base(cpu), ppc_hash32_hpt_mask(cpu),
            vsid, ptem, hash);
    tcg_mmu_slot(CPU(cpu)->cpu_index)->hash32_htab++;
    pteg_off = get_pteg_offset32(cpu, hash);
    pte_offset = ppc_hash32_pteg_search(cpu, pteg_off, 0, ptem, pte);
    if (pte_offset != -1) {
        tcg_mmu_slot(CPU(cpu)->cpu_index)->hash32_primary++;
    }
    if (pte_offset == -1) {
        /* Secondary PTEG lookup */
        qemu_log_mask(CPU_LOG_MMU, "1 htab=" HWADDR_FMT_plx "/" HWADDR_FMT_plx
                " vsid=%" PRIx32 " api=%" PRIx32
                " hash=" HWADDR_FMT_plx "\n", ppc_hash32_hpt_base(cpu),
                ppc_hash32_hpt_mask(cpu), vsid, ptem, ~hash);
        pteg_off = get_pteg_offset32(cpu, ~hash);
        pte_offset = ppc_hash32_pteg_search(cpu, pteg_off, 1, ptem, pte);
        if (pte_offset != -1) {
            tcg_mmu_slot(CPU(cpu)->cpu_index)->hash32_secondary++;
        } else {
            tcg_mmu_slot(CPU(cpu)->cpu_index)->hash32_notfound++;
        }
    }

    return pte_offset;
}

/*
 * env->tlb_pidx tells which page indexes within a segment the TLB may
 * hold a hash translation for, so that a tlbie can leave out the CPUs
 * that hold none. Only the CPU's own thread changes it, and only there
 * are translations filled. A bit is set before the PTE is read and
 * cleared only after its pages are flushed.
 */
#define HASH32_PIDX(ea)         (((ea) >> TARGET_PAGE_BITS) & 0xffff)

static void hash32_tlb_mark(CPUPPCState *env, vaddr eaddr)
{
    unsigned i = HASH32_PIDX(eaddr);
    unsigned long *w = &env->tlb_pidx[BIT_WORD(i)];

    if (!(*w & BIT_MASK(i))) {
        qatomic_set(w, *w | BIT_MASK(i));
        smp_mb(); /* pairs with smp_mb() in ppc_hash32_tlbie_pages() */
    }
}

void ppc_hash32_tlb_flushed(PowerPCCPU *cpu)
{
    CPUPPCState *env = &cpu->env;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(env->tlb_pidx); i++) {
        if (env->tlb_pidx[i]) {
            qatomic_set(&env->tlb_pidx[i], 0);
        }
    }
}

/* Drop the translations of @pages in every segment, on @cpu's thread */
static void hash32_tlbie_flush(PowerPCCPU *cpu, const target_ulong *pages,
                               unsigned nb)
{
    CPUPPCState *env = &cpu->env;
    unsigned i, n, pidx;
    unsigned long *w;

    for (i = 0; i < nb; i++) {
        pidx = HASH32_PIDX(pages[i]);
        w = &env->tlb_pidx[BIT_WORD(pidx)];
        if (!(*w & BIT_MASK(pidx))) {
            continue;
        }
        for (n = 0; n < 16; n++) {
            tlb_flush_page(CPU(cpu), (target_ulong)n << 28 |
                                     pidx << TARGET_PAGE_BITS);
        }
        qatomic_set(w, *w & ~BIT_MASK(pidx));
    }
}

typedef struct Hash32TlbiePages {
    unsigned nb;
    target_ulong pages[TLBIE_PAGES_MAX];
} Hash32TlbiePages;

static void hash32_tlbie_work(CPUState *cs, run_on_cpu_data data)
{
    Hash32TlbiePages *p = data.host_ptr;

    hash32_tlbie_flush(POWERPC_CPU(cs), p->pages, p->nb);
    g_free(p);
}

static void hash32_tlbie_sync(CPUState *cs, run_on_cpu_data data)
{
}

/*
 * Complete the tlbie pages of @cpu: when @local in its own TLB and, when
 * @remote, in the TLBs of the CPUs that may hold one of them, the issuing
 * CPU then waiting for them as for a broadcast flush. The VSID is not
 * known, as the page can belong to any address space.
 */
void ppc_hash32_tlbie_pages(PowerPCCPU *cpu, bool local, bool remote)
{
    CPUPPCState *env = &cpu->env;
    CPUState *src = CPU(cpu), *cs;
    bool queued = false;

    if (local) {
        hash32_tlbie_flush(cpu, env->tlbie_pages, env->tlbie_pages_nb);
    }
    if (!remote) {
        return;
    }
    smp_mb(); /* the PTE stores before the bits, pairs with hash32_tlb_mark */
    CPU_FOREACH(cs) {
        CPUPPCState *other = &POWERPC_CPU(cs)->env;
        Hash32TlbiePages *p;
        unsigned i, pidx;

        if (cs == src) {
            continue;
        }
        for (i = 0; i < env->tlbie_pages_nb; i++) {
            pidx = HASH32_PIDX(env->tlbie_pages[i]);
            if (qatomic_read(&other->tlb_pidx[BIT_WORD(pidx)]) &
                BIT_MASK(pidx)) {
                break;
            }
        }
        if (i == env->tlbie_pages_nb) {
            continue;
        }
        p = g_new(Hash32TlbiePages, 1);
        p->nb = env->tlbie_pages_nb;
        memcpy(p->pages, env->tlbie_pages, sizeof(p->pages[0]) * p->nb);
        async_run_on_cpu(cs, hash32_tlbie_work, RUN_ON_CPU_HOST_PTR(p));
        queued = true;
    }
    if (queued) {
        async_safe_run_on_cpu(src, hash32_tlbie_sync, RUN_ON_CPU_NULL);
    }
}

bool ppc_hash32_xlate(PowerPCCPU *cpu, vaddr eaddr, MMUAccessType access_type,
                      hwaddr *raddrp, int *psizep, int *protp, int mmu_idx,
                      bool guest_visible)
{
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;
    TCGMMUStats *st = tcg_mmu_slot(cs->cpu_index);
    target_ulong sr;
    hwaddr pte_offset, raddr;
    ppc_hash_pte32_t pte;
    bool key;
    int prot;

    st->hash32_xlate++;

    /* There are no hash32 large pages. */
    *psizep = TARGET_PAGE_BITS;

    /* 1. Handle real mode accesses */
    if (mmuidx_real(mmu_idx)) {
        /* Translation is off */
        st->hash32_real++;
        *raddrp = eaddr;
        *protp = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return true;
    }

    /* 2. Check Block Address Translation entries (BATs) */
    if (env->nb_BATs != 0) {
        raddr = ppc_hash32_bat_lookup(cpu, eaddr, access_type, protp, mmu_idx);
        if (raddr != -1) {
            if (!check_prot_access_type(*protp, access_type)) {
                if (guest_visible) {
                    if (access_type == MMU_INST_FETCH) {
                        cs->exception_index = POWERPC_EXCP_ISI;
                        env->error_code = 0x08000000;
                    } else {
                        cs->exception_index = POWERPC_EXCP_DSI;
                        env->error_code = 0;
                        env->spr[SPR_DAR] = eaddr;
                        if (access_type == MMU_DATA_STORE) {
                            env->spr[SPR_DSISR] = 0x0a000000;
                        } else {
                            env->spr[SPR_DSISR] = 0x08000000;
                        }
                    }
                }
                return false;
            }
            st->hash32_bat_hit++;
            *raddrp = raddr;
            return true;
        }
    }

    /* 3. Look up the Segment Register */
    sr = env->sr[eaddr >> 28];

    /* 4. Handle direct store segments */
    if (sr & SR32_T) {
        return ppc_hash32_direct_store(cpu, sr, eaddr, access_type,
                                       raddrp, protp, mmu_idx, guest_visible);
    }

    /* 5. Check for segment level no-execute violation */
    if (access_type == MMU_INST_FETCH && (sr & SR32_NX)) {
        if (guest_visible) {
            cs->exception_index = POWERPC_EXCP_ISI;
            env->error_code = 0x10000000;
        }
        return false;
    }

    /* 6. Locate the PTE in the hash table */
    if (current_cpu == cs) {
        hash32_tlb_mark(env, eaddr);
    }
    pte_offset = ppc_hash32_htab_lookup(cpu, sr, eaddr, &pte);
    if (pte_offset == -1) {
        if (guest_visible) {
            if (access_type == MMU_INST_FETCH) {
                cs->exception_index = POWERPC_EXCP_ISI;
                env->error_code = 0x40000000;
            } else {
                cs->exception_index = POWERPC_EXCP_DSI;
                env->error_code = 0;
                env->spr[SPR_DAR] = eaddr;
                if (access_type == MMU_DATA_STORE) {
                    env->spr[SPR_DSISR] = 0x42000000;
                } else {
                    env->spr[SPR_DSISR] = 0x40000000;
                }
            }
        }
        return false;
    }
    qemu_log_mask(CPU_LOG_MMU,
                "found PTE at offset %08" HWADDR_PRIx "\n", pte_offset);

    /* 7. Check access permissions */
    key = ppc_hash32_key(mmuidx_pr(mmu_idx), sr);
    prot = ppc_hash32_prot(key, pte.pte1 & HPTE32_R_PP, sr & SR32_NX);

    if (!check_prot_access_type(prot, access_type)) {
        /* Access right violation */
        qemu_log_mask(CPU_LOG_MMU, "PTE access rejected\n");
        if (guest_visible) {
            if (access_type == MMU_INST_FETCH) {
                cs->exception_index = POWERPC_EXCP_ISI;
                env->error_code = 0x08000000;
            } else {
                cs->exception_index = POWERPC_EXCP_DSI;
                env->error_code = 0;
                env->spr[SPR_DAR] = eaddr;
                if (access_type == MMU_DATA_STORE) {
                    env->spr[SPR_DSISR] = 0x0a000000;
                } else {
                    env->spr[SPR_DSISR] = 0x08000000;
                }
            }
        }
        return false;
    }

    qemu_log_mask(CPU_LOG_MMU, "PTE access granted !\n");

    /* 8. Update PTE referenced and changed bits if necessary */

    if (!(pte.pte1 & HPTE32_R_R)) {
        ppc_hash32_set_r(cpu, pte_offset, pte.pte1);
    }
    if (!(pte.pte1 & HPTE32_R_C)) {
        if (access_type == MMU_DATA_STORE) {
            ppc_hash32_set_c(cpu, pte_offset, pte.pte1);
        } else {
            /*
             * Treat the page as read-only for now, so that a later write
             * will pass through this function again to set the C bit
             */
            prot &= ~PAGE_WRITE;
        }
    }
    *protp = prot;

    /* 9. Determine the real address from the PTE */
    *raddrp = pte.pte1 & HPTE32_R_RPN;
    *raddrp &= TARGET_PAGE_MASK;
    *raddrp |= eaddr & ~TARGET_PAGE_MASK;
    return true;
}
