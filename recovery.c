/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"
#include "mmc_erase.h"

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "wipe_sdcard", no_argument, NULL, 'm' },  // Motorola, r42707, 25/11/2009, IKMAP-1478
  { "silent", no_argument, NULL, 'i' },       // Motorola, r42707, 25/11/2009, IKMAP-1478
  { NULL, 0, NULL, 0 },
};

static const char *COMMAND_FILE = "CACHE:recovery/command";
static const char *INTENT_FILE = "CACHE:recovery/intent";
static const char *LOG_FILE = "CACHE:recovery/log";
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";

static const char* SHELL = "SYSTEM:bin/sh";
static const char* DATA_RECOVERY = "/data/recovery";
static const char* DATA_NAME = "/data/recovery/name";
static const char* DATA_LOG = "/data/recovery/log";
static const char *MC_RESULT_FILE = "CACHE:recovery/mc_result"; // Motorola, r42707, 25/11/2009, IKMAP-1478

static char log_name_buffer[512];
static char *log_name = NULL;
static char boot_command[64];

int is_firmware_to_install();

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a file given in root:path format, mounting partitions as necessary
static FILE*
fopen_root_path(const char *root_path, const char *mode) {
    if (ensure_root_path_mounted(root_path) != 0) {
        LOGE("Can't mount %s\n", root_path);
        return NULL;
    }

    char path[PATH_MAX] = "";
    if (translate_root_path(root_path, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s\n", root_path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    if (fp == NULL) LOGE("Can't open %s\n", path);
    return fp;
}

static void check_and_fclose(FILE *fp, const char *name);
// Motorola change 2/10/2009 eddie@motorola.com
static void write_result(const char *update_file, const char *result) {
	if (update_file == NULL) {
		return;
	}
	if (result == NULL) {
		result = "failure";
	}
    snprintf(log_name_buffer, sizeof(log_name_buffer), "%s.%s", update_file, result);
    log_name = log_name_buffer;
}
// end of Motorola change


// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_root_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                (*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}

// BEGIN Motorola, r42707, 25/11/2009, IKMAP-1478 

// report shredding errors according to errcode given
static void
report_master_clear_error(WIPE_ERRCODE errcode)
{
    if (errcode == WIPE_SUCCESS) {
        // nothing to report
        return;
    }
    FILE* result_file = fopen_root_path(MC_RESULT_FILE, "w");
    if (result_file != NULL) {
        fprintf(result_file, "%d", errcode);
        check_and_fclose(result_file, MC_RESULT_FILE);
    } else {
        LOGE("Failed to write Master Clear result file\n");
    }            
}

// shred data on SD card
static WIPE_ERRCODE
erase_mmc()
{
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress(); // turn off the progress bar
    ui_print("Formatting SD card...\n");
    return mmc_erase(1);
}
// END IKMAP-1478

static void
set_sdcard_update_bootloader_message()
{
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}

// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent)
{
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_root_path(INTENT_FILE, "w");
        if (fp != NULL) {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    if (log_name == NULL) {
        log_name = LOG_FILE;
    }

    // Copy logs to cache so the system can find out what happened.
    FILE *log = fopen_root_path(log_name, "a");
    if (log != NULL) {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("Can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            static long tmplog_offset = 0;
            fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            tmplog_offset = ftell(tmplog);
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, LOG_FILE);
    }

    // Reset the bootloader message to revert to a normal main system boot.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    char path[PATH_MAX] = "";
    if (ensure_root_path_mounted(COMMAND_FILE) != 0 ||
        translate_root_path(COMMAND_FILE, path, sizeof(path)) == NULL ||
        (unlink(path) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

static int
erase_root(const char *root)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s...\n", root);
    return format_root_device(root);
}


static const char* enable_adb(void){
        FILE *fp;
	fp = fopen("/dev/usb_device_mode", "w");
	if (!fp) {
	        LOGE("Error at opening file");
		return NULL;
	}
	fprintf(fp, "msc_adb");
	fclose(fp);
	LOGI("ADB Enabled");
	return NULL;
}



static char**
prepend_title(char** headers) {
    char* title[] = { "Android system recovery <"
                          EXPAND(RECOVERY_API_VERSION) "e>",
                      "",
                      NULL };

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);

    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

static int
get_menu_selection(char** headers, char** items, int menu_only) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();

    ui_start_menu(headers, items);
    int selected = ui_menu_select(-1);
    int chosen_item = -1;

    while (chosen_item < 0) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        int action = device_handle_key(key, visible);

        if (action < 0) {
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case SELECT_ITEM:
                    chosen_item = ui_menu_select(-1); //selected;
                    break;
                case NO_ACTION:
                    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }
    }
    ui_clear_key_queue();
    ui_end_menu();
    return chosen_item;
}

static void
wipe_data(int confirm) {
    if (confirm) {
        static char** title_headers = NULL;

        if (title_headers == NULL) {
            char* headers[] = { "Enable ADB",
                                "",
                                NULL };
            title_headers = prepend_title(headers);
        }

        char* items[] = { "Yes",   // [1]
                          NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1);
        if (chosen_item != 1) {
            return;
        }
    }

    ui_print("\n-- adb enabled...\n");
    enable_adb();
    device_wipe_data();
//    erase_root("DATA:");
//    erase_root("CACHE:");
}

static void
prompt_and_wait()
{
    char** headers = prepend_title(MENU_HEADERS);

    for (;;) {
        finish_recovery(NULL);
        ui_reset_progress();

        int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0);

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        switch (chosen_item) {
            case ITEM_REBOOT:
                return;

            case ITEM_WIPE_DATA:
                wipe_data(ui_text_visible());
                if (!ui_text_visible()) return;
                break;

            case ITEM_WIPE_CACHE:
                ui_print("\n-- Wiping cache...\n");
                erase_root("CACHE:");
                ui_print("Cache wipe complete.\n");
                if (!ui_text_visible()) return;
                break;

            case ITEM_APPLY_SDCARD:
                ui_print("\n-- Install from sdcard...\n");
                set_sdcard_update_bootloader_message();
                int status = install_package(SDCARD_PACKAGE_FILE);
                if (status != INSTALL_SUCCESS) {
                    ui_set_background(BACKGROUND_ICON_ERROR);
                    ui_print("Installation aborted.\n");
                } else if (!ui_text_visible()) {
                    return;  // reboot if logs aren't visible
                } else {
                    if (firmware_update_pending()) {
                        ui_print("\nReboot via menu to complete\n"
                                 "installation.\n");
                    } else {
                        ui_print("\nInstall from sdcard complete.\n");
                    }
                }
                break;
        }
    }
}

static void
print_property(const char *key, const char *name, void *cookie)
{
    fprintf(stderr, "%s=%s\n", key, name);
}

int
main(int argc, char **argv)
{
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    fprintf(stderr, "Starting recovery on %s", ctime(&start));

    ui_init();
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0, wipe_mmc = 0;  // Motorola, r42707, 25/11/2009, IKMAP-1478
    int silent = 0;                                   // Motorola, r42707, 25/11/2009, IKMAP-1478

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'm': wipe_mmc = 1; break;   // Motorola, r42707, 25/11/2009, IKMAP-1478
        case 'c': wipe_cache = 1; break;
        case 'i': silent = 1; break;     // Motorola, r42707, 25/11/2009, IKMAP-1478
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    fprintf(stderr, "Command:");
    for (arg = 0; arg < argc; arg++) {
        fprintf(stderr, " \"%s\"", argv[arg]);
    }
    fprintf(stderr, "\n\n");

    property_list(print_property, NULL);
    fprintf(stderr, "\n");

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package);
        char* update_result = "success";
        if (status != INSTALL_SUCCESS) {
            ui_print("Installation aborted.\n");
            update_result = "failure";
        }
        write_result(update_package, update_result);

    // BEGIN Motorola, r42707, 25/11/2009, IKMAP-1478 
    } else if (wipe_data || wipe_cache || wipe_mmc) {
        WIPE_ERRCODE errcode = WIPE_SUCCESS;

        if (wipe_data && device_wipe_data()) {
            status = INSTALL_ERROR;
            errcode |= WIPE_DEV_FAILED;
            ui_print("Device wipe failed.\n");
        }
        if (wipe_data && erase_root("DATA:")) {
            status = INSTALL_ERROR;
            errcode |= WIPE_DEV_FAILED;
            ui_print("Data wipe failed.\n");
        }
        if (wipe_cache && erase_root("CACHE:")) {
            status = INSTALL_ERROR;
            errcode |= WIPE_DEV_FAILED;
            ui_print("Cache wipe failed.\n");
        }
        if (wipe_mmc) { 
            errcode |= erase_mmc();
            if (errcode != WIPE_SUCCESS) {
                // just ignore sdcard errors
                // status = INSTALL_ERROR;
            }
        }

        // Report Master Clear error if any
        report_master_clear_error(silent?WIPE_SUCCESS:errcode);

    // END IKMAP-1478

    } else {
        status = INSTALL_ERROR;  // No command specified
    }

    if (status != INSTALL_SUCCESS) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS || ui_text_visible()) prompt_and_wait();

    // If there is a radio image pending, reboot now to install it.
    maybe_install_firmware_update(send_intent);

    // BEGIN Motorola, r42707, 25/11/2009, IKMAP-1478 
    // Show confirmation if Master Clear finished successfully
    if (status == INSTALL_SUCCESS && (wipe_data || wipe_cache))
    {
        ui_reset_progress();
        ui_set_background(BACKGROUND_ICON_DONE);
        sleep(3);
    }
    // END IKMAP-1478

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    ui_print("Rebooting...\n");
    sync();
    reboot(RB_AUTOBOOT);
    return EXIT_SUCCESS;
}
