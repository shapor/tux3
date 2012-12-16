#ifndef TUX3_H
#define TUX3_H

#ifdef __KERNEL__
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/blkdev.h>	/* for struct blk_plug */
#include <linux/mutex.h>
#include <linux/magic.h>
#include <linux/slab.h>

#define printf(fmt, args...)	printk(fmt , ##args)
#define vprintf(fmt, args...)	vprintk(fmt , ##args)
#define die(code)		BUG_ON(1)

#include "trace.h"
#include "buffer.h"
#endif /* !__KERNEL__ */

#include "link.h"

#define fieldtype(compound, field) typeof(((compound *)NULL)->field)
#define vecset(d, v, n) memset((d), (v), (n) * sizeof(*(d)))
#define veccopy(d, s, n) memcpy((d), (s), (n) * sizeof(*(d)))
#define vecmove(d, s, n) memmove((d), (s), (n) * sizeof(*(d)))

typedef u64 inum_t;
typedef u64 tuxkey_t;

static inline void *encode16(void *at, unsigned val)
{
	*(__be16 *)at = cpu_to_be16(val);
	return at + sizeof(u16);
}

static inline void *encode32(void *at, unsigned val)
{
	*(__be32 *)at = cpu_to_be32(val);
	return at + sizeof(u32);
}

static inline void *encode64(void *at, u64 val)
{
	*(__be64 *)at = cpu_to_be64(val);
	return at + sizeof(u64);
}

static inline void *encode48(void *at, u64 val)
{
	at = encode16(at, val >> 32);
	return encode32(at, val);
}

static inline void *decode16(void *at, unsigned *val)
{
	*val = be16_to_cpup(at);
	return at + sizeof(u16);
}

static inline void *decode32(void *at, unsigned *val)
{
	*val = be32_to_cpup(at);
	return at + sizeof(u32);
}

static inline void *decode64(void *at, u64 *val)
{
	*val = be64_to_cpup(at);
	return at + sizeof(u64);
}

static inline void *decode48(void *at, u64 *val)
{
	unsigned part1, part2;
	at = decode16(at, &part1);
	at = decode32(at, &part2);
	*val = (u64)part1 << 32 | part2;
	return at;
}

/* Tux3 disk format */

#define TUX3_MAGIC		"tux3" "\x20\x12\x07\x02"
/*
 * TUX3_LABEL includes the date of the last incompatible disk format change
 * NOTE: Always update this history for each incompatible change!
 *
 * Disk Format Revision History
 *
 * 2008-08-06: Beginning of time
 * 2008-09-06: Actual checking starts
 * 2008-12-12: Atom dictionary size in disksuper instead of atable->i_size
 * 2009-02-28: Attributes renumbered, rdev added
 * 2009-03-10: Alignment fix of disksuper
 * 2012-02-16: Update for atomic commit
 * 2012-07-02: Use timestamp 32.32 fixed point. Increase log_balloc size.
 */

#define TUX3_MAGIC_LOG		0x10ad
#define TUX3_MAGIC_BNODE	0xb4de
#define TUX3_MAGIC_DLEAF	0x1eaf
#define TUX3_MAGIC_DLEAF2	0xbeaf
#define TUX3_MAGIC_ILEAF	0x90de
#define TUX3_MAGIC_OLEAF	0x6eaf

#define MAX_INODES_BITS		48
#define MAX_BLOCKS_BITS		48
#define MAX_EXTENT		(1 << 6)

#define SB_LOC			(1 << 12)
#define SB_LEN			(1 << 12)	/* this is maximum blocksize */

#define MAX_TUXKEY		(((tuxkey_t)1 << 48) - 1)
#define TUXKEY_LIMIT		(MAX_TUXKEY + 1)

/* Special inode numbers */
#define TUX_BITMAP_INO		0
#define TUX_VTABLE_INO		1
#define TUX_ATABLE_INO		2
#define TUX_ROOTDIR_INO		3
#define TUX_VOLMAP_INO		61	/* This doesn't have entry in ileaf */
#define TUX_LOGMAP_INO		62	/* FIXME: remove this */
#define TUX_INVALID_INO		63	/* FIXME: just for debugging */
#define TUX_NORMAL_INO		64	/* until this ino, reserved ino */

struct disksuper {
	/* Update magic on any incompatible format change */
	char magic[8];		/* Contains TUX3_LABEL magic string */
	__be64 birthdate;	/* Volume creation date */
	__be64 flags;		/* Need to assign some flags */
	__be16 blockbits;	/* Shift to get volume block size */
	__be16 unused[3];	/* Padding for alignment */
	__be64 volblocks;	/* Volume size */

	/* The rest should be moved to a "metablock" that is updated frequently */
	__be64 iroot;		/* Root of the inode table btree */
	__be64 oroot;		/* Root of the orphan table btree */
#ifndef ATOMIC /* Obsoleted by LOG_FREEBLOCKS */
	__be64 freeblocks;	/* Should match total of zero bits in allocation bitmap */
#endif
	__be64 nextalloc;	/* Get rid of this when we have a real allocation policy */
	__be64 atomdictsize;	/*
				 * Size of the atom dictionary instead of i_size
				 * FIXME: we are better to remove this somehow
				 */
	__be32 freeatom;	/* Beginning of persistent free atom list in atable */
	__be32 atomgen;		/* Next atom number if there are no free atoms */
	__be64 logchain;	/* Most recent delta commit block pointer */
	__be32 logcount;	/* Count of log blocks in the current log chain */
} __packed;

struct root {
	unsigned depth; /* btree levels not including leaf level */
	block_t block; /* disk location of btree root */
};

struct btree {
	struct rw_semaphore lock;
	struct sb *sb;		/* Convenience to reduce parameter list size */
	struct btree_ops *ops;	/* Generic btree low level operations */
	struct root root;	/* Cached description of btree root */
	u16 entries_per_leaf;	/* Used in btree leaf splitting */
};

/* Define layout of btree root on disk, endian conversion is elsewhere. */

static inline u64 pack_root(struct root *root)
{
	return (u64)root->depth << 48 | root->block;
}

static inline struct root unpack_root(u64 v)
{
	return (struct root){ .depth = v >> 48, .block = v & (-1ULL >> 16), };
}

/* Path cursor for btree traversal */

struct cursor {
	struct btree *btree;
#define CURSOR_DEBUG
#ifdef CURSOR_DEBUG
#define FREE_BUFFER	((void *)0xdbc06505)
#define FREE_NEXT	((void *)0xdbc06507)
	int maxlevel;
#endif
	int level;
	struct path_level {
		struct buffer_head *buffer;
		struct index_entry *next;
	} path[];
};

struct stash { struct flink_head head; u64 *pos, *top; };

/* Per-delta data structure for sb */
struct sb_delta_dirty {
	struct list_head dirty_inodes;	/* dirty inodes list */
};

/* Tux3-specific sb is a handle for the entire volume state */
struct sb {
	union {
		struct disksuper super;
		char thisbig[SB_LEN];
	};
	struct btree itable;	/* Inode table btree */
	struct btree otable;	/* Orphan table btree */
	struct inode *volmap;	/* Volume metadata cache (like blockdev). */
	struct inode *bitmap;	/* allocation bitmap special file */
	struct inode *rootdir;	/* root directory special file */
	struct inode *vtable;	/* version table special file */
	struct inode *atable;	/* xattr atom special file */
	unsigned delta;		/* delta commit cycle */
	unsigned rollup;	/* log rollup cycle */
	struct rw_semaphore delta_lock; /* delta transition exclusive */
	unsigned blocksize, blockbits, blockmask;
	block_t volblocks, freeblocks, nextalloc;
	unsigned entries_per_node; /* must be per-btree type, get rid of this */
	unsigned version;	/* Currently mounted volume version view */

	unsigned atomref_base;	/* Index of atom refcount base */
	unsigned unatom_base;	/* Index of unatom base */
	loff_t atomdictsize;	/* Atom dictionary size */
	unsigned freeatom;	/* Start of free atom list in atom table */
	unsigned atomgen;	/* Next atom number to allocate if no free atoms */

	struct inode *logmap;	/* Log block cache */
	unsigned lognext;	/* Index of next log block in log map */
	struct buffer_head *logbuf; /* Cached log block */
	unsigned char *logpos, *logtop; /* Where to emit next log entry */
	struct mutex loglock;	/* serialize log entries (spinlock me) */

	spinlock_t orphan_add_lock;  /* lock of orphan_add for frontend */
	struct list_head orphan_add; /* defered orphan inode add list */
	spinlock_t orphan_del_lock;  /* lock of orphan_del for frontend */
	struct list_head orphan_del; /* defered orphan inode del list */

	struct stash defree;	/* defer extent frees until after delta */
	struct stash derollup;	/* defer extent frees until after rollup */

	struct list_head rollup_buffers; /* dirty metadata flushed at rollup */

	struct list_head alloc_inodes;	/* deferred inum allocation inodes */
	struct iowait *iowait;		/* helper for waiting I/O */

	spinlock_t forked_buffers_lock;
	struct link forked_buffers;	/* forked buffers list */

	spinlock_t dirty_inodes_lock;	/* lock of dirty_inodes for frontend */
	/* Per-delta dirty data for sb */
	struct sb_delta_dirty s_ddc[TUX3_MAX_DELTA];
#ifdef __KERNEL__
	struct super_block *vfs_sb; /* Generic kernel superblock */
#else
	struct dev *dev;		/* userspace block device */
	loff_t s_maxbytes;		/* maximum file size */
#endif
};

/* logging  */

struct logblock {
	__be16 magic;		/* Magic number */
	__be16 bytes;		/* Total data bytes on this block */
	u32 unused;		/* padding */
	__be64 logchain;	/* Block number to previous logblock */
	unsigned char data[];	/* Log data */
};

enum {
	LOG_BALLOC = 0x33,	/* Log of block allocation */
	LOG_BFREE,		/* Log of freeing block after delta */
	LOG_BFREE_ON_ROLLUP,	/* Log of freeing block after rollup */
	LOG_BFREE_RELOG,	/* LOG_BFREE, but re-log of free after rollup */
	LOG_LEAF_REDIRECT,	/* Log of leaf redirect */
	LOG_LEAF_FREE,		/* Log of freeing leaf */
	LOG_BNODE_REDIRECT,	/* Log of bnode redirect */
	LOG_BNODE_ROOT,		/* Log of new bnode root allocation */
	LOG_BNODE_SPLIT,	/* Log of spliting bnode to new bnode */
	LOG_BNODE_ADD,		/* Log of adding bnode index */
	LOG_BNODE_UPDATE,	/* Log of bnode index ->block update */
	LOG_BNODE_MERGE,	/* Log of merging 2 bnodes */
	LOG_BNODE_DEL,		/* Log of deleting bnode index */
	LOG_BNODE_ADJUST,	/* Log of bnode index ->key adjust */
	LOG_BNODE_FREE,		/* Log of freeing bnode */
	LOG_ORPHAN_ADD,		/* Log of adding orphan inode */
	LOG_ORPHAN_DEL,		/* Log of deleting orphan inode */
	LOG_FREEBLOCKS,		/* Log of freeblocks in bitmap on rollup */
	LOG_ROLLUP,		/* Log of marking rollup */
	LOG_DELTA,		/* just for debugging */
	LOG_TYPES
};

/* Per-delta data structure for inode */
struct inode_delta_dirty {
	struct list_head dirty_buffers;	/* list for dirty buffers */
	struct list_head dirty_list;	/* link for dirty inode list */

	unsigned	present;
	/* inode attributes */
	umode_t		i_mode;
	uid_t		i_uid;
	gid_t		i_gid;
	unsigned int	i_nlink;
	dev_t		i_rdev;
	loff_t		i_size;
//	struct timespec	i_atime;
	struct timespec	i_mtime;
	struct timespec	i_ctime;
	u64		i_version;
};

struct xcache;
struct tux3_inode {
	struct btree btree;
	inum_t inum;			/* Inode number */
	struct xcache *xcache;		/* Extended attribute cache */
	struct list_head alloc_list;	/* link for deferred inum allocation */
	struct list_head orphan_list;	/* link for orphan inode list */

	spinlock_t lock;		/* lock for inode metadata */
	/* Per-delta dirty data for inode */
	unsigned flags;			/* flags for inode state */
	unsigned present;		/* Attributes decoded from or
					 * to be encoded to inode table */
	struct inode_delta_dirty i_ddc[TUX3_MAX_DELTA];
#ifdef __KERNEL__
	int (*io)(int rw, struct bufvec *bufvec);
#endif
	/* Generic inode */
	struct inode vfs_inode;
};

static inline struct tux3_inode *tux_inode(struct inode *inode)
{
	return container_of(inode, struct tux3_inode, vfs_inode);
}

static inline struct inode *btree_inode(struct btree *btree)
{
	return &container_of(btree, struct tux3_inode, btree)->vfs_inode;
}
#ifdef __KERNEL__
static inline struct sb *tux_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct super_block *vfs_sb(struct sb *sb)
{
	return sb->vfs_sb;
}

typedef struct address_space map_t;

static inline map_t *mapping(struct inode *inode)
{
	return inode->i_mapping;
}

static inline void *malloc(size_t size)
{
	might_sleep();
	return kmalloc(size, GFP_NOFS);
}

static inline void free(void *ptr)
{
	kfree(ptr);
}

static inline struct block_device *sb_dev(struct sb *sb)
{
	return sb->vfs_sb->s_bdev;
}
#else /* !__KERNEL__ */
static inline struct sb *tux_sb(struct sb *sb)
{
	return sb;
}

static inline struct sb *vfs_sb(struct sb *sb)
{
	return sb;
}

static inline map_t *mapping(struct inode *inode)
{
	return inode->map;
}

static inline struct dev *sb_dev(struct sb *sb)
{
	return sb->dev;
}
#endif /* !__KERNEL__ */

/* Get delta from free running counter */
static inline unsigned tux3_delta(unsigned delta)
{
	return delta & (TUX3_MAX_DELTA - 1);
}

/* Get per-delta dirty data control for sb */
static inline struct sb_delta_dirty *tux3_sb_ddc(struct sb *sb, unsigned delta)
{
	return &sb->s_ddc[tux3_delta(delta)];
}

/* Get per-delta dirty data control for inode */
static inline struct inode_delta_dirty *tux3_inode_ddc(struct inode *inode,
						       unsigned delta)
{
	return &tux_inode(inode)->i_ddc[tux3_delta(delta)];
}

static inline struct tux3_inode *i_ddc_to_inode(struct inode_delta_dirty *i_ddc,
						unsigned delta)
{
	return container_of(i_ddc, struct tux3_inode, i_ddc[tux3_delta(delta)]);
}

/* Get per-delta dirty buffers list from inode */
static inline struct list_head *tux3_dirty_buffers(struct inode *inode,
						   unsigned delta)
{
	return &tux3_inode_ddc(inode, delta)->dirty_buffers;
}

struct tux_iattr {
	kuid_t	uid;
	kgid_t	gid;
	umode_t	mode;
};

static inline struct btree *itable_btree(struct sb *sb)
{
	return &sb->itable;
}

static inline struct btree *otable_btree(struct sb *sb)
{
	return &sb->otable;
}

#define TUX_LINK_MAX (1 << 15) /* arbitrary limit, increase it */

#define TUX_NAME_LEN 255

/* directory entry */
typedef struct {
	__be64 inum;
	__be16 rec_len;
	u8 name_len, type;
	char name[];
	/*
	 * On 64bit arch sizeof(tux_dirent) == 16. We should use
	 * offsetof(tux_dirent, name) instead.
	 */
	/* u32 __pad; */
} tux_dirent;

struct btree_key_range {
	tuxkey_t start;
	unsigned len;
};

struct btree_ops {
	void (*btree_init)(struct btree *btree);
	int (*leaf_init)(struct btree *btree, void *leaf);
	tuxkey_t (*leaf_split)(struct btree *btree, tuxkey_t hint, void *from, void *into);
	/* return value: 1 - modified, 0 - not modified, < 0 - error */
	int (*leaf_chop)(struct btree *btree, tuxkey_t start, u64 len, void *leaf);
	/* return value: 1 - merged, 0 - couldn't merge */
	int (*leaf_merge)(struct btree *btree, void *into, void *from);
	int (*leaf_write)(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit, void *leaf, struct btree_key_range *key, tuxkey_t *split_hint);
	int (*leaf_read)(struct btree *btree, tuxkey_t key_bottom, tuxkey_t key_limit, void *leaf, struct btree_key_range *key);
	int (*balloc)(struct sb *sb, unsigned blocks, block_t *block);
	int (*bfree)(struct sb *sb, block_t block, unsigned blocks);

	void *private_ops;

	/*
	 * for debugging
	 */
	int (*leaf_sniff)(struct btree *btree, void *leaf);
	/* return value: 1 - can free, 0 - can't free */
	int (*leaf_can_free)(struct btree *btree, void *leaf);
	void (*leaf_dump)(struct btree *btree, void *leaf);
};

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#ifndef XATTR_CREATE
#define XATTR_CREATE 1 // fail if xattr already exists
#define XATTR_REPLACE 2 // fail if xattr does not exist
#endif

/* Information for replay */
struct replay {
	struct sb *sb;

	/* For orphan.c */
	struct list_head log_orphan_add;   /* To remember LOG_ORPHAN_ADD */
	struct list_head orphan_in_otable; /* Orphan inodes in sb->otable */

	/* For replay.c */
	void *rollup_pos;	/* position of rollup log in a log block */
	block_t rollup_index;	/* index of a log block including rollup log */
	block_t blocknrs[];	/* block address of log blocks */
};

/* Does this btree have root bnode/leaf? */
extern struct root no_root;
static inline int has_root(struct btree *btree)
{
	/* FIXME: should use conditional inode->present */
	return (btree->root.block != no_root.block) ||
		(btree->root.depth != no_root.depth);
}

/* Redirect ptr which is pointing data of src from src to dst */
static inline void *ptr_redirect(void *ptr, void *src, void *dst)
{
	if (ptr) {
		assert(ptr >= src);
		return dst + (ptr - src);
	}
	return NULL;
}

#ifdef __KERNEL__
static inline struct timespec gettime(void)
{
	return current_kernel_time();
}

static inline struct inode *buffer_inode(struct buffer_head *buffer)
{
	return buffer->b_page->mapping->host;
}

/* Get logical index of buffer */
static inline block_t bufindex(struct buffer_head *buffer)
{
	struct inode *inode = buffer_inode(buffer);
	struct page *page = buffer->b_page;

	return (page_offset(page) + bh_offset(buffer)) >> inode->i_blkbits;
}

/* dir.c */
extern const struct file_operations tux_dir_fops;
extern const struct inode_operations tux_dir_iops;

/* filemap.c */
int tux3_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh_result, int create);
int tux3_truncate_page(struct address_space *mapping,
		       loff_t from, get_block_t *get_block);
extern const struct address_space_operations tux_aops;
extern const struct address_space_operations tux_blk_aops;
extern const struct address_space_operations tux_vol_aops;

/* inode.c */
void tux3_write_failed(struct address_space *mapping, loff_t to);
int tux3_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat);
int tux3_setattr(struct dentry *dentry, struct iattr *iattr);

/* symlink.c */
extern const struct inode_operations tux_symlink_iops;

/* utility.c */
int vecio(int rw, struct block_device *dev, loff_t offset, unsigned vecs,
	  struct bio_vec *vec, bio_end_io_t endio, void *info);
int syncio(int rw, struct block_device *dev, loff_t offset, unsigned vecs,
	   struct bio_vec *vec);
int devio(int rw, struct block_device *dev, loff_t offset, void *data,
	  unsigned len);
int blockio(int rw, struct buffer_head *buffer, block_t block);
int blockio_vec(int rw, struct bufvec *bufvec, block_t block, unsigned count);

/* temporary hack for buffer */
struct buffer_head *peekblk(struct address_space *mapping, block_t iblock);
struct buffer_head *blockread(struct address_space *mapping, block_t iblock);
struct buffer_head *blockget(struct address_space *mapping, block_t iblock);
#endif /* !__KERNEL__ */

/* balloc.c */
block_t bitmap_dump(struct inode *inode, block_t start, block_t count);
block_t balloc_from_range(struct sb *sb, block_t start, block_t count,
			  unsigned blocks);
int balloc(struct sb *sb, unsigned blocks, block_t *block);
int bfree(struct sb *sb, block_t start, unsigned blocks);
int replay_update_bitmap(struct replay *rp, block_t start, unsigned count, int set);

/* btree.c */
unsigned calc_entries_per_node(unsigned blocksize);
struct buffer_head *cursor_leafbuf(struct cursor *cursor);
void release_cursor(struct cursor *cursor);
struct cursor *alloc_cursor(struct btree *btree, int);
void free_cursor(struct cursor *cursor);
void cursor_push(struct cursor *cursor, struct buffer_head *buffer, struct index_entry *next);

void init_btree(struct btree *btree, struct sb *sb, struct root root, struct btree_ops *ops);
int alloc_empty_btree(struct btree *btree);
int free_empty_btree(struct btree *btree);
struct buffer_head *new_leaf(struct btree *btree);
tuxkey_t cursor_next_key(struct cursor *cursor);
tuxkey_t cursor_this_key(struct cursor *cursor);
int cursor_advance(struct cursor *cursor);
int btree_probe(struct cursor *cursor, tuxkey_t key);
typedef int (*btree_traverse_func_t)(struct btree *btree, tuxkey_t key_bottom,
				     tuxkey_t key_limit, void *leaf,
				     tuxkey_t key, u64 len, void *data);
int btree_traverse(struct cursor *cursor, tuxkey_t key, u64 len,
		   btree_traverse_func_t func, void *data);
int btree_chop(struct btree *btree, tuxkey_t start, u64 len);
int btree_insert_leaf(struct cursor *cursor, tuxkey_t key, struct buffer_head *leafbuf);
void *btree_expand(struct cursor *cursor, tuxkey_t key, unsigned newsize);
int btree_write(struct cursor *cursor, struct btree_key_range *key);
int btree_read(struct cursor *cursor, struct btree_key_range *key);
void show_tree_range(struct btree *btree, tuxkey_t start, unsigned count);
void show_tree(struct btree *btree);
int cursor_redirect(struct cursor *cursor);
int replay_bnode_redirect(struct replay *rp, block_t oldblock, block_t newblock);
int replay_bnode_root(struct replay *rp, block_t root, unsigned count,
		      block_t left, block_t right, tuxkey_t rkey);
int replay_bnode_split(struct replay *rp, block_t src, unsigned pos, block_t dst);
int replay_bnode_add(struct replay *rp, block_t parent, block_t child, tuxkey_t key);
int replay_bnode_update(struct replay *rp, block_t parent, block_t child, tuxkey_t key);
int replay_bnode_merge(struct replay *rp, block_t src, block_t dst);
int replay_bnode_del(struct replay *rp, block_t bnode, tuxkey_t key, unsigned count);
int replay_bnode_adjust(struct replay *rp, block_t bnode, tuxkey_t from, tuxkey_t to);

/* commit.c */
void setup_sb(struct sb *sb, struct disksuper *super);
int load_sb(struct sb *sb);
int save_sb(struct sb *sb);
int apply_defered_bfree(struct sb *sb, u64 val);
int force_rollup(struct sb *sb);
int force_delta(struct sb *sb);
int change_begin(struct sb *sb);
int change_end_without_commit(struct sb *sb);
int change_end(struct sb *sb);

/* dir.c */
void tux_update_dirent(struct buffer_head *buffer, tux_dirent *entry, struct inode *new_inode);
int tux_create_dirent(struct inode *dir, const struct qstr *qstr, inum_t inum,
		      umode_t mode);
tux_dirent *tux_find_dirent(struct inode *dir, const struct qstr *qstr,
			    struct buffer_head **result);
int tux_delete_entry(struct buffer_head *buffer, tux_dirent *entry);
int tux_delete_dirent(struct buffer_head *buffer, tux_dirent *entry);
int tux_readdir(struct file *file, void *state, filldir_t filldir);
int tux_dir_is_empty(struct inode *dir);

/* dleaf.c */
#include "dleaf.h"

/* dleaf2.c */
extern struct btree_ops dtree2_ops;
static inline struct btree_ops *dtree_ops(void)
{
	return &dtree2_ops;
}

/* filemap.c */
int tux3_filemap_overwrite_io(int rw, struct bufvec *bufvec);
int tux3_filemap_redirect_io(int rw, struct bufvec *bufvec);

/* iattr.c */
void dump_attrs(struct inode *inode);
void *encode_kind(void *attrs, unsigned kind, unsigned version);
void *decode_kind(void *attrs, unsigned *kind, unsigned *version);
extern struct ileaf_attr_ops iattr_ops;

/* ileaf.c */
struct ileaf;
void *ileaf_lookup(struct btree *btree, inum_t inum, struct ileaf *leaf, unsigned *result);
int ileaf_find_free(struct btree *btree, tuxkey_t key_bottom,
		    tuxkey_t key_limit, void *leaf,
		    tuxkey_t key, u64 len, void *data);
struct ileaf_enumrate_cb {
	int (*callback)(struct btree *btree, inum_t inum, void *attrs,
			unsigned size, void *data);
	void *data;
};
int ileaf_enumerate(struct btree *btree, tuxkey_t key_bottom,
		    tuxkey_t key_limit, void *leaf,
		    tuxkey_t key, u64 len, void *data);
extern struct btree_ops itable_ops;
extern struct btree_ops otable_ops;

/* inode.c */
void tux3_inode_copy_attrs(struct inode *inode, unsigned delta);
struct inode *tux_new_volmap(struct sb *sb);
struct inode *tux_new_logmap(struct sb *sb);
void del_defer_alloc_inum(struct inode *inode);
struct inode *__tux_create_inode(struct inode *dir, inum_t goal,
				 struct tux_iattr *iattr, dev_t rdev);
struct inode *tux_create_inode(struct inode *dir, struct tux_iattr *iattr,
			       dev_t rdev);
struct inode *tux3_iget(struct sb *sb, inum_t inum);
int tux3_save_inode(struct inode *inode, unsigned delta);
void tux3_evict_inode(struct inode *inode);

/* log.c */
extern unsigned log_size[];
void log_next(struct sb *sb, int pin);
void log_drop(struct sb *sb);
void log_finish(struct sb *sb);
int log_finish_cycle(struct sb *sb);
void log_balloc(struct sb *sb, block_t block, unsigned count);
void log_bfree(struct sb *sb, block_t block, unsigned count);
void log_bfree_on_rollup(struct sb *sb, block_t block, unsigned count);
void log_bfree_relog(struct sb *sb, block_t block, unsigned count);
void log_leaf_redirect(struct sb *sb, block_t oldblock, block_t newblock);
void log_leaf_free(struct sb *sb, block_t leaf);
void log_bnode_redirect(struct sb *sb, block_t oldblock, block_t newblock);
void log_bnode_root(struct sb *sb, block_t root, unsigned count,
		    block_t left, block_t right, tuxkey_t rkey);
void log_bnode_split(struct sb *sb, block_t src, unsigned pos, block_t dst);
void log_bnode_add(struct sb *sb, block_t parent, block_t child, tuxkey_t key);
void log_bnode_update(struct sb *sb, block_t parent, block_t child,
		      tuxkey_t key);
void log_bnode_merge(struct sb *sb, block_t src, block_t dst);
void log_bnode_del(struct sb *sb, block_t node, tuxkey_t key, unsigned count);
void log_bnode_adjust(struct sb *sb, block_t node, tuxkey_t from, tuxkey_t to);
void log_bnode_free(struct sb *sb, block_t bnode);
void log_orphan_add(struct sb *sb, unsigned version, tuxkey_t inum);
void log_orphan_del(struct sb *sb, unsigned version, tuxkey_t inum);
void log_freeblocks(struct sb *sb, block_t freeblocks);
void log_delta(struct sb *sb);
void log_rollup(struct sb *sb);

typedef int (*unstash_t)(struct sb *sb, u64 val);
void stash_init(struct stash *stash);
int stash_value(struct stash *stash, u64 value);
int unstash(struct sb *sb, struct stash *defree, unstash_t actor);
int stash_walk(struct sb *sb, struct stash *stash, unstash_t actor);
int defer_bfree(struct stash *defree, block_t block, unsigned count);
void destroy_defer_bfree(struct stash *defree);

/* orphan.c */
void clean_orphan_list(struct list_head *head);
extern struct ileaf_attr_ops oattr_ops;
int tux3_rollup_orphan_add(struct sb *sb, struct list_head *orphan_add);
int tux3_rollup_orphan_del(struct sb *sb, struct list_head *orphan_del);
int tux3_mark_inode_orphan(struct inode *inode);
int tux3_clear_inode_orphan(struct inode *inode);
int replay_orphan_add(struct replay *rp, unsigned version, inum_t inum);
int replay_orphan_del(struct replay *rp, unsigned version, inum_t inum);
void replay_iput_orphan_inodes(struct sb *sb,
			       struct list_head *orphan_in_otable,
			       int destroy);
int replay_load_orphan_inodes(struct replay *rp);

/* super.c */
struct replay *tux3_init_fs(struct sb *sbi);

/* replay.c */
struct replay *replay_stage1(struct sb *sb);
int replay_stage2(struct replay *rp);
int replay_stage3(struct replay *rp, int apply);

/* utility.c */
void hexdump(void *data, unsigned size);
void set_bits(u8 *bitmap, unsigned start, unsigned count);
void clear_bits(u8 *bitmap, unsigned start, unsigned count);
int all_set(u8 *bitmap, unsigned start, unsigned count);
int all_clear(u8 *bitmap, unsigned start, unsigned count);
int bytebits(u8 c);

/* writeback.c */
void tux3_mark_btree_dirty(struct btree *btree);
void __tux3_mark_inode_dirty(struct inode *inode, int flags);
static inline void tux3_mark_inode_dirty(struct inode *inode)
{
	__tux3_mark_inode_dirty(inode, I_DIRTY);
}
static inline void tux3_mark_inode_dirty_sync(struct inode *inode)
{
	__tux3_mark_inode_dirty(inode, I_DIRTY_SYNC);
}

void tux3_dirty_inode(struct inode *inode, int flags);
void tux3_iattrdirty(struct inode *inode);
void tux3_iattr_read_and_clear(struct inode *inode);
void tux3_xattrdirty(struct inode *inode);
void tux3_xattr_read_and_clear(struct inode *inode);
void tux3_clear_dirty_inode(struct inode *inode);
void tux3_mark_buffer_dirty(struct buffer_head *buffer);
void tux3_mark_buffer_rollup(struct buffer_head *buffer);
int tux3_flush_inode(struct inode *inode, unsigned delta);
int tux3_flush_inodes(struct sb *sb, unsigned delta);

/* xattr.c */
void atable_init_base(struct sb *sb);
int xcache_dump(struct inode *inode);
void free_xcache(struct inode *inode);
int new_xcache(struct inode *inode, unsigned size);
int xcache_remove_all(struct inode *inode);
int get_xattr(struct inode *inode, const char *name, unsigned len,
	      void *data, unsigned size);
int set_xattr(struct inode *inode, const char *name, unsigned len,
	      const void *data, unsigned size, unsigned flags);
int del_xattr(struct inode *inode, const char *name, unsigned len);
int list_xattr(struct inode *inode, char *text, size_t size);
unsigned encode_xsize(struct inode *inode);
void *encode_xattrs(struct inode *inode, void *attrs, unsigned size);
unsigned decode_xsize(struct inode *inode, void *attrs, unsigned size);
void *decode_xattr(struct inode *inode, void *attrs);

static inline struct buffer_head *vol_find_get_block(struct sb *sb, block_t block)
{
	return peekblk(mapping(sb->volmap), block);
}

static inline struct buffer_head *vol_getblk(struct sb *sb, block_t block)
{
	return blockget(mapping(sb->volmap), block);
}

static inline struct buffer_head *vol_bread(struct sb *sb, block_t block)
{
	return blockread(mapping(sb->volmap), block);
}

#include "dirty-buffer.h"	/* remove this after atomic commit */
#endif /* !TUX3_H */
