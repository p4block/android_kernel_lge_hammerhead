/*
   +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2011 Synaptics, Inc.

   Permission is hereby granted, free of charge, to any person obtaining a copy of
   this software and associated documentation files (the "Software"), to deal in
   the Software without restriction, including without limitation the rights to use,
   copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
   Software, and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.


   +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
*/
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/input/touch_synaptics.h>


/* Variables for F34 functionality */
unsigned short SynaF34DataBase;
unsigned short SynaF34QueryBase;
unsigned short SynaF01DataBase;
unsigned short SynaF01CommandBase;
unsigned short SynaF01ControlBase;
unsigned short SynaF01QueryBase;

unsigned short SynaF34Reflash_BlockNum;
unsigned short SynaF34Reflash_BlockData;
unsigned short SynaF34ReflashQuery_BootID;
unsigned short SynaF34ReflashQuery_FlashPropertyQuery;
unsigned short SynaF34ReflashQuery_FirmwareBlockSize;
unsigned short SynaF34ReflashQuery_FirmwareBlockCount;
unsigned short SynaF34ReflashQuery_ConfigBlockSize;
unsigned short SynaF34ReflashQuery_ConfigBlockCount;

unsigned short SynaFirmwareBlockSize;
unsigned short SynaFirmwareBlockCount;
unsigned long SynaImageSize;

unsigned short SynaConfigBlockSize;
unsigned short SynaConfigBlockCount;
unsigned long SynaConfigImageSize;

unsigned short SynaBootloadID;

unsigned short SynaF34_FlashControl;

unsigned char *SynafirmwareImgData;
unsigned char *SynaconfigImgData;
unsigned char *SynalockImgData;
unsigned int SynafirmwareImgVersion;

//unsigned char ConfigBlock[];
//unsigned char FirmwareImage[16000];  // make smaller and dynamic
//unsigned char ConfigImage[16000];  // make smaller and dynamic

unsigned char *my_image_bin;
unsigned long my_image_size;
u8 fw_image_config_id[5];

static void CompleteReflash_Lockdown(struct synaptics_ts_data *ts);
static void SynaInitialize(struct synaptics_ts_data *ts);
static void SynaReadConfigInfo(struct synaptics_ts_data *ts);
static void SynaReadFirmwareInfo(struct synaptics_ts_data *ts);
static void SynaEnableFlashing(struct synaptics_ts_data *ts);
static void SynaProgramFirmware(struct synaptics_ts_data *ts);
static void SynaProgramConfiguration(struct synaptics_ts_data *ts);
static void SynaFinalizeReflash(struct synaptics_ts_data *ts);
static void SynaWaitForATTN(int time);

int FirmwareUpgrade(struct synaptics_ts_data *ts, const char* fw_path)
{
	int ret = 0;
	int fd = -1;
	mm_segment_t old_fs = 0;
	struct stat fw_bin_stat;
	unsigned long read_bytes;

	if (unlikely(fw_path[0] != 0)) {
		old_fs = get_fs();
		set_fs(get_ds());

		if ((fd = sys_open((const char __user *) fw_path, O_RDONLY, 0)) < 0) {
			TOUCH_ERR_MSG("Can not read FW binary from %s\n", fw_path);
			ret = -EEXIST;
			goto read_fail;
		}

		if ((ret = sys_newstat((char __user *) fw_path,
					(struct stat *)&fw_bin_stat)) < 0) {
			TOUCH_ERR_MSG("Can not read FW binary stat from %s\n", fw_path);
			goto fw_mem_alloc_fail;
		}

		my_image_size = fw_bin_stat.st_size;
		my_image_bin =
			kzalloc(sizeof(char) * (my_image_size+1), GFP_KERNEL);
		if (my_image_bin == NULL) {
			TOUCH_ERR_MSG("Can not allocate  memory\n");
			ret = -ENOMEM;
			goto fw_mem_alloc_fail;
		}

		read_bytes = sys_read(fd,
				(char __user *)my_image_bin, my_image_size);

		/* for checksum */
		*(my_image_bin+my_image_size) = 0xFF;

		TOUCH_INFO_MSG("Touch FW image read %ld bytes from %s\n",
							read_bytes, fw_path);

	} else {
		my_image_size = ts->fw_info.fw_size-1;
		my_image_bin = (unsigned char *)(&ts->fw_info.fw_start[0]);
	}

	strncpy(fw_image_config_id, &my_image_bin[0xb100], 4);

	TOUCH_INFO_MSG("fw_image_confid_id = %s\n", fw_image_config_id);

	CompleteReflash_Lockdown(ts);

	if (unlikely(fw_path[0] != 0))
		kfree(my_image_bin);

fw_mem_alloc_fail:
	sys_close(fd);
read_fail:
	set_fs(old_fs);

	return ret;
}

static int writeRMI(struct i2c_client *client, u8 reg, u8 *buf, unsigned int len)
{
	unsigned char send_buf[len + 1];
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = client->flags,
			.len = len+1,
			.buf = send_buf,
		},
	};

	send_buf[0] = (unsigned char)reg;
	memcpy(&send_buf[1], buf, len);

	if (i2c_transfer(client->adapter, msgs, 1) < 0) {
		if (printk_ratelimit())
			TOUCH_ERR_MSG("transfer error\n");
		return -EIO;
	}

	return 0;
}

static int readRMI(struct i2c_client *client, u8 reg, u8 *buf, unsigned int len)
{
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};

	if (i2c_transfer(client->adapter, msgs, 2) < 0) {
		if (printk_ratelimit())
			TOUCH_ERR_MSG("transfer error\n");
		return -EIO;
	}

	return 0;
}

/* End: Variables for F34 functionality */

/* SynaSetup scans the Page Description Table (PDT) and sets up the necessary variables
 * for the reflash process. This function is a "slim" version of the PDT scan function in
 * in PDT.c, since only F34 and F01 are needed for reflash.
 */
static void SynaSetup(struct synaptics_ts_data *ts)
{
	unsigned char address;
	unsigned char buffer[6];

	TOUCH_INFO_MSG("%s", __FUNCTION__);

	for (address = 0xe9; address > 0xd0; address = address - 6) {
		readRMI(ts->client, address, &buffer[0], 6);

		if (!buffer[5])
			break;

		switch (buffer[5]) {
		case 0x34:
			SynaF34DataBase = buffer[3];
			SynaF34QueryBase = buffer[0];
			break;
		case 0x01:
			SynaF01DataBase = buffer[3];
			SynaF01CommandBase = buffer[1];
			SynaF01ControlBase = buffer[2];
			SynaF01QueryBase = buffer[0];
			break;
		}
	}

	SynaF34Reflash_BlockNum = SynaF34DataBase;
	SynaF34Reflash_BlockData = SynaF34DataBase + 2;
	SynaF34ReflashQuery_BootID = SynaF34QueryBase;
	SynaF34ReflashQuery_FlashPropertyQuery = SynaF34QueryBase + 2;
	SynaF34ReflashQuery_FirmwareBlockSize = SynaF34QueryBase + 3;
	SynaF34ReflashQuery_FirmwareBlockCount = SynaF34QueryBase +5;
	SynaF34ReflashQuery_ConfigBlockSize = SynaF34QueryBase + 3;
	SynaF34ReflashQuery_ConfigBlockCount = SynaF34QueryBase + 7;

	SynafirmwareImgData = (unsigned char *)((&my_image_bin[0])+0x100);
	SynaconfigImgData   = (unsigned char *)(SynafirmwareImgData+SynaImageSize);
	SynafirmwareImgVersion = (unsigned int)(my_image_bin[7]);

	switch (SynafirmwareImgVersion) {
	case 2:
		SynalockImgData = (unsigned char *)((&my_image_bin[0]) + 0xD0);
		break;
	case 3:
	case 4:
		SynalockImgData = (unsigned char *)((&my_image_bin[0]) + 0xC0);
		break;
	case 5:
		SynalockImgData = (unsigned char *)((&my_image_bin[0]) + 0xB0);
		break;
	default:
		break;
	}
}

/* SynaInitialize sets up the reflahs process
 */
static void SynaInitialize(struct synaptics_ts_data *ts)
{
	unsigned char uData[2];
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nInitializing Reflash Process...");
	uData[0] = 0x00;
	writeRMI(ts->client, 0xff, &uData[0], 1);

	SynaSetup(ts);

	/*Set all interrupt enable */
	uData[0] = 0x0f;
	writeRMI(ts->client, SynaF01ControlBase+1, &uData[0], 1);

	readRMI(ts->client, SynaF34ReflashQuery_FirmwareBlockSize, &uData[0], 2);

	SynaFirmwareBlockSize = uData[0] | (uData[1] << 8);
}

/* SynaReadFirmwareInfo reads the F34 query registers and retrieves the block size and count
 * of the firmware section of the image to be reflashed
 */
static void SynaReadFirmwareInfo(struct synaptics_ts_data *ts)
{
	unsigned char uData[2];
	uData[0] = 0;
	uData[1] = 0;

	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nRead Firmware Info");

	readRMI(ts->client, SynaF34ReflashQuery_FirmwareBlockSize, &uData[0], 2);
	SynaFirmwareBlockSize = uData[0] | (uData[1] << 8);

	readRMI(ts->client, SynaF34ReflashQuery_FirmwareBlockCount, &uData[0], 2);
	SynaFirmwareBlockCount = uData[0] | (uData[1] << 8);
	SynaImageSize = SynaFirmwareBlockCount * SynaFirmwareBlockSize;
}

/* SynaReadConfigInfo reads the F34 query registers and retrieves the block size and count
 * of the configuration section of the image to be reflashed
 */
static void SynaReadConfigInfo(struct synaptics_ts_data *ts)
{
	unsigned char uData[2];
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nRead Config Info");

	readRMI(ts->client, SynaF34ReflashQuery_ConfigBlockSize, &uData[0], 2);
	SynaConfigBlockSize = uData[0] | (uData[1] << 8);

	readRMI(ts->client, SynaF34ReflashQuery_ConfigBlockCount, &uData[0], 2);
	SynaConfigBlockCount = uData[0] | (uData[1] << 8);
	SynaConfigImageSize = SynaConfigBlockCount * SynaConfigBlockSize;
}

/* SynaReadBootloadID reads the F34 query registers and retrieves the bootloader ID of the firmware
 */
static void SynaReadBootloadID(struct synaptics_ts_data *ts)
{
	unsigned char uData[2];
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	readRMI(ts->client, SynaF34ReflashQuery_BootID, &uData[0], 2);
	SynaBootloadID = uData[0] + uData[1] * 0x100;
}

/* SynaWriteBootloadID writes the bootloader ID to the F34 data register to unlock the reflash process
 */
static void SynaWriteBootloadID(struct synaptics_ts_data *ts)
{
	unsigned char uData[2];
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	uData[0] = SynaBootloadID % 0x100;
	uData[1] = SynaBootloadID / 0x100;

	writeRMI(ts->client, SynaF34Reflash_BlockData, &uData[0], 2);
}

/* SynaWaitForATTN waits for ATTN to be asserted within a certain time threshold.
 */
static void SynaWaitForATTN(int time)
{
	mdelay(time);
}

/* SynaWaitATTN waits for ATTN to be asserted within a certain time threshold.
 * The function also checks for the F34 "Program Enabled" bit and clear ATTN accordingly.
 */
static void SynaWaitATTN(struct synaptics_ts_data *ts)
{
	unsigned char uData;
	unsigned char uStatus;

	SynaWaitForATTN(10);

	do {
		readRMI(ts->client, SynaF34_FlashControl, &uData, 1);
		readRMI(ts->client, (SynaF01DataBase + 1), &uStatus, 1);
	} while (uData!= 0x80);
}

/* SynaEnableFlashing kicks off the reflash process
 */
static void SynaEnableFlashing(struct synaptics_ts_data *ts)
{
	unsigned char uData;
	unsigned char uStatus;
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nEnable Reflash...");

	/* Reflash is enabled by first reading the bootloader ID from the firmware and write it back */
	SynaReadBootloadID(ts);
	SynaWriteBootloadID(ts);

	/* Make sure Reflash is not already enabled */
	do {
		readRMI(ts->client, SynaF34_FlashControl, &uData, 1);
	} while (((uData & 0x0f) != 0x00));

	/* Clear ATTN */
	readRMI (ts->client, SynaF01DataBase, &uStatus, 1);

	if ((uStatus &0x40) == 0) {
		/*
		 * Write the "Enable Flash Programming command to
		 * F34 Control register.
		 * Wait for ATTN and then clear the ATTN.
		 */
		uData = 0x0f;
		writeRMI(ts->client, SynaF34_FlashControl, &uData, 1);
		SynaWaitForATTN(100);
		readRMI(ts->client, (SynaF01DataBase + 1), &uStatus, 1);
		/* Scan the PDT again to ensure all register offsets are correct */
		SynaSetup(ts);
		/* Read the "Program Enabled" bit of the F34 Control register,
		   and proceed only if the bit is set. */
		do {
			readRMI(ts->client, SynaF34_FlashControl, &uData, 1);
			/* In practice, if uData!=0x80 happens for multiple counts,
			   it indicates reflash */
			/* is failed to be enabled, and program should quit */
		} while (uData != 0x80);
	}
}

/* SynaProgramConfiguration writes the configuration section of the image block by block
 */
static void SynaProgramConfiguration(struct synaptics_ts_data *ts)
{
	unsigned char uData[2];
	unsigned char *puData = (unsigned char *)&my_image_bin[0xb100];

	unsigned short blockNum;
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nProgram Configuration Section...");

	for (blockNum = 0; blockNum < SynaConfigBlockCount; blockNum++) {
		uData[0] = blockNum & 0xff;
		uData[1] = (blockNum & 0xff00) >> 8;

		/*Block by blcok, write the block number and data to the corresponding F34 data registers */
		writeRMI(ts->client, SynaF34Reflash_BlockNum, &uData[0], 2);
		writeRMI(ts->client, SynaF34Reflash_BlockData, puData, SynaConfigBlockSize);
		puData += SynaConfigBlockSize;

		/* Issue the "Write Configuration Block" command */
		uData[0] = 0x06;
		writeRMI(ts->client, SynaF34_FlashControl, &uData[0], 1);
		SynaWaitATTN(ts);
		TOUCH_INFO_MSG(".");
	}
}

/* SynaFinalizeReflash finalizes the reflash process
 */
static void SynaFinalizeReflash(struct synaptics_ts_data *ts)
{
	unsigned char uData;
	unsigned char uStatus;
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nFinalizing Reflash...");

	/*
	 * Issue the "Reset" command to F01 command register to reset the chip
	 * This command will also test the new firmware image and
	 * check if its is valid
	 */
	uData = 1;
	writeRMI(ts->client, SynaF01CommandBase, &uData, 1);

	SynaWaitForATTN(100);
	readRMI(ts->client, SynaF01DataBase, &uData, 1);

	/* Sanity check that the reflash process is still enabled */
	do {
		readRMI(ts->client, SynaF34_FlashControl, &uStatus, 1);
	} while ((uStatus & 0x0f) != 0x00);
	readRMI(ts->client, (SynaF01DataBase + 1), &uStatus, 1);

	SynaSetup(ts);
	uData = 0;

	/* Check if the "Program Enabled" bit in F01 data register is cleared
	   Reflash is completed, and the image passes testing when the bit is cleared */
	do {
		readRMI(ts->client, SynaF01DataBase, &uData, 1);
	} while ((uData & 0x40) != 0);

	/* Rescan PDT the update any changed register offsets */
	SynaSetup(ts);

	TOUCH_INFO_MSG("\nReflash Completed. Please reboot.");
}

/* SynaFlashFirmwareWrite writes the firmware section of the image block by block
 */
static void SynaFlashFirmwareWrite(struct synaptics_ts_data *ts)
{
	unsigned char *puFirmwareData = (unsigned char *)&my_image_bin[0x100];
	unsigned char uData[2];
	unsigned short blockNum;
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	for (blockNum = 0; blockNum < SynaFirmwareBlockCount; ++blockNum) {
		if (blockNum%100 == 0)
			TOUCH_INFO_MSG("blockNum = [%d], (SynaFirmwareBlockCount=%d)\n", blockNum, SynaFirmwareBlockCount);
		/*Block by blcok, write the block number and data to the corresponding F34 data registers */
		uData[0] = blockNum & 0xff;
		uData[1] = (blockNum & 0xff00) >> 8;
		writeRMI(ts->client, SynaF34Reflash_BlockNum, &uData[0], 2);

		writeRMI(ts->client, SynaF34Reflash_BlockData, puFirmwareData,
					SynaFirmwareBlockSize);
		puFirmwareData += SynaFirmwareBlockSize;

		/* Issue the "Write Firmware Block" command */
		uData[0] = 2;
		writeRMI(ts->client, SynaF34_FlashControl, &uData[0], 1);

		SynaWaitATTN(ts);
	}

}

/* SynaProgramFirmware prepares the firmware writing process
 */
static void SynaProgramFirmware(struct synaptics_ts_data *ts)
{
	unsigned char uData;
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	TOUCH_INFO_MSG("\nProgram Firmware Section...");

	/*SynaReadBootloadID(ts);*/

	SynaWriteBootloadID(ts);

	uData = 3;
	writeRMI(ts->client, SynaF34_FlashControl, &uData, 1);

	msleep(1000);

	SynaWaitATTN(ts);

	SynaFlashFirmwareWrite(ts);
}

static void SynaBootloaderLock(struct synaptics_ts_data *ts)
{
	unsigned short lockBlockCount;
	unsigned char *puFirmwareData = SynalockImgData;
	unsigned char uData[2];
	unsigned short uBlockNum;

	/* Check if device is in unlocked state */
	readRMI(ts->client, (SynaF34QueryBase+ 2), &uData[0], 1);

	/*Device is unlocked */
	if (uData[0] & 0x02) {
		TOUCH_INFO_MSG("Device unlocked. Lock it first...\n");
		/*
		 * Different bootloader version has different block count
		 * for the lockdown data
		 * Need to check the bootloader version from the image file
		 * being reflashed
		 */
		switch (SynafirmwareImgVersion) {
		case 2:
			lockBlockCount = 3;
			break;
		case 3:
		case 4:
			lockBlockCount = 4;
			break;
		case 5:
			lockBlockCount = 5;
			break;
		default:
			lockBlockCount = 0;
			break;
		}

		/*
		 * Write the lockdown info block by block
		 * This reference code of lockdown process does not check
		 * for bootloader version currently programmed on the ASIC
		 * against the bootloader version of the image to be reflashed.
		 * Such case should not happen in practice.
		 * Reflashing cross different bootloader versions is not
		 * supported.
		 */
		for (uBlockNum = 0; uBlockNum < lockBlockCount; ++uBlockNum) {
			uData[0] = uBlockNum & 0xff;
			uData[1] = (uBlockNum & 0xff00) >> 8;

			/* Write Block Number */
			readRMI(ts->client, SynaF34Reflash_BlockNum, &uData[0], 2);

			/* Write Data Block */
			writeRMI(ts->client, SynaF34Reflash_BlockData,
					puFirmwareData, SynaFirmwareBlockSize);

			/* Move to next data block */
			puFirmwareData += SynaFirmwareBlockSize;

			/* Issue Write Lockdown Block command */
			uData[0] = 4;
			writeRMI(ts->client, SynaF34_FlashControl, &uData[0], 1);

			/* Wait ATTN until device is done writing the block and is ready for the next. */
			SynaWaitATTN(ts);
		}
		TOUCH_INFO_MSG("Device locking done.\n");

		/*
		 * Enable reflash again to finish the lockdown process.
		 * Since this lockdown process is part of the reflash process,
		 * we are enabling reflash instead, rather than resetting
		 * the device to finish the unlock procedure.
		 */
		SynaEnableFlashing(ts);
	} else {
		TOUCH_INFO_MSG("Device already locked.\n");
	}
}

static void CompleteReflash_Lockdown(struct synaptics_ts_data *ts)
{
	TOUCH_INFO_MSG("%s", __FUNCTION__);

	SynaInitialize(ts);

	SynaReadConfigInfo(ts);

	SynaReadFirmwareInfo(ts);

	SynaF34_FlashControl = SynaF34DataBase + SynaFirmwareBlockSize + 2;

	SynaEnableFlashing(ts);

	SynaBootloaderLock(ts);

	SynaProgramFirmware(ts);

	SynaProgramConfiguration(ts);

	SynaFinalizeReflash(ts);
}

