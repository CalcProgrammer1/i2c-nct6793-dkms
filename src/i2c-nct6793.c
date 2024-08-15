// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * i2c-nct6793 - Driver for the SMBus master functionality of
 *	       Nuvoton NCT679x Super-I/O chips
 *
 * Copyright (C) 2022  Adam Honse <calcprogrammer1@gmail.com>
 *
 * Derived from nct6775 hwmon driver
 * Copyright (C) 2012  Guenter Roeck <linux@roeck-us.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/io.h>

#define DRVNAME "i2c-nct6793"

/* Nuvoton SMBus address offsets */
#define SMBHSTDAT       (nuvoton_nct6793_smba + 0x0)
#define SMBBLKSZ        (nuvoton_nct6793_smba + 0x1)
#define SMBHSTCMD       (nuvoton_nct6793_smba + 0x2)
#define SMBHSTIDX       (nuvoton_nct6793_smba + 0x3)
#define SMBHSTCTL       (nuvoton_nct6793_smba + 0x4)
#define SMBHSTADD       (nuvoton_nct6793_smba + 0x5)
#define SMBHSTERR       (nuvoton_nct6793_smba + 0x9)
#define SMBHSTSTS       (nuvoton_nct6793_smba + 0xE)

/* Command register */
#define NCT6793_READ_BYTE		0
#define NCT6793_READ_WORD		1
#define NCT6793_READ_BLOCK		2
#define NCT6793D_BLOCK_PROC_CALL	3
#define NCT6793_PROC_CALL		4
#define NCT6793_WRITE_BYTE		8
#define NCT6793_WRITE_WORD		9
#define NCT6793_WRITE_BLOCK		10

/* Control register */
#define NCT6793_MANUAL_START		BIT(7)
#define NCT6793_SOFT_RESET		BIT(6)

/* Error register */
#define NCT6793_NO_ACK			BIT(5)

/* Status register */
#define NCT6793_FIFO_EMPTY		BIT(0)
#define NCT6793_FIFO_FULL		BIT(1)
#define NCT6793_MANUAL_ACTIVE		BIT(2)

#define NCT6793_LD_SMBUS		0x0B

/* Other settings */
#define MAX_RETRIES			400

enum kinds { nct6791, nct6792, nct6793, nct6795, nct6796, nct6798 };

struct nct6793_sio_data {
	int sioreg;
	enum kinds kind;
	struct i2c_adapter *adapter;
};

static const char * const nct6793_sio_names[] = {
	"NCT6791D",
	"NCT6792D",
	"NCT6793D",
	"NCT6795D",
	"NCT6796D",
	"NCT6798D",
};

#define SIO_REG_LDSEL			0x07	/* Logical device select */
#define SIO_REG_DEVID			0x20	/* Device ID (2 bytes) */
#define SIO_REG_SMBA			0x62	/* SMBus base address register */

#define SIO_NCT6791_ID			0xc800
#define SIO_NCT6792_ID			0xc910
#define SIO_NCT6793_ID			0xd120
#define SIO_NCT6795_ID			0xd350
#define SIO_NCT6796_ID			0xd420
#define SIO_NCT6798_ID			0xd428
#define SIO_ID_MASK			0xFFF0

static inline int
superio_inb(int ioreg, int reg)
{
	outb(reg, ioreg);
	return inb(ioreg + 1);
}

static inline void
superio_select(int ioreg, int ld)
{
	outb(SIO_REG_LDSEL, ioreg);
	outb(ld, ioreg + 1);
}

static inline int
superio_enter(int ioreg)
{
	/*
	 * Try to reserve <ioreg> and <ioreg + 1> for exclusive access.
	 */
	if (!request_muxed_region(ioreg, 2, DRVNAME))
		return -EBUSY;

	outb(0x87, ioreg);
	outb(0x87, ioreg);

	return 0;
}

static inline void
superio_exit(int ioreg)
{
	outb(0xaa, ioreg);
	outb(0x02, ioreg);
	outb(0x02, ioreg + 1);
	release_region(ioreg, 2);
}

/*
 * ISA constants
 */

#define IOREGION_ALIGNMENT	(~7)
#define IOREGION_LENGTH		2

struct i2c_nct6793_adapdata {
	unsigned short smba;
};

/* Return negative errno on error. */
static s32 nct6793_access(struct i2c_adapter *adap, u16 addr,
		 unsigned short flags, char read_write,
		 u8 command, int size, union i2c_smbus_data *data)
{
	struct i2c_nct6793_adapdata *adapdata = i2c_get_adapdata(adap);
	unsigned short nuvoton_nct6793_smba = adapdata->smba;
	int i, len, cnt;
	int timeout = 0;

	outb(NCT6793_SOFT_RESET, SMBHSTCTL);

	switch (size) {
	case I2C_SMBUS_BYTE_DATA:
		outb((addr << 1) | read_write,
				SMBHSTADD);
		outb(command, SMBHSTIDX);
		if (read_write == I2C_SMBUS_WRITE) {
			outb(data->byte, SMBHSTDAT);
			outb(NCT6793_WRITE_BYTE, SMBHSTCMD);
		} else {
			outb(NCT6793_READ_BYTE, SMBHSTCMD);
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		outb((addr << 1) | read_write,
				SMBHSTADD);
		outb(command, SMBHSTIDX);
		if (read_write == I2C_SMBUS_WRITE) {
			outb(data->word & 0xff, SMBHSTDAT);
			outb((data->word & 0xff00) >> 8, SMBHSTDAT);
			outb(NCT6793_WRITE_WORD, SMBHSTCMD);
		} else {
			outb(NCT6793_READ_WORD, SMBHSTCMD);
		}
		break;
	case I2C_SMBUS_BLOCK_DATA:
		outb((addr << 1) | read_write,
				SMBHSTADD);
		outb(command, SMBHSTIDX);
		if (read_write == I2C_SMBUS_WRITE) {
			len = data->block[0];
			if (len == 0 || len > I2C_SMBUS_BLOCK_MAX)
				return -EINVAL;
			outb(len, SMBBLKSZ);

			cnt = 1;
			if (len >= 4) {
				for (i = cnt; i <= 4; i++)
					outb(data->block[i], SMBHSTDAT);

				len -= 4;
				cnt += 4;
			} else {
				for (i = cnt; i <= len; i++)
					outb(data->block[i], SMBHSTDAT);

				len = 0;
			}

			outb(NCT6793_WRITE_BLOCK, SMBHSTCMD);
		} else {
			return -EOPNOTSUPP;
		}
		break;
	default:
		dev_warn(&adap->dev, "Unsupported transaction %d\n", size);
		return -EOPNOTSUPP;
	}

	outb(NCT6793_MANUAL_START, SMBHSTCTL);

	while (size == I2C_SMBUS_BLOCK_DATA && len > 0) {
		if (read_write == I2C_SMBUS_WRITE) {
			timeout = 0;
			while ((inb(SMBHSTSTS) & NCT6793_FIFO_EMPTY) == 0) {
				if (timeout > MAX_RETRIES)
					return -ETIMEDOUT;

				usleep_range(250, 500);
				timeout++;
			}

			/* Load more bytes into FIFO */
			if (len >= 4) {
				for (i = cnt; i <= (cnt + 4); i++)
					outb(data->block[i], SMBHSTDAT);

				len -= 4;
				cnt += 4;
			} else {
				for (i = cnt; i <= (cnt + len); i++)
					outb(data->block[i], SMBHSTDAT);

				len = 0;
			}
		}
	}

	/* Wait for manual mode to complete */
	timeout = 0;
	while ((inb(SMBHSTSTS) & NCT6793_MANUAL_ACTIVE) != 0) {
		if (timeout > MAX_RETRIES)
			return -ETIMEDOUT;

		usleep_range(250, 500);
		timeout++;
	}

	if ((inb(SMBHSTERR) & NCT6793_NO_ACK) != 0)
		return -ENXIO;

	if (read_write == I2C_SMBUS_WRITE)
		return 0;

	switch (size) {
	case I2C_SMBUS_BYTE_DATA:
		data->byte = inb(SMBHSTDAT);
		break;
	case I2C_SMBUS_WORD_DATA:
		data->word = inb(SMBHSTDAT) | (inb(SMBHSTDAT) << 8);
		break;
	}
	return 0;
}

static u32 nct6793_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
		I2C_FUNC_SMBUS_WRITE_BLOCK_DATA;
}

static const struct i2c_algorithm smbus_algorithm = {
	.smbus_xfer	= nct6793_access,
	.functionality	= nct6793_func,
};

static struct i2c_adapter *nct6793_add_adapter(struct platform_device *pdev, unsigned short smba, const char *name)
{
	struct device *dev = &pdev->dev;
	struct i2c_adapter *adap;
	struct i2c_nct6793_adapdata *adapdata;
	int retval;

	adap = devm_kzalloc(dev, sizeof(*adap), GFP_KERNEL);
	if (adap == NULL)
		return ERR_PTR(-ENOMEM);

	adap->owner = THIS_MODULE;
	adap->algo = &smbus_algorithm;

	adapdata = devm_kzalloc(dev, sizeof(*adapdata), GFP_KERNEL);
	if (adapdata == NULL) {
		return ERR_PTR(-ENOMEM);
	}

	adapdata->smba = smba;

	snprintf(adap->name, sizeof(adap->name),
		"SMBus %s adapter at %04x", name, smba);

	i2c_set_adapdata(adap, adapdata);

	retval = i2c_add_adapter(adap);
	if (retval) {
		return ERR_PTR(retval);
	}

	return adap;
}

static void nct6793_remove_adapter(struct i2c_adapter *adap)
{
	i2c_del_adapter(adap);
}

/*
 * When Super-I/O functions move to a separate file, the Super-I/O
 * bus will manage the lifetime of the device and this module will only keep
 * track of the nct6793 driver. But since we use platform_device_alloc(), we
 * must keep track of the device.
 */
static struct platform_device *pdev_devices[2];

static int nct6793_probe(struct platform_device *pdev)
{
	struct i2c_adapter *adap;
	struct device *dev = &pdev->dev;
	struct nct6793_sio_data *sio_data = dev_get_platdata(dev);
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!devm_request_region(&pdev->dev, res->start, IOREGION_LENGTH,
				 DRVNAME))
		return -EBUSY;

	switch (sio_data->kind) {
	case nct6791:
	case nct6792:
	case nct6793:
	case nct6795:
	case nct6796:
	case nct6798:
		adap = nct6793_add_adapter(pdev, res->start, nct6793_sio_names[sio_data->kind]);
		if (IS_ERR(sio_data->adapter))
			return PTR_ERR(sio_data->adapter);

		sio_data->adapter = adap;
		break;
	default:
		return -ENODEV;
	}

	return 0;
}

static struct platform_driver i2c_nct6793_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6793_probe,
};

static void __exit i2c_nct6793_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pdev_devices); i++) {
		if (pdev_devices[i]) {
			struct device *dev = &pdev_devices[i]->dev;
			struct nct6793_sio_data *sio_data = dev_get_platdata(dev);

			if (sio_data->adapter)
				nct6793_remove_adapter(sio_data->adapter);

			platform_device_unregister(pdev_devices[i]);
		}
	}
	platform_driver_unregister(&i2c_nct6793_driver);
}

/* nct6793_find() looks for a compatible Nuvoton chip in the Super-I/O config space */
static int __init nct6793_find(int sioaddr, struct nct6793_sio_data *sio_data)
{
	u16 val;
	int err;
	int addr;

	err = superio_enter(sioaddr);
	if (err)
		return err;

	val = (superio_inb(sioaddr, SIO_REG_DEVID) << 8) |
		superio_inb(sioaddr, SIO_REG_DEVID + 1);

	switch (val & SIO_ID_MASK) {
	case SIO_NCT6791_ID:
		sio_data->kind = nct6791;
		break;
	case SIO_NCT6792_ID:
		sio_data->kind = nct6792;
		break;
	case SIO_NCT6793_ID:
		sio_data->kind = nct6793;
		break;
	case SIO_NCT6795_ID:
		sio_data->kind = nct6795;
		break;
	case SIO_NCT6796_ID:
		sio_data->kind = nct6796;
		break;
	case SIO_NCT6798_ID:
		sio_data->kind = nct6798;
		break;
	default:
		if (val != 0xffff)
			pr_debug("unsupported chip ID: 0x%04x\n", val);
		superio_exit(sioaddr);
		return -ENODEV;
	}

	/* We have a known chip, find the SMBus I/O address */
	superio_select(sioaddr, NCT6793_LD_SMBUS);
	val = (superio_inb(sioaddr, SIO_REG_SMBA) << 8)
	    | superio_inb(sioaddr, SIO_REG_SMBA + 1);
	addr = val & IOREGION_ALIGNMENT;
	if (addr == 0) {
		pr_err("Refusing to enable a Super-I/O device with a base I/O port 0\n");
		superio_exit(sioaddr);
		return -ENODEV;
	}

	superio_exit(sioaddr);
	pr_info("Found %s or compatible chip at %#x:%#x\n",
		nct6793_sio_names[sio_data->kind], sioaddr, addr);
	sio_data->sioreg = sioaddr;

	return addr;
}

static int __init i2c_nct6793_init(void)
{
	int i, err;
	bool found = false;
	int address;
	struct resource res;
	struct nct6793_sio_data sio_data;
	const int sioaddr[2] = { 0x2e, 0x4e };

	err = platform_driver_register(&i2c_nct6793_driver);
	if (err)
		return err;

	/*
	 * Initialize sio_data->kind and sio_data->sioreg.
	 *
	 * When Super-I/O functions move to a separate file, the Super-I/O
	 * driver will probe 0x2e and 0x4e and auto-detect the presence of a
	 * nct6775 hardware monitor, and call probe().
	 */
	for (i = 0; i < ARRAY_SIZE(pdev_devices); i++) {
		address = nct6793_find(sioaddr[i], &sio_data);
		if (address <= 0)
			continue;

		found = true;

		pdev_devices[i] = platform_device_alloc(DRVNAME, address);
		if (!pdev_devices[i]) {
			err = -ENOMEM;
			goto exit_device_unregister;
		}

		err = platform_device_add_data(pdev_devices[i], &sio_data,
					       sizeof(struct nct6793_sio_data));
		if (err)
			goto exit_device_put;

		memset(&res, 0, sizeof(res));
		res.name = DRVNAME;
		res.start = address;
		res.end = address + IOREGION_LENGTH - 1;
		res.flags = IORESOURCE_IO;

		err = acpi_check_resource_conflict(&res);
		if (err) {
			platform_device_put(pdev_devices[i]);
			pdev_devices[i] = NULL;
			continue;
		}

		err = platform_device_add_resources(pdev_devices[i], &res, 1);
		if (err)
			goto exit_device_put;

		/* platform_device_add calls probe() */
		err = platform_device_add(pdev_devices[i]);
		if (err)
			goto exit_device_put;
	}
	if (!found) {
		err = -ENODEV;
		goto exit_unregister;
	}

	return 0;

exit_device_put:
	platform_device_put(pdev_devices[i]);
exit_device_unregister:
	while (--i >= 0) {
		if (pdev_devices[i])
			platform_device_unregister(pdev_devices[i]);
	}
exit_unregister:
	platform_driver_unregister(&i2c_nct6793_driver);
	return err;
}

MODULE_AUTHOR("Adam Honse <calcprogrammer1@gmail.com>");
MODULE_DESCRIPTION("SMBus driver for Nuvoton NCT6793D and compatible chips");
MODULE_LICENSE("GPL");

module_init(i2c_nct6793_init);
module_exit(i2c_nct6793_exit);
