#define _GNU_SOURCE
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

static void report_error_and_exit(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static int open_fs_root(const char *mnt)
{
    int fd = open(mnt, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        report_error_and_exit("Ошибка открытия точки монтирования");
    return fd;
}

static void execute_zero_all(int fd)
{
    if (ioctl(fd, SIMPLEFS_IOC_ZERO_ALL) < 0)
        report_error_and_exit("Ошибка при выполнении ioctl (zero)");
    printf("Файловая система успешно обнулена.\n");
    close(fd);
}

static void execute_erase_fs(int fd)
{
    if (ioctl(fd, SIMPLEFS_IOC_ERASE_FS) < 0)
        report_error_and_exit("Ошибка при выполнении ioctl (erase)");
    printf("Файловая система успешно стерта.\n");
    close(fd);
}

static void fetch_and_print_metadata(int fd)
{
    struct simplefs_file_list_request req = {0};
    
    if (ioctl(fd, SIMPLEFS_IOC_GET_META, &req) < 0 && errno != ENOSPC)
        report_error_and_exit("Ошибка получения количества метаданных");

    struct simplefs_file_info *list = calloc(req.count, sizeof(*list));
    if (!list) report_error_and_exit("Ошибка выделения памяти для метаданных");

    req.max_capacity = req.count;
    req.entries_ptr = (uintptr_t)list;
    if (ioctl(fd, SIMPLEFS_IOC_GET_META, &req) < 0)
        report_error_and_exit("Ошибка при чтении списка метаданных");

    printf("Список файлов:\n");
    for (uint32_t i = 0; i < req.count; i++) {
        printf("- ID=%u, %s: сектор=%llu, размер=%u, хэш=0x%08x\n",
               list[i].file_id, list[i].name, (unsigned long long)list[i].first_sector,
               list[i].sector_count, list[i].hash);
    }
    free(list);
    close(fd);
}

static void fetch_and_print_mapping(int fd, const char *file_name)
{
    struct simplefs_file_blocks_request req = {0};
    snprintf(req.name, sizeof(req.name), "%s", file_name);

    if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &req) < 0 && errno != ENOSPC)
        report_error_and_exit("Ошибка получения маппинга файла");

    uint64_t *sectors = calloc(req.sector_count, sizeof(*sectors));
    if (!sectors) report_error_and_exit("Ошибка памяти для карты секторов");

    req.max_capacity = req.sector_count;
    req.sectors_ptr = (uintptr_t)sectors;
    if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &req) < 0)
        report_error_and_exit("Ошибка чтения карты секторов");

    printf("Карта секторов для '%s':", req.name);
    for (uint32_t i = 0; i < req.sector_count; i++)
        printf(" %llu", (unsigned long long)sectors[i]);
    printf("\n");
    free(sectors);
    close(fd);
}

static void run_integrity_test(const char *mnt)
{
    DIR *dir = opendir(mnt);
    if (!dir) report_error_and_exit("Не удалось открыть директорию для теста");

    srand(time(NULL));
    unsigned int success_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", mnt, entry->d_name);

        int fd = open(full_path, O_RDWR);
        if (fd < 0) continue;

        uint32_t data = (rand() << 16) ^ rand();
        uint32_t readback;

		if (pwrite(fd, &data, sizeof(data), 0) < 0) perror("Ошибка записи");
		if (pread(fd, &readback, sizeof(readback), 0) < 0) perror("Ошибка чтения");
        
        if (data == readback) success_count++;
        close(fd);
    }
    closedir(dir);
    printf("Тест завершен. Успешно проверено файлов: %u\n", success_count);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Доступные команды:\n test <mnt> - тест целостности данных\n zero <mnt> - обнулить метаданные\n erase <mnt> - стереть файловую систему\n meta <mnt> - показать список файлов\n map <mnt> <файл> - показать карту секторов файла\n");
        return 1;
    }

    if (strcmp(argv[1], "test") == 0) {
        run_integrity_test(argv[2]);
    } 
	else {
        int fd = open_fs_root(argv[2]);
        if (strcmp(argv[1], "zero") == 0) execute_zero_all(fd);
        else if (strcmp(argv[1], "erase") == 0) execute_erase_fs(fd);
        else if (strcmp(argv[1], "meta") == 0) fetch_and_print_metadata(fd);
        else if (strcmp(argv[1], "map") == 0 && argc == 4) fetch_and_print_mapping(fd, argv[3]);
        else {
            fprintf(stderr, "Неизвестная команда.\n");
        	fprintf(stderr, "Доступные команды:\n test <mnt> - тест целостности данных\n zero <mnt> - обнулить метаданные\n erase <mnt> - стереть файловую систему\n meta <mnt> - показать список файлов\n map <mnt> <файл> - показать карту секторов файла\n");
            close(fd);
            return 1;
        }
    }
    return 0;
}