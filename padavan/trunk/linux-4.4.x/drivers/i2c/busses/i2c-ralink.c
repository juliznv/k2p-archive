/*
 * drivers/i2c/busses/i2c-ralink.c
 *
 * Copyright (C) 2013 Steven Liu <steven_liu@mediatek.com>
 * Copyright (C) 2016 Michael Lee <igvtee@gmail.com>
 *
 * Improve driver for i2cdetect from i2c-tools to detect i2c devices on the bus.
 * (C) 2014 Sittisak <sittisaks@hotmail.com>
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

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/clk.h>

#define REG_CONFIG_REG		0x00
#define REG_CLKDIV_REG		0x04
#define REG_DEVADDR_REG		0x08
#define REG_ADDR_REG		0x0C
#define REG_DATAOUT_REG		0x10
#define REG_DATAIN_REG		0x14
#define REG_STATUS_REG		0x18
#define REG_STARTXFR_REG	0x1C
#define REG_BYTECNT_REG		0x20

/* REG_CONFIG_REG */
#define I2C_ADDRLEN_OFFSET	5
#define I2C_DEVADLEN_OFFSET	2
#define I2C_ADDRLEN_MASK	0x3
#define I2C_ADDR_DIS		BIT(1)
#define I2C_DEVADDR_DIS		BIT(0)
#define I2C_ADDRLEN_8		(7 << I2C_ADDRLEN_OFFSET)
#define I2C_DEVADLEN_7		(6 << I2C_DEVADLEN_OFFSET)
#define I2C_CONF_DEFAULT	(I2C_ADDRLEN_8 | I2C_DEVADLEN_7)

/* REG_CLKDIV_REG */
#define I2C_CLKDIV_MASK		0xffff

/* REG_DEVADDR_REG */
#define I2C_DEVADDR_MASK	0x7f

/* REG_ADDR_REG */
#define I2C_ADDR_MASK		0xff

/* REG_STATUS_REG */
#define I2C_STARTERR		BIT(4)
#define I2C_ACKERR		BIT(3)
#define I2C_DATARDY		BIT(2)
#define I2C_SDOEMPTY		BIT(1)
#define I2C_BUSY		BIT(0)

/* REG_STARTXFR_REG */
#define NOSTOP_CMD		BIT(2)
#define NODATA_CMD		BIT(1)
#define READ_CMD		BIT(0)

/* REG_BYTECNT_REG */
#define BYTECNT_MAX		64
#define SET_BYTECNT(x)		(x - 1)

/* timeout waiting for I2C devices to respond (clock streching) */
#define TIMEOUT_MS              1000
#define DELAY_INTERVAL_US       100

struct rt_i2c {
	void __iomem *base;
	struct clk *clk;
	struct device *dev;
	struct i2c_adapter adap;
	u32 cur_clk;
	u32 clk_div;
	u32 flags;
};

static void rt_i2c_w32(struct rt_i2c *i2c, u32 val, unsigned reg)
{
	iowrite32(val, i2c->base + reg);
}

static u32 rt_i2c_r32(struct rt_i2c *i2c, unsigned reg)
{
	return ioread32(i2c->base + reg);
}

static int poll_down_timeout(void __iomem *addr, u32 mask)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(TIMEOUT_MS);

	do {
		if (!(readl_relaxed(addr) & mask))
			return 0;

		usleep_range(DELAY_INTERVAL_US, DELAY_INTERVAL_US + 50);
	} while (time_before(jiffies, timeout));

	return (readl_relaxed(addr) & mask) ? -EAGAIN : 0;
}

static int rt_i2c_wait_idle(struct rt_i2c *i2c)
{
	int ret;

	ret = poll_down_timeout(i2c->base + REG_STATUS_REG, I2C_BUSY);
	if (ret < 0)
		dev_dbg(i2c->dev, "idle err(%d)\n", ret);

	return ret;
}

static int poll_up_timeout(void __iomem *addr, u32 mask)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(TIMEOUT_MS);
	u32 status;

	do {
		status = readl_relaxed(addr);

		/* check error status */
		if (status & I2C_STARTERR)
			return -EAGAIN;
		else if (status & I2C_ACKERR)
			return -ENXIO;
		else if (status & mask)
			return 0;

		usleep_range(DELAY_INTERVAL_US, DELAY_INTERVAL_US + 50);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

static int rt_i2c_wait_rx_done(struct rt_i2c *i2c)
{
	int ret;

	ret = poll_up_timeout(i2c->base + REG_STATUS_REG, I2C_DATARDY);
	if (ret < 0)
		dev_dbg(i2c->dev, "rx err(%d)\n", ret);

	return ret;
}

static int rt_i2c_wait_tx_done(struct rt_i2c *i2c)
{
	int ret;

	ret = poll_up_timeout(i2c->base + REG_STATUS_REG, I2C_SDOEMPTY);
	if (ret < 0)
		dev_dbg(i2c->dev, "tx err(%d)\n", ret);

	return ret;
}

static void rt_i2c_reset(struct rt_i2c *i2c)
{
	device_reset(i2c->adap.dev.parent);
	barrier();
	rt_i2c_w32(i2c, i2c->clk_div, REG_CLKDIV_REG);
}

static void rt_i2c_dump_reg(struct rt_i2c *i2c)
{
	dev_dbg(i2c->dev, "conf %08x, clkdiv %08x, devaddr %08x, " \
			"addr %08x, dataout %08x, datain %08x, " \
			"status %08x, startxfr %08x, bytecnt %08x\n",
			rt_i2c_r32(i2c, REG_CONFIG_REG),
			rt_i2c_r32(i2c, REG_CLKDIV_REG),
			rt_i2c_r32(i2c, REG_DEVADDR_REG),
			rt_i2c_r32(i2c, REG_ADDR_REG),
			rt_i2c_r32(i2c, REG_DATAOUT_REG),
			rt_i2c_r32(i2c, REG_DATAIN_REG),
			rt_i2c_r32(i2c, REG_STATUS_REG),
			rt_i2c_r32(i2c, REG_STARTXFR_REG),
			rt_i2c_r32(i2c, REG_BYTECNT_REG));
}

static int rt_i2c_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
		int num)
{
	struct rt_i2c *i2c;
	struct i2c_msg *pmsg;
	unsigned char addr;
	int i, j, ret;
	u32 cmd;

	i2c = i2c_get_adapdata(adap);

	for (i = 0; i < num; i++) {
		pmsg = &msgs[i];
		if (i == (num - 1))
			cmd = 0;
		else
			cmd = NOSTOP_CMD;

		dev_dbg(i2c->dev, "addr: 0x%x, len: %d, flags: 0x%x, stop: %d\n",
				pmsg->addr, pmsg->len, pmsg->flags,
				(cmd == 0)? 1 : 0);

		/* wait hardware idle */
		if ((ret = rt_i2c_wait_idle(i2c)))
			goto err_timeout;

		if (pmsg->flags & I2C_M_TEN) {
			rt_i2c_w32(i2c, I2C_CONF_DEFAULT, REG_CONFIG_REG);
			/* 10 bits address */
			addr = 0x78 | ((pmsg->addr >> 8) & 0x03);
			rt_i2c_w32(i2c, addr & I2C_DEVADDR_MASK,
					REG_DEVADDR_REG);
			rt_i2c_w32(i2c, pmsg->addr & I2C_ADDR_MASK,
					REG_ADDR_REG);
		} else {
			rt_i2c_w32(i2c, I2C_CONF_DEFAULT | I2C_ADDR_DIS,
					REG_CONFIG_REG);
			/* 7 bits address */
			rt_i2c_w32(i2c, pmsg->addr & I2C_DEVADDR_MASK,
					REG_DEVADDR_REG);
		}

		/* buffer length */
		if (pmsg->len == 0)
			cmd |= NODATA_CMD;
		else
			rt_i2c_w32(i2c, SET_BYTECNT(pmsg->len),
					REG_BYTECNT_REG);

		j = 0;
		if (pmsg->flags & I2C_M_RD) {
			cmd |= READ_CMD;
			/* start transfer */
			barrier();
			rt_i2c_w32(i2c, cmd, REG_STARTXFR_REG);
			do {
				/* wait */
				if ((ret = rt_i2c_wait_rx_done(i2c)))
					goto err_timeout;
				/* read data */
				if (pmsg->len)
					pmsg->buf[j] = rt_i2c_r32(i2c,
							REG_DATAIN_REG);
				j++;
			} while (j < pmsg->len);
		} else {
			do {
				/* write data */
				if (pmsg->len)
					rt_i2c_w32(i2c, pmsg->buf[j],
							REG_DATAOUT_REG);
				/* start transfer */
				if (j == 0) {
					barrier();
					rt_i2c_w32(i2c, cmd, REG_STARTXFR_REG);
				}
				/* wait */
				if ((ret = rt_i2c_wait_tx_done(i2c)))
					goto err_timeout;
				j++;
			} while (j < pmsg->len);
		}
	}
	/* the return value is number of executed messages */
	ret = i;

	return ret;

err_timeout:
	rt_i2c_dump_reg(i2c);
	rt_i2c_reset(i2c);
	return ret;
}

static u32 rt_i2c_func(struct i2c_adapter *a)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm rt_i2c_algo = {
	.master_xfer	= rt_i2c_master_xfer,
	.functionality	= rt_i2c_func,
};

static const struct of_device_id i2c_rt_dt_ids[] = {
	{ .compatible = "ralink,rt2880-i2c" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, i2c_rt_dt_ids);

static struct i2c_adapter_quirks rt_i2c_quirks = {
        .max_write_len = BYTECNT_MAX,
        .max_read_len = BYTECNT_MAX,
};

static int rt_i2c_init(struct rt_i2c *i2c)
{
	u32 reg;

	/* i2c_sclk = periph_clk / ((2 * clk_div) + 5) */
	i2c->clk_div = (clk_get_rate(i2c->clk) - (5 * i2c->cur_clk)) /
		(2 * i2c->cur_clk);
	if (i2c->clk_div < 8)
		i2c->clk_div = 8;
	if (i2c->clk_div > I2C_CLKDIV_MASK)
		i2c->clk_div = I2C_CLKDIV_MASK;

	/* check support combinde/repeated start message */
	rt_i2c_w32(i2c, NOSTOP_CMD, REG_STARTXFR_REG);
	reg = rt_i2c_r32(i2c, REG_STARTXFR_REG) & NOSTOP_CMD;

	rt_i2c_reset(i2c);

	return reg;
}

static int rt_i2c_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct rt_i2c *i2c;
	struct i2c_adapter *adap;
	const struct of_device_id *match;
	int ret, restart;

	match = of_match_device(i2c_rt_dt_ids, &pdev->dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no memory resource found\n");
		return -ENODEV;
	}

	i2c = devm_kzalloc(&pdev->dev, sizeof(struct rt_i2c), GFP_KERNEL);
	if (!i2c) {
		dev_err(&pdev->dev, "failed to allocate i2c_adapter\n");
		return -ENOMEM;
	}

	i2c->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	i2c->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2c->clk)) {
		dev_err(&pdev->dev, "no clock defined\n");
		return -ENODEV;
	}
	clk_prepare_enable(i2c->clk);
	i2c->dev = &pdev->dev;

	if (of_property_read_u32(pdev->dev.of_node,
				"clock-frequency", &i2c->cur_clk))
		i2c->cur_clk = 100000;

	adap = &i2c->adap;
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
	adap->algo = &rt_i2c_algo;
	adap->retries = 3;
	adap->dev.parent = &pdev->dev;
	i2c_set_adapdata(adap, i2c);
	adap->dev.of_node = pdev->dev.of_node;
	strlcpy(adap->name, dev_name(&pdev->dev), sizeof(adap->name));
	adap->quirks = &rt_i2c_quirks;

	platform_set_drvdata(pdev, i2c);

	restart = rt_i2c_init(i2c);

	ret = i2c_add_adapter(adap);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add adapter\n");
		clk_disable_unprepare(i2c->clk);
		return ret;
	}

	dev_info(&pdev->dev, "clock %uKHz, re-start %ssupport\n",
			i2c->cur_clk/1000, restart ? "" : "not ");

	return ret;
}

static int rt_i2c_remove(struct platform_device *pdev)
{
	struct rt_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
	clk_disable_unprepare(i2c->clk);

	return 0;
}

static struct platform_driver rt_i2c_driver = {
	.probe		= rt_i2c_probe,
	.remove		= rt_i2c_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "i2c-ralink",
		.of_match_table = i2c_rt_dt_ids,
	},
};

static int __init i2c_rt_init (void)
{
	return platform_driver_register(&rt_i2c_driver);
}
subsys_initcall(i2c_rt_init);

static void __exit i2c_rt_exit (void)
{
	platform_driver_unregister(&rt_i2c_driver);
}
module_exit(i2c_rt_exit);

MODULE_AUTHOR("Steven Liu <steven_liu@mediatek.com>");
MODULE_DESCRIPTION("Ralink I2c host driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:Ralink-I2C");
