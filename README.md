## Сборка

Сборка проекта должна выполняться в Linux с установленными заголовочными файлами ядра (`linux-headers`). Для корректной работы требуется ядро версии 6.12.x.

```bash
make
```

## Сборка на Windows (WSL) / macOS / других окружениях

Для сборки рекомендуется использовать виртуальную машину с Linux (VirtualBox/VMware).

### Развертывание Linux в VirtualBox

1. Создайте виртуальную машину с современным дистрибутивом Linux (например, Ubuntu 24.04). Рекомендуется выделить системе минимум 2 ГБ RAM и 2 ядра CPU.

2. Внутри виртуальной машины установите необходимые зависимости:

```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r) kmod util-linux git
```

3. Перейдите в директорию с проектом и выполните сборку:

```bash
make
```

## Использование и тестирование

### 1. Подготовка диска

```bash
dd if=/dev/zero of=disk.img bs=512 count=4096
LOOP_DEV=$(sudo losetup --show -f disk.img)
```

### 2. Загрузка модуля

```bash
sudo insmod simplefs.ko \
    disk_device_path=$LOOP_DEV \
    sb_first_sector=0 \
    sb_second_sector=128 \
    max_filename_len=32 \
    max_file_sectors=4
```

### 3. Монтирование ФС

```bash
sudo mkdir -p /mnt/simplefs
sudo mount -t simplefs_hw $LOOP_DEV /mnt/simplefs
```

### 4. Демонстрации корректности работы модуля

```bash
sudo ./userspace/simplefs_cli test /mnt/simplefs
sudo ./userspace/simplefs_cli meta /mnt/simplefs
sudo ./userspace/simplefs_cli map /mnt/simplefs file0
sudo ./userspace/simplefs_cli zero /mnt/simplefs
sudo ./userspace/simplefs_cli erase /mnt/simplefs
```