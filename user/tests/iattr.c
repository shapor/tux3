/*
 * Inode table attributes
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"
#include "test.h"

#ifndef trace
#define trace trace_off
#endif

#include "kernel/iattr.c"

/* Test encode_attrs() and decode_attrs() */
static void test01(struct sb *sb)
{
	unsigned abits = RDEV_BIT|MODE_OWNER_BIT|CTIME_SIZE_BIT|LINK_COUNT_BIT|MTIME_BIT;
	struct inode *inode1 = rapid_open_inode(sb, NULL, S_IFCHR | 0644);
	struct inode *inode2 = rapid_open_inode(sb, NULL, 0x666);

	tux3_iattrdirty(inode1);

	inode1->i_rdev	= MKDEV(1, 3);
	i_uid_write(inode1, 0x12121212);
	i_gid_write(inode1, 0x34343434);
	inode1->i_size	= 0x123456789ULL;
	inode1->i_ctime	= spectime(0xdec0de01dec0de02ULL);
	inode1->i_mtime	= spectime(0xbadface1badface2ULL);
	tux_inode(inode1)->present = abits;
	tux_inode(inode1)->btree = (struct btree){
		.root = { .block = 0xcaba1f00dULL, .depth = 3 },
	};

	tux3_mark_inode_dirty(inode1);

	char *p, attrs[1000] = { };
	int size;

	struct tux3_iattr_data idata;
	tux3_iattr_read_and_clear(inode1, &idata, sb->delta);

	struct iattr_req_data iattr_data = {
		.idata	= &idata,
		.btree	= &tux_inode(inode1)->btree,
	};

	/* encode inode1 to attrs, then decode attrs to inode2 */
	size = encode_asize(idata.present);
	p = encode_attrs(itable_btree(sb), &iattr_data, attrs, size);
	test_assert(p - attrs == size);
	p = decode_attrs(inode2, attrs, size);
	test_assert(p - attrs == size);

	/* Compare inode1 and inode2 */
	struct tux3_inode *tuxnode1 = tux_inode(inode1);
	struct tux3_inode *tuxnode2 = tux_inode(inode2);
	test_assert(tuxnode1->present == tuxnode2->present);
	test_assert(inode1->i_rdev == inode2->i_rdev);
	test_assert(inode1->i_mode == inode2->i_mode);
	test_assert(uid_eq(inode1->i_uid, inode2->i_uid));
	test_assert(gid_eq(inode1->i_gid, inode2->i_gid));
	test_assert(inode1->i_size == inode2->i_size);
	test_assert(inode1->i_ctime.tv_sec == inode2->i_ctime.tv_sec);
	test_assert(inode1->i_ctime.tv_nsec == inode2->i_ctime.tv_nsec);
	test_assert(inode1->i_mtime.tv_sec == inode2->i_mtime.tv_sec);
	test_assert(inode1->i_mtime.tv_nsec == inode2->i_mtime.tv_nsec);
	test_assert(tuxnode1->btree.root.block == tuxnode2->btree.root.block);
	test_assert(tuxnode1->btree.root.depth == tuxnode2->btree.root.depth);

	free_map(inode1->map);
	free_map(inode2->map);
}

int main(int argc, char *argv[])
{
	struct dev *dev = &(struct dev){ .bits = 9 };

	struct disksuper super = INIT_DISKSB(dev->bits, 100);
	struct sb *sb = rapid_sb(dev);
	sb->super = super;
	setup_sb(sb, &super);

	test_init(argv[0]);

	if (test_start("test01"))
		test01(sb);
	test_end();

	return test_failures();
}
