/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"

#ifndef trace
#define trace trace_on
#endif

#define HASH_SHIFT	10
#define HASH_SIZE	(1 << 10)
#define HASH_MASK	(HASH_SIZE - 1)

static struct hlist_head inode_hashtable[HASH_SIZE] = {
	[0 ... (HASH_SIZE - 1)] = HLIST_HEAD_INIT,
};

static unsigned long hash(inum_t inum)
{
	u64 hash = inum * GOLDEN_RATIO_PRIME;
	return hash >> (64 - HASH_SHIFT);
}

void inode_leak_check(void)
{
	int leaks = 0;

	for (int i = 0; i < HASH_SIZE; i++) {
		struct hlist_head *head = inode_hashtable + i;
		struct hlist_node *node;
		struct inode *inode;
		hlist_for_each_entry(inode, node, head, i_hash) {
			trace_on("possible leak inode inum %Lu, i_count %d",
				 inode->inum, atomic_read(&inode->i_count));
			leaks++;
		}
	}

	assert(leaks == 0);
}

static void insert_inode_hash(struct inode *inode)
{
	struct hlist_head *b = inode_hashtable + hash(inode->inum);
	hlist_add_head(&inode->i_hash, b);
}

static void remove_inode_hash(struct inode *inode)
{
	if (!hlist_unhashed(&inode->i_hash))
		hlist_del_init(&inode->i_hash);
}

static struct inode *new_inode(struct sb *sb)
{
	struct inode *inode = malloc(sizeof(*inode));
	if (!inode)
		goto error;
	*inode = (struct inode){ INIT_INODE(*inode, sb, 0), };
	INIT_HLIST_NODE(&inode->i_hash);
	inode->map = new_map(sb->dev, NULL);
	if (!inode->map)
		goto error_map;
	inode->map->inode = inode;
	return inode;

error_map:
	free(inode);
error:
	return NULL;
}

static void free_inode(struct inode *inode)
{
	inode->i_state &= ~I_BAD;
	assert(list_empty(&inode->alloc_list));
	assert(list_empty(&inode->orphan_list));
	assert(hlist_unhashed(&inode->i_hash));
	assert(list_empty(&inode->list));
	assert(inode->i_state == I_FREEING);
	assert(mapping(inode));

	free_map(mapping(inode));
	free(inode);
}

/* This is just to clean inode is partially initialized */
static void make_bad_inode(struct inode *inode)
{
	remove_inode_hash(inode);
	inode->i_state |= I_BAD;
}

static int is_bad_inode(struct inode *inode)
{
	return inode->i_state & I_BAD;
}

static void unlock_new_inode(struct inode *inode)
{
	inode->i_state &= ~I_NEW;
}

static void iget_failed(struct inode *inode)
{
	make_bad_inode(inode);
	unlock_new_inode(inode);
	iput(inode);
}

void __iget(struct inode *inode)
{
	assert(!(inode->i_state & I_FREEING));
	if (atomic_read(&inode->i_count)) {
		atomic_inc(&inode->i_count);
		return;
	}
	/* i_count == 0 should happen only dirty inode */
	assert(inode->i_state & I_DIRTY);
	atomic_inc(&inode->i_count);
}

/* get additional reference to inode; caller must already hold one. */
void ihold(struct inode *inode)
{
	assert(!(inode->i_state & I_FREEING));
	assert(atomic_read(&inode->i_count) >= 1);
	atomic_inc(&inode->i_count);
}

struct inode *iget5_locked(struct sb *sb, inum_t inum,
			   int (*test)(struct inode *, void *),
			   int (*set)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(inum);
	struct hlist_node *node;
	struct inode *inode;

	hlist_for_each_entry(inode, node, head, i_hash) {
		if (test(inode, data)) {
			__iget(inode);
			return inode;
		}
	}

	inode = new_inode(sb);
	if (!inode)
		return NULL;
	if (set(inode, data)) {
		free_inode(inode);
		return NULL;
	}

	inode->i_state = I_NEW;
	hlist_add_head(&inode->i_hash, head);

	return inode;
}

/* Truncate partial block. If partial, we have to update last block. */
static int tux3_truncate_partial_block(struct inode *inode, loff_t newsize)
{
	struct sb *sb = tux_sb(inode->i_sb);
	block_t index = newsize >> sb->blockbits;
	unsigned offset = newsize & sb->blockmask;
	struct buffer_head *buffer;

	if (!offset)
		return 0;

	buffer = blockread(mapping(inode), index);
	if (!buffer)
		return -EIO;

	memset(bufdata(buffer) + offset, 0, inode->i_sb->blocksize - offset);
	blockput_dirty(buffer);

	return 0;
}

static void end_writeback(struct inode *inode)
{
	clear_inode(inode);
}

#include "kernel/inode.c"

static void tux_setup_inode(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);

	assert(inode->inum != TUX_INVALID_INO);
	switch (inode->inum) {
	case TUX_VOLMAP_INO:
		/* use default handler */
		break;
	case TUX_LOGMAP_INO:
		inode->map->io = dev_errio;
		break;
	case TUX_BITMAP_INO:
		/* set maximum bitmap size */
		/* FIXME: should this, tuxtruncate();? */
		inode->i_size = (sb->volblocks + 7) >> 3;
		/* FALLTHRU */
	default:
		inode->map->io = filemap_extent_io;
		break;
	}
}

/*
 * NOTE: iput() must not be called inside of change_begin/end() if
 * i_nlink == 0.  Otherwise, it will become cause of deadlock.
 */
void iput(struct inode *inode)
{
	if (atomic_dec_and_test(&inode->i_count)) {
		if (inode->i_nlink > 0 && inode->i_state & I_DIRTY) {
			/* Keep the inode on dirty list */
			return;
		}

		inode->i_state |= I_FREEING;
		tux3_evict_inode(inode);

		remove_inode_hash(inode);
		free_inode(inode);
	}
}

int tuxtruncate(struct inode *inode, loff_t size)
{
	return tux3_truncate(inode, size);
}

int write_inode(struct inode *inode)
{
	/* Those inodes must not be marked as I_DIRTY_SYNC/DATASYNC. */
	assert(tux_inode(inode)->inum != TUX_VOLMAP_INO &&
	       tux_inode(inode)->inum != TUX_LOGMAP_INO &&
	       tux_inode(inode)->inum != TUX_INVALID_INO);
	switch (tux_inode(inode)->inum) {
	case TUX_BITMAP_INO:
	case TUX_VTABLE_INO:
	case TUX_ATABLE_INO:
		/* FIXME: assert(only btree should be changed); */
		break;
	}
	return save_inode(inode);
}
