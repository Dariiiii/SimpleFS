#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/cred.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "simplefs_ioctl.h"

#define SIMPLEFS_NAME "simplefs"
#define SIMPLEFS_MAGIC 0x44415249
#define SIMPLEFS_VERSION 1
#define SIMPLEFS_SECTOR_SIZE 512
#define SIMPLEFS_ROOT_INO 1
#define SIMPLEFS_FIRST_FILE_INO 2

static char *disk_device_path = "/dev/loop0";
static unsigned long long sb_first_sector = 0;
static unsigned long long sb_second_sector = 128;
static unsigned int max_filename_len = 32;
static unsigned int max_file_sectors = 1;

module_param(disk_device_path, charp, 0444);
MODULE_PARM_DESC(disk_device_path, "Блочное устройство для SimpleFS");
module_param(sb_first_sector, ullong, 0444);
MODULE_PARM_DESC(sb_first_sector, "Сектор первой копии superblock");
module_param(sb_second_sector, ullong, 0444);
MODULE_PARM_DESC(sb_second_sector, "Сектор второй копии superblock");
module_param(max_filename_len, uint, 0444);
MODULE_PARM_DESC(max_filename_len, "Максимальная длина имени файла");
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Максимальный размер файла в секторах");

struct simplefs_disk_sb {
    __le32 magic, version, crc, sector_size;
    __le64 disk_sectors, sb_first, sb_second;
    __le32 max_name_len, max_file_sectors, file_sectors, file_count;
    __le64 generation;
} __packed;

struct simplefs_sb_info {
    u64 disk_sectors, sb_first, sb_second, generation;
    u32 max_name_len, max_file_sectors, file_sectors, file_count;
    bool erased;
    struct mutex lock;
};

static const struct super_operations simplefs_super_ops;
static const struct inode_operations simplefs_dir_iops;
static const struct file_operations simplefs_dir_fops, simplefs_file_fops;

static struct simplefs_sb_info *SIMPLEFS_SB(struct super_block *sb) { return sb->s_fs_info; }
static u64 simplefs_file_size(struct simplefs_sb_info *sbi) { return (u64)sbi->file_sectors * SIMPLEFS_SECTOR_SIZE; }
static void simplefs_name(u32 file_index, char *name_buffer, size_t buffer_len) { snprintf(name_buffer, buffer_len, "file%u", file_index); }

static u64 simplefs_data_sector(struct simplefs_sb_info *sbi, u64 data_index)
{
    u64 sb_min = min(sbi->sb_first, sbi->sb_second);
    u64 sb_max = max(sbi->sb_first, sbi->sb_second);
    u64 phys_sector = data_index;
    
    if (phys_sector >= sb_min) phys_sector++;
    if (phys_sector >= sb_max) phys_sector++;
    
    return phys_sector;
}

static u64 simplefs_file_sector(struct simplefs_sb_info *sbi, u32 file_index, u32 sector_offset)
{
    return simplefs_data_sector(sbi, (u64)file_index * sbi->file_sectors + sector_offset);
}

static u32 simplefs_sb_crc(struct simplefs_disk_sb *dsb)
{
    struct simplefs_disk_sb tmp = *dsb;
    tmp.crc = 0;
    return crc32(0, &tmp, sizeof(tmp));
}

static int simplefs_rw_sector(struct super_block *sb, u64 sector, void *buf, size_t len, bool write)
{
    struct buffer_head *bh = sb_bread(sb, (sector_t)sector);
    int ret = 0;
    if (!bh) return -EIO;
    if (write) {
        memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
        if (buf)
            memcpy(bh->b_data, buf, len);
        mark_buffer_dirty(bh);
        ret = sync_dirty_buffer(bh);
        if (!ret && buffer_write_io_error(bh))
            ret = -EIO;
    } else {
        memcpy(buf, bh->b_data, len);
    }
    brelse(bh);
    return ret;
}

static void simplefs_make_disk_sb(struct simplefs_sb_info *sbi, struct simplefs_disk_sb *dsb)
{
    memset(dsb, 0, sizeof(*dsb));
    dsb->magic = cpu_to_le32(SIMPLEFS_MAGIC);
    dsb->version = cpu_to_le32(SIMPLEFS_VERSION);
    dsb->sector_size = cpu_to_le32(SIMPLEFS_SECTOR_SIZE);
    dsb->disk_sectors = cpu_to_le64(sbi->disk_sectors);
    dsb->sb_first = cpu_to_le64(sbi->sb_first);
    dsb->sb_second = cpu_to_le64(sbi->sb_second);
    dsb->max_name_len = cpu_to_le32(sbi->max_name_len);
    dsb->max_file_sectors = cpu_to_le32(sbi->max_file_sectors);
    dsb->file_sectors = cpu_to_le32(sbi->file_sectors);
    dsb->file_count = cpu_to_le32(sbi->file_count);
    dsb->generation = cpu_to_le64(sbi->generation);
    dsb->crc = cpu_to_le32(simplefs_sb_crc(dsb));
}

static bool simplefs_valid_disk_sb(struct simplefs_sb_info *sbi, struct simplefs_disk_sb *dsb)
{
    u32 file_count, file_sectors;
    if (le32_to_cpu(dsb->magic) != SIMPLEFS_MAGIC) return false;
    if (le32_to_cpu(dsb->version) != SIMPLEFS_VERSION) return false;
    if (le32_to_cpu(dsb->crc) != simplefs_sb_crc(dsb)) return false;
    if (le32_to_cpu(dsb->sector_size) != SIMPLEFS_SECTOR_SIZE) return false;
    if (le64_to_cpu(dsb->disk_sectors) != sbi->disk_sectors) return false;
    if (le64_to_cpu(dsb->sb_first) != sbi->sb_first) return false;
    if (le64_to_cpu(dsb->sb_second) != sbi->sb_second) return false;
    if (le32_to_cpu(dsb->max_name_len) != sbi->max_name_len) return false;
    if (le32_to_cpu(dsb->max_file_sectors) != sbi->max_file_sectors) return false;
    
    file_count = le32_to_cpu(dsb->file_count);
    file_sectors = le32_to_cpu(dsb->file_sectors);
    if (!file_count || !file_sectors || file_sectors > sbi->max_file_sectors)
        return false;
    return (u64)file_count * file_sectors == sbi->disk_sectors - 2;
}

static int simplefs_write_superblocks(struct super_block *sb)
{
    struct simplefs_disk_sb dsb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    int ret;
    BUILD_BUG_ON(sizeof(dsb) > SIMPLEFS_SECTOR_SIZE);
    simplefs_make_disk_sb(sbi, &dsb);
    ret = simplefs_rw_sector(sb, sbi->sb_first, &dsb, sizeof(dsb), true);
    if (ret != 0) return ret;
    return simplefs_rw_sector(sb, sbi->sb_second, &dsb, sizeof(dsb), true);
}

static u32 simplefs_digits(u32 number)
{
    u32 digit_count = 1;
    while (number >= 10) {
        number /= 10;
        digit_count++;
    }
    return digit_count;
}

static int simplefs_format_layout(struct simplefs_sb_info *sbi)
{
    u64 usable_sectors = sbi->disk_sectors - 2;
    u64 total_files;
    u32 sectors_per_file;

    for (sectors_per_file = min_t(u64, sbi->max_file_sectors, usable_sectors); sectors_per_file > 1; sectors_per_file--) {
        if (usable_sectors % sectors_per_file == 0) break;
    }
    
    sbi->file_sectors = sectors_per_file;
    total_files = div_u64(usable_sectors, sbi->file_sectors);
    
    if (!total_files) return -ENOSPC;
    if (total_files > U32_MAX) return -EOVERFLOW;
    
    sbi->file_count = total_files;
    
    if (4 + simplefs_digits(sbi->file_count - 1) > sbi->max_name_len)
        return -EINVAL;
        
    return 0;
}

static int simplefs_load_or_format(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_disk_sb sb_primary, sb_secondary, *chosen_sb = NULL;
    bool primary_valid, secondary_valid;
    int ret, primary_ret, secondary_ret;

    memset(&sb_primary, 0, sizeof(sb_primary));
    memset(&sb_secondary, 0, sizeof(sb_secondary));
    
    primary_ret = simplefs_rw_sector(sb, sbi->sb_first, &sb_primary, sizeof(sb_primary), false);
    secondary_ret = simplefs_rw_sector(sb, sbi->sb_second, &sb_secondary, sizeof(sb_secondary), false);
    
    if (primary_ret && secondary_ret) return primary_ret;
    
    primary_valid = !primary_ret && simplefs_valid_disk_sb(sbi, &sb_primary);
    secondary_valid = !secondary_ret && simplefs_valid_disk_sb(sbi, &sb_secondary);
    
    if (primary_valid && secondary_valid)
        chosen_sb = le64_to_cpu(sb_primary.generation) >= le64_to_cpu(sb_secondary.generation) ? &sb_primary : &sb_secondary;
    else if (primary_valid)
        chosen_sb = &sb_primary;
    else if (secondary_valid)
        chosen_sb = &sb_secondary;
 
    if (chosen_sb) {
        sbi->file_count = le32_to_cpu(chosen_sb->file_count);
        sbi->file_sectors = le32_to_cpu(chosen_sb->file_sectors);
        sbi->generation = le64_to_cpu(chosen_sb->generation);
    } else {
        if (primary_ret || secondary_ret) return primary_ret ? primary_ret : secondary_ret;
        ret = simplefs_format_layout(sbi);
        sbi->generation = 1;
        if (ret) return ret;
    }
    return simplefs_write_superblocks(sb);
}

static int simplefs_parse_name(struct simplefs_sb_info *sbi, const char *name, u32 len, u32 *out_index)
{
    u64 parsed_number = 0;
    u32 char_index;
    char expected_name[SIMPLEFS_NAME_MAX];
    if (len < 5 || len > sbi->max_name_len || memcmp(name, "file", 4))
        return -ENOENT;
    for (char_index = 4; char_index < len; char_index++) {
        if (name[char_index] < '0' || name[char_index] > '9') return -ENOENT;
        parsed_number = parsed_number * 10 + name[char_index] - '0';
        if (parsed_number >= sbi->file_count) return -ENOENT;
    }
    *out_index = parsed_number;
    simplefs_name(*out_index, expected_name, sizeof(expected_name));
    return strlen(expected_name) == len && !memcmp(expected_name, name, len) ? 0 : -ENOENT;
}

static struct inode *simplefs_new_inode(struct super_block *sb, umode_t mode, u32 file_index)
{
    struct inode *inode = new_inode(sb);
    if (!inode) return NULL;
    
    inode->i_uid = current_fsuid();
    inode->i_gid = current_fsgid();
    inode->i_mode = mode;
    inode->i_mapping->a_ops = &empty_aops;
    simple_inode_init_ts(inode);
    
    if (S_ISDIR(mode)) {
        inode->i_ino = SIMPLEFS_ROOT_INO;
        inode->i_op = &simplefs_dir_iops;
        inode->i_fop = &simplefs_dir_fops;
        set_nlink(inode, 2);
    } else {
        inode->i_ino = SIMPLEFS_FIRST_FILE_INO + file_index;
        inode->i_fop = &simplefs_file_fops;
        inode->i_private = (void *)(unsigned long)file_index;
        i_size_write(inode, simplefs_file_size(SIMPLEFS_SB(sb)));
        inode->i_blocks = SIMPLEFS_SB(sb)->file_sectors;
        set_nlink(inode, 1);
    }
    return inode;
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct inode *inode;
    u32 file_index;
    
    if (simplefs_parse_name(SIMPLEFS_SB(dir->i_sb), dentry->d_name.name, dentry->d_name.len, &file_index)) {
        d_add(dentry, NULL);
        return NULL;
    }
    
    inode = simplefs_new_inode(dir->i_sb, S_IFREG | 0644, file_index);
    if (!inode) return ERR_PTR(-ENOMEM);
    d_add(dentry, inode);
    return NULL;
}

static int simplefs_iterate(struct file *file, struct dir_context *ctx)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file_inode(file)->i_sb);
    char name_buffer[SIMPLEFS_NAME_MAX];
    
    if (!dir_emit_dots(file, ctx)) return 0;
    
    while (ctx->pos < (loff_t)sbi->file_count + 2) {
        u32 file_index = ctx->pos - 2;
        simplefs_name(file_index, name_buffer, sizeof(name_buffer));
        
        if (!dir_emit(ctx, name_buffer, strlen(name_buffer), SIMPLEFS_FIRST_FILE_INO + file_index, DT_REG))
            return 0;
        ctx->pos++;
    }
    return 0;
}

static ssize_t simplefs_io(struct file *file, const char __user *wbuf, char __user *rbuf, size_t len, loff_t *ppos, bool write)
{
    struct inode *inode = file_inode(file);
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(inode->i_sb);
    u32 file_index = (u32)(unsigned long)inode->i_private;
    struct buffer_head *bh;
    u64 file_size = simplefs_file_size(sbi);
    size_t bytes_done = 0;
    int ret = 0;

    if (!len) return 0;
    if (*ppos < 0) return -EINVAL;
    if (*ppos >= file_size) return write ? -ENOSPC : 0;
    if (len > file_size - *ppos) len = file_size - *ppos;

    mutex_lock(&sbi->lock);
    if (sbi->erased) {
        ret = -EIO;
        goto out;
    }
    
    while (bytes_done < len) {
        u32 sector_index = (*ppos + bytes_done) / SIMPLEFS_SECTOR_SIZE;
        u32 sector_offset = (*ppos + bytes_done) % SIMPLEFS_SECTOR_SIZE;
        size_t bytes_to_copy = min_t(size_t, len - bytes_done, SIMPLEFS_SECTOR_SIZE - sector_offset);

        bh = sb_bread(inode->i_sb, simplefs_file_sector(sbi, file_index, sector_index));
        if (!bh) {
            ret = -EIO;
            goto out;
        }
        
        if (write && copy_from_user(bh->b_data + sector_offset, wbuf + bytes_done, bytes_to_copy)) {
            ret = -EFAULT;
        } else if (!write && copy_to_user(rbuf + bytes_done, bh->b_data + sector_offset, bytes_to_copy)) {
            ret = -EFAULT;
        } else if (write) {
            mark_buffer_dirty(bh);
            ret = sync_dirty_buffer(bh);
            if (!ret && buffer_write_io_error(bh)) ret = -EIO;
        }
        brelse(bh);
        if (ret) goto out;
        bytes_done += bytes_to_copy;
    }
out:
    mutex_unlock(&sbi->lock);
    if (ret) return ret;
    *ppos += bytes_done;
    return bytes_done;
}

static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    return simplefs_io(file, NULL, buf, len, ppos, false);
}

static ssize_t simplefs_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    return simplefs_io(file, buf, NULL, len, ppos, true);
}

static int simplefs_zero_all(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    u32 file_idx, sector_idx;
    int ret;
    
    for (file_idx = 0; file_idx < sbi->file_count; file_idx++) {
        for (sector_idx = 0; sector_idx < sbi->file_sectors; sector_idx++) {
            ret = simplefs_rw_sector(sb, simplefs_file_sector(sbi, file_idx, sector_idx), NULL, 0, true);
            if (ret) return ret;
        }
    }
    return 0;
}

static int simplefs_ioctl_meta(struct file *file, unsigned long arg)
{
    struct super_block *sb = file_inode(file)->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_file_list_request request;
    struct simplefs_file_info __user *user_entries;
    u32 file_idx, sector_idx, files_to_copy, file_hash;
    int ret = 0;

    if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
        return -EFAULT;
    
    request.count = sbi->file_count;
    files_to_copy = min(request.max_capacity, request.count);
    user_entries = (void __user *)(unsigned long)request.entries_ptr;

    mutex_lock(&sbi->lock);
    if (sbi->erased) {
        ret = -EIO;
        goto out;
    }
    
    for (file_idx = 0; file_idx < files_to_copy; file_idx++) {
        struct simplefs_file_info entry;
        file_hash = 0;
        
        for (sector_idx = 0; sector_idx < sbi->file_sectors; sector_idx++) {
            struct buffer_head *bh = sb_bread(sb, simplefs_file_sector(sbi, file_idx, sector_idx));
            if (!bh) {
                ret = -EIO;
                goto out;
            }
            file_hash = crc32(file_hash, bh->b_data, SIMPLEFS_SECTOR_SIZE);
            brelse(bh);
        }
        
        memset(&entry, 0, sizeof(entry));
        entry.file_id = file_idx;
        entry.hash = file_hash;
        entry.first_sector = simplefs_file_sector(sbi, file_idx, 0);
        entry.sector_count = sbi->file_sectors;
        simplefs_name(file_idx, entry.name, sizeof(entry.name));

        if (copy_to_user(&user_entries[file_idx], &entry, sizeof(entry))) {
            ret = -EFAULT;
            goto out;
        }
    }
    
    if (request.max_capacity < request.count)
        ret = -ENOSPC;    
out:
    mutex_unlock(&sbi->lock);
    return copy_to_user((void __user *)arg, &request, sizeof(request)) ? -EFAULT : ret;
}

static int simplefs_ioctl_map(struct file *file, unsigned long arg)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file_inode(file)->i_sb);
    struct simplefs_file_blocks_request request;
    __u64 __user *user_sectors;
    u32 file_index, sector_idx, sectors_to_copy;
    int ret;

    if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
        return -EFAULT;
        
    request.name[SIMPLEFS_NAME_MAX - 1] = '\0';
    ret = simplefs_parse_name(sbi, request.name, strnlen(request.name, sizeof(request.name)), &file_index);
    if (ret) return ret;

    request.sector_count = sbi->file_sectors;
    sectors_to_copy = min(request.max_capacity, request.sector_count);
    user_sectors = (void __user *)(unsigned long)request.sectors_ptr;

    mutex_lock(&sbi->lock);
    
    if (sbi->erased) {
        ret = -EIO;
    } else if (request.max_capacity < request.sector_count) {
        ret = -ENOSPC;
    } else {
        for (sector_idx = 0; sector_idx < sectors_to_copy; sector_idx++) {
            ret = put_user(simplefs_file_sector(sbi, file_index, sector_idx), &user_sectors[sector_idx]) ? -EFAULT : 0;
            if (ret) break;
        }
    }
    
    mutex_unlock(&sbi->lock);
    return copy_to_user((void __user *)arg, &request, sizeof(request)) ? -EFAULT : ret;
}

static long simplefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct super_block *sb = file_inode(file)->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    int ret;

    switch (cmd) {
    case SIMPLEFS_IOC_ZERO_ALL:
        mutex_lock(&sbi->lock);
        ret = sbi->erased ? -EIO : simplefs_zero_all(sb);
        if (!ret) {
            sbi->generation++;
            ret = simplefs_write_superblocks(sb);
        }
        mutex_unlock(&sbi->lock);
        return ret;
        
    case SIMPLEFS_IOC_ERASE_FS:
        mutex_lock(&sbi->lock);
        ret = simplefs_zero_all(sb);
        if (!ret) ret = simplefs_rw_sector(sb, sbi->sb_first, NULL, 0, true);
        if (!ret) ret = simplefs_rw_sector(sb, sbi->sb_second, NULL, 0, true);
        if (!ret) sbi->erased = true;
        mutex_unlock(&sbi->lock);
        return ret;
        
    case SIMPLEFS_IOC_GET_META:
        return simplefs_ioctl_meta(file, arg);
        
    case SIMPLEFS_IOC_GET_MAPPING:
        return simplefs_ioctl_map(file, arg);
        
    default:
        return -ENOTTY;
    }
}

static void simplefs_put_super(struct super_block *sb)
{
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct super_operations simplefs_super_ops = {
    .put_super = simplefs_put_super,
};

static const struct inode_operations simplefs_dir_iops = {
    .lookup = simplefs_lookup,
};

static const struct file_operations simplefs_dir_fops = {
    .owner = THIS_MODULE,
    .iterate_shared = simplefs_iterate,
    .llseek = generic_file_llseek,
    .unlocked_ioctl = simplefs_ioctl,
};

static const struct file_operations simplefs_file_fops = {
    .owner = THIS_MODULE,
    .read = simplefs_read,
    .write = simplefs_write,
    .llseek = generic_file_llseek,
    .unlocked_ioctl = simplefs_ioctl,
};

static int simplefs_check_params(void)
{
    if (!disk_device_path || !*disk_device_path || sb_first_sector == sb_second_sector)
        return -EINVAL;
    if (!max_file_sectors || max_filename_len < 5 || max_filename_len >= SIMPLEFS_NAME_MAX)
        return -EINVAL;
    return 0;
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct simplefs_sb_info *sbi;
    struct inode *root;
    int ret;
    (void)data;
    (void)silent;

    if (!sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE))
        return -EINVAL;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi) return -ENOMEM;

    mutex_init(&sbi->lock);
    sbi->disk_sectors = bdev_nr_sectors(sb->s_bdev);
    sbi->sb_first = sb_first_sector;
    sbi->sb_second = sb_second_sector;
    sbi->max_name_len = max_filename_len;
    sbi->max_file_sectors = max_file_sectors;
    
    sb->s_fs_info = sbi;
    sb->s_magic = SIMPLEFS_MAGIC;
    sb->s_op = &simplefs_super_ops;

    if (sbi->disk_sectors < 4 || sbi->sb_first >= sbi->disk_sectors || sbi->sb_second >= sbi->disk_sectors) {
        ret = -EINVAL;
        goto fail;
    }

    ret = simplefs_load_or_format(sb);
    if (ret) goto fail;

    sb->s_maxbytes = simplefs_file_size(sbi);
    root = simplefs_new_inode(sb, S_IFDIR | 0755, 0);
    if (!root) {
        ret = -ENOMEM;
        goto fail;
    }

    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto fail;
    }

    pr_info(SIMPLEFS_NAME ": mounted, files=%u\n", sbi->file_count);
    return 0;
fail:
    simplefs_put_super(sb);
    return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *type, int flags, const char *dev_name, void *data)
{
    const char *dev = (dev_name && *dev_name && strcmp(dev_name, "none")) ? dev_name : disk_device_path;
    return mount_bdev(type, flags, dev, data, simplefs_fill_super);
}

static struct file_system_type simplefs_type = {
    .owner = THIS_MODULE,
    .name = SIMPLEFS_NAME,
    .mount = simplefs_mount,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
    int ret = simplefs_check_params();
    if (ret) {
        pr_err(SIMPLEFS_NAME ": неверные параметры модуля\n");
        return ret; 
    }
    return register_filesystem(&simplefs_type);
}

static void __exit simplefs_exit(void)
{
    unregister_filesystem(&simplefs_type);
}

module_init(simplefs_init);
module_exit(simplefs_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daria");
MODULE_DESCRIPTION("Учебная ФС");