/*
 * Buffer management
 */

#include "tux3.h"
#include "tux3_fork.h"

#ifndef trace
#define trace trace_on
#endif

/*
 * FIXME: Setting delta is not atomic with dirty for this buffer_head,
 */
#define BUFDELTA_AVAIL		1
#define BUFDELTA_BITS		order_base_2(BUFDELTA_AVAIL + TUX3_MAX_DELTA)
TUX3_DEFINE_STATE_FNS(unsigned long, buf, BUFDELTA_AVAIL, BUFDELTA_BITS,
		      BH_PrivateStart);

/*
 * FIXME: this is hack to save delta to linux buffer_head.
 * Inefficient, and this is not atomic with dirty bit change. And this
 * may not work on all arch (If set_bit() and cmpxchg() is not
 * exclusive, this has race).
 */
static void tux3_set_bufdelta(struct buffer_head *buffer, int delta)
{
	unsigned long state, old_state;

	delta = tux3_delta(delta);

	state = buffer->b_state;
	for (;;) {
		old_state = state;
		state = tux3_bufsta_update(old_state, delta);
		state = cmpxchg(&buffer->b_state, old_state, state);
		if (state == old_state)
			break;
	}
}

static void tux3_clear_bufdelta(struct buffer_head *buffer)
{
	unsigned long state, old_state;

	state = buffer->b_state;
	for (;;) {
		old_state = state;
		state = tux3_bufsta_clear(old_state);
		state = cmpxchg(&buffer->b_state, old_state, state);
		if (state == old_state)
			break;
	}
}

static int tux3_bufdelta(struct buffer_head *buffer)
{
	assert(buffer_dirty(buffer));
	while (1) {
		unsigned long state = buffer->b_state;
		if (tux3_bufsta_has_delta(state))
			return tux3_bufsta_get_delta(state);
		/* The delta is not yet set. Retry */
		cpu_relax();
	}
}

/* Can we modify buffer from delta */
int buffer_can_modify(struct buffer_head *buffer, unsigned delta)
{
	/* If true, buffer is still not stabilized. We can modify. */
	if (tux3_bufdelta(buffer) == tux3_delta(delta))
		return 1;
	/* The buffer may already be in stabilized stage for backend. */
	return 0;
}

/* FIXME: we should rewrite with own buffer management */
void tux3_set_buffer_dirty_list(struct buffer_head *buffer, int delta,
				struct list_head *head)
{
	struct inode *inode = buffer_inode(buffer);
	struct address_space *mapping = inode->i_mapping;
	struct address_space *buffer_mapping = buffer->b_page->mapping;

	mark_buffer_dirty(buffer);

	if (!mapping->assoc_mapping)
		mapping->assoc_mapping = buffer_mapping;
	else
		BUG_ON(mapping->assoc_mapping != buffer_mapping);

	if (!buffer->b_assoc_map) {
		spin_lock(&buffer_mapping->private_lock);
		BUG_ON(!list_empty(&buffer->b_assoc_buffers));
		list_move_tail(&buffer->b_assoc_buffers, head);
		buffer->b_assoc_map = mapping;
		/* FIXME: hack for save delta */
		tux3_set_bufdelta(buffer, delta);
		spin_unlock(&buffer_mapping->private_lock);
	}
}

/* FIXME: we should rewrite with own buffer management */
void tux3_set_buffer_dirty(struct buffer_head *buffer, int delta)
{
	struct list_head *head = tux3_dirty_buffers(buffer_inode(buffer),delta);
	tux3_set_buffer_dirty_list(buffer, delta, head);
}

static void __tux3_clear_buffer_dirty(struct buffer_head *buffer)
{
	if (buffer->b_assoc_map) {
		spin_lock(&buffer->b_page->mapping->private_lock);
		list_del_init(&buffer->b_assoc_buffers);
		buffer->b_assoc_map = NULL;
		tux3_clear_bufdelta(buffer);
		spin_unlock(&buffer->b_page->mapping->private_lock);
	} else
		BUG_ON(!list_empty(&buffer->b_assoc_buffers));
}

void tux3_clear_buffer_dirty(struct buffer_head *buffer)
{
	__tux3_clear_buffer_dirty(buffer);
	clear_buffer_dirty(buffer);
}

/* Copied from fs/buffer.c */
static void discard_buffer(struct buffer_head *buffer)
{
	/* FIXME: we need lock_buffer()? */
	lock_buffer(buffer);
	clear_buffer_dirty(buffer);
	buffer->b_bdev = NULL;
	clear_buffer_mapped(buffer);
	clear_buffer_req(buffer);
	clear_buffer_new(buffer);
	clear_buffer_delay(buffer);
	clear_buffer_unwritten(buffer);
	unlock_buffer(buffer);
}

/* Invalidate buffer, this is called from truncate, error path on write, etc */
void tux3_invalidate_buffer(struct buffer_head *buffer)
{
	/* FIXME: we should check buffer_can_modify() to invalidate */
	__tux3_clear_buffer_dirty(buffer);
	discard_buffer(buffer);
}

#include "buffer_writeback.c"
#include "buffer_fork.c"
