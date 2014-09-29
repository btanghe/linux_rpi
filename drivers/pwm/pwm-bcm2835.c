/*
 * Copyright (C) 2014 Thomas more
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define DUTY		0x14
#define PERIOD		0x10
#define CHANNEL		0x10

#define PWM_ENABLE	(1 << 0)
#define PWM_POLARITY	(1 << 4)

#define PWM_CONTROL_MASK	0xff
#define PWM_CONTROL_ENABLE	0x80
#define PWM_CONTROL_DISABLE	0xff
#define MIN_PERIOD		108		/* 9.2Mhz max pwm clock */

struct bcm2835_pwm {
	struct pwm_chip chip;
	struct device *dev;
	int channel;
	unsigned long scaler;
	void __iomem *base;
	struct clk *clk;
};

static inline struct bcm2835_pwm  *to_bcm2835_pwm(struct pwm_chip *chip)
{
	return container_of(chip, struct bcm2835_pwm, chip);
}

static int bcm2835_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct bcm2835_pwm *pc = to_bcm2835_pwm(chip);
	u32 value;

	value = readl(pc->base) & ~(PWM_CONTROL_MASK << 8 * pwm->pwm);
	value |= (PWM_CONTROL_ENABLE << (8 * pwm->pwm));
	writel(value, pc->base);
	return 0;
}

static void bcm2835_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct bcm2835_pwm *pc = to_bcm2835_pwm(chip);
	u32 value;

	value = readl(pc->base) & ~(PWM_CONTROL_MASK << 8 * pwm->pwm);
	value &= (~PWM_CONTROL_DISABLE << (8 * pwm->pwm));
	writel(value, pc->base);
}

static int bcm2835_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			      int duty_ns, int period_ns)
{
	struct bcm2835_pwm *pc = to_bcm2835_pwm(chip);

	if (period_ns > MIN_PERIOD) {
		writel(duty_ns / pc->scaler,
			 pc->base + DUTY + pwm->pwm * CHANNEL);
		writel(period_ns / pc->scaler,
			pc->base + PERIOD + pwm->pwm * CHANNEL);
	} else {
		dev_err(pc->dev,"Period not supported\n");
	}

	return 0;
}

static int bcm2835_pwm_enable(struct pwm_chip *chip,
			      struct pwm_device *pwm)
{

	struct bcm2835_pwm *pc = to_bcm2835_pwm(chip);
	u32 value;

	//value = readl(pc->base) & ~(PWM_CONTROL_MASK << 8 * pwm->pwm); ;
	value = readl(pc->base);
	value |= (PWM_ENABLE << (8 * pwm->pwm));
	writel(value, pc->base);
	return 0;
}

static void bcm2835_pwm_disable(struct pwm_chip *chip,
				struct pwm_device *pwm)
{
	struct bcm2835_pwm *pc = to_bcm2835_pwm(chip);
	u32 value;

	//value = readl(pc->base) | ~(PWM_CONTROL_MASK << 8 * pwm->pwm);
	value = readl(pc->base);
	value &= ~(PWM_ENABLE << (8 * pwm->pwm));
	writel(value, pc->base);
}

static int bcm2835_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				enum pwm_polarity polarity)
{
	struct bcm2835_pwm *pc = to_bcm2835_pwm(chip);
	u32 value;

	if (polarity == PWM_POLARITY_NORMAL) {
		value = readl(pc->base);
		value &= ~(PWM_POLARITY << 8 * pwm->pwm);
		writel(value, pc->base);
	} else if (polarity == PWM_POLARITY_INVERSED) {
		value = readl(pc->base);
		value |= PWM_POLARITY << (8 * pwm->pwm);
		writel(value, pc->base);
	}
	return 0;
}

static const struct pwm_ops bcm2835_pwm_ops = {
	.request = bcm2835_pwm_request,
	.free = bcm2835_pwm_free,
	.config = bcm2835_pwm_config,
	.enable = bcm2835_pwm_enable,
	.disable = bcm2835_pwm_disable,
	.set_polarity = bcm2835_set_polarity,
	.owner = THIS_MODULE,
};

static int bcm2835_pwm_probe(struct platform_device *pdev)
{
	struct bcm2835_pwm *pwm;
	int ret;
	struct resource *r;
	struct clk *clk;

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;

	pwm->dev = &pdev->dev;

	clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "no clock found: %ld\n", PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	pwm->clk = clk;
	clk_prepare_enable(pwm->clk);
	pwm->scaler = NSEC_PER_SEC / clk_get_rate(clk);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pwm->base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(pwm->base)) {
		return PTR_ERR(pwm->base);
	}

	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &bcm2835_pwm_ops;
	pwm->chip.npwm = 2;

	ret = pwmchip_add(&pwm->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		return -1;
	}
	platform_set_drvdata(pdev, pwm);
	return 0;
}

static int bcm2835_pwm_remove(struct platform_device *pdev)
{
	struct bcm2835_pwm *pc = platform_get_drvdata(pdev);
	clk_disable_unprepare(pc->clk);
	return pwmchip_remove(&pc->chip);
}

static const struct of_device_id bcm2835_pwm_of_match[] = {
	{ .compatible = "brcm,bcm2835-pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bcm2835_pwm_of_match);

static struct platform_driver bcm2835_pwm_driver = {
	.driver = {
		.name = "bcm2835-pwm",
		.of_match_table = bcm2835_pwm_of_match,
	},
	.probe = bcm2835_pwm_probe,
	.remove = bcm2835_pwm_remove,
};
module_platform_driver(bcm2835_pwm_driver);

MODULE_AUTHOR("Bart Tanghe <bart.tanghe@thomasmore.be");
MODULE_DESCRIPTION("Broadcom BCM2835 PWM driver");
MODULE_LICENSE("GPL v2");
