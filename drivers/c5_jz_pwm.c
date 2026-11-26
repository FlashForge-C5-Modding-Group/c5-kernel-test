// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/jz_pwm for the X2600's PWM controller, with the ioctls and behaviour of
 * FlashForge's closed soc_pwm.ko (reverse engineered from it), so cmd_pwm and
 * programs using its ioctls directly (Kalico's creator5_beeper and the MIDI
 * player in chelper/c5_buzzer.c) work unchanged on this kernel.
 *
 * A channel is requested by pin name ("pc12"), configured with a frequency
 * and a number of levels, then driven either by level (duty = level / levels)
 * or by raw waveform counts (high << 16 | low). A level of 0 or full parks the
 * pin as a plain GPIO output at the matching level, as soc_pwm does.
 *
 * Not implemented: soc_pwm's DMA mode (PWM_DMA_INIT/UPDATE/DISABLE_LOOP);
 * nothing on the printer uses it.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

/* patches/0004: pinctrl-ingenic's locked function switch, GPIO_* encoding */
int ingenic_gpio_set_function(unsigned int pin, unsigned int func);
#define FUNC_OUTPUT0		0x4
#define FUNC_OUTPUT1		0x5

#define PWM_REQUEST		0x8001500B	/* char[12] pin name; returns the channel */
#define PWM_RELEASE		0x80045016	/* channel (the argument itself) */
#define PWM_CONFIG		0x80185021	/* struct pwm_config */
#define PWM_GET_LEVEL		0xC004502B	/* u32 channel in, level out */
#define PWM_SET_LEVEL		0xC004502C	/* u32 channel, s32 level */
#define PWM_SET_WC		0xC004502D	/* u32 channel, u32 high << 16 | low */
#define PWM_SET_PRESCALE	0xC004502E	/* u32 channel, u32 prescale */
#define PWM_NOT_REALLY_ENABLE	0x80045058	/* u32 channel */
#define PWM_NOT_REALLY_DISABLE	0x80045059	/* u32 channel */
#define PWM_ENABLE_CHANNELS	0x80045062	/* u32 channel mask */
#define PWM_DISABLE_CHANNELS	0x80045063	/* u32 channel mask */
#define PWM_DMA_INIT		0x800C5037
#define PWM_DMA_DISABLE_LOOP	0x8001504D
#define PWM_DMA_UPDATE		0xC0105042

/* Registers (pwm-ingenic-v3.h names) */
#define PWM_ENS			0x00
#define PWM_ENC			0x04
#define PWM_EN			0x08
#define PWM_UPT			0x10
#define PWM_BUSY		0x14
#define PWM_MS			0x20
#define PWM_INL			0x24
#define PWM_IDL			0x28
#define PWM_CCFG(n)		(0x40 + 4 * (n))
#define PWM_WCFG(n)		(0x80 + 4 * (n))

#define CHANNELS		16
#define CLK_RATE_MAX		500000000

struct pwm_config {
	u32 reserved;
	u32 active_level;
	u32 accuracy_priority;	/* 0: frequency, else levels */
	u32 freq;
	u32 max_level;
	u32 channel;
};

struct c5_pwm_pin {
	const char *name;
	unsigned int channel;
	unsigned int gpio;	/* port * 32 + pin */
	unsigned int func;	/* soc_pwm's: 0x10 + the pin function */
};

/* soc_pwm's pwm_gpio_array */
static const struct c5_pwm_pin c5_pwm_pins[] = {
	{ "pwm0", 0, 44, 0x12 },	/* PB12 */
	{ "pwm1", 1, 45, 0x12 },
	{ "pwm2", 2, 46, 0x12 },
	{ "pwm3", 3, 47, 0x12 },
	{ "pwm4", 4, 48, 0x12 },
	{ "pwm5", 5, 49, 0x12 },
	{ "pwm6", 6, 50, 0x12 },
	{ "pwm7", 7, 51, 0x12 },	/* PB19 */
	{ "pwm8", 8, 71, 0x10 },	/* PC07 */
	{ "pwm9", 9, 72, 0x10 },
	{ "pwm10", 10, 73, 0x10 },
	{ "pwm10", 10, 130, 0x12 },	/* PE02 */
	{ "pwm11", 11, 74, 0x10 },
	{ "pwm12", 12, 75, 0x10 },
	{ "pwm13", 13, 76, 0x10 },	/* PC12: the buzzer */
	{ "pwm14", 14, 77, 0x10 },
	{ "pwm15", 15, 78, 0x10 },	/* PC14 */
};

struct c5_pwm_chan {
	struct mutex lock;
	const struct c5_pwm_pin *pin;
	bool configured, requested, enabled;
	bool parked;		/* the pin is a GPIO output at a constant level */
	bool not_really_disable, not_really_enable;
	u32 rate;		/* channel clock after its divider */
	u32 freq;		/* actual frequency */
	u32 period;		/* counts */
	u32 max_level;
	bool active_high;
	u32 level_counts;
	u32 high, low;		/* counts of the waveform's two halves */
};

struct c5_pwm {
	void __iomem *base;
	spinlock_t lock;	/* registers shared by all channels */
	unsigned long src_rate;
	struct clk *div, *gate;
	struct c5_pwm_chan chan[CHANNELS];
	struct miscdevice misc;
};

static void c5_pwm_rmw(struct c5_pwm *pwm, unsigned int reg, u32 mask, u32 value)
{
	unsigned long flags;

	spin_lock_irqsave(&pwm->lock, flags);
	writel((readl(pwm->base + reg) & ~mask) | (value & mask), pwm->base + reg);
	spin_unlock_irqrestore(&pwm->lock, flags);
}

static void c5_pwm_pin_func(const struct c5_pwm_pin *pin, unsigned int func)
{
	ingenic_gpio_set_function(pin->gpio, func & 0xf);
}

static void c5_pwm_set_clk_div(struct c5_pwm *pwm, unsigned int ch)
{
	u32 div = pwm->src_rate / pwm->chan[ch].rate;

	if (div >= 0x10000) {
		pr_err("PWM: clk div err, pwm ch %d !\n", ch);
		div = 0xffff;
	} else {
		div -= 1;
	}
	c5_pwm_rmw(pwm, PWM_CCFG(ch), 0xffff, div);
}

/* Latches the new waveform at the end of the current period */
static int c5_pwm_update(struct c5_pwm *pwm, unsigned int ch)
{
	u32 freq = pwm->chan[ch].freq;
	unsigned int wait_us = freq && freq <= 1000000 ? 1000000 / freq : 1;
	int tries = 3;

	while (readl(pwm->base + PWM_BUSY) & BIT(ch)) {
		if (--tries == 0) {
			pr_err("PWM: pwm updata waveform config timeout\n");
			return -1;
		}
		udelay(wait_us);
	}
	writel(BIT(ch), pwm->base + PWM_UPT);
	return 0;
}

static void c5_pwm_enable(struct c5_pwm *pwm, unsigned int ch)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];

	if (c->enabled)
		return;
	c5_pwm_pin_func(c->pin, c->pin->func);
	if (!c->not_really_enable)
		writel(BIT(ch), pwm->base + PWM_ENS);
	c->enabled = true;
}

/* Parks the pin as a GPIO output (level 0 or full), as soc_pwm does */
static void c5_pwm_park(struct c5_pwm *pwm, unsigned int ch, bool high)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];

	c->parked = true;
	c5_pwm_pin_func(c->pin, high ? FUNC_OUTPUT1 : FUNC_OUTPUT0);
	writel(high ? (c->period - 1) << 16 | 1 : 1 << 16 | ((c->period - 1) & 0xffff),
	       pwm->base + PWM_WCFG(ch));
	if (!c->not_really_disable && c->enabled) {
		writel(BIT(ch), pwm->base + PWM_ENC);
		c->enabled = false;
	}
}

/* Writes the waveform and starts the channel; the caller holds c->lock */
static int c5_pwm_start(struct c5_pwm *pwm, unsigned int ch)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];

	if (c->parked) {
		c->parked = false;
		c5_pwm_pin_func(c->pin, c->pin->func);
	}
	writel(c->high << 16 ^ (c->low & 0xffff), pwm->base + PWM_WCFG(ch));
	if (c5_pwm_update(pwm, ch) < 0)
		return -1;
	c5_pwm_enable(pwm, ch);
	return 0;
}

/* "pc12" (any case): port * 32 + pin */
static int c5_pwm_str_to_gpio(const char *s)
{
	unsigned int pin;

	if (tolower(s[0]) != 'p' || tolower(s[1]) < 'a' || tolower(s[1]) > 'f' ||
	    kstrtouint(s + 2, 10, &pin) || pin >= 32)
		return -EINVAL;
	return (tolower(s[1]) - 'a') * 32 + pin;
}

static int c5_pwm_request(struct c5_pwm *pwm, const char __user *uname)
{
	const struct c5_pwm_pin *pin = NULL;
	struct c5_pwm_chan *c;
	char name[12];
	int gpio, ret, i;

	if (copy_from_user(name, uname, sizeof(name))) {
		pr_err("PWM: copy_from_user err!\n");
		return -1;
	}
	name[sizeof(name) - 1] = 0;
	gpio = c5_pwm_str_to_gpio(name);
	if (gpio < 0) {
		pr_err("PWM: gpio is invalid:%s !\n", name);
		return -1;
	}
	for (i = 0; i < ARRAY_SIZE(c5_pwm_pins) && !pin; i++)
		if (c5_pwm_pins[i].gpio == gpio)
			pin = &c5_pwm_pins[i];
	if (!pin) {
		pr_err("PWM: gpio is not pwm:%s !\n", name);
		return -1;
	}
	c = &pwm->chan[pin->channel];
	mutex_lock(&c->lock);
	ret = pin->channel;
	if (!c->requested) {
		if (gpio_request(gpio, pin->name)) {
			pr_err("PWM: pwm ch %d, request IO %s error !\n", pin->channel, name);
			ret = -1;
		} else {
			c->pin = pin;
			c->requested = true;
			c->rate = pwm->src_rate;
		}
	}
	mutex_unlock(&c->lock);
	return ret;
}

static int c5_pwm_release(struct c5_pwm *pwm, unsigned int ch)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];

	mutex_lock(&c->lock);
	if (c->enabled) {
		writel(BIT(ch), pwm->base + PWM_ENC);
		c->enabled = false;
	}
	if (c->requested) {
		gpio_free(c->pin->gpio);
		c->requested = false;
	}
	c->configured = false;
	mutex_unlock(&c->lock);
	return 0;
}

static int c5_pwm_config(struct c5_pwm *pwm, const struct pwm_config *cfg)
{
	unsigned int ch = cfg->channel;
	struct c5_pwm_chan *c = &pwm->chan[ch];
	u32 freq = cfg->freq, max = cfg->max_level, rate, period;
	int ret = -1;

	if (!freq) {
		pr_err("PWM: pwm ch %d only support frequency high than 0 Hz\n", ch);
		return -1;
	}
	if (pwm->src_rate < freq) {
		pr_err("PWM: pwm only support frequency low than %lu\n", pwm->src_rate);
		return -1;
	}
	if (max >= 0xffff) {
		pr_err("PWM: pwm ch %d levels need less than %d\n", ch, 0xffff);
		return -1;
	}
	mutex_lock(&c->lock);
	if (!c->requested) {
		pr_err("PWM: pwm ch %d is not request!\n", ch);
		goto out;
	}
	if (c->enabled) {
		pr_err("PWM: pwm ch %d Cannot configure at working\n", ch);
		goto out;
	}
	c->max_level = max;
	c->active_high = cfg->active_level != 0;

	rate = pwm->src_rate;
	period = rate / freq;
	if (period >= 0x10000) {
		rate /= period / 0xffff + 1;
		period = rate / freq;
	}
	if (!cfg->accuracy_priority) {
		/* the frequency as close as the counter allows */
		if (period >= 0x10000)
			period = 0xffff;
	} else if (period < max) {
		/* every level reachable: at least max counts */
		period = max;
	} else {
		/* a whole number of counts per level */
		period -= period % max;
		if (period >= 0x10000)
			period = 0xffff;
	}
	c->rate = rate;
	c->period = period;
	c->freq = rate / period;
	c5_pwm_set_clk_div(pwm, ch);
	c5_pwm_rmw(pwm, PWM_IDL, BIT(ch), c->active_high ? BIT(ch) : 0);
	c5_pwm_rmw(pwm, PWM_INL, BIT(ch), c->active_high ? 0 : BIT(ch));
	c5_pwm_rmw(pwm, PWM_MS, BIT(ch), 0);
	c->configured = true;
	ret = 0;
out:
	mutex_unlock(&c->lock);
	return ret;
}

static int c5_pwm_set_prescale(struct c5_pwm *pwm, unsigned int ch, u32 prescale)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];
	int ret = -1;

	mutex_lock(&c->lock);
	if (!c->configured) {
		pr_err("PWM: pwm ch%d is not config!\n", ch);
		goto out;
	}
	if (!prescale || prescale >= 0x10000) {
		pr_err("PWM: clk div err, pwm ch %d !\n", ch);
		prescale = 0x10000;
	}
	c5_pwm_rmw(pwm, PWM_CCFG(ch), 0xffff, prescale - 1);
	c->rate = pwm->src_rate / prescale;
	ret = 0;
out:
	mutex_unlock(&c->lock);
	return ret;
}

static int c5_pwm_set_wc(struct c5_pwm *pwm, unsigned int ch, u32 high, u32 low)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];
	int ret = -1;

	mutex_lock(&c->lock);
	if (!c->configured) {
		pr_err("PWM: pwm ch%d is not config!\n", ch);
		goto out;
	}
	c->high = high;
	c->low = low;
	ret = c5_pwm_start(pwm, ch);
out:
	mutex_unlock(&c->lock);
	return ret;
}

static int c5_pwm_set_level(struct c5_pwm *pwm, unsigned int ch, s32 level)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];
	u32 counts, wcfg;
	int ret = -1;

	mutex_lock(&c->lock);
	if (!c->configured) {
		pr_err("PWM: pwm ch%d is not config!\n", ch);
		goto out;
	}
	if (level < 0) {
		/* take the state over from the hardware */
		wcfg = readl(pwm->base + PWM_WCFG(ch));
		c->low = wcfg & 0xffff;
		c->high = wcfg >> 16;
		c->enabled = readl(pwm->base + PWM_EN) & BIT(ch);
		if (c->low == 1 || c->high == 1)
			c->parked = true;
		ret = 0;
		goto out;
	}
	if (level > c->max_level) {
		pr_err("PWM: pwm ch%d set level more than max level!\n", ch);
		goto out;
	}
	counts = 0;
	if (level) {
		counts = level * c->period;
		/* a level above 0 is never less than one count */
		counts = counts < c->max_level ? 1 : counts / c->max_level;
	}
	c->level_counts = counts;
	if (c->active_high) {
		c->low = counts;
		c->high = c->period - counts;
	} else {
		c->high = counts;
		c->low = c->period - counts;
	}
	ret = 0;
	if (!c->low)
		c5_pwm_park(pwm, ch, true);
	else if (!c->high)
		c5_pwm_park(pwm, ch, false);
	else
		ret = c5_pwm_start(pwm, ch);
out:
	mutex_unlock(&c->lock);
	return ret;
}

static int c5_pwm_get_level(struct c5_pwm *pwm, unsigned int ch)
{
	struct c5_pwm_chan *c = &pwm->chan[ch];
	u32 counts = c->active_high ? c->low : c->high, wcfg;

	if (!c->period)
		return 0;
	if (counts && counts != c->period && c->enabled) {
		wcfg = readl(pwm->base + PWM_WCFG(ch));
		counts = c->active_high ? wcfg & 0xffff : wcfg >> 16;
	}
	return counts * c->max_level / c->period;
}

static long c5_pwm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct c5_pwm *pwm = container_of(file->private_data, struct c5_pwm, misc);
	void __user *uarg = (void __user *)arg;
	struct pwm_config cfg;
	u32 a[2];
	int level;

	switch (cmd) {
	case PWM_REQUEST:
		return c5_pwm_request(pwm, uarg);
	case PWM_RELEASE:
		return arg < CHANNELS ? c5_pwm_release(pwm, arg) : -1;
	case PWM_CONFIG:
		if (copy_from_user(&cfg, uarg, sizeof(cfg)))
			return -1;
		return cfg.channel < CHANNELS ? c5_pwm_config(pwm, &cfg) : -1;
	case PWM_GET_LEVEL:
		if (copy_from_user(a, uarg, 4) || a[0] >= CHANNELS)
			return -1;
		level = c5_pwm_get_level(pwm, a[0]);
		return copy_to_user(uarg, &level, 4) ? -EFAULT : 0;
	case PWM_SET_LEVEL:
	case PWM_SET_WC:
	case PWM_SET_PRESCALE:
		if (copy_from_user(a, uarg, 8) || a[0] >= CHANNELS)
			return -1;
		if (cmd == PWM_SET_LEVEL)
			return c5_pwm_set_level(pwm, a[0], a[1]);
		if (cmd == PWM_SET_WC)
			return c5_pwm_set_wc(pwm, a[0], a[1] >> 16, a[1] & 0xffff);
		return c5_pwm_set_prescale(pwm, a[0], a[1]);
	case PWM_NOT_REALLY_ENABLE:
	case PWM_NOT_REALLY_DISABLE:
		if (copy_from_user(a, uarg, 4) || a[0] >= CHANNELS)
			return -1;
		if (cmd == PWM_NOT_REALLY_ENABLE)
			pwm->chan[a[0]].not_really_enable = true;
		else
			pwm->chan[a[0]].not_really_disable = true;
		return 0;
	case PWM_ENABLE_CHANNELS:
	case PWM_DISABLE_CHANNELS:
		/* raw channel masks, as soc_pwm writes them */
		if (copy_from_user(a, uarg, 4))
			return -1;
		writel(a[0], pwm->base + (cmd == PWM_ENABLE_CHANNELS ? PWM_ENS : PWM_ENC));
		return 0;
	case PWM_DMA_INIT:
	case PWM_DMA_DISABLE_LOOP:
	case PWM_DMA_UPDATE:
		return -ENOTTY;
	default:
		return -1;
	}
}

static int c5_pwm_open(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations c5_pwm_fops = {
	.owner = THIS_MODULE,
	.open = c5_pwm_open,
	.unlocked_ioctl = c5_pwm_ioctl,
};

/* soc_pwm's clock setup: the divider at up to 500 MHz, then the gate */
static int c5_pwm_clocks(struct c5_pwm *pwm, struct device *dev)
{
	unsigned long parent_rate, rate = CLK_RATE_MAX;
	struct clk *parent;
	int ret;

	parent = devm_clk_get(dev, "div_ahb2");
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	parent_rate = clk_get_rate(parent);
	pwm->div = devm_clk_get(dev, "div_pwm");
	if (IS_ERR(pwm->div))
		return PTR_ERR(pwm->div);
	pwm->gate = devm_clk_get(dev, "gate_pwm");
	if (IS_ERR(pwm->gate))
		return PTR_ERR(pwm->gate);
	if (__clk_is_enabled(pwm->div)) {
		unsigned long cur = clk_get_rate(pwm->div);

		if (cur <= CLK_RATE_MAX && cur < parent_rate)
			rate = cur;
	}
	clk_set_rate(pwm->div, rate);
	ret = clk_prepare_enable(pwm->div);
	if (ret)
		return ret;
	pwm->src_rate = clk_get_rate(pwm->div);
	ret = clk_prepare_enable(pwm->gate);
	if (ret)
		clk_disable_unprepare(pwm->div);
	return ret;
}

static int c5_pwm_probe(struct platform_device *pdev)
{
	struct c5_pwm *pwm;
	int ret, i;

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;
	pwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pwm->base))
		return PTR_ERR(pwm->base);
	spin_lock_init(&pwm->lock);
	for (i = 0; i < CHANNELS; i++)
		mutex_init(&pwm->chan[i].lock);
	ret = c5_pwm_clocks(pwm, &pdev->dev);
	if (ret)
		return ret;
	pwm->misc.minor = MISC_DYNAMIC_MINOR;
	pwm->misc.name = "jz_pwm";
	pwm->misc.fops = &c5_pwm_fops;
	pwm->misc.parent = &pdev->dev;
	platform_set_drvdata(pdev, pwm);
	ret = misc_register(&pwm->misc);
	if (ret) {
		clk_disable_unprepare(pwm->gate);
		clk_disable_unprepare(pwm->div);
	}
	return ret;
}

static int c5_pwm_remove(struct platform_device *pdev)
{
	struct c5_pwm *pwm = platform_get_drvdata(pdev);

	misc_deregister(&pwm->misc);
	clk_disable_unprepare(pwm->gate);
	clk_disable_unprepare(pwm->div);
	return 0;
}

static const struct of_device_id c5_pwm_of_match[] = {
	{ .compatible = "ingenic,x2600-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, c5_pwm_of_match);

static struct platform_driver c5_pwm_driver = {
	.probe = c5_pwm_probe,
	.remove = c5_pwm_remove,
	.driver = {
		.name = "c5-jz-pwm",
		.of_match_table = c5_pwm_of_match,
	},
};
module_platform_driver(c5_pwm_driver);

MODULE_DESCRIPTION("X2600 PWM as /dev/jz_pwm (soc_pwm compatible)");
MODULE_LICENSE("GPL");
