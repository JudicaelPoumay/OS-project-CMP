/*
 *  linux/fs/ext42/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext42 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *    (jj@sunsite.ms.mff.cuni.cz)
 */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/quotaops.h>
#include <linux/pagevec.h>
#include <linux/uio.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"

#define BLOCKSIZE 4096

/*
 * Called when an inode is released. Note that this is different
 * from ext42_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int ext42_release_file(struct inode *inode, struct file *filp)
{
    D("release a file %s (inode num %ld)", filp->f_path.dentry->d_name.name, inode->i_ino);

    if (ext42_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
        ext42_alloc_da_blocks(inode);
        ext42_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
    }
    /* if we are the last writer on the inode, drop the block reservation */
    if ((filp->f_mode & FMODE_WRITE) &&
            (atomic_read(&inode->i_writecount) == 1) &&
                !EXT4_I(inode)->i_reserved_data_blocks)
    {
        down_write(&EXT4_I(inode)->i_data_sem);
        ext42_discard_preallocations(inode);
        up_write(&EXT4_I(inode)->i_data_sem);
    }
    if (is_dx(inode) && filp->private_data)
        ext42_htree_free_dir_info(filp->private_data);

    return 0;
}

static void ext42_unwritten_wait(struct inode *inode)
{
    wait_queue_head_t *wq = ext42_ioend_wq(inode);

    wait_event(*wq, (atomic_read(&EXT4_I(inode)->i_unwritten) == 0));
}

/*
 * This tests whether the IO in question is block-aligned or not.
 * Ext4 utilizes unwritten extents when hole-filling during direct IO, and they
 * are converted to written only after the IO is complete.  Until they are
 * mapped, these blocks appear as holes, so dio_zero_block() will assume that
 * it needs to zero out portions of the start and/or end block.  If 2 AIO
 * threads are at work on the same unwritten block, they must be synchronized
 * or one thread will zero the other's data, causing corruption.
 */
static int
ext42_unaligned_aio(struct inode *inode, struct iov_iter *from, loff_t pos)
{
    struct super_block *sb = inode->i_sb;
    int blockmask = sb->s_blocksize - 1;

    if (pos >= i_size_read(inode))
        return 0;

    if ((pos | iov_iter_alignment(from)) & blockmask)
        return 1;

    return 0;
}

int hash_char(unsigned char *s)
{
    unsigned hashval;
    for (hashval = 0; *s != '\0'; s++)
        hashval = *s + 31*hashval;
    return hashval % 2147483647;
}

struct file* fileOpen(struct file* file)
{
    //get path
    char* path = vmalloc(sizeof(char)*4097);
    char* tmp = dentry_path_raw(file->f_path.dentry, path, 4096);
    strcpy(path,"/mnt");//Not appropriate but I could not find a way to retrieve the path proprely
    strcat(path,tmp);

    //open
    struct file* filp = filp_open(path,O_RDONLY,0);
    if(filp <= 0)
        D("Opening %s failed %d", path, filp);

    //return
    vfree(path);
    return filp;
}

void fileRead(struct file *file, unsigned long long offset, unsigned char *buf, unsigned int size) 
{
    //test
    if(size == 0)
    {
        buf[0] = '\0';
        return;    
    }    
        
    //read
    int retVal = kernel_read(file, offset,buf, size);
    if(retVal <= 0) 
        D("Reading failed %d", retVal);       
} 

void freeMerkelTree(merkel_tree* tree)
{
    if(tree->block < 0)
        freeMerkelTree(tree->l);        
    if(tree->block == -2)
        freeMerkelTree(tree->r);
    kfree(tree);
}

int computeDepth(int nbLeaves)
{    
    int nbParent = -1, depth = 1; 
    while(1)
    {        
        if(nbLeaves%2 == 0 || nbLeaves == 1)
            nbParent = nbLeaves/2;
        else 
            nbParent = (nbLeaves+1)/2;
        
        if(nbParent == 0)
            return depth-1;
        
        depth++;
        nbLeaves = nbParent;
    }   
}

int countLeaves(merkel_tree* tree)
{
    int a = 0,b = 0;
    if(tree->block >= 0)
        return 1;
    if(tree->block < 0)
        a = countLeaves(tree->l);        
    if(tree->block == -2)
        b = countLeaves(tree->r);
    return a + b;
}

merkel_tree* freeRight(merkel_tree* tree)
{
    merkel_tree* subLeftTree = tree->l;
    freeMerkelTree(tree->r);
    kfree(tree);
    
    return subLeftTree;
}

void updateDepthField(merkel_tree* tree, int depth)
{
    tree->depth = depth;
    if(tree->block >= 0)
        return;
    if(tree->block < 0)
        updateDepthField(tree->l, depth-1);        
    if(tree->block == -2)
        updateDepthField(tree->r, depth-1);
}

void freeRightMostLeaf(merkel_tree* tree)
{
    merkel_tree* it = tree;
    while(1)
    {
        if(it->block == -2)
        {
            if(it->r->block < 0)
            {
                it = it->r;
            }
            else
            {
                it->block = -1;  
                kfree(it->r);
                it->r = NULL;
                return;
            }
        }
        if(it->block == -1) 
        {
            if(it->l->block < 0)
            {
                it = it->l;
            }
            else
            {
                it->block = 0;                
                kfree(it->l);
                it->l = NULL;
                freeRightMostLeaf(tree);
                return;
            }
        }
    }
}

void freeLeaves(merkel_tree* tree, int nbToFree)
{
    int i;
    for(i = 0;i < nbToFree;i++)
        freeRightMostLeaf(tree);
}

int getNextPath(int path, int depth)
{
    if(depth == 0)       
        return -1;           
    if(path == -1)
    return 0;

    int i,mult = 1;
    for(i = 0,mult = 1;i < depth-1;i++)
        mult *= 2;
    while(1)
    {        
        if((path/mult)%2)
            path -= mult;
        else
    {
            path+= mult;
        return path;
    }
        mult/=2;    
    }  
    return path;
}

void setNewHasheLeaf(merkel_tree* tree, int path, int hash, int blknb)
{
    //extract node from path + update + alloc if needed
    if(path >= 0)
    {
        while(tree->depth != 0)
        {    
            int dir = path%2;
            path /= 2;
            if(dir == 0)
            {
                if(!tree->l)
                {        
                    tree->l = kmalloc(sizeof(merkel_tree),GFP_KERNEL | __GFP_HIGH | __GFP_NOFAIL);
                    tree->l->l = NULL;
                    tree->l->r = NULL;
                    tree->l->depth = tree->depth-1;
                    tree->block = -1;
                }
                tree = tree->l;
            }
            else if(dir == 1)
            {
                if(!tree->r)
                {
                    tree->r = kmalloc(sizeof(merkel_tree),GFP_KERNEL | __GFP_HIGH | __GFP_NOFAIL );  
                    tree->r->l = NULL;
                    tree->r->r = NULL;
                    tree->r->depth = tree->depth-1;              
                    tree->block = -2;
                }
                tree = tree->r;
            }
            else
                break;            
        }
    }

    //set leaf
    tree->r = NULL;
    tree->l = NULL;
    tree->depth = 0;
    tree->block = blknb;
    tree->hash  = hash;
}

void setNewHasheParents(merkel_tree* tree)
{
    if(tree->block >= 0)
        return;
    if(tree->block == -1)        
    {
        setNewHasheParents(tree->l);                
        tree->hash = tree->l->hash/2;
    }
    else if(tree->block == -2)
    {        
        setNewHasheParents(tree->l);
        setNewHasheParents(tree->r);        
        tree->hash = tree->l->hash/2 + tree->r->hash/2;
    }
}

void setNewHashes(merkel_tree* tree, int* hashes, int size, int depth)
{
    int i, path = -1;
    for(i = 0;i< size;i++)
    {
        path = getNextPath(path, depth);
        setNewHasheLeaf(tree, path, hashes[i], i);
    }
    
    setNewHasheParents(tree);
}

int* getHashes(struct file* filp, int nbblk, int size)
{
    //init
    char* buf = vmalloc(sizeof(char)*(BLOCKSIZE+1));
    int* hashes = vmalloc(sizeof(int)*(nbblk));
    int i = 0;
    
    //create hashes
    for(i = 0;i< nbblk-1;i++)
    {    
        fileRead(filp, i*BLOCKSIZE,buf, BLOCKSIZE);
        hashes[i] = hash_char(buf);    
    }
    int rest = size%BLOCKSIZE;
    fileRead(filp,i*BLOCKSIZE ,buf, rest);    
    hashes[i] = hash_char(buf);  
    
    //return
    vfree(buf);    
    return hashes;
}

void updateTree(struct file* file, struct ext42_inode* inode)
{
    D("Updating tree of %s", file->f_path.dentry->d_name.name);

    //get hashes
    struct file* filp   = fileOpen(file);
    int size            = vfs_llseek(filp,0,SEEK_END);
    int nbblk           = (size/BLOCKSIZE)+1;   
    int* hashes         = getHashes(filp, nbblk, size);  
    filp_close(filp, NULL);
       
    //recycle old tree : get old/new tree info
    merkel_tree* oldTree = inode->tree;
    int oldDepth    = oldTree->depth;
    int oldNbLeaves = countLeaves(oldTree);
    int newNbLeaves = nbblk;
    int newDepth    = computeDepth(newNbLeaves);
    
    //recycle old tree : grow or reduce size
    if(oldDepth > newDepth)
    {
        while(oldTree->depth > newDepth)
            oldTree = freeRight(oldTree);
        oldNbLeaves = countLeaves(oldTree);
        inode->tree = oldTree;
    }
    else if(oldDepth < newDepth)
    {
        updateDepthField(oldTree, newDepth);
    }
        
    //recycle old tree : remove unescecary leaves
    if(oldNbLeaves > newNbLeaves)
        freeLeaves(oldTree, oldNbLeaves-newNbLeaves);
    
    //set hashes
    setNewHashes(oldTree, hashes, newNbLeaves, newDepth);
    vfree(hashes);
}

static ssize_t
ext42_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(iocb->ki_filp);
    struct mutex *aio_mutex = NULL;
    struct blk_plug plug;
    int o_direct = iocb->ki_flags & IOCB_DIRECT;
    int overwrite = 0;
    ssize_t ret;
    
    //get ext42 inode
    struct ext42_iloc iloc;
    struct ext42_inode *raw_inode;
    iloc.bh = NULL;
    ret = ext42_get_inode_loc(inode, &iloc);
    raw_inode = ext42_raw_inode(&iloc);

    D("write to %s", file->f_path.dentry->d_name.name);

    /*
     * Unaligned direct AIO must be serialized; see comment above
     * In the case of O_APPEND, assume that we must always serialize
     */
    if (o_direct &&
        ext42_test_inode_flag(inode, EXT4_INODE_EXTENTS) &&
        !is_sync_kiocb(iocb) &&
        (iocb->ki_flags & IOCB_APPEND ||
         ext42_unaligned_aio(inode, from, iocb->ki_pos))) {
        aio_mutex = ext42_aio_mutex(inode);
        mutex_lock(aio_mutex);
        ext42_unwritten_wait(inode);
    }

    mutex_lock(&inode->i_mutex);
    ret = generic_write_checks(iocb, from);
    if (ret <= 0)
        goto out;

    /*
     * If we have encountered a bitmap-format file, the size limit
     * is smaller than s_maxbytes, which is for extent-mapped files.
     */
    if (!(ext42_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
        struct ext42_sb_info *sbi = EXT4_SB(inode->i_sb);

        if (iocb->ki_pos >= sbi->s_bitmap_maxbytes) {
            ret = -EFBIG;
            goto out;
        }
        iov_iter_truncate(from, sbi->s_bitmap_maxbytes - iocb->ki_pos);
    }

    iocb->private = &overwrite;
    if (o_direct) {
        size_t length = iov_iter_count(from);
        loff_t pos = iocb->ki_pos;
        blk_start_plug(&plug);

        /* check whether we do a DIO overwrite or not */
        if (ext42_should_dioread_nolock(inode) && !aio_mutex &&
            !file->f_mapping->nrpages && pos + length <= i_size_read(inode)) {
            struct ext42_map_blocks map;
            unsigned int blkbits = inode->i_blkbits;
            int err, len;

            map.m_lblk = pos >> blkbits;
            map.m_len = (EXT4_BLOCK_ALIGN(pos + length, blkbits) >> blkbits)
                - map.m_lblk;
            len = map.m_len;

            err = ext42_map_blocks(NULL, inode, &map, 0);
            /*
             * 'err==len' means that all of blocks has
             * been preallocated no matter they are
             * initialized or not.  For excluding
             * unwritten extents, we need to check
             * m_flags.  There are two conditions that
             * indicate for initialized extents.  1) If we
             * hit extent cache, EXT4_MAP_MAPPED flag is
             * returned; 2) If we do a real lookup,
             * non-flags are returned.  So we should check
             * these two conditions.
             */
            if (err == len && (map.m_flags & EXT4_MAP_MAPPED))
                overwrite = 1;
        }
    }

    ret = __generic_file_write_iter(iocb, from);
    mutex_unlock(&inode->i_mutex);

    if (ret > 0) {
        ssize_t err;

        err = generic_write_sync(file, iocb->ki_pos - ret, ret);
        if (err < 0)
            ret = err;
    }
    if (o_direct)
        blk_finish_plug(&plug);

    if (aio_mutex)
        mutex_unlock(aio_mutex);
    updateTree(file,raw_inode);
    return ret;

out:
    mutex_unlock(&inode->i_mutex);
    if (aio_mutex)
        mutex_unlock(aio_mutex);
    return ret;
}

#ifdef CONFIG_FS_DAX
static void ext42_end_io_unwritten(struct buffer_head *bh, int uptodate)
{
    struct inode *inode = bh->b_assoc_map->host;
    /* XXX: breaks on 32-bit > 16TB. Is that even supported? */
    loff_t offset = (loff_t)(uintptr_t)bh->b_private << inode->i_blkbits;
    int err;
    if (!uptodate)
        return;
    WARN_ON(!buffer_unwritten(bh));
    err = ext42_convert_unwritten_extents(NULL, inode, offset, bh->b_size);
}

static int ext42_dax_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
    int result;
    handle_t *handle = NULL;
    struct inode *inode = file_inode(vma->vm_file);
    struct super_block *sb = inode->i_sb;
    bool write = vmf->flags & FAULT_FLAG_WRITE;

    if (write) {
        sb_start_pagefault(sb);
        file_update_time(vma->vm_file);
        down_read(&EXT4_I(inode)->i_mmap_sem);
        handle = ext42_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
                        EXT4_DATA_TRANS_BLOCKS(sb));
    } else
        down_read(&EXT4_I(inode)->i_mmap_sem);

    if (IS_ERR(handle))
        result = VM_FAULT_SIGBUS;
    else
        result = __dax_fault(vma, vmf, ext42_get_block_dax,
                        ext42_end_io_unwritten);

    if (write) {
        if (!IS_ERR(handle))
            ext42_journal_stop(handle);
        up_read(&EXT4_I(inode)->i_mmap_sem);
        sb_end_pagefault(sb);
    } else
        up_read(&EXT4_I(inode)->i_mmap_sem);

    return result;
}

static int ext42_dax_pmd_fault(struct vm_area_struct *vma, unsigned long addr,
                        pmd_t *pmd, unsigned int flags)
{
    int result;
    handle_t *handle = NULL;
    struct inode *inode = file_inode(vma->vm_file);
    struct super_block *sb = inode->i_sb;
    bool write = flags & FAULT_FLAG_WRITE;

    if (write) {
        sb_start_pagefault(sb);
        file_update_time(vma->vm_file);
        down_read(&EXT4_I(inode)->i_mmap_sem);
        handle = ext42_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
                ext42_chunk_trans_blocks(inode,
                            PMD_SIZE / PAGE_SIZE));
    } else
        down_read(&EXT4_I(inode)->i_mmap_sem);

    if (IS_ERR(handle))
        result = VM_FAULT_SIGBUS;
    else
        result = __dax_pmd_fault(vma, addr, pmd, flags,
                ext42_get_block_dax, ext42_end_io_unwritten);

    if (write) {
        if (!IS_ERR(handle))
            ext42_journal_stop(handle);
        up_read(&EXT4_I(inode)->i_mmap_sem);
        sb_end_pagefault(sb);
    } else
        up_read(&EXT4_I(inode)->i_mmap_sem);

    return result;
}

static int ext42_dax_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf)
{
    int err;
    struct inode *inode = file_inode(vma->vm_file);

    sb_start_pagefault(inode->i_sb);
    file_update_time(vma->vm_file);
    down_read(&EXT4_I(inode)->i_mmap_sem);
    err = __dax_mkwrite(vma, vmf, ext42_get_block_dax,
                ext42_end_io_unwritten);
    up_read(&EXT4_I(inode)->i_mmap_sem);
    sb_end_pagefault(inode->i_sb);

    return err;
}

/*
 * Handle write fault for VM_MIXEDMAP mappings. Similarly to ext42_dax_mkwrite()
 * handler we check for races agaist truncate. Note that since we cycle through
 * i_mmap_sem, we are sure that also any hole punching that began before we
 * were called is finished by now and so if it included part of the file we
 * are working on, our pte will get unmapped and the check for pte_same() in
 * wp_pfn_shared() fails. Thus fault gets retried and things work out as
 * desired.
 */
static int ext42_dax_pfn_mkwrite(struct vm_area_struct *vma,
                struct vm_fault *vmf)
{
    struct inode *inode = file_inode(vma->vm_file);
    struct super_block *sb = inode->i_sb;
    int ret = VM_FAULT_NOPAGE;
    loff_t size;

    sb_start_pagefault(sb);
    file_update_time(vma->vm_file);
    down_read(&EXT4_I(inode)->i_mmap_sem);
    size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (vmf->pgoff >= size)
        ret = VM_FAULT_SIGBUS;
    up_read(&EXT4_I(inode)->i_mmap_sem);
    sb_end_pagefault(sb);

    return ret;
}

static const struct vm_operations_struct ext42_dax_vm_ops = {
    .fault        = ext42_dax_fault,
    .pmd_fault    = ext42_dax_pmd_fault,
    .page_mkwrite    = ext42_dax_mkwrite,
    .pfn_mkwrite    = ext42_dax_pfn_mkwrite,
};
#else
#define ext42_dax_vm_ops    ext42_file_vm_ops
#endif

static const struct vm_operations_struct ext42_file_vm_ops = {
    .fault        = ext42_filemap_fault,
    .map_pages    = filemap_map_pages,
    .page_mkwrite   = ext42_page_mkwrite,
};

static int ext42_file_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct inode *inode = file->f_mapping->host;

    if (ext42_encrypted_inode(inode)) {
        int err = ext42_get_encryption_info(inode);
        if (err)
            return 0;
        if (ext42_encryption_info(inode) == NULL)
            return -ENOKEY;
    }
    file_accessed(file);
    if (IS_DAX(file_inode(file))) {
        vma->vm_ops = &ext42_dax_vm_ops;
        vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;
    } else {
        vma->vm_ops = &ext42_file_vm_ops;
    }
    return 0;
}

static int ext42_file_open(struct inode * inode, struct file * filp)
{
    struct super_block *sb = inode->i_sb;
    struct ext42_sb_info *sbi = EXT4_SB(inode->i_sb);
    struct vfsmount *mnt = filp->f_path.mnt;
    struct path path;
    char buf[64], *cp;
    int ret;

    D("open a file %s (inode num %ld)", filp->f_path.dentry->d_name.name, inode->i_ino);

    if (unlikely(!(sbi->s_mount_flags & EXT4_MF_MNTDIR_SAMPLED) &&
             !(sb->s_flags & MS_RDONLY))) {
        sbi->s_mount_flags |= EXT4_MF_MNTDIR_SAMPLED;
        /*
         * Sample where the filesystem has been mounted and
         * store it in the superblock for sysadmin convenience
         * when trying to sort through large numbers of block
         * devices or filesystem images.
         */
        memset(buf, 0, sizeof(buf));
        path.mnt = mnt;
        path.dentry = mnt->mnt_root;
        cp = d_path(&path, buf, sizeof(buf));
        if (!IS_ERR(cp)) {
            handle_t *handle;
            int err;

            handle = ext42_journal_start_sb(sb, EXT4_HT_MISC, 1);
            if (IS_ERR(handle))
                return PTR_ERR(handle);
            BUFFER_TRACE(sbi->s_sbh, "get_write_access");
            err = ext42_journal_get_write_access(handle, sbi->s_sbh);
            if (err) {
                ext42_journal_stop(handle);
                return err;
            }
            strlcpy(sbi->s_es->s_last_mounted, cp,
                sizeof(sbi->s_es->s_last_mounted));
            ext42_handle_dirty_super(handle, sb);
            ext42_journal_stop(handle);
        }
    }
    if (ext42_encrypted_inode(inode)) {
        ret = ext42_get_encryption_info(inode);
        if (ret)
            return -EACCES;
        if (ext42_encryption_info(inode) == NULL)
            return -ENOKEY;
    }
    /*
     * Set up the jbd2_inode if we are opening the inode for
     * writing and the journal is present
     */
    if (filp->f_mode & FMODE_WRITE) {
        ret = ext42_inode_attach_jinode(inode);
        if (ret < 0)
            return ret;
    }
    return dquot_file_open(inode, filp);
}

/*
 * Here we use ext42_map_blocks() to get a block mapping for a extent-based
 * file rather than ext42_ext_walk_space() because we can introduce
 * SEEK_DATA/SEEK_HOLE for block-mapped and extent-mapped file at the same
 * function.  When extent status tree has been fully implemented, it will
 * track all extent status for a file and we can directly use it to
 * retrieve the offset for SEEK_DATA/SEEK_HOLE.
 */

/*
 * When we retrieve the offset for SEEK_DATA/SEEK_HOLE, we would need to
 * lookup page cache to check whether or not there has some data between
 * [startoff, endoff] because, if this range contains an unwritten extent,
 * we determine this extent as a data or a hole according to whether the
 * page cache has data or not.
 */
static int ext42_find_unwritten_pgoff(struct inode *inode,
                     int whence,
                     ext42_lblk_t end_blk,
                     loff_t *offset)
{
    struct pagevec pvec;
    unsigned int blkbits;
    pgoff_t index;
    pgoff_t end;
    loff_t endoff;
    loff_t startoff;
    loff_t lastoff;
    int found = 0;

    blkbits = inode->i_sb->s_blocksize_bits;
    startoff = *offset;
    lastoff = startoff;
    endoff = (loff_t)end_blk << blkbits;

    index = startoff >> PAGE_CACHE_SHIFT;
    end = endoff >> PAGE_CACHE_SHIFT;

    pagevec_init(&pvec, 0);
    do {
        int i, num;
        unsigned long nr_pages;

        num = min_t(pgoff_t, end - index, PAGEVEC_SIZE - 1) + 1;
        nr_pages = pagevec_lookup(&pvec, inode->i_mapping, index,
                      (pgoff_t)num);
        if (nr_pages == 0)
            break;

        for (i = 0; i < nr_pages; i++) {
            struct page *page = pvec.pages[i];
            struct buffer_head *bh, *head;

            /*
             * If current offset is smaller than the page offset,
             * there is a hole at this offset.
             */
            if (whence == SEEK_HOLE && lastoff < endoff &&
                lastoff < page_offset(pvec.pages[i])) {
                found = 1;
                *offset = lastoff;
                goto out;
            }

            if (page->index > end)
                goto out;

            lock_page(page);

            if (unlikely(page->mapping != inode->i_mapping)) {
                unlock_page(page);
                continue;
            }

            if (!page_has_buffers(page)) {
                unlock_page(page);
                continue;
            }

            if (page_has_buffers(page)) {
                lastoff = page_offset(page);
                bh = head = page_buffers(page);
                do {
                    if (lastoff + bh->b_size <= startoff)
                        goto next;
                    if (buffer_uptodate(bh) ||
                        buffer_unwritten(bh)) {
                        if (whence == SEEK_DATA)
                            found = 1;
                    } else {
                        if (whence == SEEK_HOLE)
                            found = 1;
                    }
                    if (found) {
                        *offset = max_t(loff_t,
                            startoff, lastoff);
                        unlock_page(page);
                        goto out;
                    }
next:
                    lastoff += bh->b_size;
                    bh = bh->b_this_page;
                } while (bh != head);
            }

            lastoff = page_offset(page) + PAGE_SIZE;
            unlock_page(page);
        }

        /* The no. of pages is less than our desired, we are done. */
        if (nr_pages < num)
            break;

        index = pvec.pages[i - 1]->index + 1;
        pagevec_release(&pvec);
    } while (index <= end);

    if (whence == SEEK_HOLE && lastoff < endoff) {
        found = 1;
        *offset = lastoff;
    }
out:
    pagevec_release(&pvec);
    return found;
}

/*
 * ext42_seek_data() retrieves the offset for SEEK_DATA.
 */
static loff_t ext42_seek_data(struct file *file, loff_t offset, loff_t maxsize)
{
    struct inode *inode = file->f_mapping->host;
    struct extent_status es;
    ext42_lblk_t start, last, end;
    loff_t dataoff, isize;
    int blkbits;
    int ret;

    mutex_lock(&inode->i_mutex);

    isize = i_size_read(inode);
    if (offset < 0 || offset >= isize) {
        mutex_unlock(&inode->i_mutex);
        return -ENXIO;
    }

    blkbits = inode->i_sb->s_blocksize_bits;
    start = offset >> blkbits;
    last = start;
    end = isize >> blkbits;
    dataoff = offset;

    do {
        ret = ext42_get_next_extent(inode, last, end - last + 1, &es);
        if (ret <= 0) {
            /* No extent found -> no data */
            if (ret == 0)
                ret = -ENXIO;
            mutex_unlock(&inode->i_mutex);
            return ret;
        }

        last = es.es_lblk;
        if (last != start)
            dataoff = (loff_t)last << blkbits;
        if (!ext42_es_is_unwritten(&es))
            break;

        /*
         * If there is a unwritten extent at this offset,
         * it will be as a data or a hole according to page
         * cache that has data or not.
         */
        if (ext42_find_unwritten_pgoff(inode, SEEK_DATA,
                          es.es_lblk + es.es_len, &dataoff))
            break;
        last += es.es_len;
        dataoff = (loff_t)last << blkbits;
        cond_resched();
    } while (last <= end);

    mutex_unlock(&inode->i_mutex);

    if (dataoff > isize)
        return -ENXIO;

    return vfs_setpos(file, dataoff, maxsize);
}

/*
 * ext42_seek_hole() retrieves the offset for SEEK_HOLE.
 */
static loff_t ext42_seek_hole(struct file *file, loff_t offset, loff_t maxsize)
{
    struct inode *inode = file->f_mapping->host;
    struct extent_status es;
    ext42_lblk_t start, last, end;
    loff_t holeoff, isize;
    int blkbits;
    int ret;

    mutex_lock(&inode->i_mutex);

    isize = i_size_read(inode);
    if (offset < 0 || offset >= isize) {
        mutex_unlock(&inode->i_mutex);
        return -ENXIO;
    }

    blkbits = inode->i_sb->s_blocksize_bits;
    start = offset >> blkbits;
    last = start;
    end = isize >> blkbits;
    holeoff = offset;

    do {
        ret = ext42_get_next_extent(inode, last, end - last + 1, &es);
        if (ret < 0) {
            mutex_unlock(&inode->i_mutex);
            return ret;
        }
        /* Found a hole? */
        if (ret == 0 || es.es_lblk > last) {
            if (last != start)
                holeoff = (loff_t)last << blkbits;
            break;
        }
        /*
         * If there is a unwritten extent at this offset,
         * it will be as a data or a hole according to page
         * cache that has data or not.
         */
        if (ext42_es_is_unwritten(&es) &&
            ext42_find_unwritten_pgoff(inode, SEEK_HOLE,
                          last + es.es_len, &holeoff))
            break;

        last += es.es_len;
        holeoff = (loff_t)last << blkbits;
        cond_resched();
    } while (last <= end);

    mutex_unlock(&inode->i_mutex);

    if (holeoff > isize)
        holeoff = isize;

    return vfs_setpos(file, holeoff, maxsize);
}

/*
 * ext42_llseek() handles both block-mapped and extent-mapped maxbytes values
 * by calling generic_file_llseek_size() with the appropriate maxbytes
 * value for each.
 */
loff_t ext42_llseek(struct file *file, loff_t offset, int whence)
{
    struct inode *inode = file->f_mapping->host;
    loff_t maxbytes;

    if (!(ext42_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
        maxbytes = EXT4_SB(inode->i_sb)->s_bitmap_maxbytes;
    else
        maxbytes = inode->i_sb->s_maxbytes;

    switch (whence) {
    case SEEK_SET:
    case SEEK_CUR:
    case SEEK_END:
        return generic_file_llseek_size(file, offset, whence,
                        maxbytes, i_size_read(inode));
    case SEEK_DATA:
        return ext42_seek_data(file, offset, maxbytes);
    case SEEK_HOLE:
        return ext42_seek_hole(file, offset, maxbytes);
    }

    return -EINVAL;
}

const struct file_operations ext42_file_operations = {
    .llseek        = ext42_llseek,
    .read_iter    = generic_file_read_iter,
    .write_iter    = ext42_file_write_iter,
    .unlocked_ioctl = ext42_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl    = ext42_compat_ioctl,
#endif
    .mmap        = ext42_file_mmap,
    .open        = ext42_file_open,
    .release    = ext42_release_file,
    .fsync        = ext42_sync_file,
    .splice_read    = generic_file_splice_read,
    .splice_write    = iter_file_splice_write,
    .fallocate    = ext42_fallocate,
};

const struct inode_operations ext42_file_inode_operations = {
    .setattr    = ext42_setattr,
    .getattr    = ext42_getattr,
    .setxattr    = generic_setxattr,
    .getxattr    = generic_getxattr,
    .listxattr    = ext42_listxattr,
    .removexattr    = generic_removexattr,
    .get_acl    = ext42_get_acl,
    .set_acl    = ext42_set_acl,
    .fiemap        = ext42_fiemap,
};

