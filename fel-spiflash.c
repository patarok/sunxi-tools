/*
 * (C) Copyright 2016 Siarhei Siamashka <siarhei.siamashka@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
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
#include <stdlib.h>
#include <string.h>

#include "fel_lib.h"
#include "progress.h"

#include "fel-remotefunc-spi-data-transfer.h"

/*****************************************************************************/

typedef struct {
	uint8_t   manufacturer_id;
	uint16_t  device_id;
	uint8_t   write_enable_cmd;
	uint8_t   large_erase_cmd;
	uint32_t  large_erase_size;
	uint8_t   small_erase_cmd;
	uint32_t  small_erase_size;
	uint8_t   program_cmd;
	uint32_t  program_size;

	size_t      capacity;
	const char* text_description;
} spi_flash_info_t;

static const spi_flash_info_t spi_flash_info[] = {
	{ .manufacturer_id = 0x00, .device_id = 0xE10B,
	  .capacity = 0x8000000, .text_description = "XTX XT26G01A",
	  .write_enable_cmd = 0x6,
	  .large_erase_cmd = 0xD8, .large_erase_size = 64 * 1024,
	  .small_erase_cmd = 0x20, .small_erase_size =  4 * 1024,
	  .program_cmd = 0x02, .program_size = 256
	},

	/* These are unverfified */
	{ .manufacturer_id = 0xEF, .device_id = 0x4018,
	  .capacity = 0x1000000, .text_description = "Winbond W25Qxx",
	  .write_enable_cmd = 0x6,
	  .large_erase_cmd = 0xD8, .large_erase_size = 64 * 1024,
	  .small_erase_cmd = 0x20, .small_erase_size =  4 * 1024,
	  .program_cmd = 0x02, .program_size = 256
	},
	{ .manufacturer_id = 0xC2, .device_id = 0x2018,
	  .capacity = 0x1000000, .text_description = "Macronix MX25Lxxxx",
	  .write_enable_cmd = 0x6,
	  .large_erase_cmd = 0xD8, .large_erase_size = 64 * 1024,
	  .small_erase_cmd = 0x20, .small_erase_size =  4 * 1024,
	  .program_cmd = 0x02, .program_size = 256
	},
};

static const int spi_flash_count = sizeof(spi_flash_info) / sizeof(spi_flash_info[0]);

/*****************************************************************************/

uint32_t fel_readl(feldev_handle *dev, uint32_t addr);
void fel_writel(feldev_handle *dev, uint32_t addr, uint32_t val);
#define readl(addr)                 fel_readl(dev, (addr))
#define writel(val, addr)           fel_writel(dev, (addr), (val))

#define PA                          (0)
#define PB                          (1)
#define PC                          (2)

#define CCM_SPI0_CLK                (0x01C20000 + 0xA0)
#define CCM_AHB_GATING0             (0x01C20000 + 0x60)
#define CCM_AHB_GATE_SPI0           (1 << 20)
#define SUN6I_BUS_SOFT_RST_REG0     (0x01C20000 + 0x2C0)
#define SUN6I_SPI0_RST              (1 << 20)

#define SUNIV_GPC_SPI0              (2)
#define SUNXI_GPC_SPI0              (3)
#define SUN50I_GPC_SPI0             (4)

#define SUN4I_CTL_ENABLE            (1 << 0)
#define SUN4I_CTL_MASTER            (1 << 1)
#define SUN4I_CTL_TF_RST            (1 << 8)
#define SUN4I_CTL_RF_RST            (1 << 9)
#define SUN4I_CTL_XCH               (1 << 10)

#define SUN6I_TCR_XCH               (1 << 31)

static uint32_t spi0_base;

#define SUN4I_SPI0_CCTL             (spi0_base + 0x1C)
#define SUN4I_SPI0_CTL              (spi0_base + 0x08)
#define SUN4I_SPI0_RX               (spi0_base + 0x00)
#define SUN4I_SPI0_TX               (spi0_base + 0x04)
#define SUN4I_SPI0_FIFO_STA         (spi0_base + 0x28)
#define SUN4I_SPI0_BC               (spi0_base + 0x20)
#define SUN4I_SPI0_TC               (spi0_base + 0x24)

#define SUN6I_SPI0_CCTL             (spi0_base + 0x24)
#define SUN6I_SPI0_GCR              (spi0_base + 0x04)
#define SUN6I_SPI0_TCR              (spi0_base + 0x08)
#define SUN6I_SPI0_FIFO_STA         (spi0_base + 0x1C)
#define SUN6I_SPI0_MBC              (spi0_base + 0x30)
#define SUN6I_SPI0_MTC              (spi0_base + 0x34)
#define SUN6I_SPI0_BCC              (spi0_base + 0x38)
#define SUN6I_SPI0_TXD              (spi0_base + 0x200)
#define SUN6I_SPI0_RXD              (spi0_base + 0x300)

#define CCM_SPI0_CLK_DIV_BY_2       (0x1000)
#define CCM_SPI0_CLK_DIV_BY_4       (0x1001)
#define CCM_SPI0_CLK_DIV_BY_6       (0x1002)
#define CCM_SPI0_CLK_DIV_BY_32      (0x100f)

/*
 * SPI Flash commands
 */

#define CMD_WRITE_ENABLE 0x06
#define CMD_GET_FEATURE 0x0F
#define CMD_READ_FROM_CACHE 0x0B
#define CMD_PAGE_READ_TO_CACHE 0x13
#define CMD_GET_JEDEC_ID 0x9F

typedef struct {
	uint8_t cmd;
	uint8_t manufacturer_id;
	uint16_t device_id;
} GetJEDEC;

/*
 * Configure pin function on a GPIO port
 */
static void gpio_set_cfgpin(feldev_handle *dev, int port_num, int pin_num,
			    int val)
{
	uint32_t port_base = 0x01C20800 + port_num * 0x24;
	uint32_t cfg_reg   = port_base + 4 * (pin_num / 8);
	uint32_t pin_idx   = pin_num % 8;
	uint32_t x = readl(cfg_reg);
	x &= ~(0x7 << (pin_idx * 4));
	x |= val << (pin_idx * 4);
	writel(x, cfg_reg);
}

static bool spi_is_sun6i(feldev_handle *dev)
{
	soc_info_t *soc_info = dev->soc_info;
	switch (soc_info->soc_id) {
	case 0x1623: /* A10 */
	case 0x1625: /* A13 */
	case 0x1651: /* A20 */
		return false;
	default:
		return true;
	}
}

/*
 * Init the SPI0 controller and setup pins muxing.
 */
static bool spi0_init(feldev_handle *dev)
{
	uint32_t reg_val;
	soc_info_t *soc_info = dev->soc_info;
	if (!soc_info)
		return false;

	/*
	 * suniv has the SPI0 base in the same position with A10/A13/A20, but it's
	 * a sun6i-style SPI controller.
	 */
	if (!spi_is_sun6i(dev) || soc_info->soc_id == 0x1663)
		spi0_base = 0x01c05000;
	else
		spi0_base = 0x01c68000;

	/* Setup SPI0 pins muxing */
	switch (soc_info->soc_id) {
	case 0x1663: /* Allwinner F1C100s/F1C600/R6/F1C100A/F1C500 */
		gpio_set_cfgpin(dev, PC, 0, SUNIV_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 1, SUNIV_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 2, SUNIV_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 3, SUNIV_GPC_SPI0);
		break;
	case 0x1625: /* Allwinner A13 */
	case 0x1680: /* Allwinner H3 */
	case 0x1718: /* Allwinner H5 */
		gpio_set_cfgpin(dev, PC, 0, SUNXI_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 1, SUNXI_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 2, SUNXI_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 3, SUNXI_GPC_SPI0);
		break;
	case 0x1689: /* Allwinner A64 */
		gpio_set_cfgpin(dev, PC, 0, SUN50I_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 1, SUN50I_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 2, SUN50I_GPC_SPI0);
		gpio_set_cfgpin(dev, PC, 3, SUN50I_GPC_SPI0);
		break;
	default: /* Unknown/Unsupported SoC */
		return false;
	}

	reg_val = readl(CCM_AHB_GATING0);
	reg_val |= CCM_AHB_GATE_SPI0;
	writel(reg_val, CCM_AHB_GATING0);

	if (spi_is_sun6i(dev)) {
		/* Deassert SPI0 reset */
		reg_val = readl(SUN6I_BUS_SOFT_RST_REG0);
		reg_val |= SUN6I_SPI0_RST;
		writel(reg_val, SUN6I_BUS_SOFT_RST_REG0);
		/* Enable SPI in the master mode and do a soft reset */
		reg_val = readl(SUN6I_SPI0_GCR);
		reg_val |= (1 << 31) | 3;
		writel(reg_val, SUN6I_SPI0_GCR);
		/* Wait for completion */
		while (readl(SUN6I_SPI0_GCR) & (1 << 31)) {}
	} else {
		reg_val = readl(SUN4I_SPI0_CTL);
		reg_val |= SUN4I_CTL_MASTER;
		reg_val |= SUN4I_CTL_ENABLE | SUN4I_CTL_TF_RST | SUN4I_CTL_RF_RST;
		writel(reg_val, SUN4I_SPI0_CTL);
	}

	if (soc_info->soc_id != 0x1663) {
		/* 24MHz from OSC24M */
		writel((1 << 31), CCM_SPI0_CLK);
		/* divide by 4 */
		writel(CCM_SPI0_CLK_DIV_BY_4,
		       spi_is_sun6i(dev) ? SUN6I_SPI0_CCTL :
					   SUN4I_SPI0_CCTL);
	} else {
		/*
		 * suniv doesn't have module clock for SPI0 and the
		 * clock source is AHB clock. The code will also
		 * configure AHB clock at 200MHz.
		 */
		/* Set PLL6 to 600MHz */
		writel(0x80041400, 0x01c20028);
		/* PLL6:AHB:APB = 6:2:1 */
		writel(0x00003180, 0x01c20054);
		/* divide by 32 */
		writel(CCM_SPI0_CLK_DIV_BY_32, SUN6I_SPI0_CCTL);
	}

	return true;
}

/*
 * Backup/restore the initial portion of the SRAM, which can be used as
 * a temporary data buffer.
 */
static void *backup_sram(feldev_handle *dev)
{
	soc_info_t *soc_info = dev->soc_info;
	size_t bufsize = soc_info->scratch_addr - soc_info->spl_addr;
	void *buf = malloc(bufsize);
	aw_fel_read(dev, soc_info->spl_addr, buf, bufsize);
	return buf;
}

static void restore_sram(feldev_handle *dev, void *buf)
{
	soc_info_t *soc_info = dev->soc_info;
	size_t bufsize = soc_info->scratch_addr - soc_info->spl_addr;
	aw_fel_write(dev, buf, soc_info->spl_addr, bufsize);
	free(buf);
}

static void prepare_spi_batch_data_transfer(feldev_handle *dev)
{
	soc_info_t *soc_info = dev->soc_info;

	if (spi_is_sun6i(dev)) {
		aw_fel_remotefunc_prepare_spi_batch_data_transfer(dev,
							    soc_info->spl_addr,
							    SUN6I_SPI0_TCR,
							    SUN6I_TCR_XCH,
							    SUN6I_SPI0_FIFO_STA,
							    SUN6I_SPI0_TXD,
							    SUN6I_SPI0_RXD,
							    SUN6I_SPI0_MBC,
							    SUN6I_SPI0_MTC,
							    SUN6I_SPI0_BCC);
	} else {
		aw_fel_remotefunc_prepare_spi_batch_data_transfer(dev,
							    soc_info->spl_addr,
							    SUN4I_SPI0_CTL,
							    SUN4I_CTL_XCH,
							    SUN4I_SPI0_FIFO_STA,
							    SUN4I_SPI0_TX,
							    SUN4I_SPI0_RX,
							    SUN4I_SPI0_BC,
							    SUN4I_SPI0_TC,
							    0);
	}
}

static void spi_transaction(feldev_handle *dev, void* buf, size_t len) {
	soc_info_t *soc_info = dev->soc_info;
	size_t max_chunk_size = soc_info->scratch_addr - soc_info->spl_addr;

	union {
		struct {
			uint16_t size;
			uint8_t data[];
		} ;
		uint8_t raw[max_chunk_size];
	} tx;

	if (len > max_chunk_size - 2) {
		printf("Transaction too large\n");
		return ;
	}

	uint32_t words = (len + 5) / 4; // data payload size, plus header, rounded up to nearest word

	tx.size = (len << 8) | (len >> 8);

	memcpy(tx.data, buf, len);
	aw_fel_write(dev, &tx, soc_info->spl_addr, words*4);
	aw_fel_remotefunc_execute(dev, NULL);
	aw_fel_read(dev, soc_info->spl_addr + 2, buf, len);
}

static const spi_flash_info_t* spi_get_flash_info(feldev_handle *dev) {
	GetJEDEC jedec_id = {
		.cmd = CMD_GET_JEDEC_ID
	};

	spi_transaction(dev, &jedec_id, sizeof(jedec_id));

	if (jedec_id.device_id == 0 || jedec_id.device_id == 0xFFFF) {
		printf("SPI Flash not found\n");
		return NULL;
	}

	for (int i = 0; i < spi_flash_count; i++) {
		const spi_flash_info_t* info = &spi_flash_info[i];

		if (info->manufacturer_id == jedec_id.manufacturer_id
			&& info->device_id == jedec_id.device_id) {
			return info;
		}
	}

	if (!jedec_id.device_id) {
		printf("SPI Flash not recognized (%02x:%04x)\n",
			jedec_id.manufacturer_id, jedec_id.device_id);
		return NULL;
	}

	return NULL;
}

uint8_t spi_flash_get_feature(feldev_handle *dev, uint8_t address) {
	uint8_t cmd[3] = { CMD_GET_FEATURE, address };

	spi_transaction(dev, cmd, sizeof(cmd));

	return cmd[2];
}

void aw_fel_spiflash_read(feldev_handle *dev,
			  uint32_t offset, void *buf, size_t len,
			  progress_cb_t progress)
{
	void *backup = backup_sram(dev);
	spi0_init(dev);
	prepare_spi_batch_data_transfer(dev);

	const spi_flash_info_t *flash_info = spi_get_flash_info(dev);

	if (flash_info == NULL) {
		return;
	}

	if (len + offset > flash_info->capacity) {
		printf("Truncating read to flash size\n");
		len = flash_info->capacity - offset;
	}

	progress_start(progress, len);
	while (len > 0) {
		uint32_t block = offset / 2048;

		/* Read page to cache */
		uint8_t read_to_cache[4] = {
			CMD_PAGE_READ_TO_CACHE,
			(uint8_t)(block >> 16),
			(uint8_t)(block >>  8),
			(uint8_t)(block >>  0)
		};

		spi_transaction(dev, &read_to_cache, sizeof(read_to_cache));

		/* Operation in progress */
		while (spi_flash_get_feature(dev, 0xC0) & 1) ;

		/* Read data from cache */
		uint32_t page_addr = offset % 2048;
		uint16_t bytes = 2048 - page_addr;
		if (bytes > len) bytes = len;

		uint8_t read_from_cache[2048+4] = {
			CMD_READ_FROM_CACHE,
			(uint8_t)(page_addr >>  8),
			(uint8_t)(page_addr >>  0),
			0,
		};

		spi_transaction(dev, &read_from_cache, 4+bytes);
		memcpy(buf, &read_from_cache[4], bytes);

		buf += bytes;
		offset += bytes;
		len -= bytes;

		progress_update(bytes);
	}

	restore_sram(dev, backup);
}

void aw_fel_spiflash_write(feldev_handle *dev,
			   uint32_t offset, void *buf, size_t len,
			   progress_cb_t progress)
{
	void *backup = backup_sram(dev);
	spi0_init(dev);
	prepare_spi_batch_data_transfer(dev);
	const spi_flash_info_t *flash_info = spi_get_flash_info(dev);

	if (flash_info == NULL) {
		return;
	}

	// TODO: DO YOUR WORST
	(void) offset;
	(void) buf;
	(void) len;
	(void) progress;

	restore_sram(dev, backup);
}

#if 0
/*
 * Read data from the SPI flash. Use the first 4KiB of SRAM as the data buffer.
 */
void aw_fel_spiflash_read(feldev_handle *dev,
			  uint32_t offset, void *buf, size_t len,
			  progress_cb_t progress)
{
	soc_info_t *soc_info = dev->soc_info;
	void *backup = backup_sram(dev);
	uint8_t *buf8 = (uint8_t *)buf;
	size_t max_chunk_size = soc_info->scratch_addr - soc_info->spl_addr;
	if (max_chunk_size > 0x1000)
		max_chunk_size = 0x1000;
	uint8_t *cmdbuf = malloc(max_chunk_size);
	memset(cmdbuf, 0, max_chunk_size);
	aw_fel_write(dev, cmdbuf, soc_info->spl_addr, max_chunk_size);

	spi0_init(dev);
	prepare_spi_batch_data_transfer(dev);

	progress_start(progress, len);
	while (len > 0) {
		size_t chunk_size = len;
		if (chunk_size > max_chunk_size - 8)
			chunk_size = max_chunk_size - 8;

		memset(cmdbuf, 0, max_chunk_size);
		cmdbuf[0] = (chunk_size + 4) >> 8;
		cmdbuf[1] = (chunk_size + 4);
		cmdbuf[2] = 3;
		cmdbuf[3] = offset >> 16;
		cmdbuf[4] = offset >> 8;
		cmdbuf[5] = offset;

		if (chunk_size == max_chunk_size - 8)
			aw_fel_write(dev, cmdbuf, soc_info->spl_addr, 6);
		else
			aw_fel_write(dev, cmdbuf, soc_info->spl_addr, chunk_size + 8);
		aw_fel_remotefunc_execute(dev, NULL);
		aw_fel_read(dev, soc_info->spl_addr + 6, buf8, chunk_size);

		len -= chunk_size;
		offset += chunk_size;
		buf8 += chunk_size;
		progress_update(chunk_size);
	}

	free(cmdbuf);
	restore_sram(dev, backup);
}

/*
 * Write data to the SPI flash. Use the first 4KiB of SRAM as the data buffer.
 */

void aw_fel_spiflash_write_helper(feldev_handle *dev,
				  uint32_t offset, void *buf, size_t len,
				  size_t erase_size, uint8_t erase_cmd,
				  size_t program_size, uint8_t program_cmd)
{
	soc_info_t *soc_info = dev->soc_info;
	uint8_t *buf8 = (uint8_t *)buf;
	size_t max_chunk_size = soc_info->scratch_addr - soc_info->spl_addr;
	size_t cmd_idx;

	if (max_chunk_size > 0x1000)
		max_chunk_size = 0x1000;
	uint8_t *cmdbuf = malloc(max_chunk_size);
	cmd_idx = 0;

	prepare_spi_batch_data_transfer(dev);

	while (len > 0) {
		while (len > 0 && max_chunk_size - cmd_idx > program_size + 64) {
			if (offset % erase_size == 0) {
				/* Emit write enable command */
				cmdbuf[cmd_idx++] = 0;
				cmdbuf[cmd_idx++] = 1;
				cmdbuf[cmd_idx++] = CMD_WRITE_ENABLE;
				/* Emit erase command */
				cmdbuf[cmd_idx++] = 0;
				cmdbuf[cmd_idx++] = 4;
				cmdbuf[cmd_idx++] = erase_cmd;
				cmdbuf[cmd_idx++] = offset >> 16;
				cmdbuf[cmd_idx++] = offset >> 8;
				cmdbuf[cmd_idx++] = offset;
				/* Emit wait for completion */
				cmdbuf[cmd_idx++] = 0xFF;
				cmdbuf[cmd_idx++] = 0xFF;
			}
			/* Emit write enable command */
			cmdbuf[cmd_idx++] = 0;
			cmdbuf[cmd_idx++] = 1;
			cmdbuf[cmd_idx++] = CMD_WRITE_ENABLE;
			/* Emit page program command */
			size_t write_count = program_size;
			if (write_count > len)
				write_count = len;
			cmdbuf[cmd_idx++] = (4 + write_count) >> 8;
			cmdbuf[cmd_idx++] = 4 + write_count;
			cmdbuf[cmd_idx++] = program_cmd;
			cmdbuf[cmd_idx++] = offset >> 16;
			cmdbuf[cmd_idx++] = offset >> 8;
			cmdbuf[cmd_idx++] = offset;
			memcpy(cmdbuf + cmd_idx, buf8, write_count);
			cmd_idx += write_count;
			buf8    += write_count;
			len     -= write_count;
			offset  += write_count;
			/* Emit wait for completion */
			cmdbuf[cmd_idx++] = 0xFF;
			cmdbuf[cmd_idx++] = 0xFF;
		}
		/* Emit the end marker */
		cmdbuf[cmd_idx++] = 0;
		cmdbuf[cmd_idx++] = 0;

		/* Flush */
		aw_fel_write(dev, cmdbuf, soc_info->spl_addr, cmd_idx);
		aw_fel_remotefunc_execute(dev, NULL);
		cmd_idx = 0;
	}

	free(cmdbuf);
}

void aw_fel_spiflash_write(feldev_handle *dev,
			   uint32_t offset, void *buf, size_t len,
			   progress_cb_t progress)
{
	void *backup = backup_sram(dev);
	uint8_t *buf8 = (uint8_t *)buf;

	const spi_flash_info_t *flash_info = &spi_flash_info[0]; /* FIXME */

	if ((offset % flash_info->small_erase_size) != 0) {
		fprintf(stderr, "aw_fel_spiflash_write: 'addr' must be %d bytes aligned\n",
		        flash_info->small_erase_size);
		exit(1);
	}

	spi0_init(dev);

	progress_start(progress, len);
	while (len > 0) {
		size_t write_count;
		if ((offset % flash_info->large_erase_size) != 0 ||
							len < flash_info->large_erase_size) {

			write_count = flash_info->small_erase_size;
			if (write_count > len)
				write_count = len;
			aw_fel_spiflash_write_helper(dev, offset, buf8,
				write_count,
				flash_info->small_erase_size, flash_info->small_erase_cmd,
				flash_info->program_size, flash_info->program_cmd);
		} else {
			write_count = flash_info->large_erase_size;
			if (write_count > len)
				write_count = len;
			aw_fel_spiflash_write_helper(dev, offset, buf8,
				write_count,
				flash_info->large_erase_size, flash_info->large_erase_cmd,
				flash_info->program_size, flash_info->program_cmd);
		}

		len    -= write_count;
		offset += write_count;
		buf8   += write_count;
		progress_update(write_count);
	}

	restore_sram(dev, backup);
}
#endif

/*
 * Use the read JEDEC ID (9Fh) command.
 */
void aw_fel_spiflash_info(feldev_handle *dev)
{
	void *backup = backup_sram(dev);
	spi0_init(dev);
	prepare_spi_batch_data_transfer(dev);

	const spi_flash_info_t *flash_info = spi_get_flash_info(dev);
	restore_sram(dev, backup);

	/* Assume that the MISO pin is either pulled up or down */
	if (flash_info == NULL) {
		return;
	}

	printf("Device: %s (%02Xh %04X), capacity: %ld bytes.\n",
	       flash_info->text_description, flash_info->manufacturer_id, flash_info->device_id, flash_info->capacity);
}

/*
 * Show a help message about the available "spiflash-*" commands.
 */
void aw_fel_spiflash_help(void)
{
	printf("	spiflash-info			Retrieves basic information\n"
	       "	spiflash-read addr length file	Write SPI flash contents into file\n"
	       "	spiflash-write addr file	Store file contents into SPI flash\n");
}
