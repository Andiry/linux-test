/*
 *  linux/fs/ext4/fsync.c
 *
 *  Copyright (C) 1993  Stephen Tweedie (sct@redhat.com)
 *  from
 *  Copyright (C) 1992  Remy Card (card@masi.ibp.fr)
 *                      Laboratoire MASI - Institut Blaise Pascal
 *                      Universite Pierre et Marie Curie (Paris VI)
 *  from
 *  linux/fs/minix/truncate.c   Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4fs fsync primitive
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 *  Removed unnecessary code duplication for little endian machines
 *  and excessive __inline__s.
 *        Andi Kleen, 1997
 *
 * Major simplications and cleanup - we only need to do the metadata, because
 * we can depend on generic_block_fdatasync() to sync the data blocks.
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/jbd2.h>
#include <linux/blkdev.h>

#include "ext4.h"
#include "ext4_jbd2.h"

#include <trace/events/ext4.h>

#define	CACHELINE_SIZE	64

#define X86_FEATURE_PCOMMIT	( 9*32+22) /* PCOMMIT instruction */
#define X86_FEATURE_CLFLUSHOPT	( 9*32+23) /* CLFLUSHOPT instruction */
#define X86_FEATURE_CLWB	( 9*32+24) /* CLWB instruction */

static inline bool arch_has_pcommit(void)
{
	return static_cpu_has(X86_FEATURE_PCOMMIT);
}

static inline bool arch_has_clwb(void)
{
	return static_cpu_has(X86_FEATURE_CLWB);
}

#define _mm_clflush(addr)\
	asm volatile("clflush %0" : "+m" (*(volatile char *)(addr)))
#define _mm_clflushopt(addr)\
	asm volatile(".byte 0x66; clflush %0" : "+m" (*(volatile char *)(addr)))
#define _mm_clwb(addr)\
	asm volatile(".byte 0x66; xsaveopt %0" : "+m" (*(volatile char *)(addr)))
#define _mm_pcommit()\
	asm volatile(".byte 0x66, 0x0f, 0xae, 0xf8")

static inline void PERSISTENT_BARRIER(void)
{
	asm volatile ("sfence\n" : : );
	if (arch_has_pcommit()) {
		_mm_pcommit();
		asm volatile ("sfence\n" : : );
	}
}

static inline void ext4_flush_buffer(void *buf, uint32_t len, bool fence)
{
	uint32_t i;
	len = len + ((unsigned long)(buf) & (CACHELINE_SIZE - 1));
	if (arch_has_clwb()) {
		for (i = 0; i < len; i += CACHELINE_SIZE)
			_mm_clwb(buf + i);
	} else {
		for (i = 0; i < len; i += CACHELINE_SIZE)
			_mm_clflush(buf + i);
	}
	/* Do a fence only if asked. We often don't need to do a fence
	 * immediately after clflush because even if we get context switched
	 * between clflush and subsequent fence, the context switch operation
	 * provides implicit fence. */
	if (fence)
		PERSISTENT_BARRIER();
}

/*
 * If we're not journaling and this is a just-created file, we have to
 * sync our parent directory (if it was freshly created) since
 * otherwise it will only be written by writeback, leaving a huge
 * window during which a crash may lose the file.  This may apply for
 * the parent directory's parent as well, and so on recursively, if
 * they are also freshly created.
 */
static int ext4_sync_parent(struct inode *inode)
{
	struct dentry *dentry = NULL;
	struct inode *next;
	int ret = 0;

	if (!ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY))
		return 0;
	inode = igrab(inode);
	while (ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY)) {
		ext4_clear_inode_state(inode, EXT4_STATE_NEWENTRY);
		dentry = d_find_any_alias(inode);
		if (!dentry)
			break;
		next = igrab(dentry->d_parent->d_inode);
		dput(dentry);
		if (!next)
			break;
		iput(inode);
		inode = next;
		ret = sync_mapping_buffers(inode->i_mapping);
		if (ret)
			break;
		ret = sync_inode_metadata(inode, 1);
		if (ret)
			break;
	}
	iput(inode);
	return ret;
}

static bool buffer_size_valid(struct buffer_head *bh)
{
	return bh->b_state != 0;
}

static long ext4_dax_get_addr(struct buffer_head *bh, void **addr,
	unsigned blkbits)
{
	unsigned long pfn;
	sector_t sector = bh->b_blocknr << (blkbits - 9);
	return bdev_direct_access(bh->b_bdev, sector, addr, &pfn, bh->b_size);
}

static int ext4_dax_msync(struct file *file, loff_t start, loff_t end,
	int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct buffer_head bh1, *bh;
	loff_t pos = start;
	loff_t max = start;
	loff_t bh_max = start;
	void *addr;
	int retval;

	memset(&bh1, 0, sizeof(bh1));
	bh = &bh1;

	end = min(end, i_size_read(inode));

	while (pos < end) {
		unsigned len;
		if (pos == max) {
			unsigned blkbits = inode->i_blkbits;
			sector_t block = pos >> blkbits;
			unsigned first = pos - (block << blkbits);
			long size;

			if (pos == bh_max) {
				bh->b_size = PAGE_ALIGN(end - pos);
				bh->b_state = 0;
				retval = ext4_get_block(inode, block, bh, 0);
				if (retval)
					break;
				if (!buffer_size_valid(bh))
					bh->b_size = 1 << blkbits;
				bh_max = pos - first + bh->b_size;
			} else {
				unsigned done = bh->b_size -
						(bh_max - (pos - first));
				bh->b_blocknr += done >> blkbits;
				bh->b_size -= done;
			}

			retval = ext4_dax_get_addr(bh, &addr, blkbits);
			if (retval < 0)
				break;
			addr += first;
			size = retval - first;
			max = min(pos + size, end);
		}

		len = max - pos;

		if (!len)
			break;

		ext4_flush_buffer(addr, len, 0);
		pos += len;
		addr += len;
	}

	PERSISTENT_BARRIER();
	return 0;
}

/*
 * akpm: A new design for ext4_sync_file().
 *
 * This is only called from sys_fsync(), sys_fdatasync() and sys_msync().
 * There cannot be a transaction open by this task.
 * Another task could have dirtied this inode.  Its data can be in any
 * state in the journalling system.
 *
 * What we do is just kick off a commit and wait on it.  This will snapshot the
 * inode to disk.
 */

int ext4_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct ext4_inode_info *ei = EXT4_I(inode);
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	int ret = 0, err;
	tid_t commit_tid;
	bool needs_barrier = false;

	J_ASSERT(ext4_journal_current_handle() == NULL);

	trace_ext4_sync_file_enter(file, datasync);

	if (inode->i_sb->s_flags & MS_RDONLY) {
		/* Make sure that we read updated s_mount_flags value */
		smp_rmb();
		if (EXT4_SB(inode->i_sb)->s_mount_flags & EXT4_MF_FS_ABORTED)
			ret = -EROFS;
		goto out;
	}

	if (!journal) {
		ret = generic_file_fsync(file, start, end, datasync);
		if (!ret && !hlist_empty(&inode->i_dentry))
			ret = ext4_sync_parent(inode);
		goto out;
	}

	if (IS_DAX(inode) && mapping_mapped(file->f_mapping)) {
		ext4_dax_msync(file, start, end, datasync);
		goto journal_commit;
	}

	ret = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (ret)
		return ret;
	/*
	 * data=writeback,ordered:
	 *  The caller's filemap_fdatawrite()/wait will sync the data.
	 *  Metadata is in the journal, we wait for proper transaction to
	 *  commit here.
	 *
	 * data=journal:
	 *  filemap_fdatawrite won't do anything (the buffers are clean).
	 *  ext4_force_commit will write the file data into the journal and
	 *  will wait on that.
	 *  filemap_fdatawait() will encounter a ton of newly-dirtied pages
	 *  (they were dirtied by commit).  But that's OK - the blocks are
	 *  safe in-journal, which is all fsync() needs to ensure.
	 */
	if (ext4_should_journal_data(inode)) {
		ret = ext4_force_commit(inode->i_sb);
		goto out;
	}

journal_commit:
	commit_tid = datasync ? ei->i_datasync_tid : ei->i_sync_tid;
	if (journal->j_flags & JBD2_BARRIER &&
	    !jbd2_trans_will_send_data_barrier(journal, commit_tid))
		needs_barrier = true;
	ret = jbd2_complete_transaction(journal, commit_tid);
	if (needs_barrier) {
		err = blkdev_issue_flush(inode->i_sb->s_bdev, GFP_KERNEL, NULL);
		if (!ret)
			ret = err;
	}
out:
	trace_ext4_sync_file_exit(inode, ret);
	return ret;
}
