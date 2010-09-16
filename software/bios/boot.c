/*
 * Milkymist VJ SoC (Software)
 * Copyright (C) 2007, 2008, 2009, 2010 Sebastien Bourdeauducq
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <console.h>
#include <uart.h>
#include <system.h>
#include <board.h>
#include <cffat.h>
#include <crc.h>
#include <sfl.h>

#include <net/microudp.h>
#include <net/tftp.h>

#include <hw/hpdmc.h>

#include "boot.h"

extern const struct board_desc *brd_desc;

/*
 * HACK: by defining this function as not inlinable, GCC will automatically
 * put the values we want into the good registers because it has to respect
 * the LM32 calling conventions.
 */
static void __attribute__((noinline)) __attribute__((noreturn)) boot(unsigned int r1, unsigned int r2, unsigned int r3, unsigned int addr)
{
	asm volatile( /* Invalidate instruction cache */
		"wcsr ICC, r0\n"
		"nop\n"
		"nop\n"
		"nop\n"
		"nop\n"
		"call r4\n"
	);
}

/* Note that we do not use the hw timer so that this function works
 * even if the system controller does not.
 */
static int check_ack()
{
	int timeout;
	int recognized;
	static const char str[SFL_MAGIC_LEN] = SFL_MAGIC_ACK;
	
	timeout = 4500000;
	recognized = 0;
	while(timeout > 0) {
		if(readchar_nonblock()) {
			char c;
			c = readchar();
			if(c == str[recognized]) {
				recognized++;
				if(recognized == SFL_MAGIC_LEN)
					return 1;
			} else {
				if(c == str[0])
					recognized = 1;
				else
					recognized = 0;
			}
		}
		timeout--;
	}
	return 0;
}

#define MAX_FAILED 5

void serialboot()
{
	struct sfl_frame frame;
	int failed;
	unsigned int cmdline_adr, initrdstart_adr, initrdend_adr;
	
	printf("I: Attempting serial firmware loading\n");
	putsnonl(SFL_MAGIC_REQ);
	if(!check_ack()) {
		printf("E: Timeout\n");
		return;
	}
	
	failed = 0;
	cmdline_adr = initrdstart_adr = initrdend_adr = 0;
	while(1) {
		int i;
		int actualcrc;
		int goodcrc;
		
		/* Grab one frame */
		frame.length = readchar();
		frame.crc[0] = readchar();
		frame.crc[1] = readchar();
		frame.cmd = readchar();
		for(i=0;i<frame.length;i++)
			frame.payload[i] = readchar();
		
		/* Check CRC */
		actualcrc = ((int)frame.crc[0] << 8)|(int)frame.crc[1];
		goodcrc = crc16(&frame.cmd, frame.length+1);
		if(actualcrc != goodcrc) {
			failed++;
			if(failed == MAX_FAILED) {
				printf("E: Too many consecutive errors, aborting");
				return;
			}
			writechar(SFL_ACK_CRCERROR);
			continue;
		}
		
		/* CRC OK */
		switch(frame.cmd) {
			case SFL_CMD_ABORT:
				failed = 0;
				writechar(SFL_ACK_SUCCESS);
				return;
			case SFL_CMD_LOAD: {
				char *writepointer;
				
				failed = 0;
				writepointer = (char *)(
					 ((unsigned int)frame.payload[0] << 24)
					|((unsigned int)frame.payload[1] << 16)
					|((unsigned int)frame.payload[2] << 8)
					|((unsigned int)frame.payload[3] << 0));
				for(i=4;i<frame.length;i++)
					*(writepointer++) = frame.payload[i];
				writechar(SFL_ACK_SUCCESS);
				break;
			}
			case SFL_CMD_JUMP: {
				unsigned int addr;
				
				failed = 0;
				addr =  ((unsigned int)frame.payload[0] << 24)
					|((unsigned int)frame.payload[1] << 16)
					|((unsigned int)frame.payload[2] << 8)
					|((unsigned int)frame.payload[3] << 0);
				writechar(SFL_ACK_SUCCESS);
				boot(cmdline_adr, initrdstart_adr, initrdend_adr, addr);
				break;
			}
			case SFL_CMD_CMDLINE:
				failed = 0;
				cmdline_adr =  ((unsigned int)frame.payload[0] << 24)
					      |((unsigned int)frame.payload[1] << 16)
					      |((unsigned int)frame.payload[2] << 8)
					      |((unsigned int)frame.payload[3] << 0);
				writechar(SFL_ACK_SUCCESS);
				break;
			case SFL_CMD_INITRDSTART:
				failed = 0;
				initrdstart_adr =  ((unsigned int)frame.payload[0] << 24)
					          |((unsigned int)frame.payload[1] << 16)
					          |((unsigned int)frame.payload[2] << 8)
					          |((unsigned int)frame.payload[3] << 0);
				writechar(SFL_ACK_SUCCESS);
				break;
			case SFL_CMD_INITRDEND:
				failed = 0;
				initrdend_adr =  ((unsigned int)frame.payload[0] << 24)
					        |((unsigned int)frame.payload[1] << 16)
					        |((unsigned int)frame.payload[2] << 8)
					        |((unsigned int)frame.payload[3] << 0);
				writechar(SFL_ACK_SUCCESS);
				break;
			default:
				failed++;
				if(failed == MAX_FAILED) {
					printf("E: Too many consecutive errors, aborting");
					return;
				}
				writechar(SFL_ACK_UNKNOWN);
				break;
		}
	}
}

static unsigned char macadr[] = {0xf8, 0x71, 0xfe, 0x01, 0x02, 0x03};

#define LOCALIP1 192
#define LOCALIP2 168
#define LOCALIP3 0
#define LOCALIP4 42
#define REMOTEIP1 192
#define REMOTEIP2 168
#define REMOTEIP3 0
#define REMOTEIP4 14

static int tftp_get_v(unsigned int ip, const char *filename, char *buffer)
{
	int r;

	r = tftp_get(ip, filename, buffer);
	if(r > 0)
		printf("I: Successfully downloaded %d bytes from %s over TFTP\n", r, filename);
	else
		printf("I: Unable to download %s over TFTP\n", filename);
	return r;
}

static char microudp_buf[MICROUDP_BUFSIZE];

void netboot()
{
	int size;
	unsigned int cmdline_adr, initrdstart_adr, initrdend_adr;
	unsigned int ip;

	printf("I: Booting from network...\n");
	printf("I: MAC      : %02x:%02x:%02x:%02x:%02x:%02x\n", macadr[0], macadr[1], macadr[2], macadr[3], macadr[4], macadr[5]);
	printf("I: Local IP : %d.%d.%d.%d\n", LOCALIP1, LOCALIP2, LOCALIP3, LOCALIP4);
	printf("I: Remote IP: %d.%d.%d.%d\n", REMOTEIP1, REMOTEIP2, REMOTEIP3, REMOTEIP4);

	ip = IPTOINT(REMOTEIP1, REMOTEIP2, REMOTEIP3, REMOTEIP4);
	
	microudp_start(macadr, IPTOINT(LOCALIP1, LOCALIP2, LOCALIP3, LOCALIP4), microudp_buf);
	
	if(tftp_get_v(ip, "boot.bin", (void *)SDRAM_BASE) <= 0) {
		printf("E: Network boot failed\n");
		return;
	}
	
	cmdline_adr = SDRAM_BASE+0x1000000;
	size = tftp_get_v(ip, "cmdline.txt", (void *)cmdline_adr);
	if(size <= 0) {
		printf("I: No command line parameters found\n");
		cmdline_adr = 0;
	} else
		*((char *)(cmdline_adr+size)) = 0x00;

	initrdstart_adr = SDRAM_BASE+0x1002000;
	size = tftp_get_v(ip, "initrd.bin", (void *)initrdstart_adr);
	if(size <= 0) {
		printf("I: No initial ramdisk found\n");
		initrdstart_adr = 0;
		initrdend_adr = 0;
	} else
		initrdend_adr = initrdstart_adr + size - 1;

	microudp_shutdown();

	printf("I: Booting...\n");
	boot(cmdline_adr, initrdstart_adr, initrdend_adr, SDRAM_BASE);
}

static int tryload(char *filename, unsigned int address)
{
	int devsize, realsize;
	
	devsize = cffat_load(filename, (char *)address, 16*1024*1024, &realsize);
	if(devsize <= 0)
		return -1;
	if(realsize > devsize) {
		printf("E: File size larger than the blocks read (corrupted FS or IO error ?)\n");
		cffat_done();
		return -1;
	}
	printf("I: Read a %d byte image from %s\n", realsize, filename);
	
	return realsize;
}


void cardboot(int alt)
{
	int size;
	unsigned int cmdline_adr, initrdstart_adr, initrdend_adr;

	if(brd_desc->memory_card == MEMCARD_NONE) {
		printf("E: No memory card on this board\n");
		return;
	}
	
	printf("I: Booting from CF card...\n");
	if(!cffat_init()) {
		printf("E: Unable to initialize filesystem\n");
		return;
	}

	if(tryload(alt ? "ALTBOOT.BIN" : "BOOT.BIN", SDRAM_BASE) <= 0) {
		printf("E: Firmware image not found\n");
		return;
	}

	cmdline_adr = SDRAM_BASE+0x1000000;
	size = tryload("CMDLINE.TXT", cmdline_adr);
	if(size <= 0) {
		printf("I: No command line parameters found (CMDLINE.TXT)\n");
		cmdline_adr = 0;
	} else
		*((char *)(cmdline_adr+size)) = 0x00;

	initrdstart_adr = SDRAM_BASE+0x1002000;
	size = tryload("INITRD.BIN", initrdstart_adr);
	if(size <= 0) {
		printf("I: No initial ramdisk found (INITRD.BIN)\n");
		initrdstart_adr = 0;
		initrdend_adr = 0;
	} else
		initrdend_adr = initrdstart_adr + size - 1;

	cffat_done();
	printf("I: Booting...\n");
	boot(cmdline_adr, initrdstart_adr, initrdend_adr, SDRAM_BASE);
}
