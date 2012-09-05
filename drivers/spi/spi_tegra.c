/*
 * Driver for Nvidia TEGRA spi controller.
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *     Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2010-2011 NVIDIA Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*#define DEBUG           1*/
/*#define VERBOSE_DEBUG   1*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include<linux/pm_runtime.h>

#include <linux/spi/spi.h>
#include <linux/spi-tegra.h>

#include <mach/dma.h>
#include <mach/clk.h>

#ifdef CONFIG_SERIAL_SC8800G
#define DEBUG_SPI_MASTER_OOPS
#define DEBUG_949393
#define DEBUG_960388
#define WORKAROUND_949393
#define SOLUTION_959947
#define DEBUG_962049
#define WORKAROUND_962049
#define SOLUTION_967438
//#define WORKAROUND_949393_PRINT_LOG
#endif

#define SLINK_COMMAND		0x000
#define   SLINK_BIT_LENGTH(x)		(((x) & 0x1f) << 0)
#define   SLINK_WORD_SIZE(x)		(((x) & 0x1f) << 5)
#define   SLINK_BOTH_EN			(1 << 10)
#define   SLINK_CS_SW			(1 << 11)
#define   SLINK_CS_VALUE		(1 << 12)
#define   SLINK_CS_POLARITY		(1 << 13)
#define   SLINK_IDLE_SDA_DRIVE_LOW	(0 << 16)
#define   SLINK_IDLE_SDA_DRIVE_HIGH	(1 << 16)
#define   SLINK_IDLE_SDA_PULL_LOW	(2 << 16)
#define   SLINK_IDLE_SDA_PULL_HIGH	(3 << 16)
#define   SLINK_IDLE_SDA_MASK		(3 << 16)
#define   SLINK_CS_POLARITY1		(1 << 20)
#define   SLINK_CK_SDA			(1 << 21)
#define   SLINK_CS_POLARITY2		(1 << 22)
#define   SLINK_CS_POLARITY3		(1 << 23)
#define   SLINK_IDLE_SCLK_DRIVE_LOW	(0 << 24)
#define   SLINK_IDLE_SCLK_DRIVE_HIGH	(1 << 24)
#define   SLINK_IDLE_SCLK_PULL_LOW	(2 << 24)
#define   SLINK_IDLE_SCLK_PULL_HIGH	(3 << 24)
#define   SLINK_IDLE_SCLK_MASK		(3 << 24)
#define   SLINK_M_S			(1 << 28)
#define   SLINK_WAIT			(1 << 29)
#define   SLINK_GO			(1 << 30)
#define   SLINK_ENB			(1 << 31)

#define SLINK_COMMAND2		0x004
#define   SLINK_LSBFE			(1 << 0)
#define   SLINK_SSOE			(1 << 1)
#define   SLINK_SPIE			(1 << 4)
#define   SLINK_BIDIROE			(1 << 6)
#define   SLINK_MODFEN			(1 << 7)
#define   SLINK_INT_SIZE(x)		(((x) & 0x1f) << 8)
#define   SLINK_CS_ACTIVE_BETWEEN	(1 << 17)
#define   SLINK_SS_EN_CS(x)		(((x) & 0x3) << 18)
#define   SLINK_SS_SETUP(x)		(((x) & 0x3) << 20)
#define   SLINK_FIFO_REFILLS_0		(0 << 22)
#define   SLINK_FIFO_REFILLS_1		(1 << 22)
#define   SLINK_FIFO_REFILLS_2		(2 << 22)
#define   SLINK_FIFO_REFILLS_3		(3 << 22)
#define   SLINK_FIFO_REFILLS_MASK	(3 << 22)
#define   SLINK_WAIT_PACK_INT(x)	(((x) & 0x7) << 26)
#define   SLINK_SPC0			(1 << 29)
#define   SLINK_TXEN			(1 << 30)
#define   SLINK_RXEN			(1 << 31)

#define SLINK_STATUS		0x008
#define   SLINK_COUNT(val)		(((val) >> 0) & 0x1f)
#define   SLINK_WORD(val)		(((val) >> 5) & 0x1f)
#define   SLINK_BLK_CNT(val)		(((val) >> 0) & 0xffff)
#define   SLINK_MODF			(1 << 16)
#define   SLINK_RX_UNF			(1 << 18)
#define   SLINK_TX_OVF			(1 << 19)
#define   SLINK_TX_FULL			(1 << 20)
#define   SLINK_TX_EMPTY		(1 << 21)
#define   SLINK_RX_FULL			(1 << 22)
#define   SLINK_RX_EMPTY		(1 << 23)
#define   SLINK_TX_UNF			(1 << 24)
#define   SLINK_RX_OVF			(1 << 25)
#define   SLINK_TX_FLUSH		(1 << 26)
#define   SLINK_RX_FLUSH		(1 << 27)
#define   SLINK_SCLK			(1 << 28)
#define   SLINK_ERR			(1 << 29)
#define   SLINK_RDY			(1 << 30)
#define   SLINK_BSY			(1 << 31)

#define SLINK_MAS_DATA		0x010
#define SLINK_SLAVE_DATA	0x014

#define SLINK_DMA_CTL		0x018
#define   SLINK_DMA_BLOCK_SIZE(x)	(((x) & 0xffff) << 0)
#define   SLINK_TX_TRIG_1		(0 << 16)
#define   SLINK_TX_TRIG_4		(1 << 16)
#define   SLINK_TX_TRIG_8		(2 << 16)
#define   SLINK_TX_TRIG_16		(3 << 16)
#define   SLINK_TX_TRIG_MASK		(3 << 16)
#define   SLINK_RX_TRIG_1		(0 << 18)
#define   SLINK_RX_TRIG_4		(1 << 18)
#define   SLINK_RX_TRIG_8		(2 << 18)
#define   SLINK_RX_TRIG_16		(3 << 18)
#define   SLINK_RX_TRIG_MASK		(3 << 18)
#define   SLINK_PACKED			(1 << 20)
#define   SLINK_PACK_SIZE_4		(0 << 21)
#define   SLINK_PACK_SIZE_8		(1 << 21)
#define   SLINK_PACK_SIZE_16		(2 << 21)
#define   SLINK_PACK_SIZE_32		(3 << 21)
#define   SLINK_PACK_SIZE_MASK		(3 << 21)
#define   SLINK_IE_TXC			(1 << 26)
#define   SLINK_IE_RXC			(1 << 27)
#define   SLINK_DMA_EN			(1 << 31)

#define SLINK_STATUS2		0x01c
#define   SLINK_TX_FIFO_EMPTY_COUNT(val)	(((val) & 0x3f) >> 0)
#define   SLINK_RX_FIFO_FULL_COUNT(val)		(((val) & 0x3f0000) >> 16)
#define   SLINK_SS_HOLD_TIME(val)		(((val) & 0xF) << 6)

#define SLINK_TX_FIFO		0x100
#define SLINK_RX_FIFO		0x180

#define DATA_DIR_TX		(1 << 0)
#define DATA_DIR_RX		(1 << 1)

#define SPI_FIFO_DEPTH		32
#define SLINK_DMA_TIMEOUT (msecs_to_jiffies(1000))


static const unsigned long spi_tegra_req_sels[] = {
	TEGRA_DMA_REQ_SEL_SL2B1,
	TEGRA_DMA_REQ_SEL_SL2B2,
	TEGRA_DMA_REQ_SEL_SL2B3,
	TEGRA_DMA_REQ_SEL_SL2B4,
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	TEGRA_DMA_REQ_SEL_SL2B5,
	TEGRA_DMA_REQ_SEL_SL2B6,
#endif

};

#define DEFAULT_SPI_DMA_BUF_LEN		(16*1024)
#define TX_FIFO_EMPTY_COUNT_MAX		SLINK_TX_FIFO_EMPTY_COUNT(0x20)
#define RX_FIFO_FULL_COUNT_ZERO		SLINK_RX_FIFO_FULL_COUNT(0)

#define SLINK_STATUS2_RESET \
	(TX_FIFO_EMPTY_COUNT_MAX | \
	RX_FIFO_FULL_COUNT_ZERO << 16)

#define MAX_CHIP_SELECT		4
#define SLINK_FIFO_DEPTH	4

struct spi_tegra_data {
	struct spi_master	*master;
	struct platform_device	*pdev;
	spinlock_t		lock;
	char			port_name[32];

	struct clk		*clk;
#ifdef CONFIG_SERIAL_SC8800G
	struct clk		*sclk;
#endif
	void __iomem		*base;
	phys_addr_t		phys;
	unsigned		irq;

	u32			cur_speed;

	struct list_head	queue;
	struct spi_transfer	*cur;
	struct spi_device	*cur_spi;
	unsigned		cur_pos;
	unsigned		cur_len;
	unsigned		words_per_32bit;
	unsigned		bytes_per_word;
	unsigned		curr_dma_words;

	unsigned		cur_direction;

	bool			is_dma_allowed;

	struct tegra_dma_req	rx_dma_req;
	struct tegra_dma_channel *rx_dma;
	u32			*rx_buf;
	dma_addr_t		rx_buf_phys;
	unsigned		cur_rx_pos;

	struct tegra_dma_req	tx_dma_req;
	struct tegra_dma_channel *tx_dma;
	u32			*tx_buf;
	dma_addr_t		tx_buf_phys;
	unsigned		cur_tx_pos;

	unsigned		dma_buf_size;
	unsigned		max_buf_size;
	bool			is_curr_dma_xfer;

	bool			is_clkon_always;
	bool			clk_state;
	bool			is_suspended;

	bool			is_hw_based_cs;

	struct completion	rx_dma_complete;
	struct completion	tx_dma_complete;
	bool			is_transfer_in_progress;

	u32			rx_complete;
	u32			tx_complete;
	u32			tx_status;
	u32			rx_status;
	u32			status_reg;
	bool			is_packed;
	unsigned long		packed_size;

	u32			command_reg;
	u32			command2_reg;
	u32			dma_control_reg;
	u32			def_command_reg;
	u32			def_command2_reg;

	struct spi_clk_parent	*parent_clk_list;
	int			parent_clk_count;
	unsigned long		max_rate;
	unsigned long		max_parent_rate;
	int			min_div;
	struct workqueue_struct *spi_workqueue;
	struct work_struct spi_transfer_work;
};

#ifdef DEBUG_962049
static int g_enable_debug_962049 = 0;
static int g_spi_tegra_transfer_debug_flag = 0;
static int g_tegra_spi_transfer_work_debug_flag = 0;
static int g_spi_tegra_curr_transfer_complete_debug_flag = 0;
static int g_spi_tegra_start_dma_based_transfer_debug_flag = 0;
static int g_spi_tegra_start_cpu_based_transfer_debug_flag = 0;
void spi_tegra_962049_debug_set(int enable)
{
	if ( g_enable_debug_962049 != enable ) {
		g_enable_debug_962049 = enable;
	}
}
void spi_tegra_962049_debug_get_info()
{
	pr_err("[%s] g_enable_debug_962049=[%d][%d][%d][%d][%d][%d]\n", __func__,
		g_enable_debug_962049, g_spi_tegra_transfer_debug_flag,
		g_tegra_spi_transfer_work_debug_flag, g_spi_tegra_curr_transfer_complete_debug_flag,
		g_spi_tegra_start_dma_based_transfer_debug_flag, g_spi_tegra_start_cpu_based_transfer_debug_flag);
}
#else
void spi_tegra_962049_debug_set(int enable){}
void spi_tegra_962049_debug_get_info(){}
#endif

#ifdef DEBUG_960388
static struct spi_master *tegra_masters[6] = {0};
static struct device *tegra_master_devices[6] = {0};
static struct device_private *tegra_master_device_privates[6] = {0};
static inline void *spi_master_get_devdata_debug(struct spi_master *master)
{
	int i = 0;
	if (master == NULL ) {
		spi_tegra_962049_debug_get_info();
		BUG_ON(1);
	}

	while (i < 6) {
		if (master != tegra_masters[i]) {
			i++; /* check next */
			continue;
		}
		if (unlikely(&master->dev != tegra_master_devices[i])) {
			pr_err("[%s] &master->dev=%p corrupted\n", __func__, &master->dev);
			pr_err("[%s] it was %p in store\n", __func__, tegra_master_devices[i]);
			BUG();
		}
		if (unlikely(master->dev.p != tegra_master_device_privates[i])) {
			pr_err("[%s] master->dev.p=%p corrupted\n", __func__, master->dev.p);
			pr_err("[%s] it was %p in store\n", __func__, tegra_master_devices[i]);
			BUG();
		}
		return spi_master_get_devdata(master);
	}
	pr_err("[%s] master %p doesn't match any in store\n", __func__, master);
	for (i = 0; i < 6; i++) {
		pr_err("[%s] tegra_masters[%d]=%p\n", __func__, i, tegra_masters[i]);
	}
	BUG();
	return NULL; /* fake for compiler warning */
}
#else
#define spi_master_get_devdata_debug spi_master_get_devdata
#endif

static inline unsigned long spi_tegra_readl(struct spi_tegra_data *tspi,
		    unsigned long reg)
{
	if (!tspi->clk_state)
		BUG();
	return readl(tspi->base + reg);
}

static inline void spi_tegra_writel(struct spi_tegra_data *tspi,
		    unsigned long val, unsigned long reg)
{
	if (!tspi->clk_state)
		BUG();
	writel(val, tspi->base + reg);
}

#if defined (DEBUG_949393)
static inline void print_registers(struct spi_tegra_data *tspi)
{
	unsigned addr = 0;
	unsigned long data[0x20/4];
	char line[256] = {0};
	while (addr < 0x20) {
		data[addr/4] = spi_tegra_readl(tspi, addr);
		sprintf(line, "%s0x%08X ", line, data[addr/4]);
		addr += 4;
	}
	pr_err("[%s] %s\n", __func__, line);
}
#endif

static void spi_tegra_clear_status(struct spi_tegra_data *tspi)
{
	unsigned long val;
	unsigned long val_write = 0;

	val = spi_tegra_readl(tspi, SLINK_STATUS);

	val_write = SLINK_RDY;
	if (val & SLINK_TX_OVF)
		val_write |= SLINK_TX_OVF;
	if (val & SLINK_RX_OVF)
		val_write |= SLINK_RX_OVF;
	if (val & SLINK_RX_UNF)
		val_write |= SLINK_RX_UNF;
	if (val & SLINK_TX_UNF)
		val_write |= SLINK_TX_UNF;

	spi_tegra_writel(tspi, val_write, SLINK_STATUS);
}

static unsigned long spi_tegra_get_packed_size(struct spi_tegra_data *tspi,
				  struct spi_transfer *t)
{
	unsigned long val;

	switch (tspi->bytes_per_word) {
	case 0:
		val = SLINK_PACK_SIZE_4;
		break;
	case 1:
		val = SLINK_PACK_SIZE_8;
		break;
	case 2:
		val = SLINK_PACK_SIZE_16;
		break;
	case 4:
		val = SLINK_PACK_SIZE_32;
		break;
	default:
		val = 0;
	}
	return val;
}

static unsigned spi_tegra_calculate_curr_xfer_param(
	struct spi_device *spi, struct spi_tegra_data *tspi,
	struct spi_transfer *t)
{
	unsigned remain_len = t->len - tspi->cur_pos;
	unsigned max_word;
	unsigned bits_per_word ;
	unsigned max_len;
	unsigned total_fifo_words;

	bits_per_word = t->bits_per_word ? t->bits_per_word :
						spi->bits_per_word;
	tspi->bytes_per_word = (bits_per_word - 1) / 8 + 1;

	if (bits_per_word == 8 || bits_per_word == 16) {
		tspi->is_packed = 1;
		tspi->words_per_32bit = 32/bits_per_word;
	} else {
		tspi->is_packed = 0;
		tspi->words_per_32bit = 1;
	}
	tspi->packed_size = spi_tegra_get_packed_size(tspi, t);

	if (tspi->is_packed) {
		max_len = min(remain_len, tspi->max_buf_size);
		tspi->curr_dma_words = max_len/tspi->bytes_per_word;
		total_fifo_words = remain_len/4;
	} else {
		max_word = (remain_len - 1) / tspi->bytes_per_word + 1;
		max_word = min(max_word, tspi->max_buf_size/4);
		tspi->curr_dma_words = max_word;
		total_fifo_words = remain_len/tspi->bytes_per_word;
	}
	return total_fifo_words;
}

static unsigned spi_tegra_fill_tx_fifo_from_client_txbuf(
	struct spi_tegra_data *tspi, struct spi_transfer *t)
{
	unsigned nbytes;
	unsigned tx_empty_count;
	unsigned long fifo_status;
	u8 *tx_buf = (u8 *)t->tx_buf + tspi->cur_tx_pos;
	unsigned max_n_32bit;
	unsigned i, count;
	unsigned long x;
	unsigned int written_words;

#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0 ) {
		dev_info(&tspi->pdev->dev, "[debug_962049]%s+\n",  __func__);
	}
#endif

	fifo_status = spi_tegra_readl(tspi, SLINK_STATUS2);
	tx_empty_count = SLINK_TX_FIFO_EMPTY_COUNT(fifo_status);

	if (tspi->is_packed) {
		nbytes = tspi->curr_dma_words * tspi->bytes_per_word;
		max_n_32bit = (min(nbytes,  tx_empty_count*4) - 1)/4 + 1;
		for (count = 0; count < max_n_32bit; ++count) {
			x = 0;
			for (i = 0; (i < 4) && nbytes; i++, nbytes--)
				x |= (*tx_buf++) << (i*8);
			spi_tegra_writel(tspi, x, SLINK_TX_FIFO);
		}
		written_words =  min(max_n_32bit * tspi->words_per_32bit,
					tspi->curr_dma_words);
	} else {
		max_n_32bit = min(tspi->curr_dma_words,  tx_empty_count);
		nbytes = max_n_32bit * tspi->bytes_per_word;
		for (count = 0; count < max_n_32bit; ++count) {
			x = 0;
			for (i = 0; nbytes && (i < tspi->bytes_per_word);
							++i, nbytes--)
				x |= ((*tx_buf++) << i*8);
			spi_tegra_writel(tspi, x, SLINK_TX_FIFO);
		}
		written_words = max_n_32bit;
	}
	tspi->cur_tx_pos += written_words * tspi->bytes_per_word;
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0 ) {
		dev_info(&tspi->pdev->dev, "[debug_962049]%s-\n",  __func__);
	}
#endif
	return written_words;
}

static unsigned int spi_tegra_read_rx_fifo_to_client_rxbuf(
		struct spi_tegra_data *tspi, struct spi_transfer *t)
{
	unsigned rx_full_count;
	unsigned long fifo_status;
	u8 *rx_buf = (u8 *)t->rx_buf + tspi->cur_rx_pos;
	unsigned i, count;
	unsigned long x;
	unsigned int read_words = 0;
	unsigned len;
#if defined (DEBUG_949393)
	unsigned long dma_ctl;
	unsigned dma_block_size;
#if defined (WORKAROUND_949393)
	unsigned long rx_fifo;
#endif

	dma_ctl = spi_tegra_readl(tspi, SLINK_DMA_CTL);
	dma_block_size = SLINK_DMA_BLOCK_SIZE(dma_ctl);
#endif

	fifo_status = spi_tegra_readl(tspi, SLINK_STATUS2);
	rx_full_count = SLINK_RX_FIFO_FULL_COUNT(fifo_status);

#if defined (DEBUG_949393)
	if (tspi->pdev->id == 0 && rx_full_count != (dma_block_size + 1)) {
		dev_err(&tspi->pdev->dev,
			"rx_full_count %d, dma_ctl 0x%08X, fifo_status 0x%08X\n",
			rx_full_count, dma_ctl, fifo_status);
#ifdef WORKAROUND_949393_PRINT_LOG
		print_registers(tspi);
#endif
#if defined (WORKAROUND_949393)
		if (tspi->is_packed) {
			//BUG_ON(1); /* no workaround for packed mode so far */
		} else {
			for (i = 0; i < (rx_full_count - (dma_block_size +1)); i++) {
				/* pop unexpected rx_fifo */
				rx_fifo = spi_tegra_readl(tspi, SLINK_RX_FIFO);
				dev_err(&tspi->pdev->dev,
					"rx_fifo[%i(%d, %d)] 0x%08X\n",
					i, rx_full_count, dma_block_size, rx_fifo);
				if ( i > 512 ) {
					break;
				}
			}
			rx_full_count = (dma_block_size + 1);
		//	WARN_ON(1);
		}
#else
		//BUG_ON(1);
#endif
	}
#endif
	dev_dbg(&tspi->pdev->dev, "Rx fifo count %d\n", rx_full_count);
	if (tspi->is_packed) {
		len = tspi->curr_dma_words * tspi->bytes_per_word;
		for (count = 0; count < rx_full_count; ++count) {
			x = spi_tegra_readl(tspi, SLINK_RX_FIFO);
			for (i = 0; len && (i < 4); ++i, len--)
				*rx_buf++ = (x >> i*8) & 0xFF;
		}
		tspi->cur_rx_pos += tspi->curr_dma_words * tspi->bytes_per_word;
		read_words += tspi->curr_dma_words;
	} else {
		unsigned int rx_mask, bits_per_word;

		bits_per_word = t->bits_per_word ? t->bits_per_word :
						tspi->cur_spi->bits_per_word;
		rx_mask = (1 << bits_per_word) -1;
		for (count = 0; count < rx_full_count; ++count) {
			x = spi_tegra_readl(tspi, SLINK_RX_FIFO);
			x &= rx_mask;
			for (i = 0; (i < tspi->bytes_per_word); ++i)
				*rx_buf++ = (x >> (i*8)) & 0xFF;
		}
		tspi->cur_rx_pos += rx_full_count * tspi->bytes_per_word;
		read_words += rx_full_count;
	}
	return read_words;
}

static void spi_tegra_copy_client_txbuf_to_spi_txbuf(
		struct spi_tegra_data *tspi, struct spi_transfer *t)
{
	unsigned len;
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0) {
		dev_info(&tspi->pdev->dev, "[debug_962049]%s+\n",  __func__);
	}
#endif
#ifdef SOLUTION_959947
	dma_sync_single_for_cpu(&tspi->pdev->dev, tspi->tx_dma_req.source_addr,
		tspi->dma_buf_size, DMA_TO_DEVICE);
#endif
	if (tspi->is_packed) {
		len = tspi->curr_dma_words * tspi->bytes_per_word;
		memcpy(tspi->tx_buf, t->tx_buf + tspi->cur_pos, len);
	} else {
		unsigned int i;
		unsigned int count;
		u8 *tx_buf = (u8 *)t->tx_buf + tspi->cur_tx_pos;
		unsigned consume = tspi->curr_dma_words * tspi->bytes_per_word;
		unsigned int x;

		for (count = 0; count < tspi->curr_dma_words; ++count) {
			x = 0;
			for (i = 0; consume && (i < tspi->bytes_per_word);
							++i, consume--)
				x |= ((*tx_buf++) << i*8);
			tspi->tx_buf[count] = x;
		}
	}
	tspi->cur_tx_pos += tspi->curr_dma_words * tspi->bytes_per_word;
#ifdef SOLUTION_959947
	dma_sync_single_for_device(&tspi->pdev->dev, tspi->tx_dma_req.source_addr,
		tspi->dma_buf_size, DMA_TO_DEVICE);
#endif
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0) {
		dev_info(&tspi->pdev->dev, "[debug_962049]%s-\n",  __func__);
	}
#endif
}

static void spi_tegra_copy_spi_rxbuf_to_client_rxbuf(
		struct spi_tegra_data *tspi, struct spi_transfer *t)
{
	unsigned len;
#ifdef SOLUTION_959947
	dma_sync_single_for_cpu(&tspi->pdev->dev, tspi->rx_dma_req.dest_addr,
		tspi->dma_buf_size, DMA_FROM_DEVICE);
#endif
	if (tspi->is_packed) {
		len = tspi->curr_dma_words * tspi->bytes_per_word;
		memcpy(t->rx_buf + tspi->cur_rx_pos, tspi->rx_buf, len);
	} else {
		unsigned int i;
		unsigned int count;
		unsigned char *rx_buf = t->rx_buf + tspi->cur_rx_pos;
		unsigned int x;
		unsigned int rx_mask, bits_per_word;

		bits_per_word = t->bits_per_word ? t->bits_per_word :
						tspi->cur_spi->bits_per_word;
		rx_mask = (1 << bits_per_word) -1;

		for (count = 0; count < tspi->curr_dma_words; ++count) {
			x = tspi->rx_buf[count];
			x &= rx_mask;
			for (i = 0; (i < tspi->bytes_per_word); ++i)
				*rx_buf++ = (x >> (i*8)) & 0xFF;
		}
	}
	tspi->cur_rx_pos += tspi->curr_dma_words * tspi->bytes_per_word;
#ifdef SOLUTION_959947
	dma_sync_single_for_device(&tspi->pdev->dev, tspi->rx_dma_req.dest_addr,
		tspi->dma_buf_size, DMA_FROM_DEVICE);
#endif
}

#ifdef SOLUTION_959947
static void spi_tegra_init_rx_buf(struct spi_tegra_data *tspi)
{
	int i;
	unsigned char *data = (unsigned char *)tspi->rx_buf;
	dma_sync_single_for_cpu(&tspi->pdev->dev, tspi->rx_dma_req.dest_addr,
		tspi->dma_buf_size, DMA_TO_DEVICE);
	for (i = 0; i < tspi->dma_buf_size; i++)
		data[i] = 0x5a;
	dma_sync_single_for_device(&tspi->pdev->dev, tspi->rx_dma_req.dest_addr,
		tspi->dma_buf_size, DMA_TO_DEVICE);
 }
#endif

static int spi_tegra_start_dma_based_transfer(
		struct spi_tegra_data *tspi, struct spi_transfer *t)
{
	unsigned long val;
	unsigned long test_val;
	unsigned int len;
	int ret = 0;

#ifdef WORKAROUND_962049
	u64 usec_start = 0;
	u32 usec_total = 0;
	int spi_tegra_readl_count = 0;
	static int spi_962049_resend_count = 0;
#endif

#if defined (DEBUG_949393)
#ifdef WORKAROUND_949393_PRINT_LOG
	unsigned long status2;
	unsigned long rx_fifo_full_count;
	unsigned long tx_fifo_empty_count;

	status2 = spi_tegra_readl(tspi, SLINK_STATUS2);
	rx_fifo_full_count = SLINK_RX_FIFO_FULL_COUNT(status2);
	tx_fifo_empty_count = SLINK_TX_FIFO_EMPTY_COUNT(status2);
#endif
#endif
#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 1;
#endif
	INIT_COMPLETION(tspi->rx_dma_complete);
	INIT_COMPLETION(tspi->tx_dma_complete);

	val = SLINK_DMA_BLOCK_SIZE(tspi->curr_dma_words - 1);
	val |= tspi->packed_size;
	if (tspi->is_packed)
		len = DIV_ROUND_UP(tspi->curr_dma_words * tspi->bytes_per_word,
					4) * 4;
	else
		len = tspi->curr_dma_words * 4;

	if (len & 0xF)
		val |= SLINK_TX_TRIG_1 | SLINK_RX_TRIG_1;
	else if (((len) >> 4) & 0x1)
		val |= SLINK_TX_TRIG_4 | SLINK_RX_TRIG_4;
	else
		val |= SLINK_TX_TRIG_8 | SLINK_RX_TRIG_8;

	if (tspi->cur_direction & DATA_DIR_TX)
		val |= SLINK_IE_TXC;

	if (tspi->cur_direction & DATA_DIR_RX)
		val |= SLINK_IE_RXC;

#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 2;
#endif

	spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
	tspi->dma_control_reg = val;

#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 3;
#endif

	if (tspi->cur_direction & DATA_DIR_TX) {
		spi_tegra_copy_client_txbuf_to_spi_txbuf(tspi, t);
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 4;
#endif
		wmb();
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 5;
#endif
		tspi->tx_dma_req.size = len;
		ret = tegra_dma_enqueue_req(tspi->tx_dma, &tspi->tx_dma_req);
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 6;
#endif
		if (ret < 0) {
			dev_err(&tspi->pdev->dev, "Error in starting tx dma "
						" error = %d\n", ret);
			return ret;
		}

#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 7;
#endif

		/* Wait for tx fifo to be fill before starting slink */
		test_val = spi_tegra_readl(tspi, SLINK_STATUS);
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 8;
#endif
#ifdef WORKAROUND_962049
		usec_start = cpu_clock(UINT_MAX);
		while (!(test_val & SLINK_TX_FULL)) {
			test_val = spi_tegra_readl(tspi, SLINK_STATUS);

			usec_total = ((u32)(cpu_clock(UINT_MAX) - usec_start) / 1000);

			if ( spi_tegra_readl_count > 1000 && usec_total > (500 * 1000) ) {
				/*over 500ms, trigger workaround*/
				pr_err("[%s] SLINK_TX_FULL debug message=[%d][%d][%d][%d]\n", __func__, usec_total, spi_tegra_readl_count, tspi->master->bus_num, tspi->status_reg);
				print_registers(tspi);

				tegra_dma_dequeue_req(tspi->tx_dma, &tspi->tx_dma_req);

				tegra_periph_reset_assert(tspi->clk);
				udelay(2);
				tegra_periph_reset_deassert(tspi->clk);
				WARN_ON(1);

				if ( spi_962049_resend_count > 10 ) {
					pr_err("[%s] SLINK_TX_FULL debug EIO=[%d]\n", __func__, spi_962049_resend_count);
					return -EIO;
				} else {
					pr_err("[%s] SLINK_TX_FULL debug return EIO(%d)\n", __func__, spi_962049_resend_count);
#if 0/*enable resend*/
					spi_962049_resend_count++;
					/*enable resend*/
					return -EAGAIN;
#else
					return -EIO;
#endif
				}
			}
			spi_tegra_readl_count++;

		}
		spi_962049_resend_count = 0;
#else
		while (!(test_val & SLINK_TX_FULL))
			test_val = spi_tegra_readl(tspi, SLINK_STATUS);
#endif

#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 9;
#endif
	}

	if (tspi->cur_direction & DATA_DIR_RX) {
#ifdef SOLUTION_959947
		spi_tegra_init_rx_buf(tspi);
#endif
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 10;
#endif
		tspi->rx_dma_req.size = len;
		ret = tegra_dma_enqueue_req(tspi->rx_dma, &tspi->rx_dma_req);
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 11;
#endif
		if (ret < 0) {
			dev_err(&tspi->pdev->dev, "Error in starting rx dma "
						" error = %d\n", ret);
			if (tspi->cur_direction & DATA_DIR_TX)
				tegra_dma_dequeue_req(tspi->tx_dma,
							&tspi->tx_dma_req);
			return ret;
		}
	}
#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 12;
#endif
	tspi->is_curr_dma_xfer = true;
	if (tspi->is_packed) {
		val |= SLINK_PACKED;
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 13;
#endif
		spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
		udelay(1);
#ifdef DEBUG_962049
		g_spi_tegra_start_dma_based_transfer_debug_flag = 14;
#endif
		wmb();
	}
#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 15;
#endif

	val |= SLINK_DMA_EN;
#if defined (DEBUG_949393)
#ifdef WORKAROUND_949393_PRINT_LOG
	if ( tspi->pdev->id == 0
		&& ((rx_fifo_full_count != 0) || (tx_fifo_empty_count != 0x20))) {
		dev_err(&tspi->pdev->dev, "[%s] before setting DMA_EN val=0x%08X\n",
			__func__, val);
		print_registers(tspi);
	}
#endif
#endif
#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 16;
#endif
	spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
#ifdef DEBUG_962049
	g_spi_tegra_start_dma_based_transfer_debug_flag = 17;
#endif
	return ret;
}

static int spi_tegra_start_cpu_based_transfer(
		struct spi_tegra_data *tspi, struct spi_transfer *t)
{
	unsigned long val;
	unsigned curr_words;
#if defined (DEBUG_949393)
#ifdef WORKAROUND_949393_PRINT_LOG
	unsigned long status2;
	unsigned long rx_fifo_full_count;
	unsigned long tx_fifo_empty_count;

	status2 = spi_tegra_readl(tspi, SLINK_STATUS2);
	rx_fifo_full_count = SLINK_RX_FIFO_FULL_COUNT(status2);
	tx_fifo_empty_count = SLINK_TX_FIFO_EMPTY_COUNT(status2);
#endif
#endif
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 1;
#endif
	val = tspi->packed_size;
	if (tspi->cur_direction & DATA_DIR_TX)
		val |= SLINK_IE_TXC;

	if (tspi->cur_direction & DATA_DIR_RX)
		val |= SLINK_IE_RXC;

	spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
	tspi->dma_control_reg = val;
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 2;
#endif

	if (tspi->cur_direction & DATA_DIR_TX) {
#ifdef DEBUG_962049
		g_spi_tegra_start_cpu_based_transfer_debug_flag = 3;
#endif
		curr_words = spi_tegra_fill_tx_fifo_from_client_txbuf(tspi, t);
#ifdef DEBUG_962049
		g_spi_tegra_start_cpu_based_transfer_debug_flag = 4;
#endif
	} else {
		curr_words = tspi->curr_dma_words;
	}
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 5;
#endif
	val |= SLINK_DMA_BLOCK_SIZE(curr_words - 1);
	spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
	tspi->dma_control_reg = val;
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 6;
#endif

	tspi->is_curr_dma_xfer = false;
	if (tspi->is_packed) {
		val |= SLINK_PACKED;
#ifdef DEBUG_962049
		g_spi_tegra_start_cpu_based_transfer_debug_flag = 7;
#endif
		spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
		udelay(1);
#ifdef DEBUG_962049
		g_spi_tegra_start_cpu_based_transfer_debug_flag = 8;
#endif
		wmb();
	}
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 9;
#endif
	val |= SLINK_DMA_EN;
#if defined (DEBUG_949393)
#ifdef WORKAROUND_949393_PRINT_LOG
	if ( tspi->pdev->id == 0
		&& ((rx_fifo_full_count != 0) || (tx_fifo_empty_count != 0x20))) {
		dev_err(&tspi->pdev->dev, "[%s] before setting DMA_EN val=0x%08X\n",
			__func__, val);
		print_registers(tspi);
	}
#endif
#endif
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 10;
#endif
	spi_tegra_writel(tspi, val, SLINK_DMA_CTL);
#ifdef DEBUG_962049
	g_spi_tegra_start_cpu_based_transfer_debug_flag = 11;
#endif
	return 0;
}

static void set_best_clk_source(struct spi_tegra_data *tspi,
		unsigned long speed)
{
	long new_rate;
	unsigned long err_rate;
	int rate = speed * 4;
	unsigned int fin_err = speed * 4;
	int final_index = -1;
	int count;
	int ret;
	struct clk *pclk;
	unsigned long prate, crate, nrate;
	unsigned long cdiv;

	if (!tspi->parent_clk_count || !tspi->parent_clk_list)
		return;

	/* make sure divisor is more than min_div */
	pclk = clk_get_parent(tspi->clk);
	prate = clk_get_rate(pclk);
	crate = clk_get_rate(tspi->clk);
	cdiv = DIV_ROUND_UP(prate, crate);
	if (cdiv < tspi->min_div) {
		nrate = DIV_ROUND_UP(prate, tspi->min_div);
		clk_set_rate(tspi->clk, nrate);
	}

	for (count = 0; count < tspi->parent_clk_count; ++count) {
		if (!tspi->parent_clk_list[count].parent_clk)
			continue;
		ret = clk_set_parent(tspi->clk,
			tspi->parent_clk_list[count].parent_clk);
		if (ret < 0) {
			dev_warn(&tspi->pdev->dev, "Error in setting parent "
				" clk src %s\n",
				tspi->parent_clk_list[count].name);
			continue;
		}

		new_rate = clk_round_rate(tspi->clk, rate);
		if (new_rate < 0)
			continue;

		err_rate = abs(new_rate - rate);
		if (err_rate < fin_err) {
			final_index = count;
			fin_err = err_rate;
		}
	}

	if (final_index >= 0) {
		dev_info(&tspi->pdev->dev, "Setting clk_src %s\n",
				tspi->parent_clk_list[final_index].name);
		clk_set_parent(tspi->clk,
			tspi->parent_clk_list[final_index].parent_clk);
	}
}

#ifdef WORKAROUND_962049
static void spi_tegra_curr_transfer_complete(struct spi_tegra_data *tspi, unsigned err, unsigned cur_xfer_size, unsigned long *irq_flags);
#endif

static void spi_tegra_start_transfer(struct spi_device *spi,
		    struct spi_transfer *t, bool is_first_of_msg,
		    bool is_single_xfer)
{
#ifdef DEBUG_SPI_MASTER_OOPS
	struct spi_tegra_data *tspi = NULL;
#else
	struct spi_tegra_data *tspi = spi_master_get_devdata(spi->master);
#endif
	u32 speed;
	u8 bits_per_word;
	unsigned total_fifo_words;
	int ret;
#ifdef DEBUG_SPI_MASTER_OOPS
	struct tegra_spi_device_controller_data *cdata = NULL;
#else
	struct tegra_spi_device_controller_data *cdata = spi->controller_data;
#endif
	unsigned long command;
	unsigned long command2;
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	unsigned long status2;
#endif
	int cs_setup_count;
	int cs_hold_count;

	unsigned int cs_pol_bit[] = {
			SLINK_CS_POLARITY,
			SLINK_CS_POLARITY1,
			SLINK_CS_POLARITY2,
			SLINK_CS_POLARITY3,
	};

#ifdef DEBUG_SPI_MASTER_OOPS
	/* For SPI master Oops debug */
	if ( spi == NULL ) {
		printk("[SPI_TEGRA] spi == NULL\n");
		BUG_ON(1);
	}

	if ( spi->master == NULL ) {
		dev_info(&spi->dev, "[SPI_TEGRA] spi->master == NULL, [0x%x][0x%x][0x%x]\n", spi, spi->master, spi->dev);
		dev_info(&spi->dev, "[SPI_TEGRA] [%s],[%d],[%d]\n", spi->modalias, spi->max_speed_hz, spi->chip_select);
#ifndef DEBUG_960388
		BUG_ON(1);
#endif
	}

#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(spi->master);
#else
	tspi = spi_master_get_devdata(spi->master);
#endif
	cdata = spi->controller_data;
#endif

	bits_per_word = t->bits_per_word ? t->bits_per_word :
					spi->bits_per_word;

	speed = t->speed_hz ? t->speed_hz : spi->max_speed_hz;
	if (speed != tspi->cur_speed) {
		set_best_clk_source(tspi, speed);
		clk_set_rate(tspi->clk, speed * 4);
		tspi->cur_speed = speed;
	}

	tspi->cur = t;
	tspi->cur_spi = spi;
	tspi->cur_pos = 0;
	tspi->cur_rx_pos = 0;
	tspi->cur_tx_pos = 0;
	tspi->rx_complete = 0;
	tspi->tx_complete = 0;
	total_fifo_words = spi_tegra_calculate_curr_xfer_param(spi, tspi, t);

	command2 = tspi->def_command2_reg;
	if (is_first_of_msg) {
		if (!tspi->is_clkon_always) {
			if (!tspi->clk_state) {
				pm_runtime_get_sync(&tspi->pdev->dev);
				tspi->clk_state = 1;
			}
		}

		spi_tegra_clear_status(tspi);

		command = tspi->def_command_reg;
		command |= SLINK_BIT_LENGTH(bits_per_word - 1);

		/* possibly use the hw based chip select */
		tspi->is_hw_based_cs = false;
		if (cdata && cdata->is_hw_based_cs && is_single_xfer) {
			if ((tspi->curr_dma_words * tspi->bytes_per_word) ==
						(t->len - tspi->cur_pos)) {
				cs_setup_count = cdata->cs_setup_clk_count >> 1;
				if (cs_setup_count > 3)
					cs_setup_count = 3;
				cs_hold_count = cdata->cs_hold_clk_count;
				if (cs_hold_count > 0xF)
					cs_hold_count = 0xF;
				tspi->is_hw_based_cs = true;

				command &= ~SLINK_CS_SW;
				command2 &= ~SLINK_SS_SETUP(3);
				command2 |= SLINK_SS_SETUP(cs_setup_count);
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
				status2 = spi_tegra_readl(tspi, SLINK_STATUS2);
				status2 &= ~SLINK_SS_HOLD_TIME(0xF);
				status2 |= SLINK_SS_HOLD_TIME(cs_hold_count);
				spi_tegra_writel(tspi, status2, SLINK_STATUS2);
#endif
			}
		}
		if (!tspi->is_hw_based_cs) {
			command |= SLINK_CS_SW;
			command ^= cs_pol_bit[spi->chip_select];
		}

		command &= ~SLINK_IDLE_SCLK_MASK & ~SLINK_CK_SDA;
		if (spi->mode & SPI_CPHA)
			command |= SLINK_CK_SDA;

		if (spi->mode & SPI_CPOL)
			command |= SLINK_IDLE_SCLK_DRIVE_HIGH;
		else
			command |= SLINK_IDLE_SCLK_DRIVE_LOW;
	} else {
		command = tspi->command_reg;
		command &= ~SLINK_BIT_LENGTH(~0);
		command |= SLINK_BIT_LENGTH(bits_per_word - 1);
	}

	spi_tegra_writel(tspi, command, SLINK_COMMAND);
	tspi->command_reg = command;

	dev_dbg(&tspi->pdev->dev, "The def 0x%x and written 0x%lx\n",
				tspi->def_command_reg, command);

	command2 &= ~(SLINK_SS_EN_CS(~0) | SLINK_RXEN | SLINK_TXEN);
	tspi->cur_direction = 0;
	if (t->rx_buf) {
		command2 |= SLINK_RXEN;
		tspi->cur_direction |= DATA_DIR_RX;
	}
	if (t->tx_buf) {
		command2 |= SLINK_TXEN;
		tspi->cur_direction |= DATA_DIR_TX;
	}
	command2 |= SLINK_SS_EN_CS(spi->chip_select);
	spi_tegra_writel(tspi, command2, SLINK_COMMAND2);
	tspi->command2_reg = command2;

#ifdef WORKAROUND_962049
	if (total_fifo_words > SPI_FIFO_DEPTH) {
		ret = spi_tegra_start_dma_based_transfer(tspi, t);

		 if ( ret == -EIO ) {
			unsigned long flags;
			pr_err("[%s] SLINK_TX_FULL debug ret=[%d]\n", __func__, ret);
			spin_lock_irqsave(&tspi->lock, flags);
			spi_tegra_curr_transfer_complete(tspi, true, t->len, &flags);
			spin_unlock_irqrestore(&tspi->lock, flags);
		}
	}
#else
	if (total_fifo_words > SPI_FIFO_DEPTH)
		ret = spi_tegra_start_dma_based_transfer(tspi, t);
#endif
	else
		ret = spi_tegra_start_cpu_based_transfer(tspi, t);
	WARN_ON(ret < 0);
}

static int spi_tegra_setup(struct spi_device *spi)
{
#ifdef DEBUG_960388
	struct spi_tegra_data *tspi = spi_master_get_devdata_debug(spi->master);
#else
	struct spi_tegra_data *tspi = spi_master_get_devdata(spi->master);
#endif
	unsigned long cs_bit;
	unsigned long val;
	unsigned long flags;

	dev_dbg(&spi->dev, "setup %d bpw, %scpol, %scpha, %dHz\n",
		spi->bits_per_word,
		spi->mode & SPI_CPOL ? "" : "~",
		spi->mode & SPI_CPHA ? "" : "~",
		spi->max_speed_hz);

	BUG_ON(spi->chip_select >= MAX_CHIP_SELECT);
	switch (spi->chip_select) {
	case 0:
		cs_bit = SLINK_CS_POLARITY;
		break;

	case 1:
		cs_bit = SLINK_CS_POLARITY1;
		break;

	case 2:
		cs_bit = SLINK_CS_POLARITY2;
		break;

	case 3:
		cs_bit = SLINK_CS_POLARITY3;
		break;

	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&tspi->lock, flags);
	val = tspi->def_command_reg;
	if (spi->mode & SPI_CS_HIGH)
		val |= cs_bit;
	else
		val &= ~cs_bit;
	tspi->def_command_reg |= val;

	if (!tspi->is_clkon_always && !tspi->clk_state) {
		spin_unlock_irqrestore(&tspi->lock, flags);
		pm_runtime_get_sync(&tspi->pdev->dev);
		spin_lock_irqsave(&tspi->lock, flags);
		tspi->clk_state = 1;
	}
	spi_tegra_writel(tspi, tspi->def_command_reg, SLINK_COMMAND);
	if (!tspi->is_clkon_always && tspi->clk_state) {
		tspi->clk_state = 0;
		spin_unlock_irqrestore(&tspi->lock, flags);
		pm_runtime_put_sync(&tspi->pdev->dev);
	} else
		spin_unlock_irqrestore(&tspi->lock, flags);
	return 0;
}

static void tegra_spi_transfer_work(struct work_struct *work)
{
	struct spi_tegra_data *tspi;
	struct spi_device *spi;
	struct spi_message *m;
	struct spi_transfer *t;
	int single_xfer = 0;
	unsigned long flags;

	tspi = container_of(work, struct spi_tegra_data, spi_transfer_work);

#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s+\n",  __func__);

	g_tegra_spi_transfer_work_debug_flag = 1;
#endif

	spin_lock_irqsave(&tspi->lock, flags);
#ifdef DEBUG_962049
	g_tegra_spi_transfer_work_debug_flag = 2;
#endif

	if (tspi->is_transfer_in_progress || tspi->is_suspended) {
		spin_unlock_irqrestore(&tspi->lock, flags);
#ifdef DEBUG_962049
		if ( g_enable_debug_962049 && tspi->pdev->id == 0)
			dev_info(&tspi->pdev->dev, "[debug_962049]%s-, tspi->is_transfer_in_progress=[%d],tspi->is_suspended=[%d]\n",  __func__, tspi->is_transfer_in_progress, tspi->is_suspended);
		g_tegra_spi_transfer_work_debug_flag = 3;
#endif
		return;
	}
	if (list_empty(&tspi->queue)) {
#ifdef DEBUG_962049
		if ( g_enable_debug_962049 && tspi->pdev->id == 0)
			dev_info(&tspi->pdev->dev, "[debug_962049]%s-, list_empty\n",  __func__);
		g_tegra_spi_transfer_work_debug_flag = 4;
#endif
		spin_unlock_irqrestore(&tspi->lock, flags);
		return;
	}

	m = list_first_entry(&tspi->queue, struct spi_message, queue);
#ifdef DEBUG_962049
	g_tegra_spi_transfer_work_debug_flag = 5;
#endif
	spi = m->state;
	single_xfer = list_is_singular(&m->transfers);
	m->actual_length = 0;
	m->status = 0;
	t = list_first_entry(&m->transfers, struct spi_transfer, transfer_list);
	tspi->is_transfer_in_progress = true;
#ifdef DEBUG_962049
	g_tegra_spi_transfer_work_debug_flag = 6;
#endif
	spin_unlock_irqrestore(&tspi->lock, flags);
#ifdef DEBUG_962049
	g_tegra_spi_transfer_work_debug_flag = 7;
#endif
	spi_tegra_start_transfer(spi, t, true, single_xfer);
#ifdef DEBUG_962049
	g_tegra_spi_transfer_work_debug_flag = 8;
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s-\n",  __func__);
#endif
}

static int spi_tegra_transfer(struct spi_device *spi, struct spi_message *m)
{
#ifdef DEBUG_960388
	struct spi_tegra_data *tspi = spi_master_get_devdata_debug(spi->master);
#else
	struct spi_tegra_data *tspi = spi_master_get_devdata(spi->master);
#endif
	struct spi_transfer *t;
	unsigned long flags;
	int was_empty;
	int bytes_per_word;
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s+\n",  __func__);

	g_spi_tegra_transfer_debug_flag = 1;
#endif

	if (list_empty(&m->transfers) || !m->complete)
		return -EINVAL;

#ifdef DEBUG_962049
	g_spi_tegra_transfer_debug_flag = 2;
#endif

	list_for_each_entry(t, &m->transfers, transfer_list) {
		if (t->bits_per_word > 32) // t->bits_per_word never < 0
			return -EINVAL;

		if (t->len == 0)
			return -EINVAL;

		/* Check that the all words are available */
		if (t->bits_per_word)
			bytes_per_word = (t->bits_per_word + 7)/8;
		else
			bytes_per_word = (spi->bits_per_word + 7)/8;

		if (t->len % bytes_per_word != 0)
			return -EINVAL;

		if (!t->rx_buf && !t->tx_buf)
			return -EINVAL;
	}

#ifdef DEBUG_962049
	g_spi_tegra_transfer_debug_flag = 3;
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s: lock\n",  __func__);
#endif

	spin_lock_irqsave(&tspi->lock, flags);

#ifdef DEBUG_962049
	g_spi_tegra_transfer_debug_flag = 4;
#endif

	if (WARN_ON(tspi->is_suspended)) {
		spin_unlock_irqrestore(&tspi->lock, flags);
#ifdef DEBUG_962049
		if ( g_enable_debug_962049 && tspi->pdev->id == 0)
			dev_info(&tspi->pdev->dev, "[debug_962049]%s-, tspi->is_suspended\n",  __func__);

		g_spi_tegra_transfer_debug_flag = 5;
#endif
		return -EBUSY;
	}

	m->state = spi;
	was_empty = list_empty(&tspi->queue);
	list_add_tail(&m->queue, &tspi->queue);

#ifdef DEBUG_962049
	g_spi_tegra_transfer_debug_flag = 6;
#endif

	if (was_empty)
		queue_work(tspi->spi_workqueue, &tspi->spi_transfer_work);

#ifdef DEBUG_962049
	g_spi_tegra_transfer_debug_flag = 7;
#endif

	spin_unlock_irqrestore(&tspi->lock, flags);
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s-\n",  __func__);
	g_spi_tegra_transfer_debug_flag = 8;
#endif
	return 0;
}

static void spi_tegra_curr_transfer_complete(struct spi_tegra_data *tspi,
	unsigned err, unsigned cur_xfer_size, unsigned long *irq_flags)
{
	struct spi_message *m;
	struct spi_device *spi;
	struct spi_transfer *t;
	int single_xfer = 0;

#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s+\n",  __func__);
	g_spi_tegra_curr_transfer_complete_debug_flag = 1;
#endif

	/* Check if CS need to be toggele here */
	if (tspi->cur && tspi->cur->cs_change &&
				tspi->cur->delay_usecs) {
		udelay(tspi->cur->delay_usecs);
	}

#ifdef SOLUTION_967438
	/* Check queue list to make sure it is not empty */
	if( list_empty(&tspi->queue) ) {
		pr_err("[%s] list_empty(&tspi->queue)\n", __func__);
		return;
	}
#endif

	m = list_first_entry(&tspi->queue, struct spi_message, queue);
	if (err)
		m->status = -EIO;
	spi = m->state;

	m->actual_length += cur_xfer_size;

	if ( (tspi->cur != NULL) && (!list_is_last(&tspi->cur->transfer_list, &m->transfers)) ) {
#ifdef DEBUG_962049
		g_spi_tegra_curr_transfer_complete_debug_flag = 2;
#endif
		tspi->cur = list_first_entry(&tspi->cur->transfer_list,
			struct spi_transfer, transfer_list);
		spin_unlock_irqrestore(&tspi->lock, *irq_flags);
#ifdef DEBUG_962049
		g_spi_tegra_curr_transfer_complete_debug_flag = 3;
#endif
		spi_tegra_start_transfer(spi, tspi->cur, false, 0);
#ifdef DEBUG_962049
		g_spi_tegra_curr_transfer_complete_debug_flag = 4;
#endif
		spin_lock_irqsave(&tspi->lock, *irq_flags);
#ifdef DEBUG_962049
		g_spi_tegra_curr_transfer_complete_debug_flag = 5;
#endif
	} else {
		list_del(&m->queue);
		tspi->cur = 0;
		m->complete(m->context);
		if (!list_empty(&tspi->queue)) {
			if (tspi->is_suspended) {
				spi_tegra_writel(tspi, tspi->def_command_reg,
						SLINK_COMMAND);
				spi_tegra_writel(tspi, tspi->def_command2_reg,
						SLINK_COMMAND2);
				tspi->is_transfer_in_progress = false;
#ifdef DEBUG_962049
				if ( g_enable_debug_962049 && tspi->pdev->id == 0)
					dev_info(&tspi->pdev->dev, "[debug_962049]%s-, tspi->is_suspended\n",  __func__);

				g_spi_tegra_curr_transfer_complete_debug_flag = 6;
#endif
				return;
			}
			m = list_first_entry(&tspi->queue, struct spi_message,
				queue);
			spi = m->state;
			single_xfer = list_is_singular(&m->transfers);
			m->actual_length = 0;
			m->status = 0;

			t = list_first_entry(&m->transfers, struct spi_transfer,
						transfer_list);
#ifdef DEBUG_962049
			g_spi_tegra_curr_transfer_complete_debug_flag = 7;
#endif
			spin_unlock_irqrestore(&tspi->lock, *irq_flags);
#ifdef DEBUG_962049
			g_spi_tegra_curr_transfer_complete_debug_flag = 8;
#endif
			spi_tegra_start_transfer(spi, t, true, single_xfer);
#ifdef DEBUG_962049
			g_spi_tegra_curr_transfer_complete_debug_flag = 9;
#endif
			spin_lock_irqsave(&tspi->lock, *irq_flags);
#ifdef DEBUG_962049
			g_spi_tegra_curr_transfer_complete_debug_flag = 10;
#endif
		} else {
			spi_tegra_writel(tspi, tspi->def_command_reg,
								SLINK_COMMAND);
			spi_tegra_writel(tspi, tspi->def_command2_reg,
								SLINK_COMMAND2);
#ifdef DEBUG_962049
			g_spi_tegra_curr_transfer_complete_debug_flag = 11;
#endif
			if (!tspi->is_clkon_always) {
				if (tspi->clk_state) {
					/* Provide delay to stablize the signal
					   state */
#ifdef DEBUG_962049
					g_spi_tegra_curr_transfer_complete_debug_flag = 12;
#endif
					spin_unlock_irqrestore(&tspi->lock,
							*irq_flags);
					udelay(10);
#ifdef DEBUG_962049
					g_spi_tegra_curr_transfer_complete_debug_flag = 13;
#endif
					pm_runtime_put_sync(&tspi->pdev->dev);
#ifdef DEBUG_962049
					g_spi_tegra_curr_transfer_complete_debug_flag = 14;
#endif
					spin_lock_irqsave(&tspi->lock,
							*irq_flags);
#ifdef DEBUG_962049
					g_spi_tegra_curr_transfer_complete_debug_flag = 15;
#endif
					tspi->clk_state = 0;
				}
			}
			tspi->is_transfer_in_progress = false;
#ifdef DEBUG_962049
			g_spi_tegra_curr_transfer_complete_debug_flag = 16;
#endif
			/* Check if any new request has come between
			 * clock disable */
			queue_work(tspi->spi_workqueue,
					&tspi->spi_transfer_work);
		}
	}
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s-\n",  __func__);
	g_spi_tegra_curr_transfer_complete_debug_flag = 17;
#endif
	return;
}

static void tegra_spi_tx_dma_complete(struct tegra_dma_req *req)
{
	struct spi_tegra_data *tspi = req->dev;
	//dump_stack();
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s\n",  __func__);
#endif
	complete(&tspi->tx_dma_complete);
}

static void tegra_spi_rx_dma_complete(struct tegra_dma_req *req)
{
	struct spi_tegra_data *tspi = req->dev;
#ifdef DEBUG_962049
	if ( g_enable_debug_962049 && tspi->pdev->id == 0)
		dev_info(&tspi->pdev->dev, "[debug_962049]%s\n",  __func__);
#endif
	complete(&tspi->rx_dma_complete);
}

static void handle_cpu_based_xfer(void *context_data)
{
	struct spi_tegra_data *tspi = context_data;
	struct spi_transfer *t = tspi->cur;
	unsigned long flags;

	spin_lock_irqsave(&tspi->lock, flags);
	if (tspi->tx_status ||  tspi->rx_status ||
				(tspi->status_reg & SLINK_BSY)) {
		dev_err(&tspi->pdev->dev, "%s ERROR bit set 0x%x\n",
					 __func__, tspi->status_reg);
		tegra_periph_reset_assert(tspi->clk);
		udelay(2);
		tegra_periph_reset_deassert(tspi->clk);
		WARN_ON(1);
		spi_tegra_curr_transfer_complete(tspi,
			tspi->tx_status ||  tspi->rx_status, t->len, &flags);
		goto exit;
	}

	dev_vdbg(&tspi->pdev->dev, " Current direction %x\n",
						tspi->cur_direction);
	if (tspi->cur_direction & DATA_DIR_RX)
		spi_tegra_read_rx_fifo_to_client_rxbuf(tspi, t);

	if (tspi->cur_direction & DATA_DIR_TX)
		tspi->cur_pos = tspi->cur_tx_pos;
	else if (tspi->cur_direction & DATA_DIR_RX)
		tspi->cur_pos = tspi->cur_rx_pos;
	else
		WARN_ON(1);

	dev_vdbg(&tspi->pdev->dev, "current position %d and length of the "
				"transfer %d\n", tspi->cur_pos, t->len);
	if (tspi->cur_pos == t->len) {
		spi_tegra_curr_transfer_complete(tspi,
			tspi->tx_status || tspi->rx_status, t->len, &flags);
		goto exit;
	}

	spi_tegra_calculate_curr_xfer_param(tspi->cur_spi, tspi, t);
	spi_tegra_start_cpu_based_transfer(tspi, t);
exit:
	spin_unlock_irqrestore(&tspi->lock, flags);
	return;
}

static irqreturn_t spi_tegra_isr_thread(int irq, void *context_data)
{
	struct spi_tegra_data *tspi = context_data;
	struct spi_transfer *t = tspi->cur;
	long wait_status;
	int err = 0;
	unsigned total_fifo_words;
	unsigned long flags;

	if (t == NULL)
		return IRQ_HANDLED;

	if (!tspi->is_curr_dma_xfer) {
		handle_cpu_based_xfer(context_data);
		return IRQ_HANDLED;
	}

	/* Abort dmas if any error */
	if (tspi->cur_direction & DATA_DIR_TX) {
		if (tspi->tx_status) {
			tegra_dma_dequeue(tspi->tx_dma);
			err += 1;
		} else {
			wait_status = wait_for_completion_interruptible_timeout(
				&tspi->tx_dma_complete, SLINK_DMA_TIMEOUT);

			if (wait_status == 0) {
				tegra_dma_dequeue_req(tspi->tx_dma,
				&tspi->tx_dma_req);
				dev_err(&tspi->pdev->dev, "timeout in Dma Tx "
				"transfer\n");
				err += 1;
			}

			if (wait_status <= 0) {
				tegra_dma_dequeue(tspi->tx_dma);
				dev_err(&tspi->pdev->dev, "Error in Dma Tx "
							"transfer\n");
				err += 1;
			}
		}
	}

	if (tspi->cur_direction & DATA_DIR_RX) {
		if (tspi->rx_status) {
			tegra_dma_dequeue(tspi->rx_dma);
			err += 2;
		} else {
			wait_status = wait_for_completion_interruptible_timeout(
				&tspi->rx_dma_complete, SLINK_DMA_TIMEOUT);
			if (wait_status <= 0) {
				tegra_dma_dequeue(tspi->rx_dma);
				dev_err(&tspi->pdev->dev, "Error in Dma Rx "
							"transfer\n");
				err += 2;
			}
		}
	}

	spin_lock_irqsave(&tspi->lock, flags);
	if (err) {
		dev_err(&tspi->pdev->dev, "%s ERROR bit set 0x%x\n",
					 __func__, tspi->status_reg);
		tegra_periph_reset_assert(tspi->clk);
		udelay(2);
		tegra_periph_reset_deassert(tspi->clk);
		WARN_ON(1);
		spi_tegra_curr_transfer_complete(tspi, err, t->len, &flags);
		spin_unlock_irqrestore(&tspi->lock, flags);
		return IRQ_HANDLED;
	}

	if (tspi->cur_direction & DATA_DIR_RX)
		spi_tegra_copy_spi_rxbuf_to_client_rxbuf(tspi, t);

	if (tspi->cur_direction & DATA_DIR_TX)
		tspi->cur_pos = tspi->cur_tx_pos;
	else if (tspi->cur_direction & DATA_DIR_RX)
		tspi->cur_pos = tspi->cur_rx_pos;
	else
		WARN_ON(1);

	if (tspi->cur_pos == t->len) {
		spi_tegra_curr_transfer_complete(tspi,
			tspi->tx_status || tspi->rx_status, t->len, &flags);
		spin_unlock_irqrestore(&tspi->lock, flags);
		return IRQ_HANDLED;
	}

	/* Continue transfer in current message */
	total_fifo_words = spi_tegra_calculate_curr_xfer_param(tspi->cur_spi,
							tspi, t);
	if (total_fifo_words > SPI_FIFO_DEPTH)
		err = spi_tegra_start_dma_based_transfer(tspi, t);
	else
		err = spi_tegra_start_cpu_based_transfer(tspi, t);

	spin_unlock_irqrestore(&tspi->lock, flags);
	WARN_ON(err < 0);
	return IRQ_HANDLED;
}

static irqreturn_t spi_tegra_isr(int irq, void *context_data)
{
	struct spi_tegra_data *tspi = context_data;

	tspi->status_reg = spi_tegra_readl(tspi, SLINK_STATUS);
	if (tspi->cur_direction & DATA_DIR_TX)
		tspi->tx_status = tspi->status_reg &
					(SLINK_TX_OVF | SLINK_TX_UNF);

	if (tspi->cur_direction & DATA_DIR_RX)
		tspi->rx_status = tspi->status_reg &
					(SLINK_RX_OVF | SLINK_RX_UNF);
	spi_tegra_clear_status(tspi);


	return IRQ_WAKE_THREAD;
}

static int __init spi_tegra_probe(struct platform_device *pdev)
{
	struct spi_master	*master;
	struct spi_tegra_data	*tspi;
	struct resource		*r;
	struct tegra_spi_platform_data *pdata = pdev->dev.platform_data;
	int ret, spi_irq;
	int i;
	char spi_wq_name[20];

	master = spi_alloc_master(&pdev->dev, sizeof *tspi);
	if (master == NULL) {
		dev_err(&pdev->dev, "master allocation failed\n");
		return -ENOMEM;
	}

#ifdef DEBUG_SPI_MASTER_OOPS
	dev_info(&pdev->dev, "master=[0x%x]\n", master);
#endif

#ifdef DEBUG_960388
	tegra_masters[pdev->id] = master;
	tegra_master_devices[pdev->id] = &master->dev;
	tegra_master_device_privates[pdev->id] = master->dev.p;
#endif

	/* the spi->mode bits understood by this driver: */
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;

	if (pdev->id != -1)
		master->bus_num = pdev->id;

	master->setup = spi_tegra_setup;
	master->transfer = spi_tegra_transfer;
	master->num_chipselect = MAX_CHIP_SELECT;

	dev_set_drvdata(&pdev->dev, master);
#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(master);
#else
	tspi = spi_master_get_devdata(master);
#endif
	tspi->master = master;
	tspi->pdev = pdev;
	tspi->is_transfer_in_progress = false;
	tspi->is_suspended = false;
	spin_lock_init(&tspi->lock);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL) {
		ret = -ENODEV;
		goto fail_no_mem;
	}

	if (!request_mem_region(r->start, (r->end - r->start) + 1,
				dev_name(&pdev->dev))) {
		ret = -EBUSY;
		goto fail_no_mem;
	}

	tspi->phys = r->start;
	tspi->base = ioremap(r->start, r->end - r->start + 1);
	if (!tspi->base) {
		dev_err(&pdev->dev, "can't ioremap iomem\n");
		ret = -ENOMEM;
		goto fail_io_map;
	}

	spi_irq = platform_get_irq(pdev, 0);
	if (unlikely(spi_irq < 0)) {
		dev_err(&pdev->dev, "can't find irq resource\n");
		ret = -ENXIO;
		goto fail_irq_req;
	}
	tspi->irq = spi_irq;

	sprintf(tspi->port_name, "tegra_spi_%d", pdev->id);
	ret = request_threaded_irq(tspi->irq, spi_tegra_isr,
			spi_tegra_isr_thread, IRQF_ONESHOT,
			tspi->port_name, tspi);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register ISR for IRQ %d\n",
					tspi->irq);
		goto fail_irq_req;
	}

#ifdef CONFIG_SERIAL_SC8800G
	tspi->clk = clk_get(&pdev->dev, "spi");
#else
	tspi->clk = clk_get(&pdev->dev, NULL);
#endif

	if (IS_ERR(tspi->clk)) {
		dev_err(&pdev->dev, "can not get clock\n");
		ret = PTR_ERR(tspi->clk);
		goto fail_clk_get;
	}

#ifdef CONFIG_SERIAL_SC8800G
	tspi->sclk = clk_get(&pdev->dev, "sclk");
	if (IS_ERR(tspi->sclk)) {
		dev_err(&pdev->dev, "can not get sclock\n");
		ret = PTR_ERR(tspi->sclk);
		goto fail_sclk_get;
	}
#endif
	INIT_LIST_HEAD(&tspi->queue);

	if (pdata) {
		tspi->is_clkon_always = pdata->is_clkon_always;
		tspi->is_dma_allowed = pdata->is_dma_based;
		tspi->dma_buf_size = (pdata->max_dma_buffer) ?
				pdata->max_dma_buffer : DEFAULT_SPI_DMA_BUF_LEN;
		tspi->parent_clk_count = pdata->parent_clk_count;
		tspi->parent_clk_list = pdata->parent_clk_list;
		tspi->max_rate = pdata->max_rate;
	} else {
		tspi->is_clkon_always = false;
		tspi->is_dma_allowed = true;
		tspi->dma_buf_size = DEFAULT_SPI_DMA_BUF_LEN;
		tspi->parent_clk_count = 0;
		tspi->parent_clk_list = NULL;
		tspi->max_rate = 0;
	}

	tspi->max_parent_rate = 0;
	tspi->min_div = 0;

	if (tspi->parent_clk_count) {
		tspi->max_parent_rate = tspi->parent_clk_list[0].fixed_clk_rate;
		for (i = 1; i < tspi->parent_clk_count; ++i) {
			tspi->max_parent_rate = max(tspi->max_parent_rate,
				tspi->parent_clk_list[i].fixed_clk_rate);
		}
		if (tspi->max_rate)
			tspi->min_div = DIV_ROUND_UP(tspi->max_parent_rate,
						tspi->max_rate);
	}
	tspi->max_buf_size = SLINK_FIFO_DEPTH << 2;

	if (!tspi->is_dma_allowed)
		goto skip_dma_alloc;

	init_completion(&tspi->tx_dma_complete);
	init_completion(&tspi->rx_dma_complete);


	tspi->rx_dma = tegra_dma_allocate_channel(TEGRA_DMA_MODE_ONESHOT,
				"spi_rx_%d", pdev->id);
	if (!tspi->rx_dma) {
		dev_err(&pdev->dev, "can not allocate rx dma channel\n");
		ret = -ENODEV;
		goto fail_rx_dma_alloc;
	}

	tspi->rx_buf = dma_alloc_coherent(&pdev->dev, tspi->dma_buf_size,
					 &tspi->rx_buf_phys, GFP_KERNEL);
	if (!tspi->rx_buf) {
		dev_err(&pdev->dev, "can not allocate rx bounce buffer\n");
		ret = -ENOMEM;
		goto fail_rx_buf_alloc;
	}

	memset(&tspi->rx_dma_req, 0, sizeof(struct tegra_dma_req));
	tspi->rx_dma_req.complete = tegra_spi_rx_dma_complete;
	tspi->rx_dma_req.to_memory = 1;
	tspi->rx_dma_req.dest_addr = tspi->rx_buf_phys;
	tspi->rx_dma_req.virt_addr = tspi->rx_buf;
	tspi->rx_dma_req.dest_bus_width = 32;
	tspi->rx_dma_req.source_addr = tspi->phys + SLINK_RX_FIFO;
	tspi->rx_dma_req.source_bus_width = 32;
	tspi->rx_dma_req.source_wrap = 4;
	tspi->rx_dma_req.dest_wrap = 0;
	tspi->rx_dma_req.req_sel = spi_tegra_req_sels[pdev->id];
	tspi->rx_dma_req.dev = tspi;

	tspi->tx_dma = tegra_dma_allocate_channel(TEGRA_DMA_MODE_ONESHOT,
				"spi_tx_%d", pdev->id);
	if (!tspi->tx_dma) {
		dev_err(&pdev->dev, "can not allocate tx dma channel\n");
		ret = -ENODEV;
		goto fail_tx_dma_alloc;
	}

	tspi->tx_buf = dma_alloc_coherent(&pdev->dev, tspi->dma_buf_size,
					 &tspi->tx_buf_phys, GFP_KERNEL);
	if (!tspi->tx_buf) {
		dev_err(&pdev->dev, "can not allocate tx bounce buffer\n");
		ret = -ENOMEM;
		goto fail_tx_buf_alloc;
	}

	memset(&tspi->tx_dma_req, 0, sizeof(struct tegra_dma_req));
	tspi->tx_dma_req.complete = tegra_spi_tx_dma_complete;
	tspi->tx_dma_req.to_memory = 0;
	tspi->tx_dma_req.dest_addr = tspi->phys + SLINK_TX_FIFO;
	tspi->tx_dma_req.virt_addr = tspi->tx_buf;
	tspi->tx_dma_req.dest_bus_width = 32;
	tspi->tx_dma_req.dest_wrap = 4;
	tspi->tx_dma_req.source_wrap = 0;
	tspi->tx_dma_req.source_addr = tspi->tx_buf_phys;
	tspi->tx_dma_req.source_bus_width = 32;
	tspi->tx_dma_req.req_sel = spi_tegra_req_sels[pdev->id];
	tspi->tx_dma_req.dev = tspi;
	tspi->max_buf_size = tspi->dma_buf_size;
	tspi->def_command_reg  = SLINK_CS_SW | SLINK_M_S;
	tspi->def_command2_reg = SLINK_CS_ACTIVE_BETWEEN;

skip_dma_alloc:
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);
	tspi->clk_state = 1;
	ret = spi_register_master(master);
	if (!tspi->is_clkon_always) {
		if (tspi->clk_state) {
			pm_runtime_put_sync(&pdev->dev);
			tspi->clk_state = 0;
		}
	}

	if (ret < 0) {
		dev_err(&pdev->dev, "can not register to master err %d\n", ret);
		goto fail_master_register;
	}

	/* create the workqueue for the kbc path */
	snprintf(spi_wq_name, sizeof(spi_wq_name), "spi_tegra-%d", pdev->id);
	tspi->spi_workqueue = create_singlethread_workqueue(spi_wq_name);
	if (!tspi->spi_workqueue) {
		dev_err(&pdev->dev, "Failed to create work queue\n");
		ret = -ENODEV;
		goto fail_workqueue;
	}

	INIT_WORK(&tspi->spi_transfer_work, tegra_spi_transfer_work);

	printk(KERN_INFO "[SPI] %s, tegra_spi_%d done \n", __func__, pdev->id);
	return ret;

fail_workqueue:
	spi_unregister_master(master);

fail_master_register:
	if (tspi->tx_buf)
		dma_free_coherent(&pdev->dev, tspi->dma_buf_size,
				tspi->tx_buf, tspi->tx_buf_phys);
fail_tx_buf_alloc:
	if (tspi->tx_dma)
		tegra_dma_free_channel(tspi->tx_dma);
fail_tx_dma_alloc:
	if (tspi->rx_buf)
		dma_free_coherent(&pdev->dev, tspi->dma_buf_size,
			  tspi->rx_buf, tspi->rx_buf_phys);
fail_rx_buf_alloc:
	if (tspi->rx_dma)
		tegra_dma_free_channel(tspi->rx_dma);
fail_rx_dma_alloc:
	pm_runtime_disable(&pdev->dev);
#ifdef CONFIG_SERIAL_SC8800G
	clk_put(tspi->sclk);
fail_sclk_get:
#endif
	clk_put(tspi->clk);
fail_clk_get:
	free_irq(tspi->irq, tspi);
fail_irq_req:
	iounmap(tspi->base);
fail_io_map:
	release_mem_region(r->start, (r->end - r->start) + 1);
fail_no_mem:
	spi_master_put(master);
	printk(KERN_INFO "[SPI] %s, tegra_spi_%d error \n", __func__, pdev->id);
	return ret;
}

static int __devexit spi_tegra_remove(struct platform_device *pdev)
{
	struct spi_master	*master;
	struct spi_tegra_data	*tspi;
	struct resource		*r;

	master = dev_get_drvdata(&pdev->dev);
#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(master);
#else
	tspi = spi_master_get_devdata(master);
#endif

#ifdef DEBUG_SPI_MASTER_OOPS
	dev_info(&pdev->dev, "[SPI_TEGRA] spi_tegra_remove~\n");
#endif

	if (tspi->tx_buf)
		dma_free_coherent(&pdev->dev, tspi->dma_buf_size,
				tspi->tx_buf, tspi->tx_buf_phys);
	if (tspi->tx_dma)
		tegra_dma_free_channel(tspi->tx_dma);
	if (tspi->rx_buf)
		dma_free_coherent(&pdev->dev, tspi->dma_buf_size,
			  tspi->rx_buf, tspi->rx_buf_phys);
	if (tspi->rx_dma)
		tegra_dma_free_channel(tspi->rx_dma);

	if (tspi->is_clkon_always) {
		pm_runtime_put_sync(&pdev->dev);
		tspi->clk_state = 0;
	}
	pm_runtime_disable(&pdev->dev);
#ifdef CONFIG_SERIAL_SC8800G
	clk_put(tspi->sclk);
#endif
	clk_put(tspi->clk);
	iounmap(tspi->base);

	destroy_workqueue(tspi->spi_workqueue);

	spi_master_put(master);
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (r != NULL)
		release_mem_region(r->start, (r->end - r->start) + 1);

	return 0;
}

#ifdef CONFIG_PM
static int spi_tegra_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct spi_master	*master;
	struct spi_tegra_data	*tspi;
	unsigned		limit = 50;
	unsigned long flags;

	master = dev_get_drvdata(&pdev->dev);
#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(master);
#else
	tspi = spi_master_get_devdata(master);
#endif
	spin_lock_irqsave(&tspi->lock, flags);

	/* Wait for all transfer completes */
	if (!list_empty(&tspi->queue))
		dev_warn(&pdev->dev, "The transfer list is not empty "
			"Waiting for time %d ms to complete transfer\n",
			limit * 20);

	while (!list_empty(&tspi->queue) && limit--) {
		spin_unlock_irqrestore(&tspi->lock, flags);
		msleep(20);
		spin_lock_irqsave(&tspi->lock, flags);
	}

	/* Wait for current transfer completes only */
	tspi->is_suspended = true;
	if (!list_empty(&tspi->queue)) {
		limit = 50;
		dev_err(&pdev->dev, "All transfer has not completed, "
			"Waiting for %d ms current transfer to complete\n",
			limit * 20);
		while (tspi->is_transfer_in_progress && limit--) {
			spin_unlock_irqrestore(&tspi->lock, flags);
			msleep(20);
			spin_lock_irqsave(&tspi->lock, flags);
		}
	}

	if (tspi->is_transfer_in_progress) {
		dev_err(&pdev->dev, "Spi transfer is in progress "
			"Avoiding suspend\n");
		tspi->is_suspended = false;
		spin_unlock_irqrestore(&tspi->lock, flags);
		return -EBUSY;
	}

	spin_unlock_irqrestore(&tspi->lock, flags);
	if (tspi->is_clkon_always) {
		pm_runtime_put_sync(&pdev->dev);
		tspi->clk_state = 0;
	}
	return 0;
}

static int spi_tegra_resume(struct platform_device *pdev)
{
	struct spi_master	*master;
	struct spi_tegra_data	*tspi;
	struct spi_message *m;
	struct spi_device *spi;
	struct spi_transfer *t = NULL;
	int single_xfer = 0;
	unsigned long flags;

	master = dev_get_drvdata(&pdev->dev);
#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(master);
#else
	tspi = spi_master_get_devdata(master);
#endif

	pm_runtime_get_sync(&pdev->dev);
	tspi->clk_state = 1;
	spi_tegra_writel(tspi, tspi->command_reg, SLINK_COMMAND);
	if (!tspi->is_clkon_always) {
		pm_runtime_put_sync(&pdev->dev);
		tspi->clk_state = 0;
	}
	spin_lock_irqsave(&tspi->lock, flags);

	tspi->cur_speed = 0;
	tspi->is_suspended = false;
	if (!list_empty(&tspi->queue)) {
		m = list_first_entry(&tspi->queue, struct spi_message, queue);
		spi = m->state;
		single_xfer = list_is_singular(&m->transfers);
		m->actual_length = 0;
		m->status = 0;
		t = list_first_entry(&m->transfers, struct spi_transfer,
						transfer_list);
		tspi->is_transfer_in_progress = true;
	}
	spin_unlock_irqrestore(&tspi->lock, flags);
	if (t)
		spi_tegra_start_transfer(spi, t, true, single_xfer);
	return 0;
}
#endif

MODULE_ALIAS("platform:spi_tegra");

#if defined(CONFIG_PM_RUNTIME)

static int tegra_spi_runtime_idle(struct device *dev)
{
	struct spi_master	*master;
	struct spi_tegra_data	*tspi;
	master = dev_get_drvdata(dev);
#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(master);
#else
	tspi = spi_master_get_devdata(master);
#endif

	clk_disable(tspi->clk);
#ifdef CONFIG_SERIAL_SC8800G
	clk_disable(tspi->sclk);
#endif
	return 0;
}

static int tegra_spi_runtime_resume(struct device *dev)
{
	struct spi_master	*master;
	struct spi_tegra_data	*tspi;
	master = dev_get_drvdata(dev);
#ifdef DEBUG_960388
	tspi = spi_master_get_devdata_debug(master);
#else
	tspi = spi_master_get_devdata(master);
#endif

#ifdef CONFIG_SERIAL_SC8800G
	clk_enable(tspi->sclk);
#endif
	clk_enable(tspi->clk);
	return 0;
}

static const struct dev_pm_ops tegra_spi_dev_pm_ops = {
	.runtime_idle = tegra_spi_runtime_idle,
	.runtime_resume = tegra_spi_runtime_resume,
};

#endif

static struct platform_driver spi_tegra_driver = {
	.driver = {
		.name =		"spi_tegra",
		.owner =	THIS_MODULE,
#if defined(CONFIG_PM_RUNTIME)
		.pm =		&tegra_spi_dev_pm_ops,
#endif
	},
	.remove =	__devexit_p(spi_tegra_remove),
#ifdef CONFIG_PM
	.suspend =	spi_tegra_suspend,
	.resume  =	spi_tegra_resume,
#endif
};

static int __init spi_tegra_init(void)
{
	return platform_driver_probe(&spi_tegra_driver, spi_tegra_probe);
}
subsys_initcall(spi_tegra_init);

static void __exit spi_tegra_exit(void)
{
	platform_driver_unregister(&spi_tegra_driver);
}
module_exit(spi_tegra_exit);

MODULE_LICENSE("GPL");
