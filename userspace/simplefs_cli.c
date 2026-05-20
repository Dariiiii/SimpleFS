// SPDX-License-Identifier: GPL-2.0
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "../simplefs_ioctl.h"
static void die(const char *msg)
{
	perror(msg);
	exit(1);
}
static int open_mount(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		die("не удалось открыть точку монтирования");
	return fd;
}
static void cmd_zero(const char *mnt)
{
	int fd = open_mount(mnt);
	if (ioctl(fd, SIMPLEFS_IOC_ZERO_ALL) < 0)
		die("ioctl zero");
	close(fd);
	puts("все файлы обнулены");
}
static void cmd_erase(const char *mnt)
{
	int fd = open_mount(mnt);
	if (ioctl(fd, SIMPLEFS_IOC_ERASE_FS) < 0)
		die("ioctl erase");
	close(fd);
	puts("файловая система стерта");
}
static void cmd_meta(const char *mnt)
{
	int fd = open_mount(mnt);
	struct simplefs_meta_request req;
	struct simplefs_meta_entry *entries;
	uint32_t i;
	memset(&req, 0, sizeof(req));
	if (ioctl(fd, SIMPLEFS_IOC_GET_META, &req) < 0 && errno != ENOSPC)
		die("ioctl meta count");
	entries = calloc(req.count, sizeof(*entries));
	if (!entries)
		die("calloc");
	req.capacity = req.count;
	req.entries = (uint64_t)(uintptr_t)entries;
	if (ioctl(fd, SIMPLEFS_IOC_GET_META, &req) < 0)
		die("ioctl meta");
	for (i = 0; i < req.count; i++)
		printf("%s: sector=%llu count=%u hash=0x%08x\n",
		       entries[i].name,
		       (unsigned long long)entries[i].first_sector,
		       entries[i].sector_count,
		       entries[i].hash);
	free(entries);
	close(fd);
}
static void cmd_map(const char *mnt, const char *name)
{
	int fd = open_mount(mnt);
	struct simplefs_mapping_request req;
	uint64_t *sectors;
	uint32_t i;
	memset(&req, 0, sizeof(req));
	snprintf(req.name, sizeof(req.name), "%s", name);
	if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &req) < 0 && errno != ENOSPC)
		die("ioctl map count");
	sectors = calloc(req.sector_count, sizeof(*sectors));
	if (!sectors)
		die("calloc");
	req.capacity = req.sector_count;
	req.sectors = (uint64_t)(uintptr_t)sectors;
	if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &req) < 0)
		die("ioctl map");
	printf("%s:", req.name);
	for (i = 0; i < req.sector_count; i++)
		printf(" %llu", (unsigned long long)sectors[i]);
	putchar('\n');
	free(sectors);
	close(fd);
}
static void test_one_file(const char *path, unsigned int *ok)
{
	int fd;
	uint32_t value, readback;
	fd = open(path, O_RDWR);
	if (fd < 0)
		die(path);
	value = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
	if (pwrite(fd, &value, sizeof(value), 0) != sizeof(value))
		die("write");
	if (pread(fd, &readback, sizeof(readback), 0) != sizeof(readback))
		die("read");
	if (value != readback) {
		fprintf(stderr, "ошибка проверки файла %s\n", path);
		exit(1);
	}
	close(fd);
	(*ok)++;
}
static void cmd_test(const char *mnt)
{
	DIR *dir;
	struct dirent *de;
	unsigned int ok = 0;
	srand(time(NULL));
	dir = opendir(mnt);
	if (!dir)
		die("opendir");
	while ((de = readdir(dir)) != NULL) {
		char path[4096];
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", mnt, de->d_name);
		test_one_file(path, &ok);
	}
	closedir(dir);
	printf("проверено файлов: %u\n", ok);
}
static void usage(const char *prog)
{
	fprintf(stderr,
		"использование:\n"
		"  %s test <mountpoint>\n"
		"  %s zero <mountpoint>\n"
		"  %s erase <mountpoint>\n"
		"  %s meta <mountpoint>\n"
		"  %s map <mountpoint> <file>\n",
		prog, prog, prog, prog, prog);
	exit(1);
}
int main(int argc, char **argv)
{
	if (argc < 3)
		usage(argv[0]);
	if (!strcmp(argv[1], "test"))
		cmd_test(argv[2]);
	else if (!strcmp(argv[1], "zero"))
		cmd_zero(argv[2]);
	else if (!strcmp(argv[1], "erase"))
		cmd_erase(argv[2]);
	else if (!strcmp(argv[1], "meta"))
		cmd_meta(argv[2]);
	else if (!strcmp(argv[1], "map") && argc == 4)
		cmd_map(argv[2], argv[3]);
	else
		usage(argv[0]);
	return 0;
}
