#include <common.h>
#include <command.h>
#include <asm/sizes.h>

#include <fastboot.h>
#include <malloc.h>
#include <../board/rockchip/common/config.h>
#include <../board/rockchip/common/storage.h>

extern int rk_bootm_start(bootm_headers_t *images);
extern int do_bootm_linux(int flag, int argc, char *argv[],
		        bootm_headers_t *images);

DECLARE_GLOBAL_DATA_PTR;

/* Section for Android bootimage format support
 * Refer:
 * http://android.git.kernel.org/?p=platform/system/core.git;a=blob;f=mkbootimg/bootimg.h
 */
static void bootimg_print_image_hdr(struct fastboot_boot_img_hdr *hdr)
{
#ifdef DEBUG
	int i;
	printf("   Image magic:   %s\n", hdr->magic);

	printf("   kernel_size:   0x%x\n", hdr->kernel_size);
	printf("   kernel_addr:   0x%x\n", hdr->kernel_addr);

	printf("   rdisk_size:   0x%x\n", hdr->ramdisk_size);
	printf("   rdisk_addr:   0x%x\n", hdr->ramdisk_addr);

	printf("   second_size:   0x%x\n", hdr->second_size);
	printf("   second_addr:   0x%x\n", hdr->second_addr);

	printf("   tags_addr:   0x%x\n", hdr->tags_addr);
	printf("   page_size:   0x%x\n", hdr->page_size);

	printf("   name:      %s\n", hdr->name);
	printf("   cmdline:   %s\n", hdr->cmdline);

	for (i = 0; i < 8; i++)
		printf("   id[%d]:   0x%x\n", i, hdr->id[i]);
#endif
}

#ifdef CONFIG_ROCKCHIP
extern int loadRkImage(struct fastboot_boot_img_hdr *hdr,
		const disk_partition_t *boot_ptn, const disk_partition_t *kernel_ptn);
#endif

/* booti [ <addr> | <partition> ] */
int do_booti(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	char *boot_source = "boot";
	struct fastboot_boot_img_hdr *hdr = NULL;
	const disk_partition_t* ptn;
	bootm_headers_t images;

	bool charge = false;
	if (argc >= 2) {
		if (!strcmp(argv[1], "charge")) {
			charge = true;
		} else {
			boot_source = argv[1];
		}
	}

	void *kaddr, *raddr;
	kaddr = (void*)gd->bd->bi_dram[0].start +  CONFIG_KERNEL_LOAD_ADDR;
#ifdef CONFIG_CMD_FASTBOOT
	raddr = (void*)(uint32)gd->arch.fastboot_buf_addr;
#else
	//TODO:find a place to load ramdisk.
	raddr = (void*)0;
#endif

#ifdef CONFIG_CMD_FASTBOOT
	ptn = fastboot_find_ptn(boot_source);
#else
	//TODO: find disk_partition_t other way.
#endif
	if (ptn) {
		unsigned long blksz = ptn->blksz;
		unsigned sector;
		unsigned blocks;
		hdr = malloc(blksz << 2);
		if (hdr == NULL) {
			FBTERR("error allocating blksz(%lu) buffer\n", blksz);
			goto fail;
		}
		if (StorageReadLba(ptn->start, (void *) hdr, 1 << 2) != 0) {
			FBTERR("booti: failed to read bootimg header\n");
			goto fail;
		}
		if (memcmp(hdr->magic, FASTBOOT_BOOT_MAGIC,
					FASTBOOT_BOOT_MAGIC_SIZE)) {
#ifdef CONFIG_ROCKCHIP
			memset(hdr, 0, blksz);
			hdr->kernel_addr = (uint32)kaddr;
			hdr->ramdisk_addr = (uint32)raddr;

			snprintf((char*)hdr->magic,
					FASTBOOT_BOOT_MAGIC_SIZE, "%s\n", "RKIMAGE!");
			if (loadRkImage(hdr, ptn, fastboot_find_ptn(KERNEL_NAME)) != 0) {
				FBTERR("booti: bad boot or kernel image\n");
				goto fail;
			}
#else
			FBTERR("booti: bad boot image magic\n");
			goto fail;
#endif
		} else {
			hdr->kernel_addr = (uint32)kaddr;
			hdr->ramdisk_addr = (uint32)raddr;

			sector = ptn->start + (hdr->page_size / blksz);
			blocks = DIV_ROUND_UP(hdr->kernel_size, blksz);
			if (StorageReadLba(sector, (void *) hdr->kernel_addr, \
						blocks) != 0) {
				FBTERR("booti: failed to read kernel\n");
				goto fail;
			}

			sector += ALIGN(hdr->kernel_size, hdr->page_size) / blksz;
			blocks = DIV_ROUND_UP(hdr->ramdisk_size, blksz);
			if (StorageReadLba(sector, (void *) hdr->ramdisk_addr, \
						blocks) != 0) {
				FBTERR("booti: failed to read ramdisk\n");
				goto fail;
			}
		}
	} else {
		unsigned addr;
		char *ep;

		addr = simple_strtoul(boot_source, &ep, 16);
		if (ep == boot_source || *ep != '\0') {
			printf("'%s' does not seem to be a partition nor "
					"an address\n", boot_source);
			/* this is most likely due to having no
			 * partition table in factory case, or could
			 * be argument is wrong.  in either case, start
			 * fastboot mode.
			 */
			goto fail;
		}

		hdr = malloc(sizeof(*hdr));
		if (hdr == NULL) {
			printf("error allocating buffer\n");
			goto fail;
		}

		/* set this aside somewhere safe */
		memcpy(hdr, (void *) addr, sizeof(*hdr));

		if (memcmp(hdr->magic, FASTBOOT_BOOT_MAGIC,
					FASTBOOT_BOOT_MAGIC_SIZE)) {
			printf("booti: bad boot image magic\n");
			goto fail;
		}

		hdr->ramdisk_addr = (int)raddr;
		hdr->kernel_addr = (int)kaddr;
		kaddr = (void *)(addr + hdr->page_size);
		raddr = (void *)(kaddr + ALIGN(hdr->kernel_size,
					hdr->page_size));
		memmove((void *)hdr->kernel_addr, kaddr, hdr->kernel_size);
		memmove((void *)hdr->ramdisk_addr, raddr, hdr->ramdisk_size);
	}

#if defined CONFIG_CMD_FASTBOOT || defined CONFIG_ROCKCHIP
    char* fastboot_unlocked_env = getenv(FASTBOOT_UNLOCKED_ENV_NAME);
	unsigned long unlocked = 0;
	if (fastboot_unlocked_env) {
		if (!strict_strtoul(fastboot_unlocked_env, 10, &unlocked)) {
			unlocked = unlocked? 1 : 0;
		}
	}
	if (board_fbt_boot_check(hdr, unlocked)) {
		FBTERR("booti: board check boot image error\n");
		goto fail;
	}
#endif

	bootimg_print_image_hdr(hdr);

	FBTDBG("kernel   @ %08x (%d)\n", hdr->kernel_addr, hdr->kernel_size);
	FBTDBG("ramdisk  @ %08x (%d)\n", hdr->ramdisk_addr, hdr->ramdisk_size);

#ifdef CONFIG_CMDLINE_TAG
	{
		/* static just to be safe when it comes to the stack */
		static char command_line[1024];
		/* Use the cmdline from board_fbt_finalize_bootargs instead of
		 * any hardcoded into u-boot.  Also, Android wants the
		 * serial number on the command line instead of via
		 * tags so append the serial number to the bootimg header
		 * value and set the bootargs environment variable.
		 * do_bootm_linux() will use the bootargs environment variable
		 * to pass it to the kernel.  Add the bootloader
		 * version too.
		 */

#if defined CONFIG_CMD_FASTBOOT || defined CONFIG_ROCKCHIP
		board_fbt_finalize_bootargs(command_line, sizeof(command_line),
				hdr->ramdisk_addr, hdr->ramdisk_size,
				!strcmp(boot_source, RECOVERY_NAME));
		//printf("board cmdline:\n%s\n", command_line);
#endif

		if (charge)
			snprintf(command_line, sizeof(command_line),
					"%s %s",command_line," androidboot.mode=charger");

		char *sn = getenv("fbt_sn#");
		if (sn != NULL) {
			/* append serial number if it wasn't in device_info already */
			if (!strstr(command_line, FASTBOOT_SERIALNO_BOOTARG)) {
				snprintf(command_line, sizeof(command_line),
						" %s=%s", FASTBOOT_SERIALNO_BOOTARG,
						sn);
			}
		}

		command_line[sizeof(command_line) - 1] = 0;

		setenv("bootargs", command_line);
	}
#endif /* CONFIG_CMDLINE_TAG */

	memset(&images, 0, sizeof(images));
	images.ep = hdr->kernel_addr;
	images.rd_start = hdr->ramdisk_addr;
	images.rd_end = hdr->ramdisk_addr
        + hdr->ramdisk_size;
	free(hdr);

#ifdef CONFIG_CMD_BOOTM
#ifdef CONFIG_ROCKCHIP
	if (rk_bootm_start(&images)/*it returns 1 when failed.*/) {
		puts("booti: failed to boot with fdt!\n");
		goto fail;
	}
#endif
#endif

	puts("booti: do_bootm_linux...\n");
	do_bootm_linux(0, 0, NULL, &images);

fail:
	/* if booti fails, always start fastboot */
	free(hdr); /* hdr may be NULL, but that's ok. */

#if defined CONFIG_CMD_FASTBOOT || defined CONFIG_ROCKCHIP
	board_fbt_boot_failed(boot_source);
#endif

	puts("booti: Control returned to monitor - resetting...\n");
	do_reset(cmdtp, flag, argc, argv);
	return 1;
}

U_BOOT_CMD(
		booti,	2,	1,	do_booti,
		"boot android bootimg",
#ifdef DEBUG
		"[ <addr> | <partition> ]\n    - boot application image\n"
		"\t'addr' should be the address of the boot image which is\n"
		"\tzImage+ramdisk.img if in memory.  'partition' is the name\n"
		"\tof the partition to boot from.  The default is to boot\n"
		"\tfrom the 'boot' partition.\n"
#else
		"\n"
#endif
		);
