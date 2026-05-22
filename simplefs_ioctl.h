#ifndef SIMPLEFS_IOCTL_H
#define SIMPLEFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SIMPLEFS_NAME_MAX 64
#define SIMPLEFS_IOC_MAGIC 'd'

struct simplefs_file_info {
    __u32 file_id;
    __u32 hash;
    __u64 first_sector;
    __u32 sector_count;
    char  name[SIMPLEFS_NAME_MAX];
};

struct simplefs_file_list_request {
    __u32 count;
    __u32 max_capacity;
    __u64 entries_ptr;
};

struct simplefs_file_blocks_request {
    char  name[SIMPLEFS_NAME_MAX];
    __u32 sector_count;
    __u32 max_capacity;
    __u64 sectors_ptr;
};
#define SIMPLEFS_IOC_ZERO_ALL _IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOC_ERASE_FS _IO(SIMPLEFS_IOC_MAGIC, 2)
#define SIMPLEFS_IOC_GET_META _IOWR(SIMPLEFS_IOC_MAGIC, 3, struct simplefs_file_list_request)
#define SIMPLEFS_IOC_GET_MAPPING _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_file_blocks_request)
#endif
