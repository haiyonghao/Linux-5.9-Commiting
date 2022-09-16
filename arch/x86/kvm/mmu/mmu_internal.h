/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_MMU_INTERNAL_H
#define __KVM_X86_MMU_INTERNAL_H

#include <linux/types.h>

#include <asm/kvm_host.h>

struct kvm_mmu_page {

// use this to link this page to arch.active_mmu_pages or invalid list.
	struct list_head link;

	// use this to link this page to arch.mmu_page_hash table entry, every entry is a list,
	// pages in this list has same gfn.
	struct hlist_node hash_link;

	// use this to link this page to arch.lpage_disallowed_mmu_pages, means this page
	// do not support large page config.
	struct list_head lpage_disallowed_link;

	// indicate whether this page synced or unsync.
	bool unsync;

	// mmu only has 1 valid generation and 1 invalid generation.
	u8 mmu_valid_gen;

	// not used in kvm.
	bool mmio_cached;

	// indicate whether the page support large page config.
	bool lpage_disallowed; /* Can't be replaced by an equiv large page */

	/*
	 * The following two entries are used to key the shadow page in the
	 * hash table.
	 */
	union kvm_mmu_page_role role;
	gfn_t gfn;


	/*spt holds the struct page addr, the struct page contains ptes, and 
	page->private points to kvm_mmu_page.
	*/
	u64 *spt;
	
	/* hold the gfn of each spte inside spt */
	gfn_t *gfns;
	/* count how many EPTP points the kvm_mmu_page. */
	int root_count;          /* Currently serving as active root */

	/* count how many ptes in this kvm_mmu_page is not synced. */
	unsigned int unsync_children;

	/* reverse map, map to parent ptes */
	struct kvm_rmap_head parent_ptes; /* rmap pointers to parent sptes */
	DECLARE_BITMAP(unsync_child_bitmap, 512);

#ifdef CONFIG_X86_32
	/*
	 * Used out of the mmu-lock to avoid reading spte values while an
	 * update is in progress; see the comments in __get_spte_lockless().

	 * in 32-bit mode, modification to spte is atomic, so we can count the 
	 * times spte has been modified.
	 */
	int clear_spte_count;
#endif

	/* Number of writes since the last time traversal visited this page.  
	 * count the times of emulation since last emualtion, if exceeded, we
	 * need to unmap the page.
	 */
	atomic_t write_flooding_count;
};

static inline struct kvm_mmu_page *to_shadow_page(hpa_t shadow_page)
{
	struct page *page = pfn_to_page(shadow_page >> PAGE_SHIFT);

	return (struct kvm_mmu_page *)page_private(page);
}

/* get the shadow page on which the sptep pointing entry lay. */
static inline struct kvm_mmu_page *sptep_to_sp(u64 *sptep)
{
	return to_shadow_page(__pa(sptep));
}

void kvm_mmu_gfn_disallow_lpage(struct kvm_memory_slot *slot, gfn_t gfn);
void kvm_mmu_gfn_allow_lpage(struct kvm_memory_slot *slot, gfn_t gfn);
bool kvm_mmu_slot_gfn_write_protect(struct kvm *kvm,
				    struct kvm_memory_slot *slot, u64 gfn);

#endif /* __KVM_X86_MMU_INTERNAL_H */
