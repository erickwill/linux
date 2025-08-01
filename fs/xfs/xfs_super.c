// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_bmap.h"
#include "xfs_alloc.h"
#include "xfs_fsops.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_dir2.h"
#include "xfs_extfree_item.h"
#include "xfs_mru_cache.h"
#include "xfs_inode_item.h"
#include "xfs_icache.h"
#include "xfs_trace.h"
#include "xfs_icreate_item.h"
#include "xfs_filestream.h"
#include "xfs_quota.h"
#include "xfs_sysfs.h"
#include "xfs_ondisk.h"
#include "xfs_rmap_item.h"
#include "xfs_refcount_item.h"
#include "xfs_bmap_item.h"
#include "xfs_reflink.h"
#include "xfs_pwork.h"
#include "xfs_ag.h"
#include "xfs_defer.h"
#include "xfs_attr_item.h"
#include "xfs_xattr.h"
#include "xfs_iunlink_item.h"
#include "xfs_dahash_test.h"
#include "xfs_rtbitmap.h"
#include "xfs_exchmaps_item.h"
#include "xfs_parent.h"
#include "xfs_rtalloc.h"
#include "xfs_zone_alloc.h"
#include "scrub/stats.h"
#include "scrub/rcbag_btree.h"

#include <linux/magic.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>

static const struct super_operations xfs_super_operations;

static struct dentry *xfs_debugfs;	/* top-level xfs debugfs dir */
static struct kset *xfs_kset;		/* top-level xfs sysfs dir */
#ifdef DEBUG
static struct xfs_kobj xfs_dbg_kobj;	/* global debug sysfs attrs */
#endif

enum xfs_dax_mode {
	XFS_DAX_INODE = 0,
	XFS_DAX_ALWAYS = 1,
	XFS_DAX_NEVER = 2,
};

/* Were quota mount options provided?  Must use the upper 16 bits of qflags. */
#define XFS_QFLAGS_MNTOPTS	(1U << 31)

static void
xfs_mount_set_dax_mode(
	struct xfs_mount	*mp,
	enum xfs_dax_mode	mode)
{
	switch (mode) {
	case XFS_DAX_INODE:
		mp->m_features &= ~(XFS_FEAT_DAX_ALWAYS | XFS_FEAT_DAX_NEVER);
		break;
	case XFS_DAX_ALWAYS:
		mp->m_features |= XFS_FEAT_DAX_ALWAYS;
		mp->m_features &= ~XFS_FEAT_DAX_NEVER;
		break;
	case XFS_DAX_NEVER:
		mp->m_features |= XFS_FEAT_DAX_NEVER;
		mp->m_features &= ~XFS_FEAT_DAX_ALWAYS;
		break;
	}
}

static const struct constant_table dax_param_enums[] = {
	{"inode",	XFS_DAX_INODE },
	{"always",	XFS_DAX_ALWAYS },
	{"never",	XFS_DAX_NEVER },
	{}
};

/*
 * Table driven mount option parser.
 */
enum {
	Opt_logbufs, Opt_logbsize, Opt_logdev, Opt_rtdev,
	Opt_wsync, Opt_noalign, Opt_swalloc, Opt_sunit, Opt_swidth, Opt_nouuid,
	Opt_grpid, Opt_nogrpid, Opt_bsdgroups, Opt_sysvgroups,
	Opt_allocsize, Opt_norecovery, Opt_inode64, Opt_inode32, Opt_ikeep,
	Opt_noikeep, Opt_largeio, Opt_nolargeio, Opt_attr2, Opt_noattr2,
	Opt_filestreams, Opt_quota, Opt_noquota, Opt_usrquota, Opt_grpquota,
	Opt_prjquota, Opt_uquota, Opt_gquota, Opt_pquota,
	Opt_uqnoenforce, Opt_gqnoenforce, Opt_pqnoenforce, Opt_qnoenforce,
	Opt_discard, Opt_nodiscard, Opt_dax, Opt_dax_enum, Opt_max_open_zones,
	Opt_lifetime, Opt_nolifetime, Opt_max_atomic_write,
};

static const struct fs_parameter_spec xfs_fs_parameters[] = {
	fsparam_u32("logbufs",		Opt_logbufs),
	fsparam_string("logbsize",	Opt_logbsize),
	fsparam_string("logdev",	Opt_logdev),
	fsparam_string("rtdev",		Opt_rtdev),
	fsparam_flag("wsync",		Opt_wsync),
	fsparam_flag("noalign",		Opt_noalign),
	fsparam_flag("swalloc",		Opt_swalloc),
	fsparam_u32("sunit",		Opt_sunit),
	fsparam_u32("swidth",		Opt_swidth),
	fsparam_flag("nouuid",		Opt_nouuid),
	fsparam_flag("grpid",		Opt_grpid),
	fsparam_flag("nogrpid",		Opt_nogrpid),
	fsparam_flag("bsdgroups",	Opt_bsdgroups),
	fsparam_flag("sysvgroups",	Opt_sysvgroups),
	fsparam_string("allocsize",	Opt_allocsize),
	fsparam_flag("norecovery",	Opt_norecovery),
	fsparam_flag("inode64",		Opt_inode64),
	fsparam_flag("inode32",		Opt_inode32),
	fsparam_flag("ikeep",		Opt_ikeep),
	fsparam_flag("noikeep",		Opt_noikeep),
	fsparam_flag("largeio",		Opt_largeio),
	fsparam_flag("nolargeio",	Opt_nolargeio),
	fsparam_flag("attr2",		Opt_attr2),
	fsparam_flag("noattr2",		Opt_noattr2),
	fsparam_flag("filestreams",	Opt_filestreams),
	fsparam_flag("quota",		Opt_quota),
	fsparam_flag("noquota",		Opt_noquota),
	fsparam_flag("usrquota",	Opt_usrquota),
	fsparam_flag("grpquota",	Opt_grpquota),
	fsparam_flag("prjquota",	Opt_prjquota),
	fsparam_flag("uquota",		Opt_uquota),
	fsparam_flag("gquota",		Opt_gquota),
	fsparam_flag("pquota",		Opt_pquota),
	fsparam_flag("uqnoenforce",	Opt_uqnoenforce),
	fsparam_flag("gqnoenforce",	Opt_gqnoenforce),
	fsparam_flag("pqnoenforce",	Opt_pqnoenforce),
	fsparam_flag("qnoenforce",	Opt_qnoenforce),
	fsparam_flag("discard",		Opt_discard),
	fsparam_flag("nodiscard",	Opt_nodiscard),
	fsparam_flag("dax",		Opt_dax),
	fsparam_enum("dax",		Opt_dax_enum, dax_param_enums),
	fsparam_u32("max_open_zones",	Opt_max_open_zones),
	fsparam_flag("lifetime",	Opt_lifetime),
	fsparam_flag("nolifetime",	Opt_nolifetime),
	fsparam_string("max_atomic_write",	Opt_max_atomic_write),
	{}
};

struct proc_xfs_info {
	uint64_t	flag;
	char		*str;
};

static int
xfs_fs_show_options(
	struct seq_file		*m,
	struct dentry		*root)
{
	static struct proc_xfs_info xfs_info_set[] = {
		/* the few simple ones we can get from the mount struct */
		{ XFS_FEAT_IKEEP,		",ikeep" },
		{ XFS_FEAT_WSYNC,		",wsync" },
		{ XFS_FEAT_NOALIGN,		",noalign" },
		{ XFS_FEAT_SWALLOC,		",swalloc" },
		{ XFS_FEAT_NOUUID,		",nouuid" },
		{ XFS_FEAT_NORECOVERY,		",norecovery" },
		{ XFS_FEAT_ATTR2,		",attr2" },
		{ XFS_FEAT_FILESTREAMS,		",filestreams" },
		{ XFS_FEAT_GRPID,		",grpid" },
		{ XFS_FEAT_DISCARD,		",discard" },
		{ XFS_FEAT_LARGE_IOSIZE,	",largeio" },
		{ XFS_FEAT_DAX_ALWAYS,		",dax=always" },
		{ XFS_FEAT_DAX_NEVER,		",dax=never" },
		{ XFS_FEAT_NOLIFETIME,		",nolifetime" },
		{ 0, NULL }
	};
	struct xfs_mount	*mp = XFS_M(root->d_sb);
	struct proc_xfs_info	*xfs_infop;

	for (xfs_infop = xfs_info_set; xfs_infop->flag; xfs_infop++) {
		if (mp->m_features & xfs_infop->flag)
			seq_puts(m, xfs_infop->str);
	}

	seq_printf(m, ",inode%d", xfs_has_small_inums(mp) ? 32 : 64);

	if (xfs_has_allocsize(mp))
		seq_printf(m, ",allocsize=%dk",
			   (1 << mp->m_allocsize_log) >> 10);

	if (mp->m_logbufs > 0)
		seq_printf(m, ",logbufs=%d", mp->m_logbufs);
	if (mp->m_logbsize > 0)
		seq_printf(m, ",logbsize=%dk", mp->m_logbsize >> 10);

	if (mp->m_logname)
		seq_show_option(m, "logdev", mp->m_logname);
	if (mp->m_rtname)
		seq_show_option(m, "rtdev", mp->m_rtname);

	if (mp->m_dalign > 0)
		seq_printf(m, ",sunit=%d",
				(int)XFS_FSB_TO_BB(mp, mp->m_dalign));
	if (mp->m_swidth > 0)
		seq_printf(m, ",swidth=%d",
				(int)XFS_FSB_TO_BB(mp, mp->m_swidth));

	if (mp->m_qflags & XFS_UQUOTA_ENFD)
		seq_puts(m, ",usrquota");
	else if (mp->m_qflags & XFS_UQUOTA_ACCT)
		seq_puts(m, ",uqnoenforce");

	if (mp->m_qflags & XFS_PQUOTA_ENFD)
		seq_puts(m, ",prjquota");
	else if (mp->m_qflags & XFS_PQUOTA_ACCT)
		seq_puts(m, ",pqnoenforce");

	if (mp->m_qflags & XFS_GQUOTA_ENFD)
		seq_puts(m, ",grpquota");
	else if (mp->m_qflags & XFS_GQUOTA_ACCT)
		seq_puts(m, ",gqnoenforce");

	if (!(mp->m_qflags & XFS_ALL_QUOTA_ACCT))
		seq_puts(m, ",noquota");

	if (mp->m_max_open_zones)
		seq_printf(m, ",max_open_zones=%u", mp->m_max_open_zones);
	if (mp->m_awu_max_bytes)
		seq_printf(m, ",max_atomic_write=%lluk",
				mp->m_awu_max_bytes >> 10);

	return 0;
}

static bool
xfs_set_inode_alloc_perag(
	struct xfs_perag	*pag,
	xfs_ino_t		ino,
	xfs_agnumber_t		max_metadata)
{
	if (!xfs_is_inode32(pag_mount(pag))) {
		set_bit(XFS_AGSTATE_ALLOWS_INODES, &pag->pag_opstate);
		clear_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
		return false;
	}

	if (ino > XFS_MAXINUMBER_32) {
		clear_bit(XFS_AGSTATE_ALLOWS_INODES, &pag->pag_opstate);
		clear_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
		return false;
	}

	set_bit(XFS_AGSTATE_ALLOWS_INODES, &pag->pag_opstate);
	if (pag_agno(pag) < max_metadata)
		set_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
	else
		clear_bit(XFS_AGSTATE_PREFERS_METADATA, &pag->pag_opstate);
	return true;
}

/*
 * Set parameters for inode allocation heuristics, taking into account
 * filesystem size and inode32/inode64 mount options; i.e. specifically
 * whether or not XFS_FEAT_SMALL_INUMS is set.
 *
 * Inode allocation patterns are altered only if inode32 is requested
 * (XFS_FEAT_SMALL_INUMS), and the filesystem is sufficiently large.
 * If altered, XFS_OPSTATE_INODE32 is set as well.
 *
 * An agcount independent of that in the mount structure is provided
 * because in the growfs case, mp->m_sb.sb_agcount is not yet updated
 * to the potentially higher ag count.
 *
 * Returns the maximum AG index which may contain inodes.
 */
xfs_agnumber_t
xfs_set_inode_alloc(
	struct xfs_mount *mp,
	xfs_agnumber_t	agcount)
{
	xfs_agnumber_t	index;
	xfs_agnumber_t	maxagi = 0;
	xfs_sb_t	*sbp = &mp->m_sb;
	xfs_agnumber_t	max_metadata;
	xfs_agino_t	agino;
	xfs_ino_t	ino;

	/*
	 * Calculate how much should be reserved for inodes to meet
	 * the max inode percentage.  Used only for inode32.
	 */
	if (M_IGEO(mp)->maxicount) {
		uint64_t	icount;

		icount = sbp->sb_dblocks * sbp->sb_imax_pct;
		do_div(icount, 100);
		icount += sbp->sb_agblocks - 1;
		do_div(icount, sbp->sb_agblocks);
		max_metadata = icount;
	} else {
		max_metadata = agcount;
	}

	/* Get the last possible inode in the filesystem */
	agino =	XFS_AGB_TO_AGINO(mp, sbp->sb_agblocks - 1);
	ino = XFS_AGINO_TO_INO(mp, agcount - 1, agino);

	/*
	 * If user asked for no more than 32-bit inodes, and the fs is
	 * sufficiently large, set XFS_OPSTATE_INODE32 if we must alter
	 * the allocator to accommodate the request.
	 */
	if (xfs_has_small_inums(mp) && ino > XFS_MAXINUMBER_32)
		xfs_set_inode32(mp);
	else
		xfs_clear_inode32(mp);

	for (index = 0; index < agcount; index++) {
		struct xfs_perag	*pag;

		ino = XFS_AGINO_TO_INO(mp, index, agino);

		pag = xfs_perag_get(mp, index);
		if (xfs_set_inode_alloc_perag(pag, ino, max_metadata))
			maxagi++;
		xfs_perag_put(pag);
	}

	return xfs_is_inode32(mp) ? maxagi : agcount;
}

static int
xfs_setup_dax_always(
	struct xfs_mount	*mp)
{
	if (!mp->m_ddev_targp->bt_daxdev &&
	    (!mp->m_rtdev_targp || !mp->m_rtdev_targp->bt_daxdev)) {
		xfs_alert(mp,
			"DAX unsupported by block device. Turning off DAX.");
		goto disable_dax;
	}

	if (mp->m_super->s_blocksize != PAGE_SIZE) {
		xfs_alert(mp,
			"DAX not supported for blocksize. Turning off DAX.");
		goto disable_dax;
	}

	if (xfs_has_reflink(mp) &&
	    bdev_is_partition(mp->m_ddev_targp->bt_bdev)) {
		xfs_alert(mp,
			"DAX and reflink cannot work with multi-partitions!");
		return -EINVAL;
	}

	return 0;

disable_dax:
	xfs_mount_set_dax_mode(mp, XFS_DAX_NEVER);
	return 0;
}

STATIC int
xfs_blkdev_get(
	xfs_mount_t		*mp,
	const char		*name,
	struct file		**bdev_filep)
{
	int			error = 0;
	blk_mode_t		mode;

	mode = sb_open_mode(mp->m_super->s_flags);
	*bdev_filep = bdev_file_open_by_path(name, mode,
			mp->m_super, &fs_holder_ops);
	if (IS_ERR(*bdev_filep)) {
		error = PTR_ERR(*bdev_filep);
		*bdev_filep = NULL;
		xfs_warn(mp, "Invalid device [%s], error=%d", name, error);
	}

	return error;
}

STATIC void
xfs_shutdown_devices(
	struct xfs_mount	*mp)
{
	/*
	 * Udev is triggered whenever anyone closes a block device or unmounts
	 * a file systemm on a block device.
	 * The default udev rules invoke blkid to read the fs super and create
	 * symlinks to the bdev under /dev/disk.  For this, it uses buffered
	 * reads through the page cache.
	 *
	 * xfs_db also uses buffered reads to examine metadata.  There is no
	 * coordination between xfs_db and udev, which means that they can run
	 * concurrently.  Note there is no coordination between the kernel and
	 * blkid either.
	 *
	 * On a system with 64k pages, the page cache can cache the superblock
	 * and the root inode (and hence the root directory) with the same 64k
	 * page.  If udev spawns blkid after the mkfs and the system is busy
	 * enough that it is still running when xfs_db starts up, they'll both
	 * read from the same page in the pagecache.
	 *
	 * The unmount writes updated inode metadata to disk directly.  The XFS
	 * buffer cache does not use the bdev pagecache, so it needs to
	 * invalidate that pagecache on unmount.  If the above scenario occurs,
	 * the pagecache no longer reflects what's on disk, xfs_db reads the
	 * stale metadata, and fails to find /a.  Most of the time this succeeds
	 * because closing a bdev invalidates the page cache, but when processes
	 * race, everyone loses.
	 */
	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp) {
		blkdev_issue_flush(mp->m_logdev_targp->bt_bdev);
		invalidate_bdev(mp->m_logdev_targp->bt_bdev);
	}
	if (mp->m_rtdev_targp) {
		blkdev_issue_flush(mp->m_rtdev_targp->bt_bdev);
		invalidate_bdev(mp->m_rtdev_targp->bt_bdev);
	}
	blkdev_issue_flush(mp->m_ddev_targp->bt_bdev);
	invalidate_bdev(mp->m_ddev_targp->bt_bdev);
}

/*
 * The file system configurations are:
 *	(1) device (partition) with data and internal log
 *	(2) logical volume with data and log subvolumes.
 *	(3) logical volume with data, log, and realtime subvolumes.
 *
 * We only have to handle opening the log and realtime volumes here if
 * they are present.  The data subvolume has already been opened by
 * get_sb_bdev() and is stored in sb->s_bdev.
 */
STATIC int
xfs_open_devices(
	struct xfs_mount	*mp)
{
	struct super_block	*sb = mp->m_super;
	struct block_device	*ddev = sb->s_bdev;
	struct file		*logdev_file = NULL, *rtdev_file = NULL;
	int			error;

	/*
	 * Open real time and log devices - order is important.
	 */
	if (mp->m_logname) {
		error = xfs_blkdev_get(mp, mp->m_logname, &logdev_file);
		if (error)
			return error;
	}

	if (mp->m_rtname) {
		error = xfs_blkdev_get(mp, mp->m_rtname, &rtdev_file);
		if (error)
			goto out_close_logdev;

		if (file_bdev(rtdev_file) == ddev ||
		    (logdev_file &&
		     file_bdev(rtdev_file) == file_bdev(logdev_file))) {
			xfs_warn(mp,
	"Cannot mount filesystem with identical rtdev and ddev/logdev.");
			error = -EINVAL;
			goto out_close_rtdev;
		}
	}

	/*
	 * Setup xfs_mount buffer target pointers
	 */
	mp->m_ddev_targp = xfs_alloc_buftarg(mp, sb->s_bdev_file);
	if (IS_ERR(mp->m_ddev_targp)) {
		error = PTR_ERR(mp->m_ddev_targp);
		mp->m_ddev_targp = NULL;
		goto out_close_rtdev;
	}

	if (rtdev_file) {
		mp->m_rtdev_targp = xfs_alloc_buftarg(mp, rtdev_file);
		if (IS_ERR(mp->m_rtdev_targp)) {
			error = PTR_ERR(mp->m_rtdev_targp);
			mp->m_rtdev_targp = NULL;
			goto out_free_ddev_targ;
		}
	}

	if (logdev_file && file_bdev(logdev_file) != ddev) {
		mp->m_logdev_targp = xfs_alloc_buftarg(mp, logdev_file);
		if (IS_ERR(mp->m_logdev_targp)) {
			error = PTR_ERR(mp->m_logdev_targp);
			mp->m_logdev_targp = NULL;
			goto out_free_rtdev_targ;
		}
	} else {
		mp->m_logdev_targp = mp->m_ddev_targp;
		/* Handle won't be used, drop it */
		if (logdev_file)
			bdev_fput(logdev_file);
	}

	return 0;

 out_free_rtdev_targ:
	if (mp->m_rtdev_targp)
		xfs_free_buftarg(mp->m_rtdev_targp);
 out_free_ddev_targ:
	xfs_free_buftarg(mp->m_ddev_targp);
 out_close_rtdev:
	 if (rtdev_file)
		bdev_fput(rtdev_file);
 out_close_logdev:
	if (logdev_file)
		bdev_fput(logdev_file);
	return error;
}

/*
 * Setup xfs_mount buffer target pointers based on superblock
 */
STATIC int
xfs_setup_devices(
	struct xfs_mount	*mp)
{
	int			error;

	error = xfs_configure_buftarg(mp->m_ddev_targp, mp->m_sb.sb_sectsize);
	if (error)
		return error;

	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp) {
		unsigned int	log_sector_size = BBSIZE;

		if (xfs_has_sector(mp))
			log_sector_size = mp->m_sb.sb_logsectsize;
		error = xfs_configure_buftarg(mp->m_logdev_targp,
					    log_sector_size);
		if (error)
			return error;
	}

	if (mp->m_sb.sb_rtstart) {
		if (mp->m_rtdev_targp) {
			xfs_warn(mp,
		"can't use internal and external rtdev at the same time");
			return -EINVAL;
		}
		mp->m_rtdev_targp = mp->m_ddev_targp;
	} else if (mp->m_rtname) {
		error = xfs_configure_buftarg(mp->m_rtdev_targp,
					    mp->m_sb.sb_sectsize);
		if (error)
			return error;
	}

	return 0;
}

STATIC int
xfs_init_mount_workqueues(
	struct xfs_mount	*mp)
{
	mp->m_buf_workqueue = alloc_workqueue("xfs-buf/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM),
			1, mp->m_super->s_id);
	if (!mp->m_buf_workqueue)
		goto out;

	mp->m_unwritten_workqueue = alloc_workqueue("xfs-conv/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM),
			0, mp->m_super->s_id);
	if (!mp->m_unwritten_workqueue)
		goto out_destroy_buf;

	mp->m_reclaim_workqueue = alloc_workqueue("xfs-reclaim/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM),
			0, mp->m_super->s_id);
	if (!mp->m_reclaim_workqueue)
		goto out_destroy_unwritten;

	mp->m_blockgc_wq = alloc_workqueue("xfs-blockgc/%s",
			XFS_WQFLAGS(WQ_UNBOUND | WQ_FREEZABLE | WQ_MEM_RECLAIM),
			0, mp->m_super->s_id);
	if (!mp->m_blockgc_wq)
		goto out_destroy_reclaim;

	mp->m_inodegc_wq = alloc_workqueue("xfs-inodegc/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM),
			1, mp->m_super->s_id);
	if (!mp->m_inodegc_wq)
		goto out_destroy_blockgc;

	mp->m_sync_workqueue = alloc_workqueue("xfs-sync/%s",
			XFS_WQFLAGS(WQ_FREEZABLE), 0, mp->m_super->s_id);
	if (!mp->m_sync_workqueue)
		goto out_destroy_inodegc;

	return 0;

out_destroy_inodegc:
	destroy_workqueue(mp->m_inodegc_wq);
out_destroy_blockgc:
	destroy_workqueue(mp->m_blockgc_wq);
out_destroy_reclaim:
	destroy_workqueue(mp->m_reclaim_workqueue);
out_destroy_unwritten:
	destroy_workqueue(mp->m_unwritten_workqueue);
out_destroy_buf:
	destroy_workqueue(mp->m_buf_workqueue);
out:
	return -ENOMEM;
}

STATIC void
xfs_destroy_mount_workqueues(
	struct xfs_mount	*mp)
{
	destroy_workqueue(mp->m_sync_workqueue);
	destroy_workqueue(mp->m_blockgc_wq);
	destroy_workqueue(mp->m_inodegc_wq);
	destroy_workqueue(mp->m_reclaim_workqueue);
	destroy_workqueue(mp->m_unwritten_workqueue);
	destroy_workqueue(mp->m_buf_workqueue);
}

static void
xfs_flush_inodes_worker(
	struct work_struct	*work)
{
	struct xfs_mount	*mp = container_of(work, struct xfs_mount,
						   m_flush_inodes_work);
	struct super_block	*sb = mp->m_super;

	if (down_read_trylock(&sb->s_umount)) {
		sync_inodes_sb(sb);
		up_read(&sb->s_umount);
	}
}

/*
 * Flush all dirty data to disk. Must not be called while holding an XFS_ILOCK
 * or a page lock. We use sync_inodes_sb() here to ensure we block while waiting
 * for IO to complete so that we effectively throttle multiple callers to the
 * rate at which IO is completing.
 */
void
xfs_flush_inodes(
	struct xfs_mount	*mp)
{
	/*
	 * If flush_work() returns true then that means we waited for a flush
	 * which was already in progress.  Don't bother running another scan.
	 */
	if (flush_work(&mp->m_flush_inodes_work))
		return;

	queue_work(mp->m_sync_workqueue, &mp->m_flush_inodes_work);
	flush_work(&mp->m_flush_inodes_work);
}

/* Catch misguided souls that try to use this interface on XFS */
STATIC struct inode *
xfs_fs_alloc_inode(
	struct super_block	*sb)
{
	BUG();
	return NULL;
}

/*
 * Now that the generic code is guaranteed not to be accessing
 * the linux inode, we can inactivate and reclaim the inode.
 */
STATIC void
xfs_fs_destroy_inode(
	struct inode		*inode)
{
	struct xfs_inode	*ip = XFS_I(inode);

	trace_xfs_destroy_inode(ip);

	ASSERT(!rwsem_is_locked(&inode->i_rwsem));
	XFS_STATS_INC(ip->i_mount, vn_rele);
	XFS_STATS_INC(ip->i_mount, vn_remove);
	xfs_inode_mark_reclaimable(ip);
}

static void
xfs_fs_dirty_inode(
	struct inode			*inode,
	int				flags)
{
	struct xfs_inode		*ip = XFS_I(inode);
	struct xfs_mount		*mp = ip->i_mount;
	struct xfs_trans		*tp;

	if (!(inode->i_sb->s_flags & SB_LAZYTIME))
		return;

	/*
	 * Only do the timestamp update if the inode is dirty (I_DIRTY_SYNC)
	 * and has dirty timestamp (I_DIRTY_TIME). I_DIRTY_TIME can be passed
	 * in flags possibly together with I_DIRTY_SYNC.
	 */
	if ((flags & ~I_DIRTY_TIME) != I_DIRTY_SYNC || !(flags & I_DIRTY_TIME))
		return;

	if (xfs_trans_alloc(mp, &M_RES(mp)->tr_fsyncts, 0, 0, 0, &tp))
		return;
	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, XFS_ILOCK_EXCL);
	xfs_trans_log_inode(tp, ip, XFS_ILOG_TIMESTAMP);
	xfs_trans_commit(tp);
}

/*
 * Slab object creation initialisation for the XFS inode.
 * This covers only the idempotent fields in the XFS inode;
 * all other fields need to be initialised on allocation
 * from the slab. This avoids the need to repeatedly initialise
 * fields in the xfs inode that left in the initialise state
 * when freeing the inode.
 */
STATIC void
xfs_fs_inode_init_once(
	void			*inode)
{
	struct xfs_inode	*ip = inode;

	memset(ip, 0, sizeof(struct xfs_inode));

	/* vfs inode */
	inode_init_once(VFS_I(ip));

	/* xfs inode */
	atomic_set(&ip->i_pincount, 0);
	spin_lock_init(&ip->i_flags_lock);
	init_rwsem(&ip->i_lock);
}

/*
 * We do an unlocked check for XFS_IDONTCACHE here because we are already
 * serialised against cache hits here via the inode->i_lock and igrab() in
 * xfs_iget_cache_hit(). Hence a lookup that might clear this flag will not be
 * racing with us, and it avoids needing to grab a spinlock here for every inode
 * we drop the final reference on.
 */
STATIC int
xfs_fs_drop_inode(
	struct inode		*inode)
{
	struct xfs_inode	*ip = XFS_I(inode);

	/*
	 * If this unlinked inode is in the middle of recovery, don't
	 * drop the inode just yet; log recovery will take care of
	 * that.  See the comment for this inode flag.
	 */
	if (ip->i_flags & XFS_IRECOVERY) {
		ASSERT(xlog_recovery_needed(ip->i_mount->m_log));
		return 0;
	}

	return generic_drop_inode(inode);
}

STATIC void
xfs_fs_evict_inode(
	struct inode		*inode)
{
	if (IS_DAX(inode))
		dax_break_layout_final(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void
xfs_mount_free(
	struct xfs_mount	*mp)
{
	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp)
		xfs_free_buftarg(mp->m_logdev_targp);
	if (mp->m_rtdev_targp && mp->m_rtdev_targp != mp->m_ddev_targp)
		xfs_free_buftarg(mp->m_rtdev_targp);
	if (mp->m_ddev_targp)
		xfs_free_buftarg(mp->m_ddev_targp);

	debugfs_remove(mp->m_debugfs);
	kfree(mp->m_rtname);
	kfree(mp->m_logname);
	kfree(mp);
}

STATIC int
xfs_fs_sync_fs(
	struct super_block	*sb,
	int			wait)
{
	struct xfs_mount	*mp = XFS_M(sb);
	int			error;

	trace_xfs_fs_sync_fs(mp, __return_address);

	/*
	 * Doing anything during the async pass would be counterproductive.
	 */
	if (!wait)
		return 0;

	error = xfs_log_force(mp, XFS_LOG_SYNC);
	if (error)
		return error;

	if (laptop_mode) {
		/*
		 * The disk must be active because we're syncing.
		 * We schedule log work now (now that the disk is
		 * active) instead of later (when it might not be).
		 */
		flush_delayed_work(&mp->m_log->l_work);
	}

	/*
	 * If we are called with page faults frozen out, it means we are about
	 * to freeze the transaction subsystem. Take the opportunity to shut
	 * down inodegc because once SB_FREEZE_FS is set it's too late to
	 * prevent inactivation races with freeze. The fs doesn't get called
	 * again by the freezing process until after SB_FREEZE_FS has been set,
	 * so it's now or never.  Same logic applies to speculative allocation
	 * garbage collection.
	 *
	 * We don't care if this is a normal syncfs call that does this or
	 * freeze that does this - we can run this multiple times without issue
	 * and we won't race with a restart because a restart can only occur
	 * when the state is either SB_FREEZE_FS or SB_FREEZE_COMPLETE.
	 */
	if (sb->s_writers.frozen == SB_FREEZE_PAGEFAULT) {
		xfs_inodegc_stop(mp);
		xfs_blockgc_stop(mp);
		xfs_zone_gc_stop(mp);
	}

	return 0;
}

static xfs_extlen_t
xfs_internal_log_size(
	struct xfs_mount	*mp)
{
	if (!mp->m_sb.sb_logstart)
		return 0;
	return mp->m_sb.sb_logblocks;
}

static void
xfs_statfs_data(
	struct xfs_mount	*mp,
	struct kstatfs		*st)
{
	int64_t			fdblocks =
		xfs_sum_freecounter(mp, XC_FREE_BLOCKS);

	/* make sure st->f_bfree does not underflow */
	st->f_bfree = max(0LL,
		fdblocks - xfs_freecounter_unavailable(mp, XC_FREE_BLOCKS));

	/*
	 * sb_dblocks can change during growfs, but nothing cares about reporting
	 * the old or new value during growfs.
	 */
	st->f_blocks = mp->m_sb.sb_dblocks - xfs_internal_log_size(mp);
}

/*
 * When stat(v)fs is called on a file with the realtime bit set or a directory
 * with the rtinherit bit, report freespace information for the RT device
 * instead of the main data device.
 */
static void
xfs_statfs_rt(
	struct xfs_mount	*mp,
	struct kstatfs		*st)
{
	st->f_bfree = xfs_rtbxlen_to_blen(mp,
			xfs_sum_freecounter(mp, XC_FREE_RTEXTENTS));
	st->f_blocks = mp->m_sb.sb_rblocks - xfs_rtbxlen_to_blen(mp,
			mp->m_free[XC_FREE_RTEXTENTS].res_total);
}

static void
xfs_statfs_inodes(
	struct xfs_mount	*mp,
	struct kstatfs		*st)
{
	uint64_t		icount = percpu_counter_sum(&mp->m_icount);
	uint64_t		ifree = percpu_counter_sum(&mp->m_ifree);
	uint64_t		fakeinos = XFS_FSB_TO_INO(mp, st->f_bfree);

	st->f_files = min(icount + fakeinos, (uint64_t)XFS_MAXINUMBER);
	if (M_IGEO(mp)->maxicount)
		st->f_files = min_t(typeof(st->f_files), st->f_files,
					M_IGEO(mp)->maxicount);

	/* If sb_icount overshot maxicount, report actual allocation */
	st->f_files = max_t(typeof(st->f_files), st->f_files,
			mp->m_sb.sb_icount);

	/* Make sure st->f_ffree does not underflow */
	st->f_ffree = max_t(int64_t, 0, st->f_files - (icount - ifree));
}

STATIC int
xfs_fs_statfs(
	struct dentry		*dentry,
	struct kstatfs		*st)
{
	struct xfs_mount	*mp = XFS_M(dentry->d_sb);
	struct xfs_inode	*ip = XFS_I(d_inode(dentry));

	/*
	 * Expedite background inodegc but don't wait. We do not want to block
	 * here waiting hours for a billion extent file to be truncated.
	 */
	xfs_inodegc_push(mp);

	st->f_type = XFS_SUPER_MAGIC;
	st->f_namelen = MAXNAMELEN - 1;
	st->f_bsize = mp->m_sb.sb_blocksize;
	st->f_fsid = u64_to_fsid(huge_encode_dev(mp->m_ddev_targp->bt_dev));

	xfs_statfs_data(mp, st);
	xfs_statfs_inodes(mp, st);

	if (XFS_IS_REALTIME_MOUNT(mp) &&
	    (ip->i_diflags & (XFS_DIFLAG_RTINHERIT | XFS_DIFLAG_REALTIME)))
		xfs_statfs_rt(mp, st);

	if ((ip->i_diflags & XFS_DIFLAG_PROJINHERIT) &&
	    ((mp->m_qflags & (XFS_PQUOTA_ACCT|XFS_PQUOTA_ENFD))) ==
			      (XFS_PQUOTA_ACCT|XFS_PQUOTA_ENFD))
		xfs_qm_statvfs(ip, st);

	/*
	 * XFS does not distinguish between blocks available to privileged and
	 * unprivileged users.
	 */
	st->f_bavail = st->f_bfree;
	return 0;
}

STATIC void
xfs_save_resvblks(
	struct xfs_mount	*mp)
{
	enum xfs_free_counter	i;

	for (i = 0; i < XC_FREE_NR; i++) {
		mp->m_free[i].res_saved = mp->m_free[i].res_total;
		xfs_reserve_blocks(mp, i, 0);
	}
}

STATIC void
xfs_restore_resvblks(
	struct xfs_mount	*mp)
{
	uint64_t		resblks;
	enum xfs_free_counter	i;

	for (i = 0; i < XC_FREE_NR; i++) {
		if (mp->m_free[i].res_saved) {
			resblks = mp->m_free[i].res_saved;
			mp->m_free[i].res_saved = 0;
		} else
			resblks = xfs_default_resblks(mp, i);
		xfs_reserve_blocks(mp, i, resblks);
	}
}

/*
 * Second stage of a freeze. The data is already frozen so we only
 * need to take care of the metadata. Once that's done sync the superblock
 * to the log to dirty it in case of a crash while frozen. This ensures that we
 * will recover the unlinked inode lists on the next mount.
 */
STATIC int
xfs_fs_freeze(
	struct super_block	*sb)
{
	struct xfs_mount	*mp = XFS_M(sb);
	unsigned int		flags;
	int			ret;

	/*
	 * The filesystem is now frozen far enough that memory reclaim
	 * cannot safely operate on the filesystem. Hence we need to
	 * set a GFP_NOFS context here to avoid recursion deadlocks.
	 */
	flags = memalloc_nofs_save();
	xfs_save_resvblks(mp);
	ret = xfs_log_quiesce(mp);
	memalloc_nofs_restore(flags);

	/*
	 * For read-write filesystems, we need to restart the inodegc on error
	 * because we stopped it at SB_FREEZE_PAGEFAULT level and a thaw is not
	 * going to be run to restart it now.  We are at SB_FREEZE_FS level
	 * here, so we can restart safely without racing with a stop in
	 * xfs_fs_sync_fs().
	 */
	if (ret && !xfs_is_readonly(mp)) {
		xfs_blockgc_start(mp);
		xfs_inodegc_start(mp);
		xfs_zone_gc_start(mp);
	}

	return ret;
}

STATIC int
xfs_fs_unfreeze(
	struct super_block	*sb)
{
	struct xfs_mount	*mp = XFS_M(sb);

	xfs_restore_resvblks(mp);
	xfs_log_work_queue(mp);

	/*
	 * Don't reactivate the inodegc worker on a readonly filesystem because
	 * inodes are sent directly to reclaim.  Don't reactivate the blockgc
	 * worker because there are no speculative preallocations on a readonly
	 * filesystem.
	 */
	if (!xfs_is_readonly(mp)) {
		xfs_zone_gc_start(mp);
		xfs_blockgc_start(mp);
		xfs_inodegc_start(mp);
	}

	return 0;
}

/*
 * This function fills in xfs_mount_t fields based on mount args.
 * Note: the superblock _has_ now been read in.
 */
STATIC int
xfs_finish_flags(
	struct xfs_mount	*mp)
{
	/* Fail a mount where the logbuf is smaller than the log stripe */
	if (xfs_has_logv2(mp)) {
		if (mp->m_logbsize <= 0 &&
		    mp->m_sb.sb_logsunit > XLOG_BIG_RECORD_BSIZE) {
			mp->m_logbsize = mp->m_sb.sb_logsunit;
		} else if (mp->m_logbsize > 0 &&
			   mp->m_logbsize < mp->m_sb.sb_logsunit) {
			xfs_warn(mp,
		"logbuf size must be greater than or equal to log stripe size");
			return -EINVAL;
		}
	} else {
		/* Fail a mount if the logbuf is larger than 32K */
		if (mp->m_logbsize > XLOG_BIG_RECORD_BSIZE) {
			xfs_warn(mp,
		"logbuf size for version 1 logs must be 16K or 32K");
			return -EINVAL;
		}
	}

	/*
	 * V5 filesystems always use attr2 format for attributes.
	 */
	if (xfs_has_crc(mp) && xfs_has_noattr2(mp)) {
		xfs_warn(mp, "Cannot mount a V5 filesystem as noattr2. "
			     "attr2 is always enabled for V5 filesystems.");
		return -EINVAL;
	}

	/*
	 * prohibit r/w mounts of read-only filesystems
	 */
	if ((mp->m_sb.sb_flags & XFS_SBF_READONLY) && !xfs_is_readonly(mp)) {
		xfs_warn(mp,
			"cannot mount a read-only filesystem as read-write");
		return -EROFS;
	}

	if ((mp->m_qflags & XFS_GQUOTA_ACCT) &&
	    (mp->m_qflags & XFS_PQUOTA_ACCT) &&
	    !xfs_has_pquotino(mp)) {
		xfs_warn(mp,
		  "Super block does not support project and group quota together");
		return -EINVAL;
	}

	if (!xfs_has_zoned(mp)) {
		if (mp->m_max_open_zones) {
			xfs_warn(mp,
"max_open_zones mount option only supported on zoned file systems.");
			return -EINVAL;
		}
		if (mp->m_features & XFS_FEAT_NOLIFETIME) {
			xfs_warn(mp,
"nolifetime mount option only supported on zoned file systems.");
			return -EINVAL;
		}
	}

	return 0;
}

static int
xfs_init_percpu_counters(
	struct xfs_mount	*mp)
{
	int			error;
	int			i;

	error = percpu_counter_init(&mp->m_icount, 0, GFP_KERNEL);
	if (error)
		return -ENOMEM;

	error = percpu_counter_init(&mp->m_ifree, 0, GFP_KERNEL);
	if (error)
		goto free_icount;

	error = percpu_counter_init(&mp->m_delalloc_blks, 0, GFP_KERNEL);
	if (error)
		goto free_ifree;

	error = percpu_counter_init(&mp->m_delalloc_rtextents, 0, GFP_KERNEL);
	if (error)
		goto free_delalloc;

	for (i = 0; i < XC_FREE_NR; i++) {
		error = percpu_counter_init(&mp->m_free[i].count, 0,
				GFP_KERNEL);
		if (error)
			goto free_freecounters;
	}

	return 0;

free_freecounters:
	while (--i >= 0)
		percpu_counter_destroy(&mp->m_free[i].count);
	percpu_counter_destroy(&mp->m_delalloc_rtextents);
free_delalloc:
	percpu_counter_destroy(&mp->m_delalloc_blks);
free_ifree:
	percpu_counter_destroy(&mp->m_ifree);
free_icount:
	percpu_counter_destroy(&mp->m_icount);
	return -ENOMEM;
}

void
xfs_reinit_percpu_counters(
	struct xfs_mount	*mp)
{
	percpu_counter_set(&mp->m_icount, mp->m_sb.sb_icount);
	percpu_counter_set(&mp->m_ifree, mp->m_sb.sb_ifree);
	xfs_set_freecounter(mp, XC_FREE_BLOCKS, mp->m_sb.sb_fdblocks);
	if (!xfs_has_zoned(mp))
		xfs_set_freecounter(mp, XC_FREE_RTEXTENTS,
				mp->m_sb.sb_frextents);
}

static void
xfs_destroy_percpu_counters(
	struct xfs_mount	*mp)
{
	enum xfs_free_counter	i;

	for (i = 0; i < XC_FREE_NR; i++)
		percpu_counter_destroy(&mp->m_free[i].count);
	percpu_counter_destroy(&mp->m_icount);
	percpu_counter_destroy(&mp->m_ifree);
	ASSERT(xfs_is_shutdown(mp) ||
	       percpu_counter_sum(&mp->m_delalloc_rtextents) == 0);
	percpu_counter_destroy(&mp->m_delalloc_rtextents);
	ASSERT(xfs_is_shutdown(mp) ||
	       percpu_counter_sum(&mp->m_delalloc_blks) == 0);
	percpu_counter_destroy(&mp->m_delalloc_blks);
}

static int
xfs_inodegc_init_percpu(
	struct xfs_mount	*mp)
{
	struct xfs_inodegc	*gc;
	int			cpu;

	mp->m_inodegc = alloc_percpu(struct xfs_inodegc);
	if (!mp->m_inodegc)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		gc = per_cpu_ptr(mp->m_inodegc, cpu);
		gc->cpu = cpu;
		gc->mp = mp;
		init_llist_head(&gc->list);
		gc->items = 0;
		gc->error = 0;
		INIT_DELAYED_WORK(&gc->work, xfs_inodegc_worker);
	}
	return 0;
}

static void
xfs_inodegc_free_percpu(
	struct xfs_mount	*mp)
{
	if (!mp->m_inodegc)
		return;
	free_percpu(mp->m_inodegc);
}

static void
xfs_fs_put_super(
	struct super_block	*sb)
{
	struct xfs_mount	*mp = XFS_M(sb);

	xfs_notice(mp, "Unmounting Filesystem %pU", &mp->m_sb.sb_uuid);
	xfs_filestream_unmount(mp);
	xfs_unmountfs(mp);

	xfs_rtmount_freesb(mp);
	xfs_freesb(mp);
	xchk_mount_stats_free(mp);
	free_percpu(mp->m_stats.xs_stats);
	xfs_inodegc_free_percpu(mp);
	xfs_destroy_percpu_counters(mp);
	xfs_destroy_mount_workqueues(mp);
	xfs_shutdown_devices(mp);
}

static long
xfs_fs_nr_cached_objects(
	struct super_block	*sb,
	struct shrink_control	*sc)
{
	/* Paranoia: catch incorrect calls during mount setup or teardown */
	if (WARN_ON_ONCE(!sb->s_fs_info))
		return 0;
	return xfs_reclaim_inodes_count(XFS_M(sb));
}

static long
xfs_fs_free_cached_objects(
	struct super_block	*sb,
	struct shrink_control	*sc)
{
	return xfs_reclaim_inodes_nr(XFS_M(sb), sc->nr_to_scan);
}

static void
xfs_fs_shutdown(
	struct super_block	*sb)
{
	xfs_force_shutdown(XFS_M(sb), SHUTDOWN_DEVICE_REMOVED);
}

static int
xfs_fs_show_stats(
	struct seq_file		*m,
	struct dentry		*root)
{
	struct xfs_mount	*mp = XFS_M(root->d_sb);

	if (xfs_has_zoned(mp) && IS_ENABLED(CONFIG_XFS_RT))
		xfs_zoned_show_stats(m, mp);
	return 0;
}

static const struct super_operations xfs_super_operations = {
	.alloc_inode		= xfs_fs_alloc_inode,
	.destroy_inode		= xfs_fs_destroy_inode,
	.dirty_inode		= xfs_fs_dirty_inode,
	.drop_inode		= xfs_fs_drop_inode,
	.evict_inode		= xfs_fs_evict_inode,
	.put_super		= xfs_fs_put_super,
	.sync_fs		= xfs_fs_sync_fs,
	.freeze_fs		= xfs_fs_freeze,
	.unfreeze_fs		= xfs_fs_unfreeze,
	.statfs			= xfs_fs_statfs,
	.show_options		= xfs_fs_show_options,
	.nr_cached_objects	= xfs_fs_nr_cached_objects,
	.free_cached_objects	= xfs_fs_free_cached_objects,
	.shutdown		= xfs_fs_shutdown,
	.show_stats		= xfs_fs_show_stats,
};

static int
suffix_kstrtoint(
	const char	*s,
	unsigned int	base,
	int		*res)
{
	int		last, shift_left_factor = 0, _res;
	char		*value;
	int		ret = 0;

	value = kstrdup(s, GFP_KERNEL);
	if (!value)
		return -ENOMEM;

	last = strlen(value) - 1;
	if (value[last] == 'K' || value[last] == 'k') {
		shift_left_factor = 10;
		value[last] = '\0';
	}
	if (value[last] == 'M' || value[last] == 'm') {
		shift_left_factor = 20;
		value[last] = '\0';
	}
	if (value[last] == 'G' || value[last] == 'g') {
		shift_left_factor = 30;
		value[last] = '\0';
	}

	if (kstrtoint(value, base, &_res))
		ret = -EINVAL;
	kfree(value);
	*res = _res << shift_left_factor;
	return ret;
}

static int
suffix_kstrtoull(
	const char		*s,
	unsigned int		base,
	unsigned long long	*res)
{
	int			last, shift_left_factor = 0;
	unsigned long long	_res;
	char			*value;
	int			ret = 0;

	value = kstrdup(s, GFP_KERNEL);
	if (!value)
		return -ENOMEM;

	last = strlen(value) - 1;
	if (value[last] == 'K' || value[last] == 'k') {
		shift_left_factor = 10;
		value[last] = '\0';
	}
	if (value[last] == 'M' || value[last] == 'm') {
		shift_left_factor = 20;
		value[last] = '\0';
	}
	if (value[last] == 'G' || value[last] == 'g') {
		shift_left_factor = 30;
		value[last] = '\0';
	}

	if (kstrtoull(value, base, &_res))
		ret = -EINVAL;
	kfree(value);
	*res = _res << shift_left_factor;
	return ret;
}

static inline void
xfs_fs_warn_deprecated(
	struct fs_context	*fc,
	struct fs_parameter	*param,
	uint64_t		flag,
	bool			value)
{
	/* Don't print the warning if reconfiguring and current mount point
	 * already had the flag set
	 */
	if ((fc->purpose & FS_CONTEXT_FOR_RECONFIGURE) &&
            !!(XFS_M(fc->root->d_sb)->m_features & flag) == value)
		return;
	xfs_warn(fc->s_fs_info, "%s mount option is deprecated.", param->key);
}

/*
 * Set mount state from a mount option.
 *
 * NOTE: mp->m_super is NULL here!
 */
static int
xfs_fs_parse_param(
	struct fs_context	*fc,
	struct fs_parameter	*param)
{
	struct xfs_mount	*parsing_mp = fc->s_fs_info;
	struct fs_parse_result	result;
	int			size = 0;
	int			opt;

	BUILD_BUG_ON(XFS_QFLAGS_MNTOPTS & XFS_MOUNT_QUOTA_ALL);

	opt = fs_parse(fc, xfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_logbufs:
		parsing_mp->m_logbufs = result.uint_32;
		return 0;
	case Opt_logbsize:
		if (suffix_kstrtoint(param->string, 10, &parsing_mp->m_logbsize))
			return -EINVAL;
		return 0;
	case Opt_logdev:
		kfree(parsing_mp->m_logname);
		parsing_mp->m_logname = kstrdup(param->string, GFP_KERNEL);
		if (!parsing_mp->m_logname)
			return -ENOMEM;
		return 0;
	case Opt_rtdev:
		kfree(parsing_mp->m_rtname);
		parsing_mp->m_rtname = kstrdup(param->string, GFP_KERNEL);
		if (!parsing_mp->m_rtname)
			return -ENOMEM;
		return 0;
	case Opt_allocsize:
		if (suffix_kstrtoint(param->string, 10, &size))
			return -EINVAL;
		parsing_mp->m_allocsize_log = ffs(size) - 1;
		parsing_mp->m_features |= XFS_FEAT_ALLOCSIZE;
		return 0;
	case Opt_grpid:
	case Opt_bsdgroups:
		parsing_mp->m_features |= XFS_FEAT_GRPID;
		return 0;
	case Opt_nogrpid:
	case Opt_sysvgroups:
		parsing_mp->m_features &= ~XFS_FEAT_GRPID;
		return 0;
	case Opt_wsync:
		parsing_mp->m_features |= XFS_FEAT_WSYNC;
		return 0;
	case Opt_norecovery:
		parsing_mp->m_features |= XFS_FEAT_NORECOVERY;
		return 0;
	case Opt_noalign:
		parsing_mp->m_features |= XFS_FEAT_NOALIGN;
		return 0;
	case Opt_swalloc:
		parsing_mp->m_features |= XFS_FEAT_SWALLOC;
		return 0;
	case Opt_sunit:
		parsing_mp->m_dalign = result.uint_32;
		return 0;
	case Opt_swidth:
		parsing_mp->m_swidth = result.uint_32;
		return 0;
	case Opt_inode32:
		parsing_mp->m_features |= XFS_FEAT_SMALL_INUMS;
		return 0;
	case Opt_inode64:
		parsing_mp->m_features &= ~XFS_FEAT_SMALL_INUMS;
		return 0;
	case Opt_nouuid:
		parsing_mp->m_features |= XFS_FEAT_NOUUID;
		return 0;
	case Opt_largeio:
		parsing_mp->m_features |= XFS_FEAT_LARGE_IOSIZE;
		return 0;
	case Opt_nolargeio:
		parsing_mp->m_features &= ~XFS_FEAT_LARGE_IOSIZE;
		return 0;
	case Opt_filestreams:
		parsing_mp->m_features |= XFS_FEAT_FILESTREAMS;
		return 0;
	case Opt_noquota:
		parsing_mp->m_qflags &= ~XFS_ALL_QUOTA_ACCT;
		parsing_mp->m_qflags &= ~XFS_ALL_QUOTA_ENFD;
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_quota:
	case Opt_uquota:
	case Opt_usrquota:
		parsing_mp->m_qflags |= (XFS_UQUOTA_ACCT | XFS_UQUOTA_ENFD);
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_qnoenforce:
	case Opt_uqnoenforce:
		parsing_mp->m_qflags |= XFS_UQUOTA_ACCT;
		parsing_mp->m_qflags &= ~XFS_UQUOTA_ENFD;
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_pquota:
	case Opt_prjquota:
		parsing_mp->m_qflags |= (XFS_PQUOTA_ACCT | XFS_PQUOTA_ENFD);
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_pqnoenforce:
		parsing_mp->m_qflags |= XFS_PQUOTA_ACCT;
		parsing_mp->m_qflags &= ~XFS_PQUOTA_ENFD;
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_gquota:
	case Opt_grpquota:
		parsing_mp->m_qflags |= (XFS_GQUOTA_ACCT | XFS_GQUOTA_ENFD);
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_gqnoenforce:
		parsing_mp->m_qflags |= XFS_GQUOTA_ACCT;
		parsing_mp->m_qflags &= ~XFS_GQUOTA_ENFD;
		parsing_mp->m_qflags |= XFS_QFLAGS_MNTOPTS;
		return 0;
	case Opt_discard:
		parsing_mp->m_features |= XFS_FEAT_DISCARD;
		return 0;
	case Opt_nodiscard:
		parsing_mp->m_features &= ~XFS_FEAT_DISCARD;
		return 0;
#ifdef CONFIG_FS_DAX
	case Opt_dax:
		xfs_mount_set_dax_mode(parsing_mp, XFS_DAX_ALWAYS);
		return 0;
	case Opt_dax_enum:
		xfs_mount_set_dax_mode(parsing_mp, result.uint_32);
		return 0;
#endif
	/* Following mount options will be removed in September 2025 */
	case Opt_ikeep:
		xfs_fs_warn_deprecated(fc, param, XFS_FEAT_IKEEP, true);
		parsing_mp->m_features |= XFS_FEAT_IKEEP;
		return 0;
	case Opt_noikeep:
		xfs_fs_warn_deprecated(fc, param, XFS_FEAT_IKEEP, false);
		parsing_mp->m_features &= ~XFS_FEAT_IKEEP;
		return 0;
	case Opt_attr2:
		xfs_fs_warn_deprecated(fc, param, XFS_FEAT_ATTR2, true);
		parsing_mp->m_features |= XFS_FEAT_ATTR2;
		return 0;
	case Opt_noattr2:
		xfs_fs_warn_deprecated(fc, param, XFS_FEAT_NOATTR2, true);
		parsing_mp->m_features |= XFS_FEAT_NOATTR2;
		return 0;
	case Opt_max_open_zones:
		parsing_mp->m_max_open_zones = result.uint_32;
		return 0;
	case Opt_lifetime:
		parsing_mp->m_features &= ~XFS_FEAT_NOLIFETIME;
		return 0;
	case Opt_nolifetime:
		parsing_mp->m_features |= XFS_FEAT_NOLIFETIME;
		return 0;
	case Opt_max_atomic_write:
		if (suffix_kstrtoull(param->string, 10,
				     &parsing_mp->m_awu_max_bytes)) {
			xfs_warn(parsing_mp,
 "max atomic write size must be positive integer");
			return -EINVAL;
		}
		return 0;
	default:
		xfs_warn(parsing_mp, "unknown mount option [%s].", param->key);
		return -EINVAL;
	}

	return 0;
}

static int
xfs_fs_validate_params(
	struct xfs_mount	*mp)
{
	/* No recovery flag requires a read-only mount */
	if (xfs_has_norecovery(mp) && !xfs_is_readonly(mp)) {
		xfs_warn(mp, "no-recovery mounts must be read-only.");
		return -EINVAL;
	}

	/*
	 * We have not read the superblock at this point, so only the attr2
	 * mount option can set the attr2 feature by this stage.
	 */
	if (xfs_has_attr2(mp) && xfs_has_noattr2(mp)) {
		xfs_warn(mp, "attr2 and noattr2 cannot both be specified.");
		return -EINVAL;
	}


	if (xfs_has_noalign(mp) && (mp->m_dalign || mp->m_swidth)) {
		xfs_warn(mp,
	"sunit and swidth options incompatible with the noalign option");
		return -EINVAL;
	}

	if (!IS_ENABLED(CONFIG_XFS_QUOTA) &&
	    (mp->m_qflags & ~XFS_QFLAGS_MNTOPTS)) {
		xfs_warn(mp, "quota support not available in this kernel.");
		return -EINVAL;
	}

	if ((mp->m_dalign && !mp->m_swidth) ||
	    (!mp->m_dalign && mp->m_swidth)) {
		xfs_warn(mp, "sunit and swidth must be specified together");
		return -EINVAL;
	}

	if (mp->m_dalign && (mp->m_swidth % mp->m_dalign != 0)) {
		xfs_warn(mp,
	"stripe width (%d) must be a multiple of the stripe unit (%d)",
			mp->m_swidth, mp->m_dalign);
		return -EINVAL;
	}

	if (mp->m_logbufs != -1 &&
	    mp->m_logbufs != 0 &&
	    (mp->m_logbufs < XLOG_MIN_ICLOGS ||
	     mp->m_logbufs > XLOG_MAX_ICLOGS)) {
		xfs_warn(mp, "invalid logbufs value: %d [not %d-%d]",
			mp->m_logbufs, XLOG_MIN_ICLOGS, XLOG_MAX_ICLOGS);
		return -EINVAL;
	}

	if (mp->m_logbsize != -1 &&
	    mp->m_logbsize !=  0 &&
	    (mp->m_logbsize < XLOG_MIN_RECORD_BSIZE ||
	     mp->m_logbsize > XLOG_MAX_RECORD_BSIZE ||
	     !is_power_of_2(mp->m_logbsize))) {
		xfs_warn(mp,
			"invalid logbufsize: %d [not 16k,32k,64k,128k or 256k]",
			mp->m_logbsize);
		return -EINVAL;
	}

	if (xfs_has_allocsize(mp) &&
	    (mp->m_allocsize_log > XFS_MAX_IO_LOG ||
	     mp->m_allocsize_log < XFS_MIN_IO_LOG)) {
		xfs_warn(mp, "invalid log iosize: %d [not %d-%d]",
			mp->m_allocsize_log, XFS_MIN_IO_LOG, XFS_MAX_IO_LOG);
		return -EINVAL;
	}

	return 0;
}

struct dentry *
xfs_debugfs_mkdir(
	const char	*name,
	struct dentry	*parent)
{
	struct dentry	*child;

	/* Apparently we're expected to ignore error returns?? */
	child = debugfs_create_dir(name, parent);
	if (IS_ERR(child))
		return NULL;

	return child;
}

static int
xfs_fs_fill_super(
	struct super_block	*sb,
	struct fs_context	*fc)
{
	struct xfs_mount	*mp = sb->s_fs_info;
	struct inode		*root;
	int			flags = 0, error;

	mp->m_super = sb;

	/*
	 * Copy VFS mount flags from the context now that all parameter parsing
	 * is guaranteed to have been completed by either the old mount API or
	 * the newer fsopen/fsconfig API.
	 */
	if (fc->sb_flags & SB_RDONLY)
		xfs_set_readonly(mp);
	if (fc->sb_flags & SB_DIRSYNC)
		mp->m_features |= XFS_FEAT_DIRSYNC;
	if (fc->sb_flags & SB_SYNCHRONOUS)
		mp->m_features |= XFS_FEAT_WSYNC;

	error = xfs_fs_validate_params(mp);
	if (error)
		return error;

	sb_min_blocksize(sb, BBSIZE);
	sb->s_xattr = xfs_xattr_handlers;
	sb->s_export_op = &xfs_export_operations;
#ifdef CONFIG_XFS_QUOTA
	sb->s_qcop = &xfs_quotactl_operations;
	sb->s_quota_types = QTYPE_MASK_USR | QTYPE_MASK_GRP | QTYPE_MASK_PRJ;
#endif
	sb->s_op = &xfs_super_operations;

	/*
	 * Delay mount work if the debug hook is set. This is debug
	 * instrumention to coordinate simulation of xfs mount failures with
	 * VFS superblock operations
	 */
	if (xfs_globals.mount_delay) {
		xfs_notice(mp, "Delaying mount for %d seconds.",
			xfs_globals.mount_delay);
		msleep(xfs_globals.mount_delay * 1000);
	}

	if (fc->sb_flags & SB_SILENT)
		flags |= XFS_MFSI_QUIET;

	error = xfs_open_devices(mp);
	if (error)
		return error;

	if (xfs_debugfs) {
		mp->m_debugfs = xfs_debugfs_mkdir(mp->m_super->s_id,
						  xfs_debugfs);
	} else {
		mp->m_debugfs = NULL;
	}

	error = xfs_init_mount_workqueues(mp);
	if (error)
		goto out_shutdown_devices;

	error = xfs_init_percpu_counters(mp);
	if (error)
		goto out_destroy_workqueues;

	error = xfs_inodegc_init_percpu(mp);
	if (error)
		goto out_destroy_counters;

	/* Allocate stats memory before we do operations that might use it */
	mp->m_stats.xs_stats = alloc_percpu(struct xfsstats);
	if (!mp->m_stats.xs_stats) {
		error = -ENOMEM;
		goto out_destroy_inodegc;
	}

	error = xchk_mount_stats_alloc(mp);
	if (error)
		goto out_free_stats;

	error = xfs_readsb(mp, flags);
	if (error)
		goto out_free_scrub_stats;

	error = xfs_finish_flags(mp);
	if (error)
		goto out_free_sb;

	error = xfs_setup_devices(mp);
	if (error)
		goto out_free_sb;

	/*
	 * V4 support is undergoing deprecation.
	 *
	 * Note: this has to use an open coded m_features check as xfs_has_crc
	 * always returns false for !CONFIG_XFS_SUPPORT_V4.
	 */
	if (!(mp->m_features & XFS_FEAT_CRC)) {
		if (!IS_ENABLED(CONFIG_XFS_SUPPORT_V4)) {
			xfs_warn(mp,
	"Deprecated V4 format (crc=0) not supported by kernel.");
			error = -EINVAL;
			goto out_free_sb;
		}
		xfs_warn_once(mp,
	"Deprecated V4 format (crc=0) will not be supported after September 2030.");
	}

	/* ASCII case insensitivity is undergoing deprecation. */
	if (xfs_has_asciici(mp)) {
#ifdef CONFIG_XFS_SUPPORT_ASCII_CI
		xfs_warn_once(mp,
	"Deprecated ASCII case-insensitivity feature (ascii-ci=1) will not be supported after September 2030.");
#else
		xfs_warn(mp,
	"Deprecated ASCII case-insensitivity feature (ascii-ci=1) not supported by kernel.");
		error = -EINVAL;
		goto out_free_sb;
#endif
	}

	/*
	 * Filesystem claims it needs repair, so refuse the mount unless
	 * norecovery is also specified, in which case the filesystem can
	 * be mounted with no risk of further damage.
	 */
	if (xfs_has_needsrepair(mp) && !xfs_has_norecovery(mp)) {
		xfs_warn(mp, "Filesystem needs repair.  Please run xfs_repair.");
		error = -EFSCORRUPTED;
		goto out_free_sb;
	}

	/*
	 * Don't touch the filesystem if a user tool thinks it owns the primary
	 * superblock.  mkfs doesn't clear the flag from secondary supers, so
	 * we don't check them at all.
	 */
	if (mp->m_sb.sb_inprogress) {
		xfs_warn(mp, "Offline file system operation in progress!");
		error = -EFSCORRUPTED;
		goto out_free_sb;
	}

	if (mp->m_sb.sb_blocksize > PAGE_SIZE) {
		size_t max_folio_size = mapping_max_folio_size_supported();

		if (!xfs_has_crc(mp)) {
			xfs_warn(mp,
"V4 Filesystem with blocksize %d bytes. Only pagesize (%ld) or less is supported.",
				mp->m_sb.sb_blocksize, PAGE_SIZE);
			error = -ENOSYS;
			goto out_free_sb;
		}

		if (mp->m_sb.sb_blocksize > max_folio_size) {
			xfs_warn(mp,
"block size (%u bytes) not supported; Only block size (%zu) or less is supported",
				mp->m_sb.sb_blocksize, max_folio_size);
			error = -ENOSYS;
			goto out_free_sb;
		}

		xfs_warn_experimental(mp, XFS_EXPERIMENTAL_LBS);
	}

	/* Ensure this filesystem fits in the page cache limits */
	if (xfs_sb_validate_fsb_count(&mp->m_sb, mp->m_sb.sb_dblocks) ||
	    xfs_sb_validate_fsb_count(&mp->m_sb, mp->m_sb.sb_rblocks)) {
		xfs_warn(mp,
		"file system too large to be mounted on this system.");
		error = -EFBIG;
		goto out_free_sb;
	}

	/*
	 * XFS block mappings use 54 bits to store the logical block offset.
	 * This should suffice to handle the maximum file size that the VFS
	 * supports (currently 2^63 bytes on 64-bit and ULONG_MAX << PAGE_SHIFT
	 * bytes on 32-bit), but as XFS and VFS have gotten the s_maxbytes
	 * calculation wrong on 32-bit kernels in the past, we'll add a WARN_ON
	 * to check this assertion.
	 *
	 * Avoid integer overflow by comparing the maximum bmbt offset to the
	 * maximum pagecache offset in units of fs blocks.
	 */
	if (!xfs_verify_fileoff(mp, XFS_B_TO_FSBT(mp, MAX_LFS_FILESIZE))) {
		xfs_warn(mp,
"MAX_LFS_FILESIZE block offset (%llu) exceeds extent map maximum (%llu)!",
			 XFS_B_TO_FSBT(mp, MAX_LFS_FILESIZE),
			 XFS_MAX_FILEOFF);
		error = -EINVAL;
		goto out_free_sb;
	}

	error = xfs_rtmount_readsb(mp);
	if (error)
		goto out_free_sb;

	error = xfs_filestream_mount(mp);
	if (error)
		goto out_free_rtsb;

	/*
	 * we must configure the block size in the superblock before we run the
	 * full mount process as the mount process can lookup and cache inodes.
	 */
	sb->s_magic = XFS_SUPER_MAGIC;
	sb->s_blocksize = mp->m_sb.sb_blocksize;
	sb->s_blocksize_bits = ffs(sb->s_blocksize) - 1;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_max_links = XFS_MAXLINK;
	sb->s_time_gran = 1;
	if (xfs_has_bigtime(mp)) {
		sb->s_time_min = xfs_bigtime_to_unix(XFS_BIGTIME_TIME_MIN);
		sb->s_time_max = xfs_bigtime_to_unix(XFS_BIGTIME_TIME_MAX);
	} else {
		sb->s_time_min = XFS_LEGACY_TIME_MIN;
		sb->s_time_max = XFS_LEGACY_TIME_MAX;
	}
	trace_xfs_inode_timestamp_range(mp, sb->s_time_min, sb->s_time_max);
	sb->s_iflags |= SB_I_CGROUPWB | SB_I_ALLOW_HSM;

	set_posix_acl_flag(sb);

	/* version 5 superblocks support inode version counters. */
	if (xfs_has_crc(mp))
		sb->s_flags |= SB_I_VERSION;

	if (xfs_has_dax_always(mp)) {
		error = xfs_setup_dax_always(mp);
		if (error)
			goto out_filestream_unmount;
	}

	if (xfs_has_discard(mp) && !bdev_max_discard_sectors(sb->s_bdev)) {
		xfs_warn(mp,
	"mounting with \"discard\" option, but the device does not support discard");
		mp->m_features &= ~XFS_FEAT_DISCARD;
	}

	if (xfs_has_zoned(mp)) {
		if (!xfs_has_metadir(mp)) {
			xfs_alert(mp,
		"metadir feature required for zoned realtime devices.");
			error = -EINVAL;
			goto out_filestream_unmount;
		}
		xfs_warn_experimental(mp, XFS_EXPERIMENTAL_ZONED);
	} else if (xfs_has_metadir(mp)) {
		xfs_warn_experimental(mp, XFS_EXPERIMENTAL_METADIR);
	}

	if (xfs_has_reflink(mp)) {
		if (xfs_has_realtime(mp) &&
		    !xfs_reflink_supports_rextsize(mp, mp->m_sb.sb_rextsize)) {
			xfs_alert(mp,
	"reflink not compatible with realtime extent size %u!",
					mp->m_sb.sb_rextsize);
			error = -EINVAL;
			goto out_filestream_unmount;
		}

		if (xfs_has_zoned(mp)) {
			xfs_alert(mp,
	"reflink not compatible with zoned RT device!");
			error = -EINVAL;
			goto out_filestream_unmount;
		}

		if (xfs_globals.always_cow) {
			xfs_info(mp, "using DEBUG-only always_cow mode.");
			mp->m_always_cow = true;
		}
	}

	/*
	 * If no quota mount options were provided, maybe we'll try to pick
	 * up the quota accounting and enforcement flags from the ondisk sb.
	 */
	if (!(mp->m_qflags & XFS_QFLAGS_MNTOPTS))
		xfs_set_resuming_quotaon(mp);
	mp->m_qflags &= ~XFS_QFLAGS_MNTOPTS;

	error = xfs_mountfs(mp);
	if (error)
		goto out_filestream_unmount;

	root = igrab(VFS_I(mp->m_rootip));
	if (!root) {
		error = -ENOENT;
		goto out_unmount;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		error = -ENOMEM;
		goto out_unmount;
	}

	return 0;

 out_filestream_unmount:
	xfs_filestream_unmount(mp);
 out_free_rtsb:
	xfs_rtmount_freesb(mp);
 out_free_sb:
	xfs_freesb(mp);
 out_free_scrub_stats:
	xchk_mount_stats_free(mp);
 out_free_stats:
	free_percpu(mp->m_stats.xs_stats);
 out_destroy_inodegc:
	xfs_inodegc_free_percpu(mp);
 out_destroy_counters:
	xfs_destroy_percpu_counters(mp);
 out_destroy_workqueues:
	xfs_destroy_mount_workqueues(mp);
 out_shutdown_devices:
	xfs_shutdown_devices(mp);
	return error;

 out_unmount:
	xfs_filestream_unmount(mp);
	xfs_unmountfs(mp);
	goto out_free_rtsb;
}

static int
xfs_fs_get_tree(
	struct fs_context	*fc)
{
	return get_tree_bdev(fc, xfs_fs_fill_super);
}

static int
xfs_remount_rw(
	struct xfs_mount	*mp)
{
	struct xfs_sb		*sbp = &mp->m_sb;
	int error;

	if (mp->m_logdev_targp && mp->m_logdev_targp != mp->m_ddev_targp &&
	    xfs_readonly_buftarg(mp->m_logdev_targp)) {
		xfs_warn(mp,
			"ro->rw transition prohibited by read-only logdev");
		return -EACCES;
	}

	if (mp->m_rtdev_targp && xfs_readonly_buftarg(mp->m_rtdev_targp)) {
		xfs_warn(mp,
			"ro->rw transition prohibited by read-only rtdev");
		return -EACCES;
	}

	if (xfs_has_norecovery(mp)) {
		xfs_warn(mp,
			"ro->rw transition prohibited on norecovery mount");
		return -EINVAL;
	}

	if (xfs_sb_is_v5(sbp) &&
	    xfs_sb_has_ro_compat_feature(sbp, XFS_SB_FEAT_RO_COMPAT_UNKNOWN)) {
		xfs_warn(mp,
	"ro->rw transition prohibited on unknown (0x%x) ro-compat filesystem",
			(sbp->sb_features_ro_compat &
				XFS_SB_FEAT_RO_COMPAT_UNKNOWN));
		return -EINVAL;
	}

	xfs_clear_readonly(mp);

	/*
	 * If this is the first remount to writeable state we might have some
	 * superblock changes to update.
	 */
	if (mp->m_update_sb) {
		error = xfs_sync_sb(mp, false);
		if (error) {
			xfs_warn(mp, "failed to write sb changes");
			return error;
		}
		mp->m_update_sb = false;
	}

	/*
	 * Fill out the reserve pool if it is empty. Use the stashed value if
	 * it is non-zero, otherwise go with the default.
	 */
	xfs_restore_resvblks(mp);
	xfs_log_work_queue(mp);
	xfs_blockgc_start(mp);

	/* Create the per-AG metadata reservation pool .*/
	error = xfs_fs_reserve_ag_blocks(mp);
	if (error && error != -ENOSPC)
		return error;

	/* Re-enable the background inode inactivation worker. */
	xfs_inodegc_start(mp);

	/* Restart zone reclaim */
	xfs_zone_gc_start(mp);

	return 0;
}

static int
xfs_remount_ro(
	struct xfs_mount	*mp)
{
	struct xfs_icwalk	icw = {
		.icw_flags	= XFS_ICWALK_FLAG_SYNC,
	};
	int			error;

	/* Flush all the dirty data to disk. */
	error = sync_filesystem(mp->m_super);
	if (error)
		return error;

	/*
	 * Cancel background eofb scanning so it cannot race with the final
	 * log force+buftarg wait and deadlock the remount.
	 */
	xfs_blockgc_stop(mp);

	/*
	 * Clear out all remaining COW staging extents and speculative post-EOF
	 * preallocations so that we don't leave inodes requiring inactivation
	 * cleanups during reclaim on a read-only mount.  We must process every
	 * cached inode, so this requires a synchronous cache scan.
	 */
	error = xfs_blockgc_free_space(mp, &icw);
	if (error) {
		xfs_force_shutdown(mp, SHUTDOWN_CORRUPT_INCORE);
		return error;
	}

	/*
	 * Stop the inodegc background worker.  xfs_fs_reconfigure already
	 * flushed all pending inodegc work when it sync'd the filesystem.
	 * The VFS holds s_umount, so we know that inodes cannot enter
	 * xfs_fs_destroy_inode during a remount operation.  In readonly mode
	 * we send inodes straight to reclaim, so no inodes will be queued.
	 */
	xfs_inodegc_stop(mp);

	/* Stop zone reclaim */
	xfs_zone_gc_stop(mp);

	/* Free the per-AG metadata reservation pool. */
	xfs_fs_unreserve_ag_blocks(mp);

	/*
	 * Before we sync the metadata, we need to free up the reserve block
	 * pool so that the used block count in the superblock on disk is
	 * correct at the end of the remount. Stash the current* reserve pool
	 * size so that if we get remounted rw, we can return it to the same
	 * size.
	 */
	xfs_save_resvblks(mp);

	xfs_log_clean(mp);
	xfs_set_readonly(mp);

	return 0;
}

/*
 * Logically we would return an error here to prevent users from believing
 * they might have changed mount options using remount which can't be changed.
 *
 * But unfortunately mount(8) adds all options from mtab and fstab to the mount
 * arguments in some cases so we can't blindly reject options, but have to
 * check for each specified option if it actually differs from the currently
 * set option and only reject it if that's the case.
 *
 * Until that is implemented we return success for every remount request, and
 * silently ignore all options that we can't actually change.
 */
static int
xfs_fs_reconfigure(
	struct fs_context *fc)
{
	struct xfs_mount	*mp = XFS_M(fc->root->d_sb);
	struct xfs_mount        *new_mp = fc->s_fs_info;
	int			flags = fc->sb_flags;
	int			error;

	new_mp->m_qflags &= ~XFS_QFLAGS_MNTOPTS;

	/* version 5 superblocks always support version counters. */
	if (xfs_has_crc(mp))
		fc->sb_flags |= SB_I_VERSION;

	error = xfs_fs_validate_params(new_mp);
	if (error)
		return error;

	/* attr2 -> noattr2 */
	if (xfs_has_noattr2(new_mp)) {
		if (xfs_has_crc(mp)) {
			xfs_warn(mp,
			"attr2 is always enabled for a V5 filesystem - can't be changed.");
			return -EINVAL;
		}
		mp->m_features &= ~XFS_FEAT_ATTR2;
		mp->m_features |= XFS_FEAT_NOATTR2;
	} else if (xfs_has_attr2(new_mp)) {
		/* noattr2 -> attr2 */
		mp->m_features &= ~XFS_FEAT_NOATTR2;
		mp->m_features |= XFS_FEAT_ATTR2;
	}

	/* Validate new max_atomic_write option before making other changes */
	if (mp->m_awu_max_bytes != new_mp->m_awu_max_bytes) {
		error = xfs_set_max_atomic_write_opt(mp,
				new_mp->m_awu_max_bytes);
		if (error)
			return error;
	}

	/* inode32 -> inode64 */
	if (xfs_has_small_inums(mp) && !xfs_has_small_inums(new_mp)) {
		mp->m_features &= ~XFS_FEAT_SMALL_INUMS;
		mp->m_maxagi = xfs_set_inode_alloc(mp, mp->m_sb.sb_agcount);
	}

	/* inode64 -> inode32 */
	if (!xfs_has_small_inums(mp) && xfs_has_small_inums(new_mp)) {
		mp->m_features |= XFS_FEAT_SMALL_INUMS;
		mp->m_maxagi = xfs_set_inode_alloc(mp, mp->m_sb.sb_agcount);
	}

	/*
	 * Now that mp has been modified according to the remount options, we
	 * do a final option validation with xfs_finish_flags() just like it is
	 * just like it is done during mount. We cannot use
	 * done during mount. We cannot use xfs_finish_flags() on new_mp as it
	 * contains only the user given options.
	 */
	error = xfs_finish_flags(mp);
	if (error)
		return error;

	/* ro -> rw */
	if (xfs_is_readonly(mp) && !(flags & SB_RDONLY)) {
		error = xfs_remount_rw(mp);
		if (error)
			return error;
	}

	/* rw -> ro */
	if (!xfs_is_readonly(mp) && (flags & SB_RDONLY)) {
		error = xfs_remount_ro(mp);
		if (error)
			return error;
	}

	return 0;
}

static void
xfs_fs_free(
	struct fs_context	*fc)
{
	struct xfs_mount	*mp = fc->s_fs_info;

	/*
	 * mp is stored in the fs_context when it is initialized.
	 * mp is transferred to the superblock on a successful mount,
	 * but if an error occurs before the transfer we have to free
	 * it here.
	 */
	if (mp)
		xfs_mount_free(mp);
}

static const struct fs_context_operations xfs_context_ops = {
	.parse_param = xfs_fs_parse_param,
	.get_tree    = xfs_fs_get_tree,
	.reconfigure = xfs_fs_reconfigure,
	.free        = xfs_fs_free,
};

/*
 * WARNING: do not initialise any parameters in this function that depend on
 * mount option parsing having already been performed as this can be called from
 * fsopen() before any parameters have been set.
 */
static int
xfs_init_fs_context(
	struct fs_context	*fc)
{
	struct xfs_mount	*mp;
	int			i;

	mp = kzalloc(sizeof(struct xfs_mount), GFP_KERNEL | __GFP_NOFAIL);
	if (!mp)
		return -ENOMEM;

	spin_lock_init(&mp->m_sb_lock);
	for (i = 0; i < XG_TYPE_MAX; i++)
		xa_init(&mp->m_groups[i].xa);
	mutex_init(&mp->m_growlock);
	mutex_init(&mp->m_metafile_resv_lock);
	INIT_WORK(&mp->m_flush_inodes_work, xfs_flush_inodes_worker);
	INIT_DELAYED_WORK(&mp->m_reclaim_work, xfs_reclaim_worker);
	mp->m_kobj.kobject.kset = xfs_kset;
	/*
	 * We don't create the finobt per-ag space reservation until after log
	 * recovery, so we must set this to true so that an ifree transaction
	 * started during log recovery will not depend on space reservations
	 * for finobt expansion.
	 */
	mp->m_finobt_nores = true;

	/*
	 * These can be overridden by the mount option parsing.
	 */
	mp->m_logbufs = -1;
	mp->m_logbsize = -1;
	mp->m_allocsize_log = 16; /* 64k */

	xfs_hooks_init(&mp->m_dir_update_hooks);

	fc->s_fs_info = mp;
	fc->ops = &xfs_context_ops;

	return 0;
}

static void
xfs_kill_sb(
	struct super_block		*sb)
{
	kill_block_super(sb);
	xfs_mount_free(XFS_M(sb));
}

static struct file_system_type xfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "xfs",
	.init_fs_context	= xfs_init_fs_context,
	.parameters		= xfs_fs_parameters,
	.kill_sb		= xfs_kill_sb,
	.fs_flags		= FS_REQUIRES_DEV | FS_ALLOW_IDMAP | FS_MGTIME |
				  FS_LBS,
};
MODULE_ALIAS_FS("xfs");

STATIC int __init
xfs_init_caches(void)
{
	int		error;

	xfs_buf_cache = kmem_cache_create("xfs_buf", sizeof(struct xfs_buf), 0,
					 SLAB_HWCACHE_ALIGN |
					 SLAB_RECLAIM_ACCOUNT,
					 NULL);
	if (!xfs_buf_cache)
		goto out;

	xfs_log_ticket_cache = kmem_cache_create("xfs_log_ticket",
						sizeof(struct xlog_ticket),
						0, 0, NULL);
	if (!xfs_log_ticket_cache)
		goto out_destroy_buf_cache;

	error = xfs_btree_init_cur_caches();
	if (error)
		goto out_destroy_log_ticket_cache;

	error = rcbagbt_init_cur_cache();
	if (error)
		goto out_destroy_btree_cur_cache;

	error = xfs_defer_init_item_caches();
	if (error)
		goto out_destroy_rcbagbt_cur_cache;

	xfs_da_state_cache = kmem_cache_create("xfs_da_state",
					      sizeof(struct xfs_da_state),
					      0, 0, NULL);
	if (!xfs_da_state_cache)
		goto out_destroy_defer_item_cache;

	xfs_ifork_cache = kmem_cache_create("xfs_ifork",
					   sizeof(struct xfs_ifork),
					   0, 0, NULL);
	if (!xfs_ifork_cache)
		goto out_destroy_da_state_cache;

	xfs_trans_cache = kmem_cache_create("xfs_trans",
					   sizeof(struct xfs_trans),
					   0, 0, NULL);
	if (!xfs_trans_cache)
		goto out_destroy_ifork_cache;


	/*
	 * The size of the cache-allocated buf log item is the maximum
	 * size possible under XFS.  This wastes a little bit of memory,
	 * but it is much faster.
	 */
	xfs_buf_item_cache = kmem_cache_create("xfs_buf_item",
					      sizeof(struct xfs_buf_log_item),
					      0, 0, NULL);
	if (!xfs_buf_item_cache)
		goto out_destroy_trans_cache;

	xfs_efd_cache = kmem_cache_create("xfs_efd_item",
			xfs_efd_log_item_sizeof(XFS_EFD_MAX_FAST_EXTENTS),
			0, 0, NULL);
	if (!xfs_efd_cache)
		goto out_destroy_buf_item_cache;

	xfs_efi_cache = kmem_cache_create("xfs_efi_item",
			xfs_efi_log_item_sizeof(XFS_EFI_MAX_FAST_EXTENTS),
			0, 0, NULL);
	if (!xfs_efi_cache)
		goto out_destroy_efd_cache;

	xfs_inode_cache = kmem_cache_create("xfs_inode",
					   sizeof(struct xfs_inode), 0,
					   (SLAB_HWCACHE_ALIGN |
					    SLAB_RECLAIM_ACCOUNT |
					    SLAB_ACCOUNT),
					   xfs_fs_inode_init_once);
	if (!xfs_inode_cache)
		goto out_destroy_efi_cache;

	xfs_ili_cache = kmem_cache_create("xfs_ili",
					 sizeof(struct xfs_inode_log_item), 0,
					 SLAB_RECLAIM_ACCOUNT,
					 NULL);
	if (!xfs_ili_cache)
		goto out_destroy_inode_cache;

	xfs_icreate_cache = kmem_cache_create("xfs_icr",
					     sizeof(struct xfs_icreate_item),
					     0, 0, NULL);
	if (!xfs_icreate_cache)
		goto out_destroy_ili_cache;

	xfs_rud_cache = kmem_cache_create("xfs_rud_item",
					 sizeof(struct xfs_rud_log_item),
					 0, 0, NULL);
	if (!xfs_rud_cache)
		goto out_destroy_icreate_cache;

	xfs_rui_cache = kmem_cache_create("xfs_rui_item",
			xfs_rui_log_item_sizeof(XFS_RUI_MAX_FAST_EXTENTS),
			0, 0, NULL);
	if (!xfs_rui_cache)
		goto out_destroy_rud_cache;

	xfs_cud_cache = kmem_cache_create("xfs_cud_item",
					 sizeof(struct xfs_cud_log_item),
					 0, 0, NULL);
	if (!xfs_cud_cache)
		goto out_destroy_rui_cache;

	xfs_cui_cache = kmem_cache_create("xfs_cui_item",
			xfs_cui_log_item_sizeof(XFS_CUI_MAX_FAST_EXTENTS),
			0, 0, NULL);
	if (!xfs_cui_cache)
		goto out_destroy_cud_cache;

	xfs_bud_cache = kmem_cache_create("xfs_bud_item",
					 sizeof(struct xfs_bud_log_item),
					 0, 0, NULL);
	if (!xfs_bud_cache)
		goto out_destroy_cui_cache;

	xfs_bui_cache = kmem_cache_create("xfs_bui_item",
			xfs_bui_log_item_sizeof(XFS_BUI_MAX_FAST_EXTENTS),
			0, 0, NULL);
	if (!xfs_bui_cache)
		goto out_destroy_bud_cache;

	xfs_attrd_cache = kmem_cache_create("xfs_attrd_item",
					    sizeof(struct xfs_attrd_log_item),
					    0, 0, NULL);
	if (!xfs_attrd_cache)
		goto out_destroy_bui_cache;

	xfs_attri_cache = kmem_cache_create("xfs_attri_item",
					    sizeof(struct xfs_attri_log_item),
					    0, 0, NULL);
	if (!xfs_attri_cache)
		goto out_destroy_attrd_cache;

	xfs_iunlink_cache = kmem_cache_create("xfs_iul_item",
					     sizeof(struct xfs_iunlink_item),
					     0, 0, NULL);
	if (!xfs_iunlink_cache)
		goto out_destroy_attri_cache;

	xfs_xmd_cache = kmem_cache_create("xfs_xmd_item",
					 sizeof(struct xfs_xmd_log_item),
					 0, 0, NULL);
	if (!xfs_xmd_cache)
		goto out_destroy_iul_cache;

	xfs_xmi_cache = kmem_cache_create("xfs_xmi_item",
					 sizeof(struct xfs_xmi_log_item),
					 0, 0, NULL);
	if (!xfs_xmi_cache)
		goto out_destroy_xmd_cache;

	xfs_parent_args_cache = kmem_cache_create("xfs_parent_args",
					     sizeof(struct xfs_parent_args),
					     0, 0, NULL);
	if (!xfs_parent_args_cache)
		goto out_destroy_xmi_cache;

	return 0;

 out_destroy_xmi_cache:
	kmem_cache_destroy(xfs_xmi_cache);
 out_destroy_xmd_cache:
	kmem_cache_destroy(xfs_xmd_cache);
 out_destroy_iul_cache:
	kmem_cache_destroy(xfs_iunlink_cache);
 out_destroy_attri_cache:
	kmem_cache_destroy(xfs_attri_cache);
 out_destroy_attrd_cache:
	kmem_cache_destroy(xfs_attrd_cache);
 out_destroy_bui_cache:
	kmem_cache_destroy(xfs_bui_cache);
 out_destroy_bud_cache:
	kmem_cache_destroy(xfs_bud_cache);
 out_destroy_cui_cache:
	kmem_cache_destroy(xfs_cui_cache);
 out_destroy_cud_cache:
	kmem_cache_destroy(xfs_cud_cache);
 out_destroy_rui_cache:
	kmem_cache_destroy(xfs_rui_cache);
 out_destroy_rud_cache:
	kmem_cache_destroy(xfs_rud_cache);
 out_destroy_icreate_cache:
	kmem_cache_destroy(xfs_icreate_cache);
 out_destroy_ili_cache:
	kmem_cache_destroy(xfs_ili_cache);
 out_destroy_inode_cache:
	kmem_cache_destroy(xfs_inode_cache);
 out_destroy_efi_cache:
	kmem_cache_destroy(xfs_efi_cache);
 out_destroy_efd_cache:
	kmem_cache_destroy(xfs_efd_cache);
 out_destroy_buf_item_cache:
	kmem_cache_destroy(xfs_buf_item_cache);
 out_destroy_trans_cache:
	kmem_cache_destroy(xfs_trans_cache);
 out_destroy_ifork_cache:
	kmem_cache_destroy(xfs_ifork_cache);
 out_destroy_da_state_cache:
	kmem_cache_destroy(xfs_da_state_cache);
 out_destroy_defer_item_cache:
	xfs_defer_destroy_item_caches();
 out_destroy_rcbagbt_cur_cache:
	rcbagbt_destroy_cur_cache();
 out_destroy_btree_cur_cache:
	xfs_btree_destroy_cur_caches();
 out_destroy_log_ticket_cache:
	kmem_cache_destroy(xfs_log_ticket_cache);
 out_destroy_buf_cache:
	kmem_cache_destroy(xfs_buf_cache);
 out:
	return -ENOMEM;
}

STATIC void
xfs_destroy_caches(void)
{
	/*
	 * Make sure all delayed rcu free are flushed before we
	 * destroy caches.
	 */
	rcu_barrier();
	kmem_cache_destroy(xfs_parent_args_cache);
	kmem_cache_destroy(xfs_xmd_cache);
	kmem_cache_destroy(xfs_xmi_cache);
	kmem_cache_destroy(xfs_iunlink_cache);
	kmem_cache_destroy(xfs_attri_cache);
	kmem_cache_destroy(xfs_attrd_cache);
	kmem_cache_destroy(xfs_bui_cache);
	kmem_cache_destroy(xfs_bud_cache);
	kmem_cache_destroy(xfs_cui_cache);
	kmem_cache_destroy(xfs_cud_cache);
	kmem_cache_destroy(xfs_rui_cache);
	kmem_cache_destroy(xfs_rud_cache);
	kmem_cache_destroy(xfs_icreate_cache);
	kmem_cache_destroy(xfs_ili_cache);
	kmem_cache_destroy(xfs_inode_cache);
	kmem_cache_destroy(xfs_efi_cache);
	kmem_cache_destroy(xfs_efd_cache);
	kmem_cache_destroy(xfs_buf_item_cache);
	kmem_cache_destroy(xfs_trans_cache);
	kmem_cache_destroy(xfs_ifork_cache);
	kmem_cache_destroy(xfs_da_state_cache);
	xfs_defer_destroy_item_caches();
	rcbagbt_destroy_cur_cache();
	xfs_btree_destroy_cur_caches();
	kmem_cache_destroy(xfs_log_ticket_cache);
	kmem_cache_destroy(xfs_buf_cache);
}

STATIC int __init
xfs_init_workqueues(void)
{
	/*
	 * The allocation workqueue can be used in memory reclaim situations
	 * (writepage path), and parallelism is only limited by the number of
	 * AGs in all the filesystems mounted. Hence use the default large
	 * max_active value for this workqueue.
	 */
	xfs_alloc_wq = alloc_workqueue("xfsalloc",
			XFS_WQFLAGS(WQ_MEM_RECLAIM | WQ_FREEZABLE), 0);
	if (!xfs_alloc_wq)
		return -ENOMEM;

	xfs_discard_wq = alloc_workqueue("xfsdiscard", XFS_WQFLAGS(WQ_UNBOUND),
			0);
	if (!xfs_discard_wq)
		goto out_free_alloc_wq;

	return 0;
out_free_alloc_wq:
	destroy_workqueue(xfs_alloc_wq);
	return -ENOMEM;
}

STATIC void
xfs_destroy_workqueues(void)
{
	destroy_workqueue(xfs_discard_wq);
	destroy_workqueue(xfs_alloc_wq);
}

STATIC int __init
init_xfs_fs(void)
{
	int			error;

	xfs_check_ondisk_structs();

	error = xfs_dahash_test();
	if (error)
		return error;

	printk(KERN_INFO XFS_VERSION_STRING " with "
			 XFS_BUILD_OPTIONS " enabled\n");

	xfs_dir_startup();

	error = xfs_init_caches();
	if (error)
		goto out;

	error = xfs_init_workqueues();
	if (error)
		goto out_destroy_caches;

	error = xfs_mru_cache_init();
	if (error)
		goto out_destroy_wq;

	error = xfs_init_procfs();
	if (error)
		goto out_mru_cache_uninit;

	error = xfs_sysctl_register();
	if (error)
		goto out_cleanup_procfs;

	xfs_debugfs = xfs_debugfs_mkdir("xfs", NULL);

	xfs_kset = kset_create_and_add("xfs", NULL, fs_kobj);
	if (!xfs_kset) {
		error = -ENOMEM;
		goto out_debugfs_unregister;
	}

	xfsstats.xs_kobj.kobject.kset = xfs_kset;

	xfsstats.xs_stats = alloc_percpu(struct xfsstats);
	if (!xfsstats.xs_stats) {
		error = -ENOMEM;
		goto out_kset_unregister;
	}

	error = xfs_sysfs_init(&xfsstats.xs_kobj, &xfs_stats_ktype, NULL,
			       "stats");
	if (error)
		goto out_free_stats;

	error = xchk_global_stats_setup(xfs_debugfs);
	if (error)
		goto out_remove_stats_kobj;

#ifdef DEBUG
	xfs_dbg_kobj.kobject.kset = xfs_kset;
	error = xfs_sysfs_init(&xfs_dbg_kobj, &xfs_dbg_ktype, NULL, "debug");
	if (error)
		goto out_remove_scrub_stats;
#endif

	error = xfs_qm_init();
	if (error)
		goto out_remove_dbg_kobj;

	error = register_filesystem(&xfs_fs_type);
	if (error)
		goto out_qm_exit;
	return 0;

 out_qm_exit:
	xfs_qm_exit();
 out_remove_dbg_kobj:
#ifdef DEBUG
	xfs_sysfs_del(&xfs_dbg_kobj);
 out_remove_scrub_stats:
#endif
	xchk_global_stats_teardown();
 out_remove_stats_kobj:
	xfs_sysfs_del(&xfsstats.xs_kobj);
 out_free_stats:
	free_percpu(xfsstats.xs_stats);
 out_kset_unregister:
	kset_unregister(xfs_kset);
 out_debugfs_unregister:
	debugfs_remove(xfs_debugfs);
	xfs_sysctl_unregister();
 out_cleanup_procfs:
	xfs_cleanup_procfs();
 out_mru_cache_uninit:
	xfs_mru_cache_uninit();
 out_destroy_wq:
	xfs_destroy_workqueues();
 out_destroy_caches:
	xfs_destroy_caches();
 out:
	return error;
}

STATIC void __exit
exit_xfs_fs(void)
{
	xfs_qm_exit();
	unregister_filesystem(&xfs_fs_type);
#ifdef DEBUG
	xfs_sysfs_del(&xfs_dbg_kobj);
#endif
	xchk_global_stats_teardown();
	xfs_sysfs_del(&xfsstats.xs_kobj);
	free_percpu(xfsstats.xs_stats);
	kset_unregister(xfs_kset);
	debugfs_remove(xfs_debugfs);
	xfs_sysctl_unregister();
	xfs_cleanup_procfs();
	xfs_mru_cache_uninit();
	xfs_destroy_workqueues();
	xfs_destroy_caches();
	xfs_uuid_table_free();
}

module_init(init_xfs_fs);
module_exit(exit_xfs_fs);

MODULE_AUTHOR("Silicon Graphics, Inc.");
MODULE_DESCRIPTION(XFS_VERSION_STRING " with " XFS_BUILD_OPTIONS " enabled");
MODULE_LICENSE("GPL");
