/*
 * Platform independend driver for JZ4760.
 *
 * Copyright (c) 2007 Ingenic Semiconductor Inc.
 * Author: <jlwei@ingenic.cn>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <common.h>

#if (CONFIG_COMMANDS & CFG_CMD_NAND) &&	defined(CONFIG_JZ4760)
#include <nand.h>
#if defined(CONFIG_JZ4760)
#include <asm/jz4760.h>
#endif

/* Size of ecc parities per 512 bytes, in half-byte */
static int par_size;

static struct nand_oobinfo nand_oob_bch = {
	.useecc = MTD_NANDECC_AUTOPLACE,
	.eccbytes = 52,
	.eccpos = {
		3,  4,  5,  6,  7,  8,  9,  10,
		11, 12, 13, 14, 15, 16, 17, 18,
		19, 20, 21, 22, 23, 24, 25, 26,
		27, 28, 29, 30, 31, 32, 33, 34,
		35, 36, 37, 38, 39, 40, 41, 42,
		43, 44, 45, 46, 47, 48, 49, 50,
		51, 52, 53, 54},
	.oobfree = {{2, 1}, {55, 9}}
};

static void jz_hwcontrol(struct mtd_info *mtd, int cmd)
{
	struct nand_chip *this = (struct nand_chip *)(mtd->priv);
	switch (cmd) {
		case NAND_CTL_SETNCE:
			REG_NEMC_NFCSR |= NEMC_NFCSR_NFCE1;
			break;

		case NAND_CTL_CLRNCE:
			REG_NEMC_NFCSR &= ~NEMC_NFCSR_NFCE1;
			break;

		case NAND_CTL_SETCLE:
				this->IO_ADDR_W = (void __iomem *)((unsigned long)(this->IO_ADDR_W) | NAND_CMD_OFFSET);
			break;

		case NAND_CTL_CLRCLE:
				this->IO_ADDR_W = (void __iomem *)((unsigned long)(this->IO_ADDR_W) & ~NAND_CMD_OFFSET);
			break;

		case NAND_CTL_SETALE:
				this->IO_ADDR_W = (void __iomem *)((unsigned long)(this->IO_ADDR_W) | NAND_ADDR_OFFSET);
			break;

		case NAND_CTL_CLRALE:
				this->IO_ADDR_W = (void __iomem *)((unsigned long)(this->IO_ADDR_W) & ~NAND_ADDR_OFFSET);
			break;
	}
	/* printk("this->IO_ADDR_W=0x%x:cmd=0x%x\n",this->IO_ADDR_W,cmd); */
}

static int jz_device_ready(struct mtd_info *mtd)
{
	int ready;
	udelay(20);	/* FIXME: add 20us delay */
	ready = (REG_GPIO_PXPIN(0) & 0x00100000)? 1 : 0;
	return ready;
}

/*
 * EMC setup
 */
static void jz_device_setup(void)
{
	/* Set NFE bit */
	REG_NEMC_NFCSR |= NEMC_NFCSR_NFE1;
#if CFG_NAND_BW8 == 1
	REG_NEMC_SMCR1 = CFG_NAND_SMCR1;
#else /* 16 bit */
	REG_NEMC_SMCR1 = CFG_NAND_SMCR1 | 0x40;
#endif
}

void board_nand_select_device(struct nand_chip *nand, int chip)
{
	/*
	 * Don't use "chip" to address the NAND device,
	 * generate the cs from the address where it is encoded.
	 */
}

static int jzsoc_nand_calculate_bch_ecc(struct mtd_info *mtd, const u_char * dat, u_char * ecc_code)
{
	struct nand_chip *this = (struct nand_chip *)(mtd->priv);
	volatile u8 *paraddr = (volatile u8 *)BCH_PAR0;
	short i;
#ifdef CFG_NAND_BCH_WITH_OOB
	/* Write data to REG_BCH_DR */
	for (i = 0; i < this->eccsize; i++) {
		REG_BCH_DR = ((struct buf_be_corrected *)dat)->data[i];
	}

	/* Write oob to REG_BCH_DR */
	for (i = 0; i < CFG_NAND_ECC_POS / this->eccsteps; i++) {
		REG_BCH_DR = ((struct buf_be_corrected *)dat)->oob[i];
	}
#else
	/* Write data to REG_BCH_DR */
	for (i = 0; i < this->eccsize; i++) {
		REG_BCH_DR = dat[i];
	}
#endif
	__ecc_encode_sync();
	__ecc_disable();

	for (i = 0; i < (par_size+1)/2; i++) {
		ecc_code[i] = *paraddr++;
	}

	return 0;
}

static void jzsoc_nand_enable_bch_hwecc(struct mtd_info* mtd, int mode)
{
	struct nand_chip *this = (struct nand_chip *)(mtd->priv);

	REG_BCH_INTS = 0xffffffff;

	if (mode == NAND_ECC_READ) {
		if (CFG_NAND_BCH_BIT == 8)
			__ecc_decoding_8bit();
		else
			__ecc_decoding_4bit();
#ifdef CFG_NAND_BCH_WITH_OOB
		__ecc_cnt_dec(2*(this->eccsize + CFG_NAND_ECC_POS / this->eccsteps) + par_size);
#else
		__ecc_cnt_dec(2*this->eccsize + par_size);
#endif
	}

	if (mode == NAND_ECC_WRITE) {
		if (CFG_NAND_BCH_BIT == 8)
			__ecc_encoding_8bit();
		else
			__ecc_encoding_4bit();
#ifdef CFG_NAND_BCH_WITH_OOB
		__ecc_cnt_enc(2*(this->eccsize + CFG_NAND_ECC_POS / this->eccsteps));
#else
		__ecc_cnt_enc(2*(this->eccsize));
#endif
	}
}

/**
 * bch_correct
 * @dat:        data to be corrected
 * @idx:        the index of error bit in an eccsize
 */
static void bch_correct(struct mtd_info *mtd, u8 * dat, int idx)
{
	struct nand_chip *this = (struct nand_chip *)(mtd->priv);
	int i, bit;		/* the 'bit' of i byte is error */
	i = (idx - 1) >> 3;
	bit = (idx - 1) & 0x7;

#ifdef CFG_NAND_BCH_WITH_OOB
	if (i < this->eccsize)
		((struct buf_be_corrected *)dat)->data[i] ^= (1 << bit);
	else if (i <  (this->eccsize + CFG_NAND_ECC_POS / this->eccsteps))
		((struct buf_be_corrected *)dat)->oob[i - this->eccsize] ^= (1 << bit);
#else
	if (i < this->eccsize)
		dat[i] ^= (1 << bit);
#endif
}

/**
 * jzsoc_nand_bch_correct_data:  calc_ecc points to oob_buf for us
 * @mtd:	mtd info structure
 * @dat:        data to be corrected
 * @read_ecc:   pointer to ecc buffer calculated when nand writing
 * @calc_ecc:   no used
 */
static int jzsoc_nand_bch_correct_data(struct mtd_info *mtd, u_char * dat, u_char * read_ecc, u_char * calc_ecc)
{
	struct nand_chip *this = (struct nand_chip *)(mtd->priv);
	short k;
	u32 stat;

#ifdef CFG_NAND_BCH_WITH_OOB
	/* Write data to REG_BCH_DR */
	for (k = 0; k < this->eccsize; k++) {
		REG_BCH_DR = ((struct buf_be_corrected *)dat)->data[k];
	}
	/* Write oob to REG_BCH_DR */
	for (k = 0; k < CFG_NAND_ECC_POS / this->eccsteps; k++) {
		REG_BCH_DR = ((struct buf_be_corrected *)dat)->oob[k];
	}
#else
	/* Write data to REG_BCH_DR */
	for (k = 0; k < this->eccsize; k++) {
		REG_BCH_DR = dat[k];
	}
#endif

	/* Write parities to REG_BCH_DR */
	for (k = 0; k < (par_size+1)/2; k++) {
		REG_BCH_DR = read_ecc[k];
	}

	/* Wait for completion */
	__ecc_decode_sync();
	__ecc_disable();

	/* Check decoding */
	stat = REG_BCH_INTS;

	if (stat & BCH_INTS_ERR) {
		/* Error occurred */
		if (stat & BCH_INTS_UNCOR) {
			printk("NAND: Uncorrectable ECC error--\n");
			return -1;
		} else {
			u32 errcnt = (stat & BCH_INTS_ERRC_MASK) >> BCH_INTS_ERRC_BIT;
			switch (errcnt) {
			case 8:
			  bch_correct(mtd, dat, (REG_BCH_ERR3 & BCH_ERR_INDEX_ODD_MASK) >> BCH_ERR_INDEX_ODD_BIT);
				/* FALL-THROUGH */
			case 7:
			  bch_correct(mtd, dat, (REG_BCH_ERR3 & BCH_ERR_INDEX_EVEN_MASK) >> BCH_ERR_INDEX_EVEN_BIT);
				/* FALL-THROUGH */
			case 6:
			  bch_correct(mtd, dat, (REG_BCH_ERR2 & BCH_ERR_INDEX_ODD_MASK) >> BCH_ERR_INDEX_ODD_BIT);
				/* FALL-THROUGH */
			case 5:
			  bch_correct(mtd, dat, (REG_BCH_ERR2 & BCH_ERR_INDEX_EVEN_MASK) >> BCH_ERR_INDEX_EVEN_BIT);
				/* FALL-THROUGH */
			case 4:
			  bch_correct(mtd, dat, (REG_BCH_ERR1 & BCH_ERR_INDEX_ODD_MASK) >> BCH_ERR_INDEX_ODD_BIT);
				/* FALL-THROUGH */
			case 3:
			  bch_correct(mtd, dat, (REG_BCH_ERR1 & BCH_ERR_INDEX_EVEN_MASK) >> BCH_ERR_INDEX_EVEN_BIT);
				/* FALL-THROUGH */
			case 2:
			  bch_correct(mtd, dat, (REG_BCH_ERR0 & BCH_ERR_INDEX_ODD_MASK) >> BCH_ERR_INDEX_ODD_BIT);
				/* FALL-THROUGH */
			case 1:
			  bch_correct(mtd, dat, (REG_BCH_ERR0 & BCH_ERR_INDEX_EVEN_MASK) >> BCH_ERR_INDEX_EVEN_BIT);;
			  break;
			default:
				break;
			}
		}
	}

	return 0;
}

/*
 * Main initialization routine
 */
void board_nand_init(struct nand_chip *nand)
{
	jz_device_setup();

	if (CFG_NAND_BCH_BIT == 8) {
		par_size = 26;
		nand->eccmode = NAND_ECC_HW13_512;
	} else {
		par_size = 13;
		nand->eccmode = NAND_ECC_HW7_512;
	}

        nand->hwcontrol = jz_hwcontrol;
        nand->dev_ready = jz_device_ready;
	
	nand->correct_data  = jzsoc_nand_bch_correct_data;
	nand->enable_hwecc  = jzsoc_nand_enable_bch_hwecc;
	nand->calculate_ecc = jzsoc_nand_calculate_bch_ecc;

        /* Set address of NAND IO lines */
        nand->IO_ADDR_R = (void __iomem *) CFG_NAND_BASE;
        nand->IO_ADDR_W = (void __iomem *) CFG_NAND_BASE;

        /* 20 us command delay time */
        nand->chip_delay = 20;
//	nand->autooob    = &nand_oob_bch; // init in nand_base.c

	nand->options &= ~NAND_BUSWIDTH_16;
#if CFG_NAND_BW8 == 0
	nand->options |= NAND_BUSWIDTH_16;
#endif

}
#endif /* (CONFIG_COMMANDS & CFG_CMD_NAND) */
