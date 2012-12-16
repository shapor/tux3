/*
 * Orphan inode management
 *
 * LOG_ORPHAN_ADD and LOG_ORPHAN_DEL are log records of frontend
 * operation for orphan state. With it, we don't need any write to FS
 * except log blocks. If the orphan is short life, it will be handled
 * by this.
 *
 * However, if the orphan is long life, it can make log blocks too long.
 * So, to prevent it, if orphan inodes are still living until rollup, we
 * store those inum into sb->otable. With it, we can obsolete log blocks.
 *
 * On replay, we can know the inum of orphan inodes yet not destroyed by
 * checking sb->otable, LOG_ORPHAN_ADD, and LOG_ORPHAN_DEL. (Note, orphan
 * inum of LOG_ORPHAN_ADD can be destroyed by same inum of LOG_ORPHAN_DEL).
 */

#include "tux3.h"
#include "ileaf.h"

#ifndef trace
#define trace trace_on
#endif

/* Frontend modification cache for orphan */
struct orphan {
	inum_t inum;
	struct list_head list;
};

/* list_entry() for orphan inode list */
#define orphan_list_entry(x)	list_entry(x, struct tux3_inode, orphan_list)
/* list_entry() for orphan object (orphan del, LOG_ORPHAN_ADD on replay) list */
#define orphan_entry(x)		list_entry(x, struct orphan, list)

static struct orphan *alloc_orphan(inum_t inum)
{
	struct orphan *orphan = malloc(sizeof(struct orphan));
	if (!orphan)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&orphan->list);
	orphan->inum = inum;
	return orphan;
}

static void free_orphan(struct orphan *orphan)
{
	free(orphan);
}

/* Caller must care about locking if needed */
void clean_orphan_list(struct list_head *head)
{
	while (!list_empty(head)) {
		struct orphan *orphan = orphan_entry(head->next);
		list_del(&orphan->list);
		free_orphan(orphan);
	}
}

/*
 * FIXME: maybe, we can share code more with inode.c and iattr.c.
 *
 * And this is supporting ORPHAN_ATTR only, and assuming there is only
 * ORPHAN_ATTR. We are better to support multiple attributes in
 * otable.
 */
enum { ORPHAN_ATTR, };
static unsigned orphan_asize[] = {
	/* Fixed size attrs */
	[ORPHAN_ATTR] = 0,
};

static int oattr_encoded_size(struct btree *btree, void *data)
{
	return orphan_asize[ORPHAN_ATTR] + 2;
}

static void oattr_encode(struct btree *btree, void *data, void *attrs, int size)
{
	encode_kind(attrs, ORPHAN_ATTR, btree->sb->version);
}

struct ileaf_attr_ops oattr_ops = {
	.magic		= cpu_to_be16(TUX3_MAGIC_OLEAF),
	.encoded_size	= oattr_encoded_size,
	.encode		= oattr_encode,
};

/* Add inum into sb->otable */
int tux3_rollup_orphan_add(struct sb *sb, struct list_head *orphan_add)
{
	struct btree *otable = otable_btree(sb);
	struct cursor *cursor;
	int err = 0;

	if (list_empty(orphan_add))
		return 0;

	down_write(&otable->lock);
	if (!has_root(otable))
		err = alloc_empty_btree(otable);
	up_write(&otable->lock);
	if (err)
		return err;

	/* FIXME: +1 may not be enough to add multiple */
	cursor = alloc_cursor(otable, 1); /* +1 for new depth */
	if (!cursor)
		return -ENOMEM;

	down_write(&cursor->btree->lock);
	while (!list_empty(orphan_add)) {
		struct tux3_inode *tuxnode =orphan_list_entry(orphan_add->next);

		trace("inum %Lu", tuxnode->inum);

		/* FIXME: what to do if error? */
		err = btree_probe(cursor, tuxnode->inum);
		if (err)
			goto out;

		/* Write orphan inum into orphan btree */
		struct ileaf_req rq = {
			.key = {
				.start	= tuxnode->inum,
				.len	= 1,
			},
		};
		err = btree_write(cursor, &rq.key);
		release_cursor(cursor);
		if (err)
			goto out;

		list_del_init(&tuxnode->orphan_list);
	}
out:
	up_write(&cursor->btree->lock);
	free_cursor(cursor);

	return err;
}

/* Delete inum from sb->otable */
int tux3_rollup_orphan_del(struct sb *sb, struct list_head *orphan_del)
{
	struct btree *otable = otable_btree(sb);
	int err;

	/* FIXME: we don't want to remove inum one by one */
	while (!list_empty(orphan_del)) {
		struct orphan *orphan = orphan_entry(orphan_del->next);

		trace("inum %Lu", orphan->inum);

		/* Remove inum from orphan btree */
		err = btree_chop(otable, orphan->inum, 1);
		if (err)
			return err;

		list_del(&orphan->list);
		free_orphan(orphan);
	}

	return 0;
}

/*
 * Make inode as orphan, and logging it. Then if orphan is living until
 * rollup, orphan will be written to sb->otable.
 */
int tux3_make_orphan_add(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct tux3_inode *tuxnode = tux_inode(inode);

	trace("inum %Lu", tuxnode->inum);

	assert(list_empty(&tuxnode->orphan_list));
	list_add(&tuxnode->orphan_list, &sb->orphan_add);

	log_orphan_add(sb, sb->version, tuxnode->inum);

	return 0;
}

/*
 * FIXME: We may be able to merge this with inode deletion, and this
 * may be able to be used for deferred inode deletion too with some
 * tweaks.
 */
static int add_defer_oprhan_del(struct sb *sb, inum_t inum)
{
	struct orphan *orphan = alloc_orphan(inum);
	if (IS_ERR(orphan))
		return PTR_ERR(orphan);

	/* Add orphan deletion (from sb->otable) request. */
	list_add(&orphan->list, &sb->orphan_del);

	return 0;
}

/* Clear inode as orphan (inode was destroyed), and logging it. */
int tux3_make_orphan_del(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct tux3_inode *tuxnode = tux_inode(inode);

	trace("inum %Lu", tuxnode->inum);

	if (!list_empty(&tuxnode->orphan_list)) {
		/* This orphan is not applied to sb->otable yet. */
		list_del_init(&tuxnode->orphan_list);
	} else {
		/* This orphan was already applied to sb->otable. */
		int err = add_defer_oprhan_del(sb, tuxnode->inum);
		if (err) {
			/* FIXME: what to do? */
			warn("orphan inum %Lu was leaved due to low memory",
			     tuxnode->inum);
			return err;
		}
	}

	log_orphan_del(sb, sb->version, tuxnode->inum);

	return 0;
}

/*
 * On replay, we collects orphan logs at first. Then, we reconstruct
 * infos for orphan at end of replay.
 */

static struct orphan *replay_find_orphan(struct list_head *head, inum_t inum)
{
	struct orphan *orphan;
	list_for_each_entry(orphan, head, list) {
		if (orphan->inum == inum)
			return orphan;
	}
	return NULL;
}

int replay_orphan_add(struct replay *rp, unsigned version, inum_t inum)
{
	struct sb *sb = rp->sb;
	struct orphan *orphan;

	if (sb->version != version)
		return 0;

	orphan = alloc_orphan(inum);
	if (IS_ERR(orphan))
		return PTR_ERR(orphan);

	assert(!replay_find_orphan(&rp->log_orphan_add, inum));
	/* Remember LOG_ORPHAN_ADD */
	list_add(&orphan->list, &rp->log_orphan_add);

	return 0;
}

int replay_orphan_del(struct replay *rp, unsigned version, inum_t inum)
{
	struct sb *sb = rp->sb;
	struct orphan *orphan;

	if (sb->version != version)
		return 0;

	orphan = replay_find_orphan(&rp->log_orphan_add, inum);
	if (orphan) {
		/* There was prior LOG_ORPHAN_ADD, cancel it. */
		list_del(&orphan->list);
		free_orphan(orphan);
		return 0;
	}

	/* Orphan inum in sb->otable became dead. Add deletion request. */
	return add_defer_oprhan_del(sb, inum);
}

/* Destroy or clean orphan inodes of destroy candidate */
void replay_iput_orphan_inodes(struct sb *sb,
			       struct list_head *orphan_in_otable,
			       int destroy)
{
	struct tux3_inode *tuxnode, *safe;

	/* orphan inodes not in sb->otable */
	list_for_each_entry_safe(tuxnode, safe, &sb->orphan_add, orphan_list) {
		struct inode *inode = &tuxnode->vfs_inode;

		if (!destroy) {
			/* Set i_nlink = 1 prevent to destroy inode. */
			set_nlink(inode, 1);
			list_del_init(&tuxnode->orphan_list);
		}
		iput(inode);
	}

	/* orphan inodes in sb->otable */
	list_for_each_entry_safe(tuxnode, safe, orphan_in_otable, orphan_list) {
		struct inode *inode = &tuxnode->vfs_inode;

		/* list_empty(&inode->orphan_list) tells it is in otable */
		list_del_init(&tuxnode->orphan_list);

		if (!destroy) {
			/* Set i_nlink = 1 prevent to destroy inode. */
			set_nlink(inode, 1);
		}
		iput(inode);
	}
}

static int load_orphan_inode(struct sb *sb, inum_t inum, struct list_head *head)
{
	struct inode *inode;

	inode = tux3_iget(sb, inum);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	assert(inode->i_nlink == 0);

	tux3_mark_inode_orphan(tux_inode(inode));
	/* List inode up, then caller will decide what to do */
	list_add(&tux_inode(inode)->orphan_list, head);

	return 0;
}

static int load_enum_inode(struct btree *btree, inum_t inum, void *attrs,
			   unsigned size, void *data)
{
	struct replay *rp = data;
	struct sb *sb = rp->sb;
	unsigned kind, version;

	assert(size == 2);
	decode_kind(attrs, &kind, &version);
	if (version != sb->version || kind != ORPHAN_ATTR)
		return 0;

	/* If inum is in orphan_del, it was dead already */
	if (replay_find_orphan(&sb->orphan_del, inum))
		return 0;

	return load_orphan_inode(sb, inum, &rp->orphan_in_otable);
}

/* Load orphan inode from sb->otable */
static int load_otable_orphan_inode(struct replay *rp)
{
	struct sb *sb = rp->sb;
	struct btree *otable = otable_btree(sb);
	struct ileaf_enumrate_cb cb = {
		.callback	= load_enum_inode,
		.data		= rp,
	};
	int err;

	if (!has_root(&sb->otable))
		return 0;

	struct cursor *cursor = alloc_cursor(otable, 0);
	if (!cursor)
		return -ENOMEM;

	down_write(&cursor->btree->lock);
	err = btree_probe(cursor, 0);
	if (err)
		goto error;

	err = btree_traverse(cursor, 0, TUXKEY_LIMIT, ileaf_enumerate, &cb);
	/* FIXME: error handling */

	release_cursor(cursor);
error:
	up_write(&cursor->btree->lock);
	free_cursor(cursor);

	return err;
}

/* Load all orphan inodes */
int replay_load_orphan_inodes(struct replay *rp)
{
	struct sb *sb = rp->sb;
	struct list_head *head;
	int err;

	head = &rp->log_orphan_add;
	while (!list_empty(head)) {
		struct orphan *orphan = orphan_entry(head->next);

		err = load_orphan_inode(sb, orphan->inum, &sb->orphan_add);
		if (err)
			goto error;

		list_del(&orphan->list);
		free_orphan(orphan);
	}

	err = load_otable_orphan_inode(rp);
	if (err)
		goto error;

	return 0;

error:
	replay_iput_orphan_inodes(sb, &rp->orphan_in_otable, 0);
	return err;
}
