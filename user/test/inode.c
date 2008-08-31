/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#define trace trace_on

#define main notmain0
#include "balloc.c"
#undef main

#define main notmain1
#include "dleaf.c"
#undef main

#define main notmain2
#include "ileaf.c"
#undef main

#define main notmain3
#include "dir.c"
#undef main

#define main notmain4
#include "btree.c"
#undef main

/* High level operations */

int filemap_blockio(struct buffer *buffer, int write)
{
	struct map *filemap = buffer->map;
	struct inode *inode = filemap->inode;
	struct sb *sb = inode->sb;
	struct map *devmap = sb->devmap;
	struct dev *dev = devmap->dev;
	warn("%s <%Lx:%Lx>", write ? "write" : "read", (L)inode->inum, buffer->index);
	if (buffer->index & (-1LL << MAX_BLOCKS_BITS))
		return -EIO;
	assert(dev->bits >= 8 && dev->fd);

	int err, levels = inode->btree.root.depth;
	struct path path[levels + 1];
	if (!levels) {
		if (write)
			return -EIO;
		goto unmapped;
	}
	if ((err = probe(&inode->btree, buffer->index, path)))
		return err;
	struct buffer *leafbuf = path[levels].buffer;
	
	unsigned count = 0;
	struct extent *found = leaf_lookup(&inode->btree, buffer->index, leafbuf->data, &count);
	//dleaf_dump(&inode->btree, leafbuf->data);
	block_t physical;

	if (write) {
		if (count) {
			physical = found->block;
			trace(warn("found block [%Lx]", (L)physical);)
		} else {
			physical = balloc(sb); // !!! need an error return
			struct extent *store = tree_expand(&inode->btree, buffer->index, sizeof(struct extent), path);
			if (!store)
				goto eek;
			*store = (struct extent){ .block = physical };
		}
		release_path(path, levels + 1);
		return diskwrite(dev->fd, buffer->data, sb->blocksize, physical << dev->bits);
	}
	/* read */
	release_path(path, levels + 1);
	if (!count)
		goto unmapped;
	physical = found->block;
	trace(warn("found physical block %Lx", (long long)physical);)
	return diskread(dev->fd, buffer->data, sb->blocksize, physical << dev->bits);
eek:
	warn("unable to add extent to tree: %s", strerror(-err));
	free_block(sb, physical);
	return -EIO;
unmapped:
	/* found a hole */
	trace(warn("unmapped block %Lx", buffer->index);)
	memset(buffer->data, 0, sb->blocksize);
	return 0;
}

struct map_ops filemap_ops = { .blockio = filemap_blockio };

struct inode *new_inode(SB, inum_t inum)
{
	struct map *map = new_map(sb->devmap->dev, &filemap_ops);
	struct inode *inode = malloc(sizeof(*inode));
	*inode = (struct inode){ .sb = sb, .map = map, .inum = inum };
	map->inode = inode;
	return inode;
}

void free_inode(struct inode *inode)
{
	free_map(inode->map);
	free(inode);
}

/*
 * Inode table expansion algorithm
 *
 * First probe for the inode goal.  This retreives the rightmost leaf that
 * contains an inode less than or equal to the goal.  (We could in theory avoid
 * retrieving any leaf at all in some cases if we observe that the the goal must
 * fall into an unallocated gap between two index keys, for what that is worth.
 * Probably not very much.)
 *
 * If not at end then next key is greater than goal.  This block has the highest
 * ibase less than or equal to goal.  Ibase should be equal to btree key, so
 * assert.  Search block even if ibase is way too low.  If goal comes back equal
 * to next_key then there is no room to create more inodes in it, so advance to
 * the next block and repeat.
 *
 * Otherwise, expand the inum goal that came back.  If ibase was too low to
 * create the inode in that block then the low level split will fail and expand
 * will create a new inode table block with ibase at the goal.  We round the
 * goal down to some binary multiple in ileaf_split to reduce the chance of
 * creating inode table blocks with only a small number of inodes.  (Actually
 * we should only round down the split point, not the returned goal.)
 */

int make_inode(struct inode *inode, struct iattr *iattr)
{
	SB = inode->sb;
	int err = -ENOENT, levels = sb->itree.root.depth;
	struct path path[levels + 1];
	if ((err = probe(&sb->itree, inode->inum, path)))
		return err;
	struct buffer *leafbuf = path[levels].buffer;
	struct ileaf *leaf = to_ileaf(leafbuf->data);

	trace(warn("create inode 0x%Lx", (L)inode->inum);)
	assert(!inode->btree.root.depth);
	inum_t inum = inode->inum;
	assert(inum < next_key(path, levels));
	while (1) {
		printf("find empty inode in [%Lx] base %Lx\n", (L)leafbuf->index, (L)leaf->ibase);
		inum = find_empty_inode(&sb->itree, leafbuf->data, (L)inum);
		printf("result inum is %Lx, next is %Lx\n", (L)inum, (L)next_key(path, levels));
		if (inum < next_key(path, levels))
			break;
		int more = advance(leafbuf->map, path, levels);
		printf("no more inode space here, advance %i\n", more);
		if (!more)
			goto errout;
	}
	inode->inum = inum;
	inode->i_mode = iattr->mode;
	inode->i_uid = iattr->uid;
	inode->i_gid = iattr->gid;
	inode->i_mtime = inode->i_ctime = inode->i_atime = iattr->mtime;
	inode->i_links = 1;
	inode->btree = new_btree(sb, &dtree_ops); // error???
	inode->present = MODE_OWNER_BIT|DATA_BTREE_BIT;
	unsigned size = howbig(MODE_OWNER_BIT|DATA_BTREE_BIT);
	void *base = tree_expand(&sb->itree, inum, size, path);
	if (!base)
		goto errmem; // what was the error???
	void *attrs = encode_attrs(sb, base, size, inode);
	assert(attrs == base + size);
	release_path(path, levels + 1);
	return 0;
errmem:
	err = -ENOMEM;
	release_path(path, levels + 1);
errout:
	warn("make_inode 0x%Lx failed (%s)", (L)inode->inum, strerror(-err));
	return err;
}

int open_inode(struct inode *inode)
{
	SB = inode->sb;
	int err = -ENOENT, levels = sb->itree.root.depth;
	struct path path[levels + 1];
	if ((err = probe(&sb->itree, inode->inum, path)))
		return err;
	unsigned size;
	void *attrs = ileaf_lookup(&sb->itree, inode->inum, path[levels].buffer->data, &size);
	if (!attrs) {
		release_path(path, levels + 1);
		warn("open_inode 0x%Lx failed (%s)", (L)inode->inum, strerror(-err));
		return -ENOENT;
	}
	trace(warn("found inode 0x%Lx", (L)inode->inum);)
	//ileaf_dump(&sb->itree, path[levels].buffer->data);
	//hexdump(attrs, size);
	decode_attrs(sb, attrs, size, inode);
	dump_attrs(sb, inode);
	release_path(path, levels + 1);
	return 0;
}

int save_inode(struct inode *inode)
{
	trace(warn("save inode 0x%Lx", (L)inode->inum);)
	SB = inode->sb;
	int err, levels = sb->itree.root.depth;
	struct path path[levels + 1];
	if ((err = probe(&sb->itree, inode->inum, path)))
		return err;
	unsigned size;
	void *base = ileaf_lookup(&sb->itree, inode->inum, path[levels].buffer->data, &size);
	if (!size)
		return -EINVAL;
	if (inode->i_size)
		inode->present |= CTIME_SIZE_BIT;
	size = howbig(inode->present);
	base = tree_expand(&sb->itree, inode->inum, size, path); // error???
	void *attrs = encode_attrs(sb, base, size, inode);
	assert(attrs == base + size);
	release_path(path, levels + 1);
	dump_attrs(sb, inode);
	return 0;
}

int tuxio(struct file *file, char *data, unsigned len, int write)
{
	struct inode *inode = file->f_inode;
	printf("%s %u bytes, isize = 0x%Lx\n", write ? "write" : "read", len, (L)inode->i_size);
	loff_t pos = file->f_pos;
	if (pos + len > MAX_FILESIZE)
		return -EFBIG;
	if (!write && pos + len > inode->i_size) {
		if (pos >= inode->i_size)
			return 0;
		len = inode->i_size - pos;
	}
	unsigned bbits = inode->sb->blockbits;
	unsigned bsize = inode->sb->blocksize;
	unsigned bmask = inode->sb->blockmask;
	loff_t tail = len;
	while (tail) {
		unsigned from = pos & bmask;
		unsigned some = from + tail > bsize ? bsize - from : tail;
		int full = write && some == bsize;
		struct buffer *buffer = (full ? getblk : bread)(inode->map, pos >> bbits);
		if (!buffer) {
			errno = EIO;
			break;
		}
		if (write)
			memcpy(buffer->data + from, data, some);
		else
			memcpy(data, buffer->data + from, some);
		printf("transfer %u bytes, block 0x%Lx, buffer %p\n", some, (L)buffer->index, buffer);
		hexdump(buffer->data + from, some);
		set_buffer_dirty(buffer);
		brelse(buffer);
		tail -= some;
		data += some;
		pos += some;
	}
	file->f_pos = pos;
	if (write && inode->i_size < pos)
		inode->i_size = pos;
	return errno ? -errno : len - tail;
}

int tuxread(struct file *file, char *data, unsigned len)
{
	return tuxio(file, data, len, 0);
}

int tuxwrite(struct file *file, char *data, unsigned len)
{
	return tuxio(file, data, len, 1);
}

void tuxseek(struct file *file, loff_t pos)
{
	warn("seek to 0x%Lx", (L)pos);
	file->f_pos = pos;
}

int purge_inum(BTREE, inum_t inum)
{
	int err = -ENOENT, levels = btree->sb->itree.root.depth;
	struct path path[levels + 1];
	if (!(err = probe(btree, inum, path))) {
		err = ileaf_purge(btree, inum, to_ileaf(path[levels].buffer));
		release_path(path, levels + 1);
	}
	return err;
}

struct inode *tuxopen(struct inode *dir, char *name, int len)
{
	struct buffer *buffer;
	ext2_dirent *entry = ext2_find_entry(dir, name, len, &buffer);
	if (!buffer)
		return NULL;
	inum_t inum = entry->inum;
	brelse(buffer);
	struct inode *inode = new_inode(dir->sb, inum);
	return open_inode(inode) ? NULL : inode;
}

struct inode *tuxcreate(struct inode *dir, char *name, int len, struct iattr *iattr)
{
	struct buffer *buffer;
	ext2_dirent *entry = ext2_find_entry(dir, name, len, &buffer);
	if (entry) {
		brelse(buffer);
		return NULL; // err_ptr(-EEXIST) ???
	}
	/*
	 * For now the inum allocation goal is the same as the block allocation
	 * goal.  This allows a maximum inum density of one per block and should
	 * give pretty good spacial correlation between inode table blocks and
	 * file data belonging to those inodes provided somebody sets the block
	 * allocation goal based on the directory the file will be in.
	 */
	struct inode *inode = new_inode(dir->sb, dir->sb->nextalloc);
	if (!inode)
		return NULL; // err ???
	int err = make_inode(inode, iattr);
	if (err)
		return NULL; // err ???
	if (!ext2_create_entry(dir, name, len, inode->inum, iattr->mode))
		return inode;
	purge_inum(&dir->sb->itree, inode->inum); // test me!!!
	free_inode(inode);
	inode = NULL;
	return NULL; // err ???
}

void tuxflush(struct inode *inode)
{
	int err = flush_buffers(inode->map);
	if (err)
		warn("Sync failed (%s)", strerror(-err));
	//encode_csize(attrs, 0, inode->i_size);
}

void tuxclose(struct inode *inode)
{
	tuxflush(inode);
	free_inode(inode);
}

void init_tux3(SB) // why am I separate?
{
	struct inode *bitmap = new_inode(sb, 0);
	sb->bitmap = bitmap;
	sb->super.blockbits = sb->devmap->dev->bits;
	sb->blocksize = 1 << sb->super.blockbits;
	sb->itree = new_btree(sb, &itree_ops);
	sb->itree.entries_per_leaf = 64; // !!! should depend on blocksize
	bitmap->btree = new_btree(sb, &dtree_ops);
}

int main(int argc, char *argv[])
{
	int err = 0;
	char *name = argv[1];
	fd_t fd = open(name, O_CREAT|O_TRUNC|O_RDWR, S_IRWXU);
	ftruncate(fd, 1 << 24);
	u64 size = 0;
	if (fdsize64(fd, &size))
		error("fdsize64 failed for '%s' (%s)", name, strerror(errno));
	printf("fd '%s' = %i (0x%Lx bytes)\n", name, fd, (L)size);

	struct dev *dev = &(struct dev){ fd, .bits = 12 };
	struct map *map = new_map(dev, NULL);
	struct sb *sb = &(struct sb){
		.super = { .magic = SB_MAGIC, .blocks = size >> dev->bits },
		.max_inodes_per_block = 64,
		.entries_per_node = 20,
		.devmap = map,
		.blockbits = dev->bits,
		.blocksize = 1 << dev->bits,
		.blockmask = (1 << dev->bits) - 1,
		.nextalloc = 0x40,
	};

	init_buffers(dev, 1 << 20);
	init_tux3(sb);

	printf("---- create root ----\n");
	struct inode *root = new_inode(sb, 0xd);
	make_inode(root, &(struct iattr){ .mode = S_IFREG | S_IRWXU }); // error???
	printf("---- create file ----\n");
	struct inode *inode = tuxcreate(root, "foo", 3, &(struct iattr){ .mode = S_IFREG | S_IRWXU });
	if (!inode)
		return 1;
	ext2_dump_entries(getblk(root->map, 0), sb->blocksize);

	printf("---- write file ----\n");
	char buf[100] = { };
	struct file *file = &(struct file){ .f_inode = inode };
	tuxseek(file, (1LL << 60) - 12);
	tuxseek(file, 4092);
	err = tuxwrite(file, "hello ", 6);
	err = tuxwrite(file, "world!", 6);
#if 0
	tuxflush(sb->bitmap);
	flush_buffers(sb->devmap);
#endif
#if 1
	printf("---- close file ----\n");
	save_inode(inode);
	tuxclose(inode);
	printf("---- open file ----\n");
	file = &(struct file){ .f_inode = tuxopen(root, "foo", 3) };
#endif

	printf("---- read file ----\n");
	tuxseek(file, (1LL << 60) - 12);
	tuxseek(file, 4092);
	memset(buf, 0, sizeof(buf));
	int got = tuxread(file, buf, sizeof(buf));
	//printf("got %x bytes\n", got);
	if (got < 0)
		return 1;
	hexdump(buf, got);
	printf("---- show state ----\n");
	show_buffers(file->f_inode->map);
	show_buffers(root->map);
	show_buffers(sb->devmap);
	bitmap_dump(sb->bitmap, 0, sb->super.blocks);
	show_tree_range(&sb->itree, 0, -1);
	return 0;
}
