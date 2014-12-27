/*
 * HooToo MTD writer for OpenWrt
 * Copyright (C) 2014 Vittorio Gambaletta <openwrt@vittgam.net>
 *
 * This code is based on mtd.c from the HooToo RT5350 GPL tarball.
 *
 * mtd - simple memory technology device manipulation tool
 *
 * Copyright (C) 2005 Waldemar Brodkorb <wbx@dass-it.de>,
 *	                  Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * $Id: mtd.c,v 1.1.6.1 2010-08-03 08:38:31 winfred Exp $
 *
 * The code is based on the linux-mtd examples.
 */

#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

#include "linux/autoconf.h"
#include "mtd.h"

#define BUFSIZE (1 * 1024)

static const char usage_text[] = "Usage: hootoo_mtd_writer <imagefile> <device> <devoffset> <reboot_after>\n\n"
	"Write <imagefile> to <device>, starting at <devoffset> in the device, while verifying it.\n"
	"The device is in the format of its label.\n"
	"Reboot after a successful write if <reboot_after> is 1.\n\n"
	"Example: To write openwrt.bin to mtd0 labeled as ALL at offset 327680 from device start, and reboot afterwards:\n"
	"  hootoo_mtd_writer openwrt.bin ALL 327680 1\n\n";

int main(int argc, char **argv)
{
	int imagefd, devfd = -1, i, ret = 1, reboot_after, devoffset;
	char dev[PATH_MAX];
	FILE *fp = NULL;
	unsigned char *writebuf, *testbuf;
	ssize_t r, w, e, len;
	struct mtd_info_user mtdinfo;
	struct erase_info_user mtderaseinfo;
	struct stat statbuf;

	if (argc != 5) {
		fprintf(stderr, usage_text);
		goto cleanup4;
	}

	devoffset = atoi(argv[3]);
	reboot_after = atoi(argv[4]);

	if ((imagefd = open(argv[1], O_RDONLY)) == -1) {
		fprintf(stderr, "Cannot open image file\n");
		goto cleanup4;
	}

	if (fstat(imagefd, &statbuf) == -1) {
		fprintf(stderr, "Cannot get image file size\n");
		goto cleanup3;
	}
	len = statbuf.st_size;

	if ((fp = fopen("/proc/mtd", "r"))) {
		while (fgets(dev, sizeof(dev), fp)) {
			if (sscanf(dev, "mtd%d:", &i) && strstr(dev, argv[2])) {
				snprintf(dev, sizeof(dev), "/dev/mtd/%d", i);
				if ((devfd = open(dev, O_RDWR | O_SYNC)) == -1) {
					snprintf(dev, sizeof(dev), "/dev/mtd%d", i);
					devfd = open(dev, O_RDWR | O_SYNC);
				}
				break;
			}
		}
		fclose(fp);
	}

	/*if (devfd == -1)
		devfd = open(argv[2], O_RDWR | O_SYNC);*/

	if (devfd == -1) {
		fprintf(stderr, "Cannot open device for writing\n");
		goto cleanup3;
	}

	if (ioctl(devfd, MEMGETINFO, &mtdinfo)) {
		fprintf(stderr, "Cannot get device info\n");
		goto cleanup2;
	}

	if (devoffset % mtdinfo.erasesize != 0) {
		fprintf(stderr, "devoffset (%d) must be a multiple of erasesize (%d)\n", devoffset, mtdinfo.erasesize);
		goto cleanup2;
	}

	if (len > mtdinfo.size - devoffset) {
		fprintf(stderr, "Image file (%d) is too big for the device (%d)\n", len, mtdinfo.size - devoffset);
		goto cleanup2;
	}

	if (lseek(devfd, devoffset, SEEK_SET) == -1) {
		fprintf(stderr, "lseek in device to %d failed\n", devoffset);
		goto cleanup2;
	}

	if (!(writebuf = malloc(BUFSIZE))) {
		fprintf(stderr, "malloc'ing writebuf failed\n");
		goto cleanup2;
	}

	if (!(testbuf = malloc(BUFSIZE))) {
		fprintf(stderr, "malloc'ing testbuf failed\n");
		goto cleanup1;
	}

	sync();

	fprintf(stderr, "Unlocking device...\n");
	mtderaseinfo.start = devoffset;
	mtderaseinfo.length = mtdinfo.size - devoffset;
	if (ioctl(devfd, MEMUNLOCK, &mtderaseinfo)) {
		fprintf(stderr, "Failed to unlock device\n");
		goto cleanup;
	}

	fprintf(stderr, "Writing image to device... [ ]");
	fflush(stderr);

	w = e = devoffset;
	for (; len;) {
		r = read(imagefd, writebuf, len < (BUFSIZE) ? len : BUFSIZE);

		/* EOF */
		if (r <= 0)
			break;

		w += r;
		len -= r;

		/* need to erase the next block before writing data to it */
		while (w > e) {
			mtderaseinfo.start = e;
			mtderaseinfo.length = mtdinfo.erasesize;

			fprintf(stderr, "\b\b\b[e]");
			fflush(stderr);
			/* erase the chunk */
			if (ioctl(devfd, MEMERASE, &mtderaseinfo) < 0) {
				fprintf(stderr, "\nErasing device failed\n");
				goto cleanup;
			}
			e += mtdinfo.erasesize;
		}

		fprintf(stderr, "\b\b\b[w]");
		fflush(stderr);

		if (write(devfd, writebuf, r) != r) {
			fprintf(stderr, "\nError writing image.\n");
			goto cleanup;
		}

		/* verify the block just written */
		fprintf(stderr, "\b\b\b[s]");
		fflush(stderr);
		sync();

		fprintf(stderr, "\b\b\b[v]");
		fflush(stderr);
		if (lseek(devfd, -r, SEEK_CUR) == -1) {
			fprintf(stderr, "\nlseek failed.\n");
			goto cleanup;
		}
		if (read(devfd, testbuf, r) != r) {
			fprintf(stderr, "\nError reading mtd for verification.\n");
			goto cleanup;
		}
		if (memcmp(writebuf, testbuf, r)) {
			fprintf(stderr, "\nPost-write verification has failed.\n");
			goto cleanup;
		}

	}

	fprintf(stderr, "\b\b\b[s]");
	fflush(stderr);
	sync();

	fprintf(stderr, "\b\b\b\b\nDone!\n");
	fflush(stderr);

	ret = 0;

cleanup:
	free(testbuf);
cleanup1:
	free(writebuf);
cleanup2:
	close(devfd);
cleanup3:
	close(imagefd);
cleanup4:

	if (!ret && reboot_after) {
		fprintf(stderr, "Rebooting...\n");
		fflush(stderr);
		syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, NULL);
	}

	return ret;
}
