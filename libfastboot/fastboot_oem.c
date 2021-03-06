/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Authors: Sylvain Chouleur <sylvain.chouleur@intel.com>
 *          Jeremy Compostella <jeremy.compostella@intel.com>
 *          Jocelyn Falempe <jocelyn.falempe@intel.com>
 *          Andrew Boie <andrew.p.boie@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <lib.h>
#include <vars.h>
#include <storage.h>
#include <slot.h>

#include "uefi_utils.h"
#include "flash.h"
#include "hashes.h"
#include "fastboot.h"
#include "fastboot_ui.h"
#include "gpt.h"
#include "authenticated_action.h"

#include "fastboot_oem.h"
#include "intel_variables.h"
#include "text_parser.h"
#ifdef USE_SLOT
#include "libavb/libavb.h"
#include "libavb/uefi_avb_ops.h"

#define VBMETA_LABEL		L"vbmeta"
#endif
#define OFF_MODE_CHARGE		"off-mode-charge"
#define CRASH_EVENT_MENU	"crash-event-menu"
#define SLOT_FALLBACK		"slot-fallback"
#ifdef RPMB_STORAGE
#include "rpmb_storage.h"
#endif

static cmdlist_t cmdlist;

static EFI_STATUS fastboot_oem_publish(void)
{
	EFI_STATUS ret;

	ret = fastboot_publish(OFF_MODE_CHARGE, get_off_mode_charge() ? "1" : "0");
	if (EFI_ERROR(ret))
		return ret;

	return publish_intel_variables();
}

static EFI_STATUS cmd_oem_set_boolean(INTN argc, CHAR8 **argv,
				      char *name, EFI_STATUS (*set_fun)(BOOLEAN value))
{
	EFI_STATUS ret;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return EFI_INVALID_PARAMETER;
	}

	if (strcmp(argv[1], (CHAR8* )"1") && strcmp(argv[1], (CHAR8 *)"0")) {
		fastboot_fail("Invalid value");
		error(L"Please specify 1 or 0 to enable/disable %a", name);
		return EFI_INVALID_PARAMETER;
	}

        ret = set_fun(!strcmp(argv[1], (CHAR8* )"1"));
	if (EFI_ERROR(ret))
		fastboot_fail("Failed to set %a", OFF_MODE_CHARGE);

	return ret;
}

static void cmd_oem_off_mode_charge(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;

	ret = cmd_oem_set_boolean(argc, argv, OFF_MODE_CHARGE, set_off_mode_charge);
	if (EFI_ERROR(ret))
		return;

	ret = fastboot_oem_publish();
	if (EFI_ERROR(ret))
		fastboot_fail("Failed to publish OEM variables");
	else
		fastboot_okay("");
}

static void cmd_oem_crash_event_menu(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;

	ret = cmd_oem_set_boolean(argc, argv, CRASH_EVENT_MENU, set_crash_event_menu);
	if (EFI_ERROR(ret))
		return;

	ret = fastboot_oem_publish();
	if (EFI_ERROR(ret))
		fastboot_fail("Failed to publish OEM variables");
	else
		fastboot_okay("");
}

static void cmd_oem_setvar(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	CHAR16 *varname;
	CHAR8 *value = NULL;

	if (argc < 2 || argc > 3) {
		fastboot_fail("Invalid parameter");
		return;
	}

	varname = stra_to_str(argv[1]);
	if (argc == 3)
		value = argv[2];

	if (!value)
		ret = del_efi_variable(&loader_guid, varname);
	else
		ret = set_efi_variable(&loader_guid, varname,
				       strlen(value) + 1, value,
				       TRUE, TRUE);
	if (EFI_ERROR(ret))
		fastboot_fail("Unable to %a '%s' variable",
			      value ? "set" : "clear", varname);
	else
		fastboot_okay("");

	FreePool(varname);
}

static void cmd_oem_reboot(INTN argc, CHAR8 **argv)
{
	enum boot_target bt;
	CHAR16 *target;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	target = stra_to_str(argv[1]);
	if (!target) {
		fastboot_fail("Unable to convert string");
		return;
	}

	bt = name_to_boot_target(target);
	FreePool(target);
	if (bt == UNKNOWN_TARGET) {
		fastboot_fail("Unknown %a boot target", argv[1]);
		return;
	}

	fastboot_reboot(bt, L"Rebooting to requested target ...");
}

#ifdef __SUPPORT_ABL_BOOT
#define IFWI_CAPSULE_UPDATE			L"IfwiCapsuleUpdate"
static void cmd_oem_fw_update(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	CHAR8 *capsule_buf;
	INTN capsule_buf_len;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	capsule_buf = argv[1];
	capsule_buf_len = strlen(capsule_buf);

	if (capsule_buf[1] < '0' || capsule_buf[1] > '9' || capsule_buf[2] != ':'
		|| capsule_buf_len > 33) {
		fastboot_fail("Illegal capsule buffer");
		return;
	}

	ret = set_efi_variable(&loader_guid, IFWI_CAPSULE_UPDATE, capsule_buf_len + 1,
						   capsule_buf, TRUE, TRUE);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Unable to set %s", IFWI_CAPSULE_UPDATE);
		return;
	}

	fastboot_okay("");
}
#endif

static void cmd_oem_garbage_disk(__attribute__((__unused__)) INTN argc,
				 __attribute__((__unused__)) CHAR8 **argv)
{
	EFI_STATUS ret = garbage_disk();

	if (ret == EFI_SUCCESS)
		fastboot_okay("");
	else
		fastboot_fail("Garbage disk failed, %r", ret);
}

static struct oem_hash {
	const CHAR16 *name;
	EFI_STATUS (*hash)(const CHAR16 *name);
	BOOLEAN fail_if_missing;
} OEM_HASH[] = {
	{ BOOT_LABEL,		get_boot_image_hash,	TRUE },
	{ RECOVERY_LABEL,	get_boot_image_hash,	FALSE },
#ifdef USE_TRUSTY
#ifdef USE_MULTIBOOT
	{ MULTIBOOT_LABEL,	get_ias_image_hash,	TRUE },
#endif
	{ TOS_LABEL,		get_boot_image_hash,	TRUE },
#endif
	{ BOOTLOADER_LABEL,	get_bootloader_hash,	FALSE },
#ifdef USE_SLOT
	{ VBMETA_LABEL,		get_boot_image_hash,	FALSE },
#endif
	{ SYSTEM_LABEL,		get_fs_hash,		TRUE },
	{ VENDOR_LABEL,		get_fs_hash,		FALSE }
};

static void cmd_oem_gethashes(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	UINTN i;

	if (argc == 2) {
		ret = set_hash_algorithm(argv[1]);
		if (EFI_ERROR(ret)) {
			fastboot_fail("Fail to set the algorithm, %r", ret);
			return;
		}
	}

	for (i = 0; i < ARRAY_SIZE(OEM_HASH); i++) {
		ret = OEM_HASH[i].hash(slot_label(OEM_HASH[i].name));
		if (EFI_ERROR(ret)
		    && (ret != EFI_NOT_FOUND || OEM_HASH[i].fail_if_missing)) {
			fastboot_fail("Failed to get hash for %s, %r",
				      OEM_HASH[i].name, ret);
			return;
		}
	}

	fastboot_okay("");
}

#ifndef USER
static void cmd_oem_set_storage(INTN argc, CHAR8 **argv)
{
	enum storage_type type;
	EFI_STATUS ret;

	if (argc != 2) {
		fastboot_fail("Supported storage: ufs, emmc, nvme");
		return;
	}

	if (!strcmp(argv[1], (CHAR8*)"emmc")) {
		type = STORAGE_EMMC;
		goto set;
	}
	if (!strcmp(argv[1], (CHAR8*)"ufs")) {
		type = STORAGE_UFS;
		goto set;
	}
	if (!strcmp(argv[1], (CHAR8*)"nvme")) {
		type = STORAGE_NVME;
		goto set;
	}
	fastboot_fail("Unsupported storage");
	return;
set:
	ret = identify_boot_device(type);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to set storage: %r", ret);
		return;
	}

	ret = gpt_refresh();
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to refresh partition table: %r", ret);
		return;
	}

	ret = refresh_partition_var();
	if (EFI_ERROR(ret))
		fastboot_fail("Failed to refresh partition vars: %r", ret);
	else
		fastboot_okay("");
}

static void cmd_oem_reprovision(__attribute__((__unused__)) INTN argc,
			        __attribute__((__unused__)) CHAR8 **argv)
{
	if (EFI_ERROR(reprovision_state_vars())) {
		fastboot_fail("Unable to clear provisioning variables");
		return;
	}
	fastboot_reboot(DNX, L"Rebooting to dnx ...");
}

static void cmd_oem_rm(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	EFI_FILE_IO_INTERFACE *io;
	const CHAR8 prefix[] = "/ESP/";
	CHAR8 *filename;
	CHAR16 *filename16;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	if (strncmp(prefix, argv[1], sizeof(prefix) - 1)) {
		fastboot_fail("File deletion is restricted to the ESP");
		return;
	}

	ret = get_esp_fs(&io);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to get partition ESP");
		return;
	}

	filename = &argv[1][ARRAY_SIZE(prefix) - 1];
	CHAR8 *tmp;
	for (tmp = filename; *tmp; tmp++)
		if (*tmp == '/')
			*tmp = '\\';

	filename16 = stra_to_str(filename);
	if (!filename16) {
		efi_perror(ret, L"failed to allocate CHAR16 filename");
		fastboot_fail("failed to allocate CHAR16 filename");
		return;
	}

	ret = uefi_delete_file(io, filename16);
	FreePool(filename16);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to delete file '%a', %r", filename, ret);
		return;
	}

	fastboot_okay("");
}

static void cmd_oem_set_watchdog_counter_max(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	unsigned long value;
	char *endptr;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	value = strtoul((char *)argv[1], &endptr, 10);
	if (*endptr != '\0' || value > (UINT8)-1) {
		fastboot_fail("Invalid value");
		return;
	}

	ret = set_watchdog_counter_max(value);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to set watchdog counter max, %r", ret);
		return;
	}

	fastboot_okay("");
}

static void cmd_oem_disable_slot_fallback(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;

	ret = cmd_oem_set_boolean(argc, argv, SLOT_FALLBACK, set_slot_fallback);
	if (EFI_ERROR(ret))
		return;

	fastboot_okay("");
}

static void cmd_oem_erase_efivars(__attribute__((__unused__)) INTN argc,
				  __attribute__((__unused__)) CHAR8 **argv)
{
	EFI_STATUS ret;

	if (argc != 1) {
		fastboot_fail("Invalid parameter");
		return;
	}

	ret = erase_efivars();
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to erase all the EFI variables, %r", ret);
		return;
	}

	fastboot_okay("");
}

#ifdef RPMB_STORAGE
static void cmd_oem_erase_rpmb(INTN argc, __attribute__((__unused__)) CHAR8 **argv)
{
	EFI_STATUS ret;

	if (argc != 1) {
		fastboot_fail("Invalid parameter");
		return;
	}

	ret = erase_rpmb_all_blocks();
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to erase all rpmb data, %r", ret);
		return;
	}

	fastboot_okay("");
}
#endif
#endif

static void cmd_oem_get_logs(INTN argc, __attribute__((__unused__)) CHAR8 **argv)
{
	EFI_STATUS ret;
	UINT32 flags;
	char *buf;
	UINTN size;

	if (argc != 1) {
		fastboot_fail("Invalid parameter");
		return;
	}

	ret = get_efi_variable(&loader_guid, LOG_VAR, &size, (VOID **)&buf, &flags);
	if (EFI_ERROR(ret)) {
		fastboot_fail("failed to get log buffer from variable, %r", ret);
		return;
	}

	ret = parse_text_buffer(buf, size, fastboot_info_long_string, NULL);
	FreePool(buf);
	if (EFI_ERROR(ret)) {
		fastboot_fail("Failed to parse log buffer, %r", ret);
		return;
	}

	fastboot_okay("");
}

static void cmd_oem(INTN argc, CHAR8 **argv)
{
	if (argc < 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	fastboot_run_cmd(cmdlist, (char *)argv[1], argc - 1, argv + 1);
}

#ifdef BOOTLOADER_POLICY
#ifndef BOOTLOADER_POLICY_EFI_VAR
#error "Fastboot EFI does not support Bootloader policy without EFI variables."
#endif
static void cmd_oem_get_action_nonce(INTN argc, __attribute__((__unused__)) CHAR8 **argv)
{
	char *nonce;

	if (argc != 2) {
		fastboot_fail("Invalid parameter");
		return;
	}

	nonce = authenticated_action_new_nonce((char *)argv[1]);
	if (!nonce) {
		fastboot_fail("Failed to generate new nonce");
		return;
	}

	fastboot_info_long_string(nonce, NULL);
	fastboot_okay("");
}
#endif

static struct fastboot_cmd COMMANDS[] = {
	{ OFF_MODE_CHARGE,		LOCKED,		cmd_oem_off_mode_charge  },
	/* The following commands are not part of the Google
	 * requirements.  They are provided for engineering and
	 * provisioning purpose only.  */
	{ CRASH_EVENT_MENU,		LOCKED,		cmd_oem_crash_event_menu  },
	{ "setvar",			UNLOCKED,	cmd_oem_setvar  },
	{ "garbage-disk",		UNLOCKED,	cmd_oem_garbage_disk  },
	{ "reboot",			LOCKED,		cmd_oem_reboot  },
#ifdef __SUPPORT_ABL_BOOT
	{ "fw-update",			UNLOCKED,	cmd_oem_fw_update  },
#endif
#ifndef USER
	{ "set-storage",		LOCKED,		cmd_oem_set_storage  },
	{ "reprovision",		LOCKED,		cmd_oem_reprovision  },
	{ "rm",				LOCKED,		cmd_oem_rm },
	{ "set-watchdog-counter-max",	LOCKED,		cmd_oem_set_watchdog_counter_max },
	{ SLOT_FALLBACK,		LOCKED,		cmd_oem_disable_slot_fallback },
	{ "erase-efivars",		LOCKED,		cmd_oem_erase_efivars },
#ifdef RPMB_STORAGE
	{ "clear-rpmb",			LOCKED,		cmd_oem_erase_rpmb },
#endif
#endif
	{ "get-hashes",			LOCKED,		cmd_oem_gethashes  },
	{ "get-provisioning-logs",	LOCKED,		cmd_oem_get_logs },
#ifdef BOOTLOADER_POLICY
	{ "get-action-nonce",		LOCKED,		cmd_oem_get_action_nonce }
#endif
};

static struct fastboot_cmd oem = { "oem", LOCKED, cmd_oem };

EFI_STATUS fastboot_oem_init(void)
{
	EFI_STATUS ret;
	UINTN i;

	ret = fastboot_oem_publish();
	if (EFI_ERROR(ret))
		return ret;

	for (i = 0; i < ARRAY_SIZE(COMMANDS); i++) {
		ret = fastboot_register_into(&cmdlist, &COMMANDS[i]);
		if (EFI_ERROR(ret))
			return ret;
	}

	fastboot_register(&oem);

	return EFI_SUCCESS;
}

void fastboot_oem_free()
{
	fastboot_cmdlist_unregister(&cmdlist);
}
