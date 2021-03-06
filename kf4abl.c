/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Authors: Jeremy Compostella <jeremy.compostella@intel.com>
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

#include <efi.h>
#include <efiapi.h>
#include <lib.h>
#include <fastboot.h>
#include <vars.h>
#ifdef CRASHMODE_USE_ADB
#include <adb.h>
#endif

#include "options.h"
#if defined(IOC_USE_SLCAN) || defined(IOC_USE_CBC)
#include "ioc_can.h"
#endif
#include "android.h"
#include "slot.h"
#include "timer.h"
#ifdef USE_AVB
#include "avb_init.h"
#include "libavb/libavb.h"
#include "libavb/uefi_avb_ops.h"
#endif
#include "security.h"
#include <libtipc.h>
#ifdef RPMB_STORAGE
#include "rpmb.h"
#include "rpmb_storage.h"
#endif
#ifdef USE_TRUSTY
#include "trusty.h"

#include <hecisupport.h>
#include <openssl/hkdf.h>

#define TRUSTY_PARAM_STRING          "trusty.param_addr="
#define BOOTLOADER_SEED_MAX_ENTRIES  4
#define MMC_PROD_NAME_WITH_PSN_LEN   15
#define LENGTH_TRUSTY_PARAM_STRING   18
#define TRUSTY_SEED_LEN              32

/* structure of seed info */
typedef struct _seed_info {
	uint8_t svn;
	uint8_t padding[3];
	uint8_t seed[TRUSTY_SEED_LEN];
}__attribute__((packed)) seed_info_t;

typedef struct {
	/* version of the struct. 0x0001 for this version */
	uint16_t 			Version;
	/* Trusty’s mem base address */
	uint32_t 			TrustyMemBase;
	/* assumed to be 16MB */
	uint32_t 			TrustyMemSize;
	/* seed value retrieved from CSE */
	uint32_t			num_seeds;
	seed_info_t 		seed_list[BOOTLOADER_SEED_MAX_ENTRIES];
	struct rot_data_t 	RotData;
} __attribute__((packed)) trusty_boot_params_t;

typedef struct trusty_startup_params {
	/* Size of this structure */
	uint64_t size_of_this_struct;
	/* Load time base address of trusty */
	uint32_t load_base;
	/* Load time size of trusty */
	uint32_t load_size;
	/* Seed */
	uint32_t num_seeds;
	seed_info_t seed_list[BOOTLOADER_SEED_MAX_ENTRIES];
	/* Rot */
	struct rot_data_t RotData;
	/* Concatenation of mmc product name with a string representation of PSN */
	char serial[MMC_PROD_NAME_WITH_PSN_LEN];
}__attribute__((packed)) trusty_startup_params_t;

static trusty_boot_params_t *p_trusty_boot_params;
static UINT8 out_key[BOOTLOADER_SEED_MAX_ENTRIES][RPMB_KEY_SIZE] = {{0}, {0}, {0}, {0}};
#endif
typedef union {
	uint32_t raw;
	struct {
		uint32_t patch_M:4;
		uint32_t patch_Y:7;
		uint32_t version_C:7;
		uint32_t version_B:7;
		uint32_t version_A:7;
	};
} os_version_t;

#define MAX_CMD_BUF 0x1000
static CHAR8 cmd_buf[MAX_CMD_BUF];
struct rot_data_t g_rot_data = {0};

#ifdef CRASHMODE_USE_ADB
static EFI_STATUS enter_crashmode(enum boot_target *target)
{
	EFI_STATUS ret;

#ifdef USER
#error "adb in crashmode MUST be disabled on a USER build"
#endif

	ret = adb_init();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to initialize adb");
		return ret;
	}

	debug(L"adb implementation is initialized");
	for (;;) {
		ret = adb_run();
		if (EFI_ERROR(ret))
			break;

		*target = adb_get_boot_target();
		if (*target != UNKNOWN_TARGET)
			break;
	}
	adb_exit();

	return ret;
}
#endif
#ifndef __FORCE_FASTBOOT
static enum boot_target check_bcb(CHAR16 **target_path, BOOLEAN *oneshot)
{
	EFI_STATUS ret;
	struct bootloader_message bcb;
	CHAR16 *target = NULL;
	enum boot_target t;
	CHAR8 *bcb_cmd;
	BOOLEAN dirty;

	*oneshot = FALSE;
	*target_path = NULL;

	ret = read_bcb(MISC_LABEL, &bcb);
	if (EFI_ERROR(ret)) {
		error(L"Unable to read BCB");
		t = NORMAL_BOOT;
		goto out;
	}

	dirty = bcb.status[0] != '\0';
	/* We own the status field; clear it in case there is any stale data */
	bcb.status[0] = '\0';
	bcb_cmd = (CHAR8 *)bcb.command;
	if (!strncmpa(bcb_cmd, (CHAR8 *)"boot-", 5)) {
		target = stra_to_str(bcb_cmd + 5);
		debug(L"BCB boot target: '%s'", target);
	} else if (!strncmpa(bcb_cmd, (CHAR8 *)"bootonce-", 9)) {
		target = stra_to_str(bcb_cmd + 9);
		bcb_cmd[0] = '\0';
		dirty = TRUE;
		debug(L"BCB oneshot boot target: '%s'", target);
		*oneshot = TRUE;
	}

	if (dirty) {
		ret = write_bcb(MISC_LABEL, &bcb);
		if (EFI_ERROR(ret))
			error(L"Unable to update BCB contents!");
	}

	if (!target) {
		t = NORMAL_BOOT;
		goto out;
	}

	t = name_to_boot_target(target);
	if (t != UNKNOWN_TARGET)
		goto out;

	error(L"Unknown boot target in BCB: '%s'", target);
	t = NORMAL_BOOT;

out:
	FreePool(target);
	return t;
}
#endif

static EFI_STATUS process_bootimage(void *bootimage, UINTN imagesize)
{
	EFI_STATUS ret;

	if (bootimage) {
		/* 'fastboot boot' case, only allowed on unlocked devices.*/
		if (device_is_unlocked()) {
			UINT32 crc;

			ret = uefi_call_wrapper(BS->CalculateCrc32, 3, bootimage, imagesize, &crc);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"CalculateCrc32 failed");
				return ret;
			}

			ret = android_image_start_buffer_abl(bootimage,
								NORMAL_BOOT, BOOT_STATE_GREEN, NULL,
								NULL, (const CHAR8 *)cmd_buf);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"Couldn't load Boot image");
				return ret;
			}
		}
	}

	return EFI_SUCCESS;
}

static EFI_STATUS enter_fastboot_mode(enum boot_target *target)
{
	EFI_STATUS ret;
	void *efiimage, *bootimage;
	UINTN imagesize;

#if defined(IOC_USE_SLCAN) || defined(IOC_USE_CBC)
	ret = notify_ioc_ready();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"notify ioc ready failed");
	}
#endif

	for (;;) {
		*target = UNKNOWN_TARGET;
		bootimage = NULL;
		efiimage = NULL;

		ret = fastboot_start(&bootimage, &efiimage, &imagesize, target);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Fastboot mode failed");
			break;
		}

		ret = process_bootimage(bootimage, imagesize);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Process bootimage failed");
			if (bootimage) {
				FreePool(bootimage);
				bootimage = NULL;
			}
			break;
		}

		if (*target == UNKNOWN_TARGET)
			continue;

		if ((*target == NORMAL_BOOT) || (*target == FASTBOOT))
			reboot_to_target(*target, EfiResetCold);
		break;
	}

	return ret;
}

/*
 *  Boot mode field definitions.
 */
static union bootMode
{
	UINT16 _bits;
	struct {
		UINT16 target           : 5; /* [4:0] */
		UINT16 do_mrc_training  : 1; /* [5] */
		UINT16 do_save_mrc_data : 1; /* [6] */
		UINT16 do_flash_update  : 1; /* [7] */
		UINT16 silent           : 1; /* [8] */
		UINT16 _reserved        : 1; /* [9] */
		UINT16 action           : 2; /* [11:10] 0:boot,1:CLI,2:halt,3:reset */
		UINT16 dipsw            : 4; /* [15:12] */
	};
} bootMode;

static enum boot_target check_command_line(EFI_HANDLE image, CHAR8 *cmd_buf, UINTN max_cmd_size)
{
	EFI_STATUS ret;
	enum boot_target target = FASTBOOT;
	static EFI_LOADED_IMAGE *limg;
	UINTN argc, i;
	CHAR16 **argv;
	UINTN cmd_len = 0;
	CHAR8 arg8[256] = "";
	UINTN arglen;
	CHAR8 *secureboot_str = (CHAR8 *)"ABL.secureboot=";
	UINTN secureboot_str_len;
	CHAR8 *bootmode_info_str = (CHAR8 *)"ABL.boot=";
	UINTN bootmode_info_str_len;
	CHAR8 *boot_target_str = (CHAR8 *)"ABL.boot_target=";
	UINTN boot_target_str_len;
	CHAR8 *nptr = NULL;

	ret = uefi_call_wrapper(BS->OpenProtocol, 6, image,
				&LoadedImageProtocol, (VOID **)&limg,
				image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to open LoadedImageProtocol");
		return FASTBOOT;
	}

	ret = get_argv(limg, &argc, &argv);
	if (EFI_ERROR(ret))
		return FASTBOOT;

#ifdef KERNELFLINGER_BUILD_FOR_SLIMBOOT
	if(argc <= 0)
	{
		efi_snprintf(cmd_buf, max_cmd_size + 1,
				(CHAR8 *)"androidboot.serialno=%a" \
				" androidboot.bootreason=not_applicable" \
				" pci=nocrs" \
				" nowatchdog" \
				" androidboot.bootloader=slimboot_android_payload-07_03-userdebug" \
				" gpt", get_serial_number());

		set_abl_secure_boot(0);
		log(L"KERNELFLINGER_BUILD_FOR_SLIMBOOT: argc == %d, default parameters added !\n", argc);
		return NORMAL_BOOT;
	}
#endif

	cmd_buf[0] = 0;
	secureboot_str_len = strlen((CHAR8 *)secureboot_str);
	bootmode_info_str_len = strlen((CHAR8 *)bootmode_info_str);
	boot_target_str_len = strlen((CHAR8 *)boot_target_str);

	/*Parse boot target*/
	for (i = 0; i < argc; i++) {
		debug(L" abl cmd %02d: %s", i, argv[i]);
		arglen = StrLen(argv[i]);
		if (arglen > (int)sizeof(arg8) - 2)
			arglen = sizeof(arg8) - 2;
		str_to_stra((CHAR8 *)arg8, argv[i], arglen + 1);
		if (cmd_len + arglen + 1 < max_cmd_size) {
			if (cmd_buf[0] != 0) {
				strncpy((CHAR8 *)(cmd_buf + cmd_len), (const CHAR8 *)" ", 1);
				cmd_len++;
			}

			/* Parse "ABL.boot_target=xxxx" */
			if ((arglen > boot_target_str_len) &&
				!strncmp(arg8, boot_target_str, boot_target_str_len)) {
				nptr = (CHAR8 *)(arg8 + boot_target_str_len);
				/* Only handle CRASHMODE case, other mode should be decided by "ABL.boot". */
				if (!strcmp(nptr, (CHAR8 *)"CRASHMODE")) {
					target = CRASHMODE;
					break;
				} else {
					continue;
				}
			} else
			/* Parse "ABL.boot=xx" */
			if ((arglen > bootmode_info_str_len) &&
				(!strncmp(arg8, bootmode_info_str, bootmode_info_str_len))) {
				nptr = (CHAR8 *)(arg8 + bootmode_info_str_len);
				bootMode._bits = (UINT16)strtoul((char *)nptr, 0, 16);
				target = bootMode.target;
			} else
#ifdef USE_TRUSTY
			/* Parse "trusty.param_addr=xxxxx" */
			if ((arglen > LENGTH_TRUSTY_PARAM_STRING) &&
			    (!strncmp(arg8, (CHAR8 *)TRUSTY_PARAM_STRING, LENGTH_TRUSTY_PARAM_STRING))) {
				UINT32 num;
				nptr = (CHAR8 *)(arg8 + LENGTH_TRUSTY_PARAM_STRING);
				num = strtoul((char *)nptr, 0, 16);
				debug(L"Parsed trusty param addr is 0x%x", num);
				p_trusty_boot_params = (trusty_boot_params_t *)num;
				continue;
			} else
#endif
			/* Parse "ABL.secureboot=x" */
			if ((arglen > secureboot_str_len) && (!strncmp(arg8, (CHAR8 *)secureboot_str, secureboot_str_len))) {
				UINT8 val;
				nptr = (CHAR8 *)(arg8 + secureboot_str_len);
				val = (UINT8)strtoul((char *)nptr, 0, 10);
				ret = set_abl_secure_boot(val);
				if (EFI_ERROR(ret))
					efi_perror(ret, L"Failed to set secure boot");
			}

			strncpy((CHAR8 *)(cmd_buf + cmd_len), (const CHAR8 *)arg8, arglen);
			cmd_len += arglen;
		}
	}

	debug(L"boot target: %d", target);
	FreePool(argv);
	return target;
}

#ifndef USE_AVB
/* Load a boot image into RAM.
 *
 * boot_target  - Boot image to load. Values supported are NORMAL_BOOT, RECOVERY,
 *                and ESP_BOOTIMAGE (for 'fastboot boot')
 * target_path  - Path to load boot image from for ESP_BOOTIMAGE case, ignored
 *                otherwise.
 * bootimage    - Returned allocated pointer value for the loaded boot image.
 * oneshot      - For ESP_BOOTIMAGE case, flag indicating that the image should
 *                be deleted.
 *
 * Return values:
 * EFI_INVALID_PARAMETER - Unsupported boot target type, key is not well-formed,
 *                         or loaded boot image was missing or corrupt
 * EFI_ACCESS_DENIED     - Validation failed against OEM or embedded certificate,
 *                         boot image still usable
 */
static EFI_STATUS load_boot_image(
				IN enum boot_target boot_target,
				IN CHAR16 *target_path,
				OUT VOID **bootimage,
				IN BOOLEAN oneshot)
{
	EFI_STATUS ret;

	switch (boot_target) {
	case NORMAL_BOOT:
		ret = EFI_NOT_FOUND;
		if (use_slot() && !slot_get_active())
			break;
		do {
			const CHAR16 *label = slot_label(BOOT_LABEL);
			ret = android_image_load_partition(label, bootimage);
			if (EFI_ERROR(ret)) {
				efi_perror(ret, L"Failed to load boot image from %s partition",
						label);
				if (use_slot())
					slot_boot_failed(boot_target);
			}
		} while (EFI_ERROR(ret) && slot_get_active());
		break;

	case RECOVERY:
		if (recovery_in_boot_partition()) {
			ret = load_boot_image(NORMAL_BOOT, target_path, bootimage, oneshot);
			break;
		}
		if (use_slot() && !slot_recovery_tries_remaining()) {
			ret = EFI_NOT_FOUND;
			break;
		}
		ret = android_image_load_partition(RECOVERY_LABEL, bootimage);
		break;
	default:
		*bootimage = NULL;
		return EFI_INVALID_PARAMETER;
	}

	if (!EFI_ERROR(ret))
		debug(L"boot image loaded");

	return ret;
}
#endif


#ifdef USE_AVB
static EFI_STATUS start_boot_image(VOID *bootimage, UINT8 boot_state,
				enum boot_target boot_target,
				AvbSlotVerifyData *slot_data,
				CHAR8 *abl_cmd_line)
#else
static EFI_STATUS start_boot_image(VOID *bootimage, UINT8 boot_state,
				enum boot_target boot_target,
				X509 *verifier_cert,
				CHAR8 *abl_cmd_line)
#endif
{
	EFI_STATUS ret;
#ifdef USER
	/* per bootloaderequirements.pdf */
	if (boot_state == BOOT_STATE_ORANGE) {
		ret = android_clear_memory();
		if (EFI_ERROR(ret)) {
			error(L"Failed to clear memory. Load image aborted.");
			return ret;
		}
	}
#endif

#ifdef USER
	if (boot_state == BOOT_STATE_RED) {
		if (is_abl_secure_boot_enabled()) {
			return EFI_SECURITY_VIOLATION;
		}
	}
#endif

	set_efi_variable(&fastboot_guid, BOOT_STATE_VAR, sizeof(boot_state),
					&boot_state, FALSE, TRUE);

#ifdef OS_SECURE_BOOT
	ret = set_os_secure_boot(boot_state == BOOT_STATE_GREEN);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to set os secure boot");
#endif

#ifndef USE_SLOT
	ret = slot_boot(boot_target);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to write slot boot");
		return ret;
	}
#endif
	log(L"chainloading boot image, boot state is %s\n",
	boot_state_to_string(boot_state));
#ifdef USE_AVB
	ret = android_image_start_buffer_abl(bootimage,
						boot_target, boot_state, NULL,
						slot_data, (const CHAR8 *)abl_cmd_line);
#else
	ret = android_image_start_buffer_abl(bootimage,
						boot_target, boot_state, NULL,
						verifier_cert, (const CHAR8 *)abl_cmd_line);
#endif
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Couldn't load Boot image");

	ret = slot_boot_failed(boot_target);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to write slot failure");

	return ret;
}

#ifdef USE_TRUSTY
static EFI_STATUS init_trusty_startup_params(trusty_startup_params_t *param, UINTN base, UINTN sz, uint32_t num, seed_info_t *seed_list)
{
	char *serialno;

	if (!param || !seed_list || num > BOOTLOADER_SEED_MAX_ENTRIES || num == 0)
		return EFI_INVALID_PARAMETER;

	memset(param, 0, sizeof(trusty_startup_params_t));
	param->size_of_this_struct = sizeof(trusty_startup_params_t);
	param->load_base = base;
	param->load_size = sz;
	param->num_seeds = num;
	serialno = get_serial_number();
	if (!serialno)
		return EFI_NOT_FOUND;

	memcpy(param->serial, serialno, MMC_PROD_NAME_WITH_PSN_LEN);
	memcpy(param->seed_list, seed_list, sizeof(param->seed_list));

	memset(seed_list, 0, sizeof(param->seed_list));

	return EFI_SUCCESS;
}

#define TRUSTY_VMCALL_SMC 0x74727500
static EFI_STATUS launch_trusty_os(trusty_startup_params_t *param)
{
	if (!param)
		return EFI_INVALID_PARAMETER;

	asm volatile(
		"vmcall; \n"
		: : "a"(TRUSTY_VMCALL_SMC), "D"((uint32_t)param));

	return EFI_SUCCESS;
}
#endif

#ifndef USE_AVB
/* Validate an image.
 *
 * Parameters:
 * boot_target    - Boot image to load. Values supported are NORMAL_BOOT,
 *                  RECOVERY, and ESP_BOOTIMAGE (for 'fastboot boot')
 * bootimage      - Bootimage to validate
 * verifier_cert  - Return the certificate that validated the boot image
 *
 * Return values:
 * BOOT_STATE_GREEN  - Boot image is valid against provided certificate
 * BOOT_STATE_YELLOW - Boot image is valid against embedded certificate
 * BOOT_STATE_RED    - Boot image is not valid
 */
static UINT8 validate_bootimage(
		IN enum boot_target boot_target,
		IN VOID *bootimage,
		OUT X509 **verifier_cert)
{
	CHAR16 target[BOOT_TARGET_SIZE];
	CHAR16 *expected;
	CHAR16 *expected2 = NULL;
	UINT8 boot_state;

	boot_state = verify_android_boot_image(bootimage, oem_cert,
						oem_cert_size, target,
						verifier_cert);

	if (boot_state == BOOT_STATE_RED) {
		error(L"boot image doesn't verify");
		return boot_state;
	}

	switch (boot_target) {
	case NORMAL_BOOT:
		expected = L"/boot";
		/* in case of multistage ota */
		expected2 = L"/recovery";
		break;
	case RECOVERY:
		if (recovery_in_boot_partition())
			expected = L"/boot";
		else
			expected = L"/recovery";
		break;
	default:
		expected = NULL;
	}

	if ((!expected || StrCmp(expected, target)) &&
		(!expected2 || StrCmp(expected2, target))) {
		error(L"boot image has unexpected target name");
		return BOOT_STATE_RED;
	}

	return boot_state;
}
#endif

#ifdef USE_TRUSTY
/* HWCRYPTO Server App UUID */
const EFI_GUID  crypo_uuid = { 0x23fe5938, 0xccd5, 0x4a78,
	{ 0x8b, 0xaf, 0x0f, 0x3d, 0x05, 0xff, 0xc2, 0xdf } };

static EFI_STATUS derive_rpmb_key_with_index(UINT8 index, VOID *kbuf)
{
	EFI_STATUS ret;
	UINT8 rpmb_key[RPMB_KEY_SIZE] = {0};
	UINT8 serial[MMC_PROD_NAME_WITH_PSN_LEN] = {0};
	char *serialno;

	serialno = get_serial_number();

	if (!serialno)
		return EFI_NOT_FOUND;

	/* Clear Byte 2 and 0 for CID[6] PRV and CID[0] CRC for eMMC Field Firmware Updates
	 * serial[0] = cid[0];	-- CRC
	 * serial[2] = cid[6];	-- PRV
	 */
	memcpy(serial, serialno, sizeof(serial));
	serial[0] ^= serial[0];
	serial[2] ^= serial[2];

	if (index < p_trusty_boot_params->num_seeds) {
		if (!HKDF(rpmb_key, sizeof(rpmb_key), EVP_sha256(),
			  (const uint8_t *)p_trusty_boot_params->seed_list[index].seed, RPMB_KEY_SIZE,
			  (const uint8_t *)&crypo_uuid, sizeof(EFI_GUID),
			  (const uint8_t *)serial, sizeof(serial))) {
			error(L"HDKF failed \n");
			ret = EFI_INVALID_PARAMETER;
			goto out;
		}

		memcpy(kbuf, rpmb_key, RPMB_KEY_SIZE);
	}

	ret = EFI_SUCCESS;

out:
	memset(rpmb_key, 0, sizeof(rpmb_key));
	return ret;
}

static EFI_STATUS get_rpmb_derived_key(VOID *kbuf, size_t kbuf_len)
{
	UINT32 i;

	if (kbuf_len < p_trusty_boot_params->num_seeds * RPMB_KEY_SIZE)
		return EFI_INVALID_PARAMETER;

	for (i = 0; i < p_trusty_boot_params->num_seeds; i++) {
		if (EFI_SUCCESS != derive_rpmb_key_with_index(i, kbuf + i * RPMB_KEY_SIZE)) {
			memset(kbuf + i * RPMB_KEY_SIZE, 0, RPMB_KEY_SIZE);
			return EFI_INVALID_PARAMETER;
		}
	}

	return EFI_SUCCESS;
}
#endif

#ifdef RPMB_STORAGE
EFI_STATUS  osloader_rpmb_key_init(VOID)
{
	EFI_STATUS ret;
	UINT8 key[RPMB_KEY_SIZE] = {0};
	ret = EFI_SUCCESS;

#ifdef USE_TRUSTY
	UINT16 i;
	RPMB_RESPONSE_RESULT result;

	if (is_eom_and_secureboot_enabled()) {
		ret = clear_teedata_flag();
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Clear teedata flag failed");
			return ret;
		}
	}

	ret = get_rpmb_derived_key(out_key, sizeof(out_key));
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Get RPMB derived key failed");
		return ret;
	}

	for (i = 0; i < p_trusty_boot_params->num_seeds; i++) {
		memcpy(key, out_key[i], RPMB_KEY_SIZE);
		ret = rpmb_read_counter(key, &result);
		if (ret == EFI_SUCCESS)
			break;

		if (result == RPMB_RES_NO_AUTH_KEY_PROGRAM) {
			efi_perror(ret, L"key is not programmed, use the first seed to derive keys.");
			break;
		}

		if (result != RPMB_RES_AUTH_FAILURE) {
			efi_perror(ret, L"rpmb_read_counter unexpected error: %d.", result);
			goto err_get_rpmb_key;
		}
	}

	if (i >= BOOTLOADER_SEED_MAX_ENTRIES) {
		error(L"All keys are not match!");
		goto err_get_rpmb_key;
	}

	if (i != 0)
		error(L"seed changed to %d ", i);
#endif

	if (!is_rpmb_programed()) {
		debug(L"rpmb not programmed");
		ret = program_rpmb_key(key);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"rpmb key program failed");
			return ret;
		}
	} else {
		debug(L"rpmb already programmed");
		set_rpmb_key(key);
	}

#ifdef USE_TRUSTY
err_get_rpmb_key:
	memset(out_key, 0, sizeof(out_key));
	memset(key, 0, sizeof(key));
#endif

	return ret;
}
#endif

#ifdef USE_AVB
EFI_STATUS avb_boot_android(enum boot_target boot_target, CHAR8 *abl_cmd_line)
{
	AvbOps *ops;
	const char *slot_suffix = "";
	AvbPartitionData *boot;
	AvbSlotVerifyData *slot_data = NULL;
	AvbSlotVerifyResult verify_result;
	const char *requested_partitions[] = {"boot", NULL};
	EFI_STATUS ret;
	VOID *bootimage = NULL;
	UINT8 boot_state = BOOT_STATE_GREEN;
	bool allow_verification_error = FALSE;
	AvbSlotVerifyFlags flags;
#ifdef USE_TRUSTY
	const struct boot_img_hdr *header;
	AvbSlotVerifyData *slot_data_tos = NULL;
	UINT8 tos_state = BOOT_STATE_GREEN;
	const uint8_t *vbmeta_pub_key;
	uint32_t vbmeta_pub_key_len;
	UINTN load_base;
	AvbPartitionData *tos;
	trusty_startup_params_t trusty_startup_params;
#endif

	debug(L"Loading boot image");
#ifndef USE_SLOT
	if (boot_target == RECOVERY) {
		requested_partitions[0] = "recovery";
	}
#endif
	ops = avb_init();
	if (ops) {
		if (ops->read_is_device_unlocked(ops, &allow_verification_error) != AVB_IO_RESULT_OK) {
			avb_fatal("Error determining whether device is unlocked.\n");
			return EFI_ABORTED;
		}
	} else {
		return EFI_OUT_OF_RESOURCES;
	}

#ifdef USE_SLOT
	slot_suffix = slot_get_active();
	if (!slot_suffix) {
		error(L"suffix is null");
		slot_suffix = "";
	}
#endif

	flags = AVB_SLOT_VERIFY_FLAGS_NONE;
	if (allow_verification_error) {
		flags |= AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR;
	}
	verify_result = avb_slot_verify(ops,
					requested_partitions,
					slot_suffix,
					flags,
					AVB_HASHTREE_ERROR_MODE_RESTART,
					&slot_data);

	ret = get_avb_result(slot_data,
			    allow_verification_error,
			    verify_result,
			    &boot_state);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get avb result for boot");
		goto fail;
	}

	boot = &slot_data->loaded_partitions[0];
	bootimage = boot->data;

#ifdef USE_TRUSTY
	if (boot_target == NORMAL_BOOT) {
		requested_partitions[0] = "tos";
		verify_result = avb_slot_verify(ops,
					requested_partitions,
					slot_suffix,
					flags,
					AVB_HASHTREE_ERROR_MODE_RESTART,
					&slot_data_tos);

		ret = get_avb_result(slot_data_tos,
				    false,
				    verify_result,
				    &tos_state);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to get avb result for tos");
			goto fail;
		} else if ((tos_state != BOOT_STATE_GREEN) && is_abl_secure_boot_enabled()) {
			ret = EFI_ABORTED;
			goto fail;
		}


		tos = &slot_data_tos->loaded_partitions[0];
		header = (const struct boot_img_hdr *)tos->data;
		load_base = (UINTN)(tos->data + header->page_size);
		ret = init_trusty_startup_params(&trusty_startup_params, load_base,
				header->kernel_size, p_trusty_boot_params->num_seeds, p_trusty_boot_params->seed_list);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to init trusty startup params");
			goto fail;
		}

		ret = launch_trusty_os(&trusty_startup_params);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to launch trusty os");
			goto fail;
		}

		if (slot_data_tos) {
			avb_slot_verify_data_free(slot_data_tos);
			slot_data_tos = NULL;
		}

		ret = avb_vbmeta_image_verify(slot_data->vbmeta_images[0].vbmeta_data,
				slot_data->vbmeta_images[0].vbmeta_size,
				&vbmeta_pub_key,
				&vbmeta_pub_key_len);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to get the vbmeta_pub_key");
			goto fail;
		}

		ret = get_rot_data(bootimage, boot_state, vbmeta_pub_key, vbmeta_pub_key_len, &g_rot_data);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to init trusty rot params");
			goto fail;
		}

		trusty_ipc_init();
		trusty_ipc_shutdown();

		// Send EOP heci messages
		ret = heci_end_of_post();
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to send EOP message to CSE FW, halt");
			goto fail;
		}
	}
#endif

	if (boot_state == BOOT_STATE_GREEN) {
		avb_update_stored_rollback_indexes_for_slot(ops, slot_data);
	}

	ret = start_boot_image(bootimage, boot_state, boot_target, slot_data, abl_cmd_line);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to start boot image");
		goto fail;
	}

fail:
#ifdef USE_TRUSTY
	if (slot_data_tos) {
		avb_slot_verify_data_free(slot_data_tos);
		slot_data_tos = NULL;
	}
	memset(trusty_startup_params.seed_list, 0, sizeof(trusty_startup_params.seed_list));
#endif

	return ret;
}
#endif

#ifndef USE_AVB
EFI_STATUS boot_android(enum boot_target boot_target, CHAR8 *abl_cmd_line)
{
	EFI_STATUS ret;
	CHAR16 *target_path = NULL;
	VOID *bootimage = NULL;
	BOOLEAN oneshot = FALSE;
	UINT8 boot_state = BOOT_STATE_GREEN;
	X509 *verifier_cert = NULL;
#ifdef USE_TRUSTY
	VOID *tosimage = NULL;
	UINTN load_base;
	struct boot_img_hdr *hdr;
	trusty_startup_params_t trusty_startup_params;
#endif

	debug(L"Loading boot image");
	ret = load_boot_image(boot_target, target_path, &bootimage, oneshot);
	FreePool(target_path);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to load boot image");
		return ret;
	}
	boot_state = validate_bootimage(boot_target, bootimage, &verifier_cert);
#ifdef USE_TRUSTY
	if (boot_target == NORMAL_BOOT) {
		ret = load_tos_image(&tosimage);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to load trusty image");
			return ret;
		}

		hdr = get_bootimage_header(tosimage);
		if (!hdr)
			return EFI_INVALID_PARAMETER;

		load_base = (UINTN)((UINT8 *)tosimage + hdr->page_size);

		ret = init_trusty_startup_params(&trusty_startup_params, load_base,
				hdr->kernel_size, p_trusty_boot_params->num_seeds, p_trusty_boot_params->seed_list);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to init trusty startup params");
			goto exit;
		}

		/*  keymaster interface always use the g_rot_data as its input param */
		ret = get_rot_data(bootimage, boot_state, verifier_cert, &g_rot_data);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to init trusty rot params");
			goto exit;
		}

		ret = launch_trusty_os(&trusty_startup_params);
		if (tosimage)
			FreePool(tosimage);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to launch trusty os");
			goto exit;
		}

		trusty_ipc_init();
		trusty_ipc_shutdown();

		// Send EOP heci messages
		ret = heci_end_of_post();
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to send EOP message to CSE FW, halt");
			goto exit;
		}
	}
#endif

	ret = start_boot_image(bootimage, boot_state, boot_target, verifier_cert, abl_cmd_line);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to start boot image");
		goto exit;
	}

	ret = EFI_INVALID_PARAMETER;
exit:
#ifdef USE_TRUSTY
	memset(trusty_startup_params.seed_list, 0, sizeof(trusty_startup_params.seed_list));
#endif
	return ret;
}
#endif

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table)
{
	enum boot_target target;
	EFI_STATUS ret;

#ifndef __FORCE_FASTBOOT
	BOOLEAN oneshot = FALSE;
	CHAR16 *target_path = NULL;
	enum boot_target bcb_target;
#endif

	set_boottime_stamp(TM_EFI_MAIN);
	InitializeLib(image, sys_table);
	target = check_command_line(image, cmd_buf, sizeof(cmd_buf) - 1);

#ifdef RPMB_STORAGE
	emmc_rpmb_init(NULL);
	rpmb_storage_init(is_eom_and_secureboot_enabled());
#endif

	ret = slot_init();
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Slot management initialization failed");
		return ret;
	}

#ifdef __FORCE_FASTBOOT
	target = FASTBOOT;
#endif

#ifndef __FORCE_FASTBOOT
	debug(L"Before Check BCB target is %d", target);
	bcb_target = check_bcb(&target_path, &oneshot);
	debug(L"BCB target is %d", bcb_target);
	if (bcb_target == RECOVERY)
		target = bcb_target;
	debug(L"After Check BCB target is %d", target);
#endif

	debug(L"target=%d", target);

#ifdef RPMB_STORAGE
	if (target != CRASHMODE) {
		ret = osloader_rpmb_key_init();
		if (EFI_ERROR(ret))
			error(L"rpmb key init failure for osloader");
	}
#endif

	for (;;) {
		switch (target) {
		case NORMAL_BOOT:
		case RECOVERY:
#ifdef USE_AVB
			ret = avb_boot_android(target, cmd_buf);
#else
			ret = boot_android(target, cmd_buf);
#endif
			if (EFI_ERROR(ret))
				target = FASTBOOT;
			break;
		case UNKNOWN_TARGET:
#ifndef CRASHMODE_USE_ADB
		case CRASHMODE:
#endif
		case FASTBOOT:
			enter_fastboot_mode(&target);
			break;
#ifdef CRASHMODE_USE_ADB
		case CRASHMODE:
			enter_crashmode(&target);
			break;
#endif
		default:
			reboot_to_target(target, EfiResetCold);
		}
	}

	return EFI_SUCCESS;
}
