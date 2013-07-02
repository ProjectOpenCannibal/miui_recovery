/**
 * Copyright (c) 2013, Project Open Cannibal
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include <sys/vfs.h>

#include "nandroid.h"
#include "mtdutils/mounts.h"

#include "flashutils/flashutils.h"

#include "miui/src/miui.h"
#include "miui_intent.h"

// What's this do anyway???
#define MIUI_RECOVERY "miui_recovery"

// This should be moved to a device set location.
int minimum_storage=512;

/* After getting the root path of "/sdcard" or similar we need to add
 * a storage folder; default of course will be cotrecovery but this is
 * where the user_defined backup options come into play.
 * possible return would be "/sdcard/cotrecovery" */
void nandroid_get_assigned_backup_path(const char* backup_path)
{
	/* Will be pulling in the proper handling for multi-sdcard and normal
	 * sdcard, currently I'm having an issue getting files on external sdcard
	 * to show up visibly on emmc devices (at all in NG or 2.y...) */
    char root_path[PATH_MAX] = "/emmc";
//    nandroid_get_root_backup_path(root_path, other_sd);
    char tmp[PATH_MAX];
    struct stat st;
    if (stat(USER_DEFINED_BACKUP_MARKER, &st) == 0) {
        FILE *file = fopen_path(USER_DEFINED_BACKUP_MARKER, "r");
        fscanf(file, "%s", &tmp);
        fclose(file);
    } else {
        sprintf(tmp, "%s", DEFAULT_BACKUP_PATH);
    }
    sprintf(backup_path, "%s/%s", root_path, tmp);
}

/* Grab the full path from assigned_backup_path than add /backup/ to
 * the end. */
void nandroid_get_backup_path(const char* backup_path)
{
    char tmp[PATH_MAX];
	nandroid_get_assigned_backup_path(tmp);
    sprintf(backup_path, "%s/backup/", tmp);
}

/* Take our final backup path from get_backup_path and add a timestamp
 * folder location */
void nandroid_generate_timestamp_path(const char* backup_path)
{
	nandroid_get_backup_path(backup_path);
    time_t t = time(NULL);
    struct tm *bktime = localtime(&t);
	char tmp[PATH_MAX];
    if (bktime == NULL) {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(tmp, "%d", tp.tv_sec);
		strcat(backup_path, tmp);
    } else {
        strftime(tmp, sizeof(tmp), "%F.%H.%M.%S", bktime);
		strcat(backup_path, tmp);
    }
}

int ensure_directory(const char* dir) {
	char tmp[PATH_MAX];
	sprintf(tmp, "mkdir -p %s ; chmod 777 %s", dir, dir);
	__system(tmp);
	return 0;
}

static int print_and_error(const char* message) {
    ui_print("%s", message);
    return 1;
}

static long delta_milliseconds(struct timeval from, struct timeval to) {
  long delta_sec = (to.tv_sec - from.tv_sec)*1000;
  long delta_usec = (to.tv_usec - from.tv_usec)/1000;
  return (delta_sec + delta_usec);
}

#define NANDROID_UPDATE_INTERVAL 1000

static struct timeval lastupdate = (struct timeval) {0};
static int nandroid_files_total = 0;
static int nandroid_files_count = 0;

static void nandroid_callback(const char* filename) {
	if (filename == NULL)
		return;
	const char* justfile = basename(filename);
	char tmp[PATH_MAX];
	struct timeval curtime;
	gettimeofday(&curtime,NULL);
	/*
	 * Only update once every NANDROID_UPDATE_INTERVAL
	 * milli seconds.  We don't need frequent progress
	 * updates and updating every file uses WAY
	 * too much CPU time.
	 */
	nandroid_files_count++;
	if(delta_milliseconds(lastupdate,curtime) > NANDROID_UPDATE_INTERVAL) {
		strcpy(tmp, justfile);
		if(tmp[strlen(tmp) - 1] == '\n')
			tmp[strlen(tmp) - 1] = NULL;
		if(strlen(tmp) < 30) {
			lastupdate = curtime;
			ui_print("%s", tmp);
		}

		if (nandroid_files_total != 0)
			ui_set_progress((float)nandroid_files_count / (float)nandroid_files_total);
	}
}

static void compute_directory_stats(const char* directory)
{
    char tmp[PATH_MAX];
    sprintf(tmp, "find %s | wc -l > /tmp/dircount", directory);
    __system(tmp);
    char count_text[100];
    FILE* f = fopen("/tmp/dircount", "r");
    fread(count_text, 1, sizeof(count_text), f);
    fclose(f);
    nandroid_files_count = 0;
    nandroid_files_total = atoi(count_text);
    ui_reset_progress();
    ui_show_progress(1, 0);
}

typedef void (*file_event_callback)(const char* filename);
typedef int (*nandroid_backup_handler)(const char* backup_path, const char* backup_file_image, int callback);

static int mkyaffs2image_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd %s ; mkyaffs2image . %s.img ; exit $?", backup_path, backup_file_image);

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute mkyaffs2image.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = NULL;
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static int tar_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar ; (tar cv %s $(basename %s) | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = NULL;
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static nandroid_backup_handler get_backup_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_compress_wrapper;
    }

    // cwr5, we prefer tar for everything except yaffs2
    if (strcmp("yaffs2", mv->filesystem) == 0) {
        return mkyaffs2image_wrapper;
    }

    char str[255];
    char* partition;
    property_get("ro.cwm.prefer_tar", str, "true");
    if (strcmp("true", str) != 0) {
        return mkyaffs2image_wrapper;
    }

    return tar_compress_wrapper;
}


int nandroid_backup_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    struct stat file_info;

    int callback = stat("/sdcard/cotrecovery/.hidenandroidprogress", &file_info) != 0;

    ui_print("Backing up %s...\n", name);
    if (0 != (ret = ensure_path_mounted(mount_point) != 0)) {
        ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }

    compute_directory_stats(mount_point);
    char tmp[PATH_MAX];
	scan_mounted_volumes();
	Volume *v = volume_for_path(mount_point);
	MountedVolume *mv = NULL;
	if(v != NULL)
		mv = find_mounted_volume_by_mount_point(v->mount_point);
	if(mv == NULL || mv->filesystem == NULL)
		sprintf(tmp, "%s/%s.auto", backup_path, name);
    else
        sprintf(tmp, "%s/%s.%s", backup_path, name, mv->filesystem);
    nandroid_backup_handler backup_handler = get_backup_handler(mount_point);
    if (backup_handler == NULL) {
        ui_print("Error finding an appropriate backup handler.\n");
        return -2;
    }
    ret = backup_handler(mount_point, tmp, callback);
    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    if (0 != ret) {
        ui_print("Error while making a backup image of %s!\n", mount_point);
        return ret;
    }
    return 0;
}

int nandroid_backup_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists before attempting anything...
    if (vol == NULL || vol->fs_type == NULL)
        return NULL;

    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    
    // see if we need a raw backup (mtd)
    char tmp[PATH_MAX];
    int ret;
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        const char* name = basename(root);
        sprintf(tmp, "%s/%s.img", backup_path, name);
        ui_print("Backing up %s image...\n", name);
        if (0 != (ret = backup_raw_partition(vol->fs_type, vol->device, tmp))) {
            ui_print("Error while backing up %s image!\n", name);
            return ret;
        }
        return 0;
    }
    return nandroid_backup_partition_extended(backup_path, root, 1);
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int recalc_sdcard_space(const char* backup_path) {
	struct statfs s;
	int ret;
	Volume* volume = volume_for_path(backup_path);
	if (0 != (ret = statfs(volume->mount_point, &s))) {
		return print_and_error("Can't mount sdcard.\n");
	}
	uint64_t bavail = s.f_bavail;
    uint64_t bsize = s.f_bsize;
    uint64_t sdcard_free = bavail * bsize;
    uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
    return sdcard_free_mb;
}

int nandroid_backup(const char* backup_path)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    
    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    
    Volume* volume = volume_for_path(backup_path);
    if (NULL == volume) {
      if (strstr(backup_path, "/sdcard") == backup_path && is_data_media())
          volume = volume_for_path("/data");
      else
          return print_and_error("Unable to find volume for backup path.\n");
    }
    int ret;
    struct statfs s;
    if (0 != (ret = statfs(volume->mount_point, &s)))
        return print_and_error("Unable to stat backup path.\n");

    uint64_t sdcard_free_mb = recalc_sdcard_space(backup_path);

    ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
    if (sdcard_free_mb < minimum_storage) {
		/**
		 * TODO
		 * 	Add in a lowspace menu :P
		 * 
        if (show_lowspace_menu(sdcard_free_mb, backup_path) == 1) {
        */
			return 0;
		///}
	}

	// Cancel the backup if ensure_directory fails for any reason.
	if(0 != (ret = ensure_directory(backup_path)))
		return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/boot")))
        return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/recovery")))
        return ret;

	char tmp[PATH_MAX];
    Volume *vol = volume_for_path("/wimax");
    if (vol != NULL && 0 == stat(vol->device, &s))
    {
        char serialno[PROPERTY_VALUE_MAX];
        ui_print("Backing up WiMAX...\n");
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);
        ret = backup_raw_partition(vol->fs_type, vol->device, tmp);
        if (0 != ret)
            return print_and_error("Error while dumping WiMAX image!\n");
    }

    if (ensure_path_mounted("/system") != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    if (0 != (ret = nandroid_backup_partition(backup_path, "/system")))
        return ret;

    if (ensure_path_mounted("/data") != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    if (0 != (ret = nandroid_backup_partition(backup_path, "/data")))
        return ret;

    if (has_datadata()) {
        if (ensure_path_mounted("/datadata") != 0) {
            return print_and_error("Can't mount backup path.\n");
        }
        if (0 != (ret = nandroid_backup_partition(backup_path, "/datadata")))
            return ret;
    }

    if (0 != stat("/sdcard/.android_secure", &s)) {
        ui_print("No /sdcard/.android_secure found. Skipping backup of applications on external storage.\n");
    } else {
        if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
            return ret;
    }

    if (ensure_path_mounted("/cache") != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/cache", 0)))
        return ret;

    vol = volume_for_path("/sd-ext");
    if (vol == NULL || 0 != stat(vol->device, &s))
    {
        ui_print("No sd-ext found. Skipping backup of sd-ext.\n");
    }
    else
    {
        if (0 != ensure_path_mounted("/sd-ext"))
            ui_print("Could not mount sd-ext. sd-ext backup may not be supported on this device. Skipping backup of sd-ext.\n");
        else if (0 != (ret = nandroid_backup_partition(backup_path, "/sd-ext")))
            return ret;
    }

    ui_print("Generating md5 sum...\n");
    sprintf(tmp, "nandroid-md5.sh %s", backup_path);
    if (0 != (ret = __system(tmp))) {
        ui_print("Error while generating md5 sum!\n");
        return ret;
    }

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\nBackup complete!\n");
    return 0;
}

int nandroid_advanced_backup(const char* backup_path, int boot, int recovery, int system, int data, int cache, int sdext)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);

	char tmp[PATH_MAX];
	if (ensure_path_mounted(backup_path) != 0) {
		sprintf(tmp, "Can't mount %s\n", backup_path);
        return print_and_error(tmp);
	}

    int ret;
    struct statfs s;
    Volume* volume = volume_for_path(backup_path);
    if (0 != (ret = statfs(volume->mount_point, &s)))
        return print_and_error("Unable to stat backup path.\n");

    uint64_t sdcard_free_mb = recalc_sdcard_space(backup_path);

    ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
    if (sdcard_free_mb < minimum_storage) {
        if (show_lowspace_menu(sdcard_free_mb, backup_path) == 1) {
			return 0;
		}
	}

	// Cancel the backup if ensure_directory fails for any reason.
	if(0 != (ret = ensure_directory(backup_path)))
		return ret;

    if (boot && 0 != (ret = nandroid_backup_partition(backup_path, "/boot")))
        return ret;

    if (recovery && 0 != (ret = nandroid_backup_partition(backup_path, "/recovery")))
        return ret;

    if (system && 0 != (ret = nandroid_backup_partition(backup_path, "/system")))
        return ret;

    if (data && 0 != (ret = nandroid_backup_partition(backup_path, "/data")))
        return ret;

    if (data && has_datadata()) {
        if (0 != (ret = nandroid_backup_partition(backup_path, "/datadata")))
            return ret;
    }

    if (0 != stat("/sdcard/.android_secure", &s)) {
        ui_print("No /sdcard/.android_secure found. Skipping backup of applications on external storage.\n");
    } else {
        if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
            return ret;
    }

    if (cache && 0 != (ret = nandroid_backup_partition_extended(backup_path, "/cache", 0)))
        return ret;

    ui_print("Generating md5 sum...\n");
    sprintf(tmp, "nandroid-md5.sh %s", backup_path);
    if (0 != (ret = __system(tmp))) {
        ui_print("Error while generating md5 sum!\n");
        return ret;
    }

    sync();
    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    ui_reset_progress();
    ui_print("\nBackup complete!\n");
    return 0;
}

typedef int (*nandroid_restore_handler)(const char* backup_file_image, const char* backup_path, int callback);

static int unyaffs_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd %s ; unyaffs %s ; exit $?", backup_path, backup_file_image);
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute unyaffs.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = NULL;
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static int tar_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; cat %s* | tar xv ; exit $?", backup_path, backup_file_image);
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = NULL;
        if (callback)
            nandroid_callback(tmp);
    }

    return __pclose(fp);
}

static nandroid_restore_handler get_restore_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    scan_mounted_volumes();
    MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_extract_wrapper;
    }

    // cwr 5, we prefer tar for everything unless it is yaffs2
    char str[255];
    char* partition;
    property_get("ro.cwm.prefer_tar", str, "false");
    if (strcmp("true", str) != 0) {
        return unyaffs_wrapper;
    }

    if (strcmp("yaffs2", mv->filesystem) == 0) {
        return unyaffs_wrapper;
    }

    return tar_extract_wrapper;
}

int nandroid_restore_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    nandroid_restore_handler restore_handler = NULL;
    const char *filesystems[] = { "yaffs2", "ext2", "ext3", "ext4", "vfat", "rfs", NULL };
    const char* backup_filesystem = NULL;
    Volume *vol = volume_for_path(mount_point);
    const char *device = NULL;
    if (vol != NULL)
        device = vol->device;

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    struct stat file_info;
    if (0 != (ret = statfs(tmp, &file_info))) {
        // can't find the backup, it may be the new backup format?
        // iterate through the backup types
        printf("couldn't find default\n");
        char *filesystem;
        int i = 0;
        while ((filesystem = filesystems[i]) != NULL) {
            sprintf(tmp, "%s/%s.%s.img", backup_path, name, filesystem);
            if (0 == (ret = statfs(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = unyaffs_wrapper;
                break;
            }
            sprintf(tmp, "%s/%s.%s.tar", backup_path, name, filesystem);
            if (0 == (ret = statfs(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = tar_extract_wrapper;
                break;
            }
            i++;
        }

        if (backup_filesystem == NULL || restore_handler == NULL) {
            ui_print("%s.img not found. Skipping restore of %s.\n", name, mount_point);
            return 0;
        } else {
            printf("Found new backup image: %s\n", tmp);
        }

        // If the fs_type of this volume is "auto" or mount_point is /data
        // and is_data_media (redundantly, and vol for /sdcard is NULL), let's revert
        // to using a rm -rf, rather than trying to do a
        // ext3/ext4/whatever format.
        // This is because some phones (like DroidX) will freak out if you
        // reformat the /system or /data partitions, and not boot due to
        // a locked bootloader.
        // Other devices, like the Galaxy Nexus, XOOM, and Galaxy Tab 10.1
        // have a /sdcard symlinked to /data/media. /data is set to "auto"
        // so that when the format occurs, /data/media is not erased.
        // The "auto" fs type preserves the file system, and does not
        // trigger that lock.
        // Or of volume does not exist (.android_secure), just rm -rf.
        if (vol == NULL || 0 == strcmp(vol->fs_type, "auto"))
            backup_filesystem = NULL;
        else if (0 == strcmp(vol->mount_point, "/data") && volume_for_path("/sdcard") == NULL && is_data_media())
	         backup_filesystem = NULL;
    }

	// Cancel the backup if ensure_directory fails for any reason.
	if(0 != (ret = ensure_directory(mount_point)))
		return ret;

    int callback = stat("/sdcard/cotrecovery/.hidenandroidprogress", &file_info) != 0;

    ui_print("Restoring %s...\n", name);
    /* In the case of a NULL backup_filesystem use the intent format instead of
     * calling format_device directly. */
    if (backup_filesystem == NULL) {
		if(ret = miuiIntent_send(INTENT_FORMAT, 1, mount_point) != RET_OK) {
            ui_print("Error while formatting %s!\n", mount_point);
            return ret;
        }
    } // Otherwise treat as normal.
    else if (0 != (ret = format_device(device, mount_point, backup_filesystem))) {
        ui_print("Error while formatting %s!\n", mount_point);
        return ret;
    }

    if (0 != (ret = ensure_path_mounted(mount_point))) {
        ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }

    if (restore_handler == NULL)
        restore_handler = get_restore_handler(mount_point);
    if (restore_handler == NULL) {
        ui_print("Error finding an appropriate restore handler.\n");
        return -2;
    }
    if (0 != (ret = restore_handler(tmp, mount_point, callback))) {
        ui_print("Error while restoring %s!\n", mount_point);
        return ret;
    }

    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    return 0;
}

int nandroid_restore_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw restore (mtd)
    char tmp[PATH_MAX];
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        int ret;
        const char* name = basename(root);
        ui_print("Erasing %s before restore...\n", name);
        /* This should only be used on specific device types, all other 
         * instances should use the INTENT_FORMAT */
        if (0 != (ret = format_volume(root))) {
            ui_print("Error while erasing %s image!", name);
            return ret;
        }
        sprintf(tmp, "%s%s.img", backup_path, root);
        ui_print("Restoring %s image...\n", name);
        if (0 != (ret = restore_raw_partition(vol->fs_type, vol->device, tmp))) {
            ui_print("Error while flashing %s image!", name);
            return ret;
        }
        return 0;
    }
    return nandroid_restore_partition_extended(backup_path, root, 1);
}

int nandroid_restore(const char* backup_path, int restore_boot, int restore_system, int restore_data, int restore_cache, int restore_sdext)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    nandroid_files_total = 0;

    if (ensure_path_mounted(backup_path) != 0)
        return print_and_error("Can't mount backup path\n");

	char tmp[PATH_MAX];
	if (ensure_path_mounted(backup_path) != 0) {
		sprintf(tmp, "Can't mount %s\n", backup_path);
		return print_and_error(tmp);
	}

    ui_print("Checking MD5 sums...\n");
    sprintf(tmp, "cd %s && md5sum -c nandroid.md5", backup_path);
    if (0 != __system(tmp))
        return print_and_error("MD5 mismatch!\n");

    int ret;

    if (restore_boot && NULL != volume_for_path("/boot") && 0 != (ret = nandroid_restore_partition(backup_path, "/boot")))
        return ret;

    if (restore_system && 0 != (ret = nandroid_restore_partition(backup_path, "/system")))
        return ret;

    if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/data")))
        return ret;

	if (has_datadata()) {
		if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/datadata")))
			return ret;
	}

    if (restore_data && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
        return ret;

    if (restore_cache && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/cache", 0)))
        return ret;

    if (restore_sdext && 0 != (ret = nandroid_restore_partition(backup_path, "/sd-ext")))
        return ret;

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\nRestore complete!\n");
    return 0;
}

int nandroid_usage()
{
    printf("Usage: nandroid backup\n");
    printf("Usage: nandroid restore <directory>\n");
    return 1;
}

int nandroid_main(int argc, char** argv)
{
    if (argc > 3 || argc < 2)
        return nandroid_usage();

    if (strcmp("backup", argv[1]) == 0) {
        if (argc != 2)
            return nandroid_usage();

        char backup_path[PATH_MAX];
        nandroid_generate_timestamp_path(backup_path);
        return nandroid_backup(backup_path);
    }
    if (strcmp("restore", argv[1]) == 0) {
        if (argc != 3)
            return nandroid_usage();
        return nandroid_restore(argv[2], 1, 1, 1, 1, 1);
    }
    return nandroid_usage();
}
