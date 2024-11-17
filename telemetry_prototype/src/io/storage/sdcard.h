#ifndef APP_IO_SDCARD_H_
#define APP_IO_SDCARD_H_

#include <zephyr/fs/fs.h>
#include <ff.h>

/*
 *  Note the fatfs library is able to mount only strings inside _VOLUME_STRS
 *  in ffconf.h
 */
#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/"DISK_DRIVE_NAME":"

static FATFS fat_fs;

#define FS_RET_OK FR_OK

#define MAX_PATH 128
#define SOME_FILE_NAME "some.dat"
#define SOME_DIR_NAME "some"
#define SOME_REQUIRED_LEN MAX(sizeof(SOME_FILE_NAME), sizeof(SOME_DIR_NAME))

static int lsdir(const char *path);

#endif /* APP_IO_SDCARD_H_ */
