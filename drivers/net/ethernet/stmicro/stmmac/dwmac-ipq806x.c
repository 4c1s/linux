/*
 * Qualcomm Atheros IPQ806x GMAC glue layer
 *
 * Copyright (C) 2015 The Linux Foundation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/stmmac.h>
#include <linux/of_mdio.h>
#include <linux/module.h>
#include <linux/sys_soc.h>
#include <linux/bitfield.h>

#include "stmmac_platform.h"

#define NSS_COMMON_CLK_GATE			0x8
#define NSS_COMMON_CLK_GATE_PTP_EN(x)		BIT(0x10 + x)
#define NSS_COMMON_CLK_GATE_RGMII_RX_EN(x)	BIT(0x9 + (x * 2))
#define NSS_COMMON_CLK_GATE_RGMII_TX_EN(x)	BIT(0x8 + (x * 2))
#define NSS_COMMON_CLK_GATE_GMII_RX_EN(x)	BIT(0x4 + x)
#define NSS_COMMON_CLK_GATE_GMII_TX_EN(x)	BIT(0x0 + x)

#define NSS_COMMON_CLK_DIV0			0xC
#define NSS_COMMON_CLK_DIV_OFFSET(x)		(x * 8)
#define NSS_COMMON_CLK_DIV_MASK			0x7f

#define NSS_COMMON_CLK_SRC_CTRL			0x14
#define NSS_COMMON_CLK_SRC_CTRL_OFFSET(x)	(x)
/* Mode is coded on 1 bit but is different depending on the MAC ID:
 * MAC0: QSGMII=0 RGMII=1
 * MAC1: QSGMII=0 SGMII=0 RGMII=1
 * MAC2 & MAC3: QSGMII=0 SGMII=1
 */
#define NSS_COMMON_CLK_SRC_CTRL_RGMII(x)	1
#define NSS_COMMON_CLK_SRC_CTRL_SGMII(x)	((x >= 2) ? 1 : 0)

#define NSS_COMMON_GMAC_CTL(x)			(0x30 + (x * 4))
#define NSS_COMMON_GMAC_CTL_CSYS_REQ		BIT(19)
#define NSS_COMMON_GMAC_CTL_PHY_IFACE_SEL	BIT(16)
#define NSS_COMMON_GMAC_CTL_IFG_LIMIT_OFFSET	8
#define NSS_COMMON_GMAC_CTL_IFG_OFFSET		0

#define NSS_COMMON_CLK_DIV_RGMII_1000		1
#define NSS_COMMON_CLK_DIV_RGMII_100		9
#define NSS_COMMON_CLK_DIV_RGMII_10		99
#define NSS_COMMON_CLK_DIV_SGMII_1000		0
#define NSS_COMMON_CLK_DIV_SGMII_100		4
#define NSS_COMMON_CLK_DIV_SGMII_10		49

#define QSGMII_PCS_ALL_CH_CTL			0x80
#define QSGMII_PCS_CH_SPEED_FORCE		BIT(1)
#define QSGMII_PCS_CH_SPEED_10			0x0
#define QSGMII_PCS_CH_SPEED_100			BIT(2)
#define QSGMII_PCS_CH_SPEED_1000		BIT(3)
#define QSGMII_PCS_CH_SPEED_MASK		(QSGMII_PCS_CH_SPEED_FORCE | \
						 QSGMII_PCS_CH_SPEED_10 | \
						 QSGMII_PCS_CH_SPEED_100 | \
						 QSGMII_PCS_CH_SPEED_1000)
#define QSGMII_PCS_CH_SPEED_SHIFT(x)		((x) * 4)

#define QSGMII_PCS_CAL_LCKDT_CTL		0x120
#define QSGMII_PCS_CAL_LCKDT_CTL_RST		BIT(19)

/* Only GMAC1/2/3 support SGMII and their CTL register are not contiguous */
#define QSGMII_PHY_SGMII_CTL(x)			((x == 1) ? 0x134 : \
						 (0x13c + (4 * (x - 2))))
#define QSGMII_PHY_CDR_EN			BIT(0)
#define QSGMII_PHY_RX_FRONT_EN			BIT(1)
#define QSGMII_PHY_RX_SIGNAL_DETECT_EN		BIT(2)
#define QSGMII_PHY_TX_DRIVER_EN			BIT(3)
#define QSGMII_PHY_QSGMII_EN			BIT(7)
#define QSGMII_PHY_DEEMPHASIS_LVL_MASK		GENMASK(11, 10)
#define QSGMII_PHY_DEEMPHASIS_LVL(x)		FIELD_PREP(QSGMII_PHY_DEEMPHASIS_LVL_MASK, (x))
#define QSGMII_PHY_PHASE_LOOP_GAIN_MASK		GENMASK(14, 12)
#define QSGMII_PHY_PHASE_LOOP_GAIN(x)		FIELD_PREP(QSGMII_PHY_PHASE_LOOP_GAIN_MASK, (x))
#define QSGMII_PHY_RX_DC_BIAS_MASK		GENMASK(19, 18)
#define QSGMII_PHY_RX_DC_BIAS(x)		FIELD_PREP(QSGMII_PHY_RX_DC_BIAS_MASK, (x))
#define QSGMII_PHY_RX_INPUT_EQU_MASK		GENMASK(21, 20)
#define QSGMII_PHY_RX_INPUT_EQU(x)		FIELD_PREP(QSGMII_PHY_RX_INPUT_EQU_MASK, (x))
#define QSGMII_PHY_CDR_PI_SLEW_MASK		GENMASK(23, 22)
#define QSGMII_PHY_CDR_PI_SLEW(x)		FIELD_PREP(QSGMII_PHY_CDR_PI_SLEW_MASK, (x))
#define QSGMII_PHY_TX_SLEW_MASK			GENMASK(27, 26)
#define QSGMII_PHY_TX_SLEW(x)			FIELD_PREP(QSGMII_PHY_TX_SLEW_MASK, (x))
#define QSGMII_PHY_TX_DRV_AMP_MASK		GENMASK(31, 28)
#define QSGMII_PHY_TX_DRV_AMP(x)		FIELD_PREP(QSGMII_PHY_TX_DRV_AMP_MASK, (x))

struct ipq806x_gmac {
	struct platform_device *pdev;
	struct regmap *nss_common;
	struct regmap *qsgmii_csr;
	uint32_t id;
	struct clk *core_clk;
	phy_interface_t phy_mode;
};

static int get_clk_div_sgmii(struct ipq806x_gmac *gmac, int speed)
{
	struct device *dev = &gmac->pdev->dev;
	int div;

	switch (speed) {
	case SPEED_1000:
		div = NSS_COMMON_CLK_DIV_SGMII_1000;
		break;

	case SPEED_100:
		div = NSS_COMMON_CLK_DIV_SGMII_100;
		break;

	case SPEED_10:
		div = NSS_COMMON_CLK_DIV_SGMII_10;
		break;

	default:
		dev_err(dev, "Speed %dMbps not supported in SGMII\n", speed);
		return -EINVAL;
	}

	return div;
}

static int get_clk_div_rgmii(struct ipq806x_gmac *gmac, int speed)
{
	struct device *dev = &gmac->pdev->dev;
	int div;

	switch (speed) {
	case SPEED_1000:
		div = NSS_COMMON_CLK_DIV_RGMII_1000;
		break;

	case SPEED_100:
		div = NSS_COMMON_CLK_DIV_RGMII_100;
		break;

	case SPEED_10:
		div = NSS_COMMON_CLK_DIV_RGMII_10;
		break;

	default:
		dev_err(dev, "Speed %dMbps not supported in RGMII\n", speed);
		return -EINVAL;
	}

	return div;
}

static int ipq806x_gmac_set_speed(struct ipq806x_gmac *gmac, int speed)
{
	uint32_t clk_bits, val;
	int div;

	switch (gmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		div = get_clk_div_rgmii(gmac, speed);
		clk_bits = NSS_COMMON_CLK_GATE_RGMII_RX_EN(gmac->id) |
			   NSS_COMMON_CLK_GATE_RGMII_TX_EN(gmac->id);
		break;

	case PHY_INTERFACE_MODE_SGMII:
		div = get_clk_div_sgmii(gmac, speed);
		clk_bits = NSS_COMMON_CLK_GATE_GMII_RX_EN(gmac->id) |
			   NSS_COMMON_CLK_GATE_GMII_TX_EN(gmac->id);
		break;

	default:
		dev_err(&gmac->pdev->dev, "Unsupported PHY mode: \"%s\"\n",
			phy_modes(gmac->phy_mode));
		return -EINVAL;
	}

	/* Disable the clocks */
	regmap_read(gmac->nss_common, NSS_COMMON_CLK_GATE, &val);
	val &= ~clk_bits;
	regmap_write(gmac->nss_common, NSS_COMMON_CLK_GATE, val);

	/* Set the divider */
	regmap_read(gmac->nss_common, NSS_COMMON_CLK_DIV0, &val);
	val &= ~(NSS_COMMON_CLK_DIV_MASK
		 << NSS_COMMON_CLK_DIV_OFFSET(gmac->id));
	val |= div << NSS_COMMON_CLK_DIV_OFFSET(gmac->id);
	regmap_write(gmac->nss_common, NSS_COMMON_CLK_DIV0, val);

	/* Enable the clock back */
	regmap_read(gmac->nss_common, NSS_COMMON_CLK_GATE, &val);
	val |= clk_bits;
	regmap_write(gmac->nss_common, NSS_COMMON_CLK_GATE, val);

	return 0;
}

static int ipq806x_gmac_of_parse(struct ipq806x_gmac *gmac,
				 struct plat_stmmacenet_data *plat_dat)
{
	struct device *dev = &gmac->pdev->dev;

	gmac->phy_mode = plat_dat->phy_interface;

	if (of_property_read_u32(dev->of_node, "qcom,id", &gmac->id) < 0) {
		dev_err(dev, "missing qcom id property\n");
		return -EINVAL;
	}

	/* The GMACs are called 1 to 4 in the documentation, but to simplify the
	 * code and keep it consistent with the Linux convention, we'll number
	 * them from 0 to 3 here.
	 */
	if (gmac->id > 3) {
		dev_err(dev, "invalid gmac id\n");
		return -EINVAL;
	}

	gmac->core_clk = devm_clk_get(dev, "stmmaceth");
	if (IS_ERR(gmac->core_clk)) {
		dev_err(dev, "missing stmmaceth clk property\n");
		return PTR_ERR(gmac->core_clk);
	}
	clk_set_rate(gmac->core_clk, 266000000);

	/* Setup the register map for the nss common registers */
	gmac->nss_common = syscon_regmap_lookup_by_phandle(dev->of_node,
							   "qcom,nss-common");
	if (IS_ERR(gmac->nss_common)) {
		dev_err(dev, "missing nss-common node\n");
		return PTR_ERR(gmac->nss_common);
	}

	/* Setup the register map for the qsgmii csr registers */
	gmac->qsgmii_csr = syscon_regmap_lookup_by_phandle(dev->of_node,
							   "qcom,qsgmii-csr");
	if (IS_ERR(gmac->qsgmii_csr))
		dev_err(dev, "missing qsgmii-csr node\n");

	return PTR_ERR_OR_ZERO(gmac->qsgmii_csr);
}

static int ipq806x_gmac_set_clk_tx_rate(void *bsp_priv, struct clk *clk_tx_i,
					phy_interface_t interface, int speed)
{
	struct ipq806x_gmac *gmac = bsp_priv;

	return ipq806x_gmac_set_speed(gmac, speed);
}

static int
ipq806x_gmac_configure_qsgmii_pcs_speed(struct ipq806x_gmac *gmac)
{
	struct platform_device *pdev = gmac->pdev;
	struct device *dev = &pdev->dev;
	struct device_node *dn;
	int link_speed;
	int val = 0;
	int ret;

	/* Some bootloader may apply wrong configuration and cause
	 * not functioning port. If fixed link is not set,
	 * reset the force speed bit.
	 */
	if (!of_phy_is_fixed_link(pdev->dev.of_node))
		goto write;

	dn = of_get_child_by_name(pdev->dev.of_node, "fixed-link");
	ret = of_property_read_u32(dn, "speed", &link_speed);
	of_node_put(dn);
	if (ret) {
		dev_err(dev, "found fixed-link node with no speed");
		return ret;
	}

	val = QSGMII_PCS_CH_SPEED_FORCE;

	switch (link_speed) {
	case SPEED_1000:
		val |= QSGMII_PCS_CH_SPEED_1000;
		break;
	case SPEED_100:
		val |= QSGMII_PCS_CH_SPEED_100;
		break;
	case SPEED_10:
		val |= QSGMII_PCS_CH_SPEED_10;
		break;
	}

write:
	regmap_update_bits(gmac->qsgmii_csr, QSGMII_PCS_ALL_CH_CTL,
			   QSGMII_PCS_CH_SPEED_MASK <<
			   QSGMII_PCS_CH_SPEED_SHIFT(gmac->id),
			   val <<
			   QSGMII_PCS_CH_SPEED_SHIFT(gmac->id));

	return 0;
}

static const struct soc_device_attribute ipq806x_gmac_soc_v1[] = {
	{
		.revision = "1.*",
	},
	{
		/* sentinel */
	}
};

static int
ipq806x_gmac_configure_qsgmii_params(struct ipq806x_gmac *gmac)
{
	struct platform_device *pdev = gmac->pdev;
	const struct soc_device_attribute *soc;
	struct device *dev = &pdev->dev;
	u32 qsgmii_param;

	switch (gmac->id) {
	case 1:
		soc = soc_device_match(ipq806x_gmac_soc_v1);

		if (soc)
			qsgmii_param = QSGMII_PHY_TX_DRV_AMP(0xc) |
				       QSGMII_PHY_TX_SLEW(0x2) |
				       QSGMII_PHY_DEEMPHASIS_LVL(0x2);
		else
			qsgmii_param = QSGMII_PHY_TX_DRV_AMP(0xd) |
				       QSGMII_PHY_TX_SLEW(0x0) |
				       QSGMII_PHY_DEEMPHASIS_LVL(0x0);

		qsgmii_param |= QSGMII_PHY_RX_DC_BIAS(0x2);
		break;
	case 2:
	case 3:
		qsgmii_param = QSGMII_PHY_RX_DC_BIAS(0x3) |
			       QSGMII_PHY_TX_DRV_AMP(0xc);
		break;
	default: /* gmac 0 can't be set in SGMII mode */
		dev_err(dev, "gmac id %d can't be in SGMII mode", gmac->id);
		return -EINVAL;
	}

	/* Common params across all gmac id */
	qsgmii_param |= QSGMII_PHY_CDR_EN |
			QSGMII_PHY_RX_FRONT_EN |
			QSGMII_PHY_RX_SIGNAL_DETECT_EN |
			QSGMII_PHY_TX_DRIVER_EN |
			QSGMII_PHY_QSGMII_EN |
			QSGMII_PHY_PHASE_LOOP_GAIN(0x4) |
			QSGMII_PHY_RX_INPUT_EQU(0x1) |
			QSGMII_PHY_CDR_PI_SLEW(0x2);

	regmap_write(gmac->qsgmii_csr, QSGMII_PHY_SGMII_CTL(gmac->id),
		     qsgmii_param);

	return 0;
}

static int ipq806x_gmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct ipq806x_gmac *gmac;
	int val;
	int err;

	val = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (val)
		return val;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	gmac = devm_kzalloc(dev, sizeof(*gmac), GFP_KERNEL);
	if (!gmac)
		return -ENOMEM;

	gmac->pdev = pdev;

	err = ipq806x_gmac_of_parse(gmac, plat_dat);
	if (err) {
		dev_err(dev, "device tree parsing error\n");
		return err;
	}

	regmap_write(gmac->qsgmii_csr, QSGMII_PCS_CAL_LCKDT_CTL,
		     QSGMII_PCS_CAL_LCKDT_CTL_RST);

	/* Inter frame gap is set to 12 */
	val = 12 << NSS_COMMON_GMAC_CTL_IFG_OFFSET |
	      12 << NSS_COMMON_GMAC_CTL_IFG_LIMIT_OFFSET;
	/* We also initiate an AXI low power exit request */
	val |= NSS_COMMON_GMAC_CTL_CSYS_REQ;
	switch (gmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val |= NSS_COMMON_GMAC_CTL_PHY_IFACE_SEL;
		break;
	case PHY_INTERFACE_MODE_SGMII:
		val &= ~NSS_COMMON_GMAC_CTL_PHY_IFACE_SEL;
		break;
	default:
		goto err_unsupported_phy;
	}
	regmap_write(gmac->nss_common, NSS_COMMON_GMAC_CTL(gmac->id), val);

	/* Configure the clock src according to the mode */
	regmap_read(gmac->nss_common, NSS_COMMON_CLK_SRC_CTRL, &val);
	val &= ~(1 << NSS_COMMON_CLK_SRC_CTRL_OFFSET(gmac->id));
	switch (gmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val |= NSS_COMMON_CLK_SRC_CTRL_RGMII(gmac->id) <<
			NSS_COMMON_CLK_SRC_CTRL_OFFSET(gmac->id);
		break;
	case PHY_INTERFACE_MODE_SGMII:
		val |= NSS_COMMON_CLK_SRC_CTRL_SGMII(gmac->id) <<
			NSS_COMMON_CLK_SRC_CTRL_OFFSET(gmac->id);
		break;
	default:
		goto err_unsupported_phy;
	}
	regmap_write(gmac->nss_common, NSS_COMMON_CLK_SRC_CTRL, val);

	/* Enable PTP clock */
	regmap_read(gmac->nss_common, NSS_COMMON_CLK_GATE, &val);
	val |= NSS_COMMON_CLK_GATE_PTP_EN(gmac->id);
	switch (gmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val |= NSS_COMMON_CLK_GATE_RGMII_RX_EN(gmac->id) |
			NSS_COMMON_CLK_GATE_RGMII_TX_EN(gmac->id);
		break;
	case PHY_INTERFACE_MODE_SGMII:
		val |= NSS_COMMON_CLK_GATE_GMII_RX_EN(gmac->id) |
				NSS_COMMON_CLK_GATE_GMII_TX_EN(gmac->id);
		break;
	default:
		goto err_unsupported_phy;
	}
	regmap_write(gmac->nss_common, NSS_COMMON_CLK_GATE, val);

	if (gmac->phy_mode == PHY_INTERFACE_MODE_SGMII) {
		err = ipq806x_gmac_configure_qsgmii_params(gmac);
		if (err)
			return err;

		err = ipq806x_gmac_configure_qsgmii_pcs_speed(gmac);
		if (err)
			return err;
	}

	plat_dat->has_gmac = true;
	plat_dat->bsp_priv = gmac;
	plat_dat->set_clk_tx_rate = ipq806x_gmac_set_clk_tx_rate;
	plat_dat->multicast_filter_bins = 0;
	plat_dat->tx_fifo_size = 8192;
	plat_dat->rx_fifo_size = 8192;

	return stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);

err_unsupported_phy:
	dev_err(&pdev->dev, "Unsupported PHY mode: \"%s\"\n",
		phy_modes(gmac->phy_mode));
	return -EINVAL;
}

static const struct of_device_id ipq806x_gmac_dwmac_match[] = {
	{ .compatible = "qcom,ipq806x-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, ipq806x_gmac_dwmac_match);

static struct platform_driver ipq806x_gmac_dwmac_driver = {
	.probe = ipq806x_gmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name		= "ipq806x-gmac-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table	= ipq806x_gmac_dwmac_match,
	},
};
module_platform_driver(ipq806x_gmac_dwmac_driver);

MODULE_AUTHOR("Mathieu Olivari <mathieu@codeaurora.org>");
MODULE_DESCRIPTION("Qualcomm Atheros IPQ806x DWMAC specific glue layer");
MODULE_LICENSE("Dual BSD/GPL");
