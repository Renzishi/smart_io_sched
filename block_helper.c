#include <linux/types.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/container_of.h>
#include <linux/rcupdate.h>
#include <linux/pagemap.h>
#include <linux/blk-cgroup.h>
#include <linux/cgroup.h>

#include <block/bfq-iosched.h>
#include <block/elevator.h>

#include "smart_io_types.h"
#include "smart_io_log.h"

static inline u16 get_file_ext_id(const char *ext)
{
	if (!ext)
		return SMART_IO_FILE_EXT_UNKNOWN;
	if (!strcmp(ext, ".db"))
		return SMART_IO_FILE_EXT_DB;
	if (!strcmp(ext, ".so"))
		return SMART_IO_FILE_EXT_SO;
	if (!strcmp(ext, ".dex"))
		return SMART_IO_FILE_EXT_DEX;
	if (!strcmp(ext, ".apk"))
		return SMART_IO_FILE_EXT_APK;
	if (!strcmp(ext, ".log"))
		return SMART_IO_FILE_EXT_LOG;
	if (!strcmp(ext, ".vdex"))
		return SMART_IO_FILE_EXT_VDEX;
	if (!strcmp(ext, ".odex"))
		return SMART_IO_FILE_EXT_ODEX;
	return SMART_IO_FILE_EXT_UNKNOWN;
}

static inline const char *find_file_ext(const char *name)
{
	const char *ext;
	ext = strrchr(name, '.');
	if (!ext || !ext[1])
		return NULL;
	return ext;
}

static inline void extract_fs_type_from_inode(struct inode *inode,
					      char *fs_type,
					      size_t fs_type_len)
{
	struct super_block *sb;

	sb = inode->i_sb;
	if (!sb->s_type || !sb->s_type->name)
		return;

	strscpy(fs_type, sb->s_type->name, fs_type_len);
}

static char *blk_get_pathname(struct inode *inode, char *buf, size_t buf_len)
{
	struct dentry *d;
	char *full_path;

	if (!inode)
		return ERR_PTR(-EINVAL);

	d = d_find_alias(inode);
	if (!d)
		return ERR_PTR(-ENOENT);

	full_path = dentry_path_raw(d, buf, buf_len);
	if (IS_ERR(full_path)) {
		long err = PTR_ERR(full_path);

		dput(d);
		return ERR_PTR(err);
	}

	dput(d);

	return full_path;
}

static inline void set_vfs_placeholder(char *ext_str, size_t ext_str_len,
			       const char *value)
{
	if (!ext_str || !ext_str_len || !value)
		return;

	strscpy(ext_str, value, ext_str_len);
}

static const char *inode_fs_name(struct inode *inode)
{
	struct super_block *sb;

	if (!inode)
		return "unknown";

	sb = inode->i_sb;
	if (!sb || !sb->s_type || !sb->s_type->name)
		return "unknown";

	return sb->s_type->name;
}

static const char *format_private_inode_class(const char *fs_name,
				      char *buf, size_t buf_len)
{
	ssize_t copied;

	if (!buf || !buf_len)
		return "private";

	copied = strscpy(buf, "private_", buf_len);
	if (copied < 0 || copied >= (ssize_t)buf_len)
		return buf;

	strscpy(buf + copied, fs_name, buf_len - copied);
	return buf;
}

static const char *classify_mapping_reason(struct folio *folio,
					   struct address_space *mapping,
					   bool *stable)
{
	*stable = false;
	if (mapping)
		return NULL;

	if (folio_test_swapcache(folio)) {
		*stable = true;
		return "swap_nomap";
	}

	if (folio_test_anon(folio)) {
		*stable = true;
		return "anon_nomap";
	}

	return "nomapping";
}

static const char *classify_hostless_mapping(struct folio *folio, bool *stable)
{
	*stable = false;

	if (folio_test_swapcache(folio)) {
		*stable = true;
		return "swap_nohost";
	}

	if (folio_test_anon(folio)) {
		*stable = true;
		return "anon_nohost";
	}

	return "hostless";
}

static const char *classify_inode_identity(struct inode *inode,
					   bool *stable,
					   char *buf, size_t buf_len)
{
	const char *fs_name;

	*stable = false;
	if (!inode)
		return "null_inode";

	fs_name = inode_fs_name(inode);

	if (inode->i_flags & S_PRIVATE) {
		*stable = true;
		return format_private_inode_class(fs_name, buf, buf_len);
	}

	if (S_ISBLK(inode->i_mode)) {
		*stable = true;
		return "blkdev_file";
	}

	if (S_ISCHR(inode->i_mode)) {
		*stable = true;
		return "chrdev_file";
	}

	if (!strcmp(fs_name, "tmpfs") ||
	    !strcmp(fs_name, "overlay") ||
	    !strcmp(fs_name, "fuse") ||
	    !strcmp(fs_name, "nfs") ||
	    !strcmp(fs_name, "pipefs") ||
	    !strcmp(fs_name, "anon_inodefs")) {
		*stable = true;
		return fs_name;
	}

	if (!inode->i_ino)
		return "ino_zero";

	if (!strcmp(fs_name, "ext4") && inode->i_ino <= 11) {
		*stable = true;
		return "ext4_reserved";
	}

	return NULL;
}

bool extract_vfs_info(struct bio *bio, u64 *inode_hash, u16 *ext, u64 *folio_index,
		      char *ext_str, size_t ext_str_len,
		      char *fs_type, size_t fs_type_len)
{
	struct address_space *mapping = NULL;
	struct inode *inode = NULL;
	struct folio *folio;
	struct dentry *dentry;
	struct page *page;
	const char *reason;
	bool stable;
	dev_t s_dev;
	char class_buf[64];
	char path_buf[512];
	char *full_path;
	const char *file_name;

	if (!bio_has_data(bio)) {
		set_vfs_placeholder(ext_str, ext_str_len, "nodata");
		return false;
	}

	page = bio_page(bio);
	if (!page) {
		set_vfs_placeholder(ext_str, ext_str_len, "nopage");
		return false;
	}

	rcu_read_lock();
	folio = page_folio(page);
	*folio_index = folio->index;
	mapping = folio_mapping(folio);
	reason = classify_mapping_reason(folio, mapping, &stable);
	if (reason) {
		rcu_read_unlock();
		set_vfs_placeholder(ext_str, ext_str_len, reason);
		return stable;
	}

	inode = mapping->host;
	if (!inode) {
		reason = classify_hostless_mapping(folio, &stable);
		rcu_read_unlock();
		set_vfs_placeholder(ext_str, ext_str_len, reason);
		return stable;
	}

	if (inode->i_sb) {
		s_dev = inode->i_sb->s_dev;
		extract_fs_type_from_inode(inode, fs_type, fs_type_len);
	} else {
		s_dev = inode->i_rdev;
	}

	*inode_hash = (u64)inode->i_ino ^ ((u64)s_dev << 32);
	reason = classify_inode_identity(inode, &stable, class_buf,
					 sizeof(class_buf));
	if (reason) {
		rcu_read_unlock();
		set_vfs_placeholder(ext_str, ext_str_len, reason);
		return stable;
	}
	
	hlist_for_each_entry_rcu(dentry, &inode->i_dentry, d_u.d_alias) {
		const char *file_ext;

		if (!dentry || !dentry->d_name.name || !dentry->d_name.len)
			continue;

		file_ext = find_file_ext(dentry->d_name.name);
		if (file_ext) {
			strscpy(ext_str, file_ext, ext_str_len);
			*ext = get_file_ext_id(file_ext);
		} else {
			strscpy(ext_str, "none", ext_str_len);
		}
		break;
	}
	rcu_read_unlock();

	full_path = blk_get_pathname(inode, path_buf, 512);
	if (IS_ERR(full_path)) {
		set_vfs_placeholder(ext_str, ext_str_len, "nopath");
		return false;
	}

	file_name = strrchr(full_path, '/');
	if (file_name)
		file_name++;
	if (!file_name || !*file_name)
		file_name = full_path;
	strscpy(ext_str, file_name, ext_str_len);
	return true;
}

void extract_device_name(struct request *rq, char *dev_name, size_t dev_name_len)
{
	struct bio *bio = rq->bio;
	if (bio && bio->bi_bdev && bio->bi_bdev->bd_disk) {
		strscpy(dev_name, bio->bi_bdev->bd_disk->disk_name,
			dev_name_len);
	} else if (rq->q && rq->q->disk) {
		strscpy(dev_name, rq->q->disk->disk_name, dev_name_len);
	}
}

u32 get_cgroup_weight(struct request *rq)
{
	return CGROUP_WEIGHT_DFL;
}
