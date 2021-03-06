#include "tux3user.h"

void clear_inode(struct inode *inode)
{
	int has_refcnt = !list_empty(&inode->list);

	list_del_init(&inode->list);
	inode->state = 0;
	if (has_refcnt)
		iput(inode);
}

void __mark_inode_dirty(struct inode *inode, unsigned flags)
{
	if ((inode->state & flags) != flags) {
		inode->state |= flags;
		if (list_empty(&inode->list)) {
			__iget(inode);
			list_add_tail(&inode->list, &inode->i_sb->dirty_inodes);
		}
	}
}

void mark_inode_dirty(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY);
}

void mark_inode_dirty_sync(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY_SYNC);
}

void mark_buffer_dirty(struct buffer_head *buffer)
{
	if (!buffer_dirty(buffer)) {
		set_buffer_dirty(buffer);
		__mark_inode_dirty(buffer_inode(buffer), I_DIRTY_PAGES);
	}
}

int sync_inode(struct inode *inode)
{
	unsigned dirty = inode->state;
	int err;

	if (inode->state & I_DIRTY_PAGES) {
		/* To handle redirty, this clears before flushing */
		inode->state &= ~I_DIRTY_PAGES;
		err = flush_buffers(mapping(inode));
		if (err)
			goto error;
	}
	if (inode->state & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) {
		/* To handle redirty, this clears before flushing */
		inode->state &= ~(I_DIRTY_SYNC | I_DIRTY_DATASYNC);
		err = write_inode(inode);
		if (err)
			goto error;
	}

	if (dirty && !(inode->state & I_DIRTY)) {
		list_del_init(&inode->list);
		iput(inode);
	}

	return 0;

error:
	inode->state = dirty;
	return err;
}

static int sync_inodes(struct sb *sb)
{
	struct inode *inode, *safe;
	LIST_HEAD(dirty_inodes);
	int err;

	list_splice_init(&sb->dirty_inodes, &dirty_inodes);

	list_for_each_entry_safe(inode, safe, &dirty_inodes, list) {
		/*
		 * FIXME: this is hack. those inodes is dirtied by
		 * sync_inode() of other inodes, so it should be
		 * flushed after other inodes.
		 */
		switch (inode->inum) {
		case TUX_BITMAP_INO:
		case TUX_VOLMAP_INO:
			continue;
		}

		err = sync_inode(inode);
		if (err)
			goto error;
	}
	err = sync_inode(sb->bitmap);
	if (err)
		goto error;
	err = sync_inode(sb->volmap);
	if (err)
		goto error;
	assert(list_empty(&dirty_inodes)); /* someone redirtied own inode? */

	return 0;

error:
	list_splice_init(&dirty_inodes, &sb->dirty_inodes);
	return err;
}

static void cleanup_garbage_for_writeback(struct sb *sb)
{
	/*
	 * Clean garbage (atomic commit) stuff. Don't forget to update
	 * this, if you update the atomic commit.
	 */
	log_finish(sb);

	sb->logchain = 0;
	sb->logbase = sb->next_logbase = 0;
	sb->logthis = sb->lognext = 0;
	invalidate_buffers(sb->logmap->map);

	assert(flink_empty(&sb->defree.head));
	assert(flink_empty(&sb->derollup.head));
	assert(flink_empty(&sb->decycle.head));
	assert(flink_empty(&sb->new_decycle.head));
	assert(list_empty(&sb->pinned));
}

int sync_super(struct sb *sb)
{
	int err;

	printf("sync inodes\n");
	if ((err = sync_inodes(sb)))
		return err;

	cleanup_garbage_for_writeback(sb);

	printf("sync super\n");
	if ((err = save_sb(sb)))
		return err;

	return 0;
}
