/*
 *  linux/fs/nfs/inode.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  nfs inode and superblock handling functions
 *
 *  Modularised by Alan Cox <Alan.Cox@linux.org>, while hacking some
 *  experimental NFS changes. Modularisation taken straight from SYS5 fs.
 *
 *  Change to nfs_read_super() to permit NFS mounts to multi-homed hosts.
 *  J.S.Peatfield@damtp.cam.ac.uk
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/locks.h>
#include <linux/unistd.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/stats.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/nfs_flushd.h>
#include <linux/lockd/bind.h>
#include <linux/smp_lock.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#define CONFIG_NFS_SNAPSHOT 1
#define NFSDBG_FACILITY		NFSDBG_VFS
#define NFS_PARANOIA 1

static struct inode * __nfs_fhget(struct super_block *, struct nfs_fattr *);
void nfs_zap_caches(struct inode *);
static void nfs_invalidate_inode(struct inode *);

static void nfs_read_inode(struct inode *);
static void nfs_put_inode(struct inode *);
static void nfs_delete_inode(struct inode *);
static void nfs_put_super(struct super_block *);
static void nfs_umount_begin(struct super_block *);
static int  nfs_statfs(struct super_block *, struct statfs *);

static struct super_operations nfs_sops = { 
	read_inode:	nfs_read_inode,
	put_inode:	nfs_put_inode,
	delete_inode:	nfs_delete_inode,
	put_super:	nfs_put_super,
	statfs:		nfs_statfs,
	umount_begin:	nfs_umount_begin,
};

/*
 * RPC cruft for NFS
 */
struct rpc_stat			nfs_rpcstat = { &nfs_program };
static struct rpc_version *	nfs_version[] = {
	NULL,
	NULL,
	&nfs_version2,
#ifdef CONFIG_NFS_V3
	&nfs_version3,
#endif
};

struct rpc_program		nfs_program = {
	"nfs",
	NFS_PROGRAM,
	sizeof(nfs_version) / sizeof(nfs_version[0]),
	nfs_version,
	&nfs_rpcstat,
};

static inline unsigned long
nfs_fattr_to_ino_t(struct nfs_fattr *fattr)
{
	return nfs_fileid_to_ino_t(fattr->fileid);
}

/*
 * The "read_inode" function doesn't actually do anything:
 * the real data is filled in later in nfs_fhget. Here we
 * just mark the cache times invalid, and zero out i_mode
 * (the latter makes "nfs_refresh_inode" do the right thing
 * wrt pipe inodes)
 */
static void
nfs_read_inode(struct inode * inode)
{
	inode->i_blksize = inode->i_sb->s_blocksize;
	inode->i_mode = 0;
	inode->i_rdev = 0;
	NFS_FILEID(inode) = 0;
	NFS_FSID(inode) = 0;
	INIT_LIST_HEAD(&inode->u.nfs_i.dirty);
	INIT_LIST_HEAD(&inode->u.nfs_i.commit);
	INIT_LIST_HEAD(&inode->u.nfs_i.writeback);
	inode->u.nfs_i.ndirty = 0;
	inode->u.nfs_i.ncommit = 0;
	inode->u.nfs_i.npages = 0;
	NFS_CACHEINV(inode);
	NFS_ATTRTIMEO(inode) = NFS_MINATTRTIMEO(inode);
	NFS_ATTRTIMEO_UPDATE(inode) = jiffies;
}

static void
nfs_put_inode(struct inode * inode)
{
	dprintk("NFS: put_inode(%x/%ld)\n", inode->i_dev, inode->i_ino);
	/*
	 * We want to get rid of unused inodes ...
	 */
	if (inode->i_count == 1)
		inode->i_nlink = 0;
}

static void
nfs_delete_inode(struct inode * inode)
{
	dprintk("NFS: delete_inode(%x/%ld)\n", inode->i_dev, inode->i_ino);

	/*
	 * The following can never actually happen...
	 */
	if (nfs_have_writebacks(inode)) {
		printk(KERN_ERR "nfs_delete_inode: inode %ld has pending RPC requests\n", inode->i_ino);
	}

	clear_inode(inode);
}

void
nfs_put_super(struct super_block *sb)
{
	struct nfs_server *server = &sb->u.nfs_sb.s_server;
	struct rpc_clnt	*rpc;

	/*
	 * First get rid of the request flushing daemon.
	 * Relies on rpc_shutdown_client() waiting on all
	 * client tasks to finish.
	 */
	nfs_reqlist_exit(server);

	if ((rpc = server->client) != NULL)
		rpc_shutdown_client(rpc);

	nfs_reqlist_free(server);

	if (!(server->flags & NFS_MOUNT_NONLM))
		lockd_down();	/* release rpc.lockd */
	rpciod_down();		/* release rpciod */

	kfree(server->hostname);
}

void
nfs_umount_begin(struct super_block *sb)
{
	struct nfs_server *server = &sb->u.nfs_sb.s_server;
	struct rpc_clnt	*rpc;

	/* -EIO all pending I/O */
	if ((rpc = server->client) != NULL)
		rpc_killall_tasks(rpc);
}


static inline unsigned long
nfs_block_bits(unsigned long bsize, unsigned char *nrbitsp)
{
	/* make sure blocksize is a power of two */
	if ((bsize & (bsize - 1)) || nrbitsp) {
		unsigned char	nrbits;

		for (nrbits = 31; nrbits && !(bsize & (1 << nrbits)); nrbits--)
			;
		bsize = 1 << nrbits;
		if (nrbitsp)
			*nrbitsp = nrbits;
	}

	return bsize;
}

/*
 * Calculate the number of 512byte blocks used.
 */
static inline unsigned long
nfs_calc_block_size(u64 tsize)
{
	loff_t used = (tsize + 511) >> 9;
	return (used > ULONG_MAX) ? ULONG_MAX : used;
}

/*
 * Compute and set NFS server blocksize
 */
static inline unsigned long
nfs_block_size(unsigned long bsize, unsigned char *nrbitsp)
{
	if (bsize < 1024)
		bsize = NFS_DEF_FILE_IO_BUFFER_SIZE;
	else if (bsize >= NFS_MAX_FILE_IO_BUFFER_SIZE)
		bsize = NFS_MAX_FILE_IO_BUFFER_SIZE;

	return nfs_block_bits(bsize, nrbitsp);
}

/*
 * Obtain the root inode of the file system.
 */
static struct inode *
nfs_get_root(struct super_block *sb, struct nfs_fh *rootfh)
{
	struct nfs_server	*server = &sb->u.nfs_sb.s_server;
	struct nfs_fattr	fattr;
	struct inode		*inode;
	int			error;

	if ((error = server->rpc_ops->getroot(server, rootfh, &fattr)) < 0) {
		printk(KERN_NOTICE "nfs_get_root: getattr error = %d\n", -error);
		return NULL;
	}

	inode = __nfs_fhget(sb, &fattr);
	return inode;
}

extern struct nfs_fh *nfs_fh_alloc(void);
extern void nfs_fh_free(struct nfs_fh *p);

/*
 * The way this works is that the mount process passes a structure
 * in the data argument which contains the server's IP address
 * and the root file handle obtained from the server's mount
 * daemon. We stash these away in the private superblock fields.
 */
struct super_block *
nfs_read_super(struct super_block *sb, void *raw_data, int silent)
{
	struct nfs_mount_data	*data = (struct nfs_mount_data *) raw_data;
	struct nfs_server	*server;
	struct rpc_xprt		*xprt = NULL;
	struct rpc_clnt		*clnt = NULL;
	struct nfs_fh		*root = &data->root, *root_fh, fh;
	struct inode		*root_inode = NULL;
	unsigned int		authflavor;
	struct sockaddr_in	srvaddr;
	struct rpc_timeout	timeparms;
	struct nfs_fsinfo	fsinfo;
	int			tcp, version, maxlen;

	memset(&sb->u.nfs_sb, 0, sizeof(sb->u.nfs_sb));
	if (!data)
		goto out_miss_args;

	memset(&fh, 0, sizeof(fh));
	if (data->version != NFS_MOUNT_VERSION) {
		printk("nfs warning: mount version %s than kernel\n",
			data->version < NFS_MOUNT_VERSION ? "older" : "newer");
		if (data->version < 2)
			data->namlen = 0;
		if (data->version < 3)
			data->bsize  = 0;
		if (data->version < 4) {
			data->flags &= ~NFS_MOUNT_VER3;
			root = &fh;
			root->size = NFS2_FHSIZE;
			memcpy(root->data, data->old_root.data, NFS2_FHSIZE);
		}
	}

	/* We now require that the mount process passes the remote address */
	memcpy(&srvaddr, &data->addr, sizeof(srvaddr));
	if (srvaddr.sin_addr.s_addr == INADDR_ANY)
		goto out_no_remote;

	sb->s_flags |= MS_ODD_RENAME; /* This should go away */

	sb->s_magic      = NFS_SUPER_MAGIC;
	sb->s_op         = &nfs_sops;
	sb->s_blocksize_bits = 0;
	sb->s_blocksize  = nfs_block_size(data->bsize, &sb->s_blocksize_bits);
	server           = &sb->u.nfs_sb.s_server;
	server->rsize    = nfs_block_size(data->rsize, NULL);
	server->wsize    = nfs_block_size(data->wsize, NULL);
	server->flags    = data->flags & NFS_MOUNT_FLAGMASK;

	if (data->flags & NFS_MOUNT_NOAC) {
		data->acregmin = data->acregmax = 0;
		data->acdirmin = data->acdirmax = 0;
	}
	server->acregmin = data->acregmin*HZ;
	server->acregmax = data->acregmax*HZ;
	server->acdirmin = data->acdirmin*HZ;
	server->acdirmax = data->acdirmax*HZ;

	server->namelen  = data->namlen;
	server->hostname = kmalloc(strlen(data->hostname) + 1, GFP_KERNEL);
	if (!server->hostname)
		goto out_unlock;
	strcpy(server->hostname, data->hostname);

 nfsv3_try_again:
	/* Check NFS protocol revision and initialize RPC op vector
	 * and file handle pool. */
	if (data->flags & NFS_MOUNT_VER3) {
#ifdef CONFIG_NFS_V3
		server->rpc_ops = &nfs_v3_clientops;
		version = 3;
		if (data->version < 4) {
			printk(KERN_NOTICE "NFS: NFSv3 not supported by mount program.\n");
			goto out_unlock;
		}
#else
		printk(KERN_NOTICE "NFS: NFSv3 not supported.\n");
		goto out_unlock;
#endif
	} else {
		server->rpc_ops = &nfs_v2_clientops;
		version = 2;
        }

	/* Which protocol do we use? */
	tcp   = (data->flags & NFS_MOUNT_TCP);

	/* Initialize timeout values */
	timeparms.to_initval = data->timeo * HZ / 10;
	timeparms.to_retries = data->retrans;
	timeparms.to_maxval  = tcp? RPC_MAX_TCP_TIMEOUT : RPC_MAX_UDP_TIMEOUT;
	timeparms.to_exponential = 1;

	if (!timeparms.to_initval)
		timeparms.to_initval = (tcp ? 600 : 11) * HZ / 10;
	if (!timeparms.to_retries)
		timeparms.to_retries = 5;

	/* Now create transport and client */
	xprt = xprt_create_proto(tcp? IPPROTO_TCP : IPPROTO_UDP,
						&srvaddr, &timeparms);
	if (xprt == NULL)
		goto out_no_xprt;

	/* Choose authentication flavor */
	authflavor = RPC_AUTH_UNIX;
	if (data->flags & NFS_MOUNT_SECURE)
		authflavor = RPC_AUTH_DES;
	else if (data->flags & NFS_MOUNT_KERBEROS)
		authflavor = RPC_AUTH_KRB;

	clnt = rpc_create_client(xprt, server->hostname, &nfs_program,
				 version, authflavor);
	if (clnt == NULL)
		goto out_no_client;

	clnt->cl_intr     = (data->flags & NFS_MOUNT_INTR)? 1 : 0;
	clnt->cl_softrtry = (data->flags & NFS_MOUNT_SOFT)? 1 : 0;
	clnt->cl_chatty   = 1;
	server->client    = clnt;

	/* Fire up rpciod if not yet running */
	if (rpciod_up() != 0)
		goto out_no_iod;

	/*
	 * Keep the super block locked while we try to get 
	 * the root fh attributes.
	 */
	root_fh = nfs_fh_alloc();
	if (!root_fh)
		goto out_no_fh;
	memcpy((u8*)root_fh, (u8*)root, sizeof(*root));

	/* Did getting the root inode fail? */
	if (!(root_inode = nfs_get_root(sb, root))
	    && (data->flags & NFS_MOUNT_VER3)) {
		data->flags &= ~NFS_MOUNT_VER3;
		nfs_fh_free(root_fh);
		rpciod_down();
		rpc_shutdown_client(server->client);
		goto nfsv3_try_again;
	}

	if (!root_inode)
		goto out_no_root;
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root)
		goto out_no_root;

	sb->s_root->d_op = &nfs_dentry_operations;
	sb->s_root->d_fsdata = root_fh;

	/* Get some general file system info */
        if (server->rpc_ops->statfs(server, root, &fsinfo) >= 0) {
		if (server->namelen == 0)
			server->namelen = fsinfo.namelen;
	} else {
		printk(KERN_NOTICE "NFS: cannot retrieve file system info.\n");
		goto out_no_root;
        }

	/* Work out a lot of parameters */
	if (data->rsize == 0)
		server->rsize = nfs_block_size(fsinfo.rtpref, NULL);
	if (data->wsize == 0)
		server->wsize = nfs_block_size(fsinfo.wtpref, NULL);
	server->dtsize = nfs_block_size(fsinfo.dtpref, NULL);
	/* NFSv3: we don't have bsize, but rather rtmult and wtmult... */
	if (!fsinfo.bsize)
		fsinfo.bsize = (fsinfo.rtmult>fsinfo.wtmult) ? fsinfo.rtmult : fsinfo.wtmult;
	/* Also make sure we don't go below rsize/wsize since
	 * RPC calls are expensive */
	if (fsinfo.bsize < server->rsize)
		fsinfo.bsize = server->rsize;
	if (fsinfo.bsize < server->wsize)
		fsinfo.bsize = server->wsize;

	if (data->bsize == 0)
		sb->s_blocksize = nfs_block_bits(fsinfo.bsize, &sb->s_blocksize_bits);
	if (server->rsize > fsinfo.rtmax)
		server->rsize = fsinfo.rtmax;
	if (server->rsize > PAGE_CACHE_SIZE)
		server->rsize = PAGE_CACHE_SIZE;
	if (server->wsize > fsinfo.wtmax)
		server->wsize = fsinfo.wtmax;
        if (server->wsize > NFS_WRITE_MAXIOV << PAGE_CACHE_SHIFT)
                server->wsize = NFS_WRITE_MAXIOV << PAGE_CACHE_SHIFT;

        maxlen = (version == 2) ? NFS2_MAXNAMLEN : NFS3_MAXNAMLEN;

        if (server->namelen == 0 || server->namelen > maxlen)
                server->namelen = maxlen;

	/* Fire up the writeback cache */
	if (nfs_reqlist_alloc(server) < 0) {
		printk(KERN_NOTICE "NFS: cannot initialize writeback cache.\n");
		goto failure_kill_reqlist;
	}

	/* We're airborne */

	/* Check whether to start the lockd process */
	if (!(server->flags & NFS_MOUNT_NONLM))
		lockd_up();
	return sb;

	/* Yargs. It didn't work out. */
 failure_kill_reqlist:
	nfs_reqlist_exit(server);
out_no_root:
	printk("nfs_read_super: get root inode failed\n");
	iput(root_inode);
	nfs_fh_free(root_fh);
out_no_fh:
	rpciod_down();
	goto out_shutdown;

out_no_iod:
	printk(KERN_WARNING "NFS: couldn't start rpciod!\n");
out_shutdown:
	rpc_shutdown_client(server->client);
	goto out_free_host;

out_no_client:
	printk(KERN_WARNING "NFS: cannot create RPC client.\n");
	xprt_destroy(xprt);
	goto out_free_host;

out_no_xprt:
	printk(KERN_WARNING "NFS: cannot create RPC transport.\n");

out_free_host:
	nfs_reqlist_free(server);
	kfree(server->hostname);
out_unlock:
	goto out_fail;

out_no_remote:
	printk("NFS: mount program didn't pass remote address!\n");
	goto out_fail;

out_miss_args:
	printk("nfs_read_super: missing data argument\n");

out_fail:
	return NULL;
}

static int
nfs_statfs(struct super_block *sb, struct statfs *buf)
{
	struct nfs_server *server = &sb->u.nfs_sb.s_server;
	unsigned char blockbits;
	unsigned long blockres;
	struct nfs_fsinfo res;
	int error;

	error = server->rpc_ops->statfs(server, NFS_FH(sb->s_root), &res);
	buf->f_type = NFS_SUPER_MAGIC;
	if (error < 0)
		goto out_err;

	if (res.bsize == 0)
		res.bsize = sb->s_blocksize;
	buf->f_bsize = nfs_block_bits(res.bsize, &blockbits);
	blockres = (1 << blockbits) - 1;
	buf->f_blocks = (res.tbytes + blockres) >> blockbits;
	buf->f_bfree = (res.fbytes + blockres) >> blockbits;
	buf->f_bavail = (res.abytes + blockres) >> blockbits;
	buf->f_files = res.tfiles;
	buf->f_ffree = res.afiles;
	if (res.namelen == 0 || res.namelen > server->namelen)
		res.namelen = server->namelen;
	buf->f_namelen = res.namelen;
	return 0;
 out_err:
	printk("nfs_statfs: statfs error = %d\n", -error);
	buf->f_bsize = buf->f_blocks = buf->f_bfree = buf->f_bavail = -1;
	return 0;
}

/*
 * Free all unused dentries in an inode's alias list.
 *
 * Subtle note: we have to be very careful not to cause
 * any IO operations with the stale dentries, as this
 * could cause file corruption. But since the dentry
 * count is 0 and all pending IO for a dentry has been
 * flushed when the count went to 0, we're safe here.
 * Also returns the number of unhashed dentries
 */
static int
nfs_free_dentries(struct inode *inode)
{
	struct list_head *tmp, *head = &inode->i_dentry;
	int unhashed;

restart:
	tmp = head;
	unhashed = 0;
	while ((tmp = tmp->next) != head) {
		struct dentry *dentry = list_entry(tmp, struct dentry, d_alias);
		dprintk("nfs_free_dentries: found %s/%s, d_count=%d, hashed=%d\n",
			dentry->d_parent->d_name.name, dentry->d_name.name,
			dentry->d_count, !d_unhashed(dentry));
		if (!list_empty(&dentry->d_subdirs))
			shrink_dcache_parent(dentry);
		if (!dentry->d_count) {
			dget(dentry);
			d_drop(dentry);
			dput(dentry);
			goto restart;
		}
		if (d_unhashed(dentry))
			unhashed++;
	}
	return unhashed;
}

/*
 * Invalidate the local caches
 */
void
nfs_zap_caches(struct inode *inode)
{
	NFS_ATTRTIMEO(inode) = NFS_MINATTRTIMEO(inode);
	NFS_ATTRTIMEO_UPDATE(inode) = jiffies;

	invalidate_inode_pages(inode);

	memset(NFS_COOKIEVERF(inode), 0, sizeof(NFS_COOKIEVERF(inode)));
	NFS_CACHEINV(inode);
}

/*
 * Invalidate, but do not unhash, the inode
 */
static void
nfs_invalidate_inode(struct inode *inode)
{
	umode_t save_mode = inode->i_mode;

	make_bad_inode(inode);
	inode->i_mode = save_mode;
	nfs_zap_caches(inode);
}

/*
 * Fill in inode information from the fattr.
 */
static void
nfs_fill_inode(struct inode *inode, struct nfs_fattr *fattr)
{
	/*
	 * Check whether the mode has been set, as we only want to
	 * do this once. (We don't allow inodes to change types.)
	 */
	if (inode->i_mode == 0) {
		NFS_FILEID(inode) = fattr->fileid;
		NFS_FSID(inode) = fattr->fsid;
		inode->i_mode = fattr->mode;
		/* Why so? Because we want revalidate for devices/FIFOs, and
		 * that's precisely what we have in nfs_file_inode_operations.
		 */
		inode->i_op = &nfs_file_inode_operations;
		if (S_ISREG(inode->i_mode)) {
			inode->i_fop = &nfs_file_operations;
			inode->i_data.a_ops = &nfs_file_aops;
		} else if (S_ISDIR(inode->i_mode)) {
			inode->i_op = &nfs_dir_inode_operations;
			inode->i_fop = &nfs_dir_operations;
		} else if (S_ISLNK(inode->i_mode))
			inode->i_op = &nfs_symlink_inode_operations;
		else
			init_special_inode(inode, inode->i_mode, fattr->rdev);
		/*
		 * Preset the size and mtime, as there's no need
		 * to invalidate the caches.
		 */ 
		inode->i_size  = nfs_size_to_loff_t(fattr->size);
		inode->i_mtime = nfs_time_to_secs(fattr->mtime);
		inode->i_atime = nfs_time_to_secs(fattr->atime);
		inode->i_ctime = nfs_time_to_secs(fattr->ctime);
		NFS_CACHE_CTIME(inode) = fattr->ctime;
		NFS_CACHE_MTIME(inode) = fattr->mtime;
		NFS_CACHE_ATIME(inode) = fattr->atime;
		NFS_CACHE_ISIZE(inode) = fattr->size;
		NFS_ATTRTIMEO(inode) = NFS_MINATTRTIMEO(inode);
		NFS_ATTRTIMEO_UPDATE(inode) = jiffies;
	}
	nfs_refresh_inode(inode, fattr);
}

/*
 * In NFSv3 we can have 64bit inode numbers. In order to support
 * this, and re-exported directories (also seen in NFSv2)
 * we are forced to allow 2 different inodes to have the same
 * i_ino.
 */
static int
nfs_find_actor(struct inode *inode, unsigned long ino, void *opaque)
{
	struct nfs_fattr *fattr = (struct nfs_fattr *)opaque;
	if (NFS_FSID(inode) != fattr->fsid)
		return 0;
	if (NFS_FILEID(inode) != fattr->fileid)
		return 0;
	return 1;
}

static int
nfs_inode_is_stale(struct inode *inode, struct nfs_fattr *fattr)
{
	int unhashed;
	int is_stale = 0;

	if (inode->i_mode &&
	    (fattr->mode & S_IFMT) != (inode->i_mode & S_IFMT))
		is_stale = 1;

	if (is_bad_inode(inode))
		is_stale = 1;

	/*
	 * If the inode seems stale, free up cached dentries.
	 */
	unhashed = nfs_free_dentries(inode);

	/* Assume we're holding an i_count
	 *
	 * NB: sockets sometimes have volatile file handles
	 *     don't invalidate their inodes even if all dentries are
	 *     unhashed.
	 */
	if (unhashed && inode->i_count == unhashed + 1
	    && !S_ISSOCK(inode->i_mode) && !S_ISFIFO(inode->i_mode))
		is_stale = 1;

	return is_stale;
}

/*
 * This is our own version of iget that looks up inodes by file handle
 * instead of inode number.  We use this technique instead of using
 * the vfs read_inode function because there is no way to pass the
 * file handle or current attributes into the read_inode function.
 *
 * We provide a special check for NetApp .snapshot directories to avoid
 * inode aliasing problems. All snapshot inodes are anonymous (unhashed).
 */
struct inode *
nfs_fhget(struct dentry *dentry, struct nfs_fh *fhandle,
				 struct nfs_fattr *fattr)
{
	struct super_block *sb = dentry->d_sb;

	dprintk("NFS: nfs_fhget(%s/%s fileid=%Ld)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
		(long long)fattr->fileid);

	/* Install the file handle in the dentry */
	*((struct nfs_fh *) dentry->d_fsdata) = *fhandle;

#ifdef CONFIG_NFS_SNAPSHOT
	/*
	 * Check for NetApp snapshot dentries, and get an 
	 * unhashed inode to avoid aliasing problems.
	 */
	if ((dentry->d_parent->d_inode->u.nfs_i.flags & NFS_IS_SNAPSHOT) ||
	    (dentry->d_name.len == 9 &&
	     memcmp(dentry->d_name.name, ".snapshot", 9) == 0)) {
		struct inode *inode = get_empty_inode();
		if (!inode)
			goto out;	
		inode->i_sb = sb;
		inode->i_dev = sb->s_dev;
		inode->i_flags = 0;
		inode->i_ino = nfs_fattr_to_ino_t(fattr);
		nfs_read_inode(inode);
		nfs_fill_inode(inode, fattr);
		inode->u.nfs_i.flags |= NFS_IS_SNAPSHOT;
		dprintk("NFS: nfs_fhget(snapshot ino=%ld)\n", inode->i_ino);
	out:
		return inode;
	}
#endif
	return __nfs_fhget(sb, fattr);
}

/*
 * Look up the inode by super block and fattr->fileid.
 *
 * Note carefully the special handling of busy inodes (i_count > 1).
 * With the kernel 2.1.xx dcache all inodes except hard links must
 * have i_count == 1 after iget(). Otherwise, it indicates that the
 * server has reused a fileid (i_ino) and we have a stale inode.
 */
static struct inode *
__nfs_fhget(struct super_block *sb, struct nfs_fattr *fattr)
{
	struct inode *inode = NULL;
	unsigned long ino;

	if ((fattr->valid & NFS_ATTR_FATTR) == 0)
		goto out_no_inode;

	if (!fattr->nlink) {
		printk("NFS: Buggy server - nlink == 0!\n");
		goto out_no_inode;
	}

	ino = nfs_fattr_to_ino_t(fattr);

	while((inode = iget4(sb, ino, nfs_find_actor, fattr)) != NULL) {

		/*
		 * Check for busy inodes, and attempt to get rid of any
		 * unused local references. If successful, we release the
		 * inode and try again.
		 *
		 * Note that the busy test uses the values in the fattr,
		 * as the inode may have become a different object.
		 * (We can probably handle modes changes here, too.)
		 */
		if (!nfs_inode_is_stale(inode,fattr))
			break;

		dprintk("__nfs_fhget: inode %ld still busy, i_count=%d\n",
		       inode->i_ino, inode->i_count);
		nfs_zap_caches(inode);
		remove_inode_hash(inode);
		iput(inode);
	}

	if (!inode)
		goto out_no_inode;

	nfs_fill_inode(inode, fattr);
	dprintk("NFS: __nfs_fhget(%x/%ld ct=%d)\n",
		inode->i_dev, inode->i_ino, inode->i_count);

out:
	return inode;

out_no_inode:
	printk("__nfs_fhget: iget failed\n");
	goto out;
}

int
nfs_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct nfs_fattr fattr;
	int error;

	/*
	 * Make sure the inode is up-to-date.
	 */
	error = nfs_revalidate(dentry);
	if (error) {
#ifdef NFS_PARANOIA
printk("nfs_notify_change: revalidate failed, error=%d\n", error);
#endif
		goto out;
	}

	if (!S_ISREG(inode->i_mode))
		attr->ia_valid &= ~ATTR_SIZE;

	error = nfs_wb_all(inode);
	if (error)
		goto out;

	error = NFS_PROTO(inode)->setattr(dentry, &fattr, attr);
	if (error)
		goto out;
	/*
	 * If we changed the size or mtime, update the inode
	 * now to avoid invalidating the page cache.
	 */
	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size != fattr.size)
			printk("nfs_notify_change: attr=%Ld, fattr=%Ld??\n",
			       (long long) attr->ia_size, (long long)fattr.size);
		vmtruncate(inode, attr->ia_size);
	}

	/*
	 * If we changed the size or mtime, update the inode
	 * now to avoid invalidating the page cache.
	 */
	if (!(fattr.valid & NFS_ATTR_WCC)) {
		fattr.pre_size = NFS_CACHE_ISIZE(inode);
		fattr.pre_mtime = NFS_CACHE_MTIME(inode);
		fattr.pre_ctime = NFS_CACHE_CTIME(inode);
		fattr.valid |= NFS_ATTR_WCC;
	}
	error = nfs_refresh_inode(inode, &fattr);
out:
	return error;
}

/*
 * Wait for the inode to get unlocked.
 * (Used for NFS_INO_LOCKED and NFS_INO_REVALIDATING).
 */
int
nfs_wait_on_inode(struct inode *inode, int flag)
{
	struct rpc_clnt	*clnt = NFS_CLIENT(inode);
	int error;
	if (!(NFS_FLAGS(inode) & flag))
		return 0;
	inode->i_count++;
	error = nfs_wait_event(clnt, inode->i_wait, !(NFS_FLAGS(inode) & flag));
	iput(inode);
	return error;
}

/*
 * Externally visible revalidation function
 */
int
nfs_revalidate(struct dentry *dentry)
{
	return nfs_revalidate_inode(NFS_DSERVER(dentry), dentry);
}

/*
 * These are probably going to contain hooks for
 * allocating and releasing RPC credentials for
 * the file. I'll have to think about Tronds patch
 * a bit more..
 */
int nfs_open(struct inode *inode, struct file *filp)
{
	return 0;
}

int nfs_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * This function is called whenever some part of NFS notices that
 * the cached attributes have to be refreshed.
 */
int
__nfs_revalidate_inode(struct nfs_server *server, struct dentry *dentry)
{
	struct inode	*inode = dentry->d_inode;
	int		 status = 0;
	struct nfs_fattr fattr;

	dfprintk(PAGECACHE, "NFS: revalidating %s/%s, ino=%ld\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
		inode->i_ino);

	if (!inode || is_bad_inode(inode))
		return -ESTALE;

	while (NFS_REVALIDATING(inode)) {
		status = nfs_wait_on_inode(inode, NFS_INO_REVALIDATING);
		if (status < 0)
			return status;
		if (time_before(jiffies,NFS_READTIME(inode)+NFS_ATTRTIMEO(inode)))
			return 0;
	}
	NFS_FLAGS(inode) |= NFS_INO_REVALIDATING;

	status = NFS_PROTO(inode)->getattr(dentry, &fattr);
	if (status) {
		struct dentry *dir = dentry->d_parent;
		struct inode *dir_i = dir->d_inode;
		int error;
		u32 *fh;
		struct nfs_fh fhandle;
		dfprintk(PAGECACHE, "nfs_revalidate_inode: %s/%s getattr failed, ino=%ld, error=%d\n",
			 dir->d_name.name, dentry->d_name.name,
			 inode->i_ino, status);
		if (status != -ESTALE)
			goto out;
		/*
		 * A "stale filehandle" error ... show the current fh
		 * and find out what the filehandle should be.
		 */
		fh = (u32 *) NFS_FH(dentry)->data;
		dfprintk(PAGECACHE, "NFS: bad fh %08x%08x%08x%08x%08x%08x%08x%08x\n",
			fh[0],fh[1],fh[2],fh[3],fh[4],fh[5],fh[6],fh[7]);
		error = NFS_PROTO(dir_i)->lookup(dir, &dentry->d_name,
						 &fhandle, &fattr);
		if (error) {
			dfprintk(PAGECACHE, "NFS: lookup failed, error=%d\n", error);
			goto out;
		}
		fh = (u32 *) fhandle.data;
		dfprintk(PAGECACHE, "            %08x%08x%08x%08x%08x%08x%08x%08x\n",
			fh[0],fh[1],fh[2],fh[3],fh[4],fh[5],fh[6],fh[7]);
		goto out;
	}

	status = nfs_refresh_inode(inode, &fattr);
	if (status) {
		dfprintk(PAGECACHE, "nfs_revalidate_inode: %s/%s refresh failed, ino=%ld, error=%d\n",
			dentry->d_parent->d_name.name,
			dentry->d_name.name, inode->i_ino, status);
		goto out;
	}
	dfprintk(PAGECACHE, "NFS: %s/%s revalidation complete\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);
out:
	NFS_FLAGS(inode) &= ~NFS_INO_REVALIDATING;
	wake_up(&inode->i_wait);
	return status;
}

/*
 * Many nfs protocol calls return the new file attributes after
 * an operation.  Here we update the inode to reflect the state
 * of the server's inode.
 *
 * This is a bit tricky because we have to make sure all dirty pages
 * have been sent off to the server before calling invalidate_inode_pages.
 * To make sure no other process adds more write requests while we try
 * our best to flush them, we make them sleep during the attribute refresh.
 *
 * A very similar scenario holds for the dir cache.
 */
int
nfs_refresh_inode(struct inode *inode, struct nfs_fattr *fattr)
{
	__u64		new_size, new_mtime;
	loff_t		new_isize;
	int		invalid = 0;
	int		error = -EIO;

	if (!inode || !fattr) {
		printk(KERN_ERR "nfs_refresh_inode: inode or fattr is NULL\n");
		goto out;
	}
	if (inode->i_mode == 0) {
		printk(KERN_ERR "nfs_refresh_inode: empty inode\n");
		goto out;
	}

	if ((fattr->valid & NFS_ATTR_FATTR) == 0)
		goto out;

	if (is_bad_inode(inode))
		goto out;

	dfprintk(VFS, "NFS: refresh_inode(%x/%ld ct=%d info=0x%x)\n",
			inode->i_dev, inode->i_ino, inode->i_count,
			fattr->valid);


	if (NFS_FSID(inode) != fattr->fsid ||
	    NFS_FILEID(inode) != fattr->fileid) {
		printk(KERN_ERR "nfs_refresh_inode: inode number mismatch\n"
		       "expected (0x%Lx/0x%Lx), got (0x%Lx/0x%Lx)\n",
		       (long long)NFS_FSID(inode), (long long)NFS_FILEID(inode),
		       (long long)fattr->fsid, (long long)fattr->fileid);
		goto out;
	}

	/*
	 * Make sure the inode's type hasn't changed.
	 */
	if ((inode->i_mode & S_IFMT) != (fattr->mode & S_IFMT))
		goto out_changed;

 	new_mtime = fattr->mtime;
	new_size = fattr->size;
 	new_isize = nfs_size_to_loff_t(fattr->size);

	error = 0;

	/*
	 * Update the read time so we don't revalidate too often.
	 */
	NFS_READTIME(inode) = jiffies;

	/*
	 * Note: NFS_CACHE_ISIZE(inode) reflects the state of the cache.
	 *       NOT inode->i_size!!!
	 */
	if (NFS_CACHE_ISIZE(inode) != new_size) {
#ifdef NFS_DEBUG_VERBOSE
		printk(KERN_DEBUG "NFS: isize change on %x/%ld\n", inode->i_dev, inode->i_ino);
#endif
		invalid = 1;
	}

	/*
	 * Note: we don't check inode->i_mtime since pipes etc.
	 *       can change this value in VFS without requiring a
	 *	 cache revalidation.
	 */
	if (NFS_CACHE_MTIME(inode) != new_mtime) {
#ifdef NFS_DEBUG_VERBOSE
		printk(KERN_DEBUG "NFS: mtime change on %x/%ld\n", inode->i_dev, inode->i_ino);
#endif
		invalid = 1;
	}

	/* Check Weak Cache Consistency data.
	 * If size and mtime match the pre-operation values, we can
	 * assume that any attribute changes were caused by our NFS
         * operation, so there's no need to invalidate the caches.
         */
        if ((fattr->valid & NFS_ATTR_WCC)
	    && NFS_CACHE_ISIZE(inode) == fattr->pre_size
	    && NFS_CACHE_MTIME(inode) == fattr->pre_mtime) {
		invalid = 0;
	}

	/*
	 * If we have pending writebacks, things can get
	 * messy.
	 */
	if (nfs_have_writebacks(inode) && new_isize < inode->i_size)
		new_isize = inode->i_size;

	NFS_CACHE_CTIME(inode) = fattr->ctime;
	inode->i_ctime = nfs_time_to_secs(fattr->ctime);
	/* If we've been messing around with atime, don't
	 * update it. Save the server value in NFS_CACHE_ATIME.
	 */
	NFS_CACHE_ATIME(inode) = fattr->atime;
	if (time_before(inode->i_atime, nfs_time_to_secs(fattr->atime)))
		inode->i_atime = nfs_time_to_secs(fattr->atime);

	NFS_CACHE_MTIME(inode) = new_mtime;
	inode->i_mtime = nfs_time_to_secs(new_mtime);

	NFS_CACHE_ISIZE(inode) = new_size;
	inode->i_size = new_isize;

	inode->i_mode = fattr->mode;
	inode->i_nlink = fattr->nlink;
	inode->i_uid = fattr->uid;
	inode->i_gid = fattr->gid;

	if (fattr->valid & NFS_ATTR_FATTR_V3) {
		/*
		 * report the blocks in 512byte units
		 */
		inode->i_blocks = nfs_calc_block_size(fattr->du.nfs3.used);
		inode->i_blksize = inode->i_sb->s_blocksize;
 	} else {
 		inode->i_blocks = fattr->du.nfs2.blocks;
 		inode->i_blksize = fattr->du.nfs2.blocksize;
 	}
 	inode->i_rdev = 0;
 	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
 		inode->i_rdev = to_kdev_t(fattr->rdev);
 
	/* Update attrtimeo value */
	if (!invalid && time_after(jiffies, NFS_ATTRTIMEO_UPDATE(inode)+NFS_ATTRTIMEO(inode))) {
		if ((NFS_ATTRTIMEO(inode) <<= 1) > NFS_MAXATTRTIMEO(inode))
			NFS_ATTRTIMEO(inode) = NFS_MAXATTRTIMEO(inode);
		NFS_ATTRTIMEO_UPDATE(inode) = jiffies;
	}

	if (invalid)
		nfs_zap_caches(inode);

out:
	return error;

out_changed:
	/*
	 * Big trouble! The inode has become a different object.
	 */
#ifdef NFS_PARANOIA
	printk(KERN_DEBUG "nfs_refresh_inode: inode %ld mode changed, %07o to %07o\n",
	       inode->i_ino, inode->i_mode, fattr->mode);
#endif
	/*
	 * No need to worry about unhashing the dentry, as the
	 * lookup validation will know that the inode is bad.
	 * (But we fall through to invalidate the caches.)
	 */
	nfs_invalidate_inode(inode);
	goto out;
}

/*
 * File system information
 */
static DECLARE_FSTYPE(nfs_fs_type, "nfs", nfs_read_super, 0);

extern int nfs_init_fhcache(void);
extern void nfs_destroy_fhcache(void);
extern int nfs_init_nfspagecache(void);
extern void nfs_destroy_nfspagecache(void);

/*
 * Initialize NFS
 */
int
init_nfs_fs(void)
{
	int err;

	err = nfs_init_fhcache();
	if (err)
		return err;

	err = nfs_init_nfspagecache();
	if (err)
		return err;

#ifdef CONFIG_PROC_FS
	rpc_proc_register(&nfs_rpcstat);
#endif
        return register_filesystem(&nfs_fs_type);
}

/*
 * Every kernel module contains stuff like this.
 */
#ifdef MODULE

EXPORT_NO_SYMBOLS;
/* Not quite true; I just maintain it */
MODULE_AUTHOR("Olaf Kirch <okir@monad.swb.de>");

int
init_module(void)
{
	return init_nfs_fs();
}

void
cleanup_module(void)
{
	nfs_destroy_nfspagecache();
	nfs_destroy_fhcache();
#ifdef CONFIG_PROC_FS
	rpc_proc_unregister("nfs");
#endif
	unregister_filesystem(&nfs_fs_type);
}
#endif
