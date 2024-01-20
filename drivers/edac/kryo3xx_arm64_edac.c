/* Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/edac.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>

#include <asm/cputype.h>

#include "edac_core.h"

#include <linux/sec_debug_partition.h>

#ifdef CONFIG_EDAC_KRYO3XX_ARM64_POLL
static int poll_msec = 1000;
module_param(poll_msec, int, 0444);
#endif

#ifdef CONFIG_EDAC_KRYO3XX_ARM64_PANIC_ON_CE
static bool panic_on_ce = 1;
#else
static bool panic_on_ce;
#endif
module_param_named(panic_on_ce, panic_on_ce, bool, 0664);

#ifdef CONFIG_EDAC_KRYO3XX_ARM64_PANIC_ON_UE
#define ARM64_ERP_PANIC_ON_UE 1
#else
#define ARM64_ERP_PANIC_ON_UE 0
#endif

#define L1 0x0
#define L2 0x1
#define L3 0x2

#define EDAC_CPU	"kryo3xx_edac"

#define KRYO3XX_ERRXSTATUS_VALID(a)	((a >> 30) & 0x1)
#define KRYO3XX_ERRXSTATUS_UE(a)	((a >> 29) & 0x1)
#define KRYO3XX_ERRXSTATUS_SERR(a)	(a & 0xFF)

#define KRYO3XX_ERRXMISC_LVL(a)		((a >> 1) & 0x7)
#define KRYO3XX_ERRXMISC_WAY(a)		((a >> 28) & 0xF)

static inline void set_errxctlr_el1(void)
{
	u64 val = 0x10f;

	asm volatile("msr s3_0_c5_c4_1, %0" : : "r" (val));
}

static inline void set_errxmisc_overflow(void)
{
	u64 val = 0x7F7F00000000ULL;

	asm volatile("msr s3_0_c5_c5_0, %0" : : "r" (val));
}

static inline void write_errselr_el1(u64 val)
{
	asm volatile("msr s3_0_c5_c3_1, %0" : : "r" (val));
}

static inline u64 read_errxstatus_el1(void)
{
	u64 val;

	asm volatile("mrs %0, s3_0_c5_c4_2" : "=r" (val));
	return val;
}

static inline u64 read_errxmisc_el1(void)
{
	u64 val;

	asm volatile("mrs %0, s3_0_c5_c5_0" : "=r" (val));
	return val;
}

static inline void clear_errxstatus_valid(u64 val)
{
	asm volatile("msr s3_0_c5_c4_2, %0" : : "r" (val));
}

struct errors_edac {
	const char * const msg;
	void (*func)(struct edac_device_ctl_info *edac_dev,
			int inst_nr, int block_nr, const char *msg);
};

static const struct errors_edac errors[] = {
	{"Kryo3xx L1 Correctable Error", edac_device_handle_ce },
	{"Kryo3xx L1 Uncorrectable Error", edac_device_handle_ue },
	{"Kryo3xx L2 Correctable Error", edac_device_handle_ce },
	{"Kryo3xx L2 Uncorrectable Error", edac_device_handle_ue },
	{"L3 Correctable Error", edac_device_handle_ce },
	{"L3 Uncorrectable Error", edac_device_handle_ue },
};

#define KRYO3XX_L1_CE 0
#define KRYO3XX_L1_UE 1
#define KRYO3XX_L2_CE 2
#define KRYO3XX_L2_UE 3
#define KRYO3XX_L3_CE 4
#define KRYO3XX_L3_UE 5

#define DATA_BUF_ERR		0x2
#define CACHE_DATA_ERR		0x6
#define CACHE_TAG_DIRTY_ERR	0x7
#define TLB_PARITY_ERR_DATA	0x8
#define TLB_PARITY_ERR_TAG	0x9
#define BUS_ERROR		0x12

struct erp_drvdata {
	struct edac_device_ctl_info *edev_ctl;
	struct erp_drvdata __percpu **erp_cpu_drvdata;
	struct notifier_block nb_pm;
	int ppi;
};

static struct erp_drvdata *panic_handler_drvdata;

static DEFINE_SPINLOCK(local_handler_lock);

static ap_health_t *p_health;
enum {
	ID_L1_CACHE = 0,
	ID_L2_CACHE,
	ID_L3_CACHE,
	ID_BUS_ERR,
};

static int update_arm64_edac_count(int cpu, int block, int etype)
{
	if (!p_health)
		p_health = ap_health_data_read();

	if (cpu < 0 || cpu >= num_present_cpus()) {
		edac_printk(KERN_CRIT, EDAC_CPU,
				"%s : not available cpu = %d\n", __func__, cpu);
		return -1;
	}

	if (p_health) {
		switch (block) {
		case ID_L1_CACHE:
			if (etype == KRYO3XX_L1_CE) {
				p_health->cache.edac[cpu][block].ce_cnt++;
				p_health->daily_cache.edac[cpu][block].ce_cnt++;
			} else if (etype == KRYO3XX_L1_UE) {
				p_health->cache.edac[cpu][block].ue_cnt++;
				p_health->daily_cache.edac[cpu][block].ue_cnt++;
			}
			break;
		case ID_L2_CACHE:
			if (etype == KRYO3XX_L2_CE) {
				p_health->cache.edac[cpu][block].ce_cnt++;
				p_health->daily_cache.edac[cpu][block].ce_cnt++;
			} else if (etype == KRYO3XX_L2_UE) {
				p_health->cache.edac[cpu][block].ue_cnt++;
				p_health->daily_cache.edac[cpu][block].ue_cnt++;
			}
			break;
		case ID_L3_CACHE:
			if (etype == KRYO3XX_L3_CE) {
				p_health->cache.edac_l3.ce_cnt++;
				p_health->daily_cache.edac_l3.ce_cnt++;
			} else if (etype == KRYO3XX_L3_UE) {
				p_health->cache.edac_l3.ue_cnt++;
				p_health->daily_cache.edac_l3.ue_cnt++;
			}
			break;
		case ID_BUS_ERR:
			p_health->cache.edac_bus_cnt++;
			p_health->daily_cache.edac_bus_cnt++;
			break;
		}
		ap_health_data_write(p_health);
	}

	return 0;
}


static void l1_l2_irq_enable(void *info)
{
	int irq = *(int *)info;

	enable_percpu_irq(irq, IRQ_TYPE_LEVEL_HIGH);
}

static int request_erp_irq(struct platform_device *pdev, const char *propname,
			const char *desc, irq_handler_t handler,
			void *ed, int percpu)
{
	int rc;
	struct resource *r;
	struct erp_drvdata *drv = ed;

	r = platform_get_resource_byname(pdev, IORESOURCE_IRQ, propname);

	if (!r) {
		pr_err("ARM64 CPU ERP: Could not find <%s> IRQ property. Proceeding anyway.\n",
			propname);
		goto out;
	}

	if (!percpu) {
		rc = devm_request_threaded_irq(&pdev->dev, r->start, NULL,
					       handler,
					       IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					       desc,
					       ed);

		if (rc) {
			pr_err("ARM64 CPU ERP: Failed to request IRQ %d: %d (%s / %s). Proceeding anyway.\n",
			       (int) r->start, rc, propname, desc);
			goto out;
		}

	} else {
		drv->erp_cpu_drvdata = alloc_percpu(struct erp_drvdata *);
		if (!drv->erp_cpu_drvdata) {
			pr_err("Failed to allocate percpu erp data\n");
			goto out;
		}

		*raw_cpu_ptr(drv->erp_cpu_drvdata) = drv;
		rc = request_percpu_irq(r->start, handler, desc,
				drv->erp_cpu_drvdata);

		if (rc) {
			pr_err("ARM64 CPU ERP: Failed to request IRQ %d: %d (%s / %s). Proceeding anyway.\n",
			       (int) r->start, rc, propname, desc);
			goto out_free;
		}

		drv->ppi = r->start;
		on_each_cpu(l1_l2_irq_enable, &(r->start), 1);
	}

	return 0;

out_free:
	free_percpu(drv->erp_cpu_drvdata);
	drv->erp_cpu_drvdata = NULL;
out:
	return -EINVAL;
}

static void dump_err_reg(int errorcode, int level, u64 errxstatus, u64 errxmisc,
	struct edac_device_ctl_info *edev_ctl)
{
	edac_printk(KERN_CRIT, EDAC_CPU, "ERRXSTATUS_EL1: %llx\n", errxstatus);
	edac_printk(KERN_CRIT, EDAC_CPU, "ERRXMISC_EL1: %llx\n", errxmisc);
	edac_printk(KERN_CRIT, EDAC_CPU, "Cache level: L%d\n", level + 1);

	switch (KRYO3XX_ERRXSTATUS_SERR(errxstatus)) {
	case DATA_BUF_ERR:
		edac_printk(KERN_CRIT, EDAC_CPU, "ECC Error from internal data buffer\n");
		break;

	case CACHE_DATA_ERR:
		edac_printk(KERN_CRIT, EDAC_CPU, "ECC Error from cache data RAM\n");
		break;

	case CACHE_TAG_DIRTY_ERR:
		edac_printk(KERN_CRIT, EDAC_CPU, "ECC Error from cache tag or dirty RAM\n");
		break;

	case TLB_PARITY_ERR_DATA:
		edac_printk(KERN_CRIT, EDAC_CPU, "Parity error on TLB RAM\n");
		break;

	case TLB_PARITY_ERR_TAG:
		edac_printk(KERN_CRIT, EDAC_CPU, "Parity error on TLB DATA\n");

	case BUS_ERROR:
		edac_printk(KERN_CRIT, EDAC_CPU, "Bus Error\n");
		break;
	}

	if (level == L3)
		edac_printk(KERN_CRIT, EDAC_CPU,
			"Way: %d\n", (int) KRYO3XX_ERRXMISC_WAY(errxmisc));
	else
		edac_printk(KERN_CRIT, EDAC_CPU,
			"Way: %d\n", (int) KRYO3XX_ERRXMISC_WAY(errxmisc) >> 2);

	if (level == L3)
		update_arm64_edac_count(0, level, errorcode);
	else
		update_arm64_edac_count(smp_processor_id(), level, errorcode);

	edev_ctl->panic_on_ce = panic_on_ce;
	errors[errorcode].func(edev_ctl, smp_processor_id(),
				level, errors[errorcode].msg);
}

static void kryo3xx_parse_l1_l2_cache_error(u64 errxstatus, u64 errxmisc,
	struct edac_device_ctl_info *edev_ctl)
{
	switch (KRYO3XX_ERRXMISC_LVL(errxmisc)) {
	case L1:
		if (KRYO3XX_ERRXSTATUS_UE(errxstatus))
			dump_err_reg(KRYO3XX_L1_UE, L1, errxstatus, errxmisc,
				edev_ctl);
		else
			dump_err_reg(KRYO3XX_L1_CE, L1, errxstatus, errxmisc,
				edev_ctl);
		break;
	case L2:
		if (KRYO3XX_ERRXSTATUS_UE(errxstatus))
			dump_err_reg(KRYO3XX_L2_UE, L2, errxstatus, errxmisc,
				edev_ctl);
		else
			dump_err_reg(KRYO3XX_L2_CE, L2, errxstatus, errxmisc,
				edev_ctl);
		break;
	}

}

void kryo3xx_check_l1_l2_ecc(void *info)
{
	struct edac_device_ctl_info *edev_ctl = info;
	u64 errxstatus = 0;
	u64 errxmisc = 0;
	unsigned long flags;

	if (panic_handler_drvdata == NULL)
		return;

	if (edev_ctl == NULL)
		edev_ctl = panic_handler_drvdata->edev_ctl;
	spin_lock_irqsave(&local_handler_lock, flags);
	write_errselr_el1(0);
	errxstatus = read_errxstatus_el1();
	if (KRYO3XX_ERRXSTATUS_VALID(errxstatus)) {
		errxmisc = read_errxmisc_el1();
		edac_printk(KERN_CRIT, EDAC_CPU,
		"Kryo3xx CPU%d detected a L1/L2 cache error\n",
		smp_processor_id());

		kryo3xx_parse_l1_l2_cache_error(errxstatus, errxmisc, edev_ctl);
		clear_errxstatus_valid(errxstatus);
	}
	spin_unlock_irqrestore(&local_handler_lock, flags);
}

static bool l3_is_bus_error(u64 errxstatus)
{
	if (KRYO3XX_ERRXSTATUS_SERR(errxstatus) == BUS_ERROR) {
		edac_printk(KERN_CRIT, EDAC_CPU, "Bus Error\n");
		update_arm64_edac_count(0, ID_BUS_ERR, BUS_ERROR);
		return true;
	}

	return false;
}

void kryo3xx_check_l3_scu_error(void *info)
{
	struct edac_device_ctl_info *edev_ctl = info;
	u64 errxstatus = 0;
	u64 errxmisc = 0;
	unsigned long flags;

	if (panic_handler_drvdata == NULL)
		return;

	if (edev_ctl == NULL)
		edev_ctl = panic_handler_drvdata->edev_ctl;
	spin_lock_irqsave(&local_handler_lock, flags);
	write_errselr_el1(1);
	errxstatus = read_errxstatus_el1();
	errxmisc = read_errxmisc_el1();

	if (KRYO3XX_ERRXSTATUS_VALID(errxstatus) &&
		KRYO3XX_ERRXMISC_LVL(errxmisc) == L3) {
		if (l3_is_bus_error(errxstatus)) {
			if (edev_ctl->panic_on_ue)
				panic("Causing panic due to Bus Error\n");
			return;
		}
		if (KRYO3XX_ERRXSTATUS_UE(errxstatus)) {
			edac_printk(KERN_CRIT, EDAC_CPU, "Detected L3 uncorrectable error\n");
			dump_err_reg(KRYO3XX_L3_UE, L3, errxstatus, errxmisc,
				edev_ctl);
		} else {
			edac_printk(KERN_CRIT, EDAC_CPU, "Detected L3 correctable error\n");
			dump_err_reg(KRYO3XX_L3_CE, L3, errxstatus, errxmisc,
				edev_ctl);
		}

		clear_errxstatus_valid(errxstatus);
	}
	spin_unlock_irqrestore(&local_handler_lock, flags);
}

#ifdef CONFIG_EDAC_KRYO3XX_ARM64_POLL
static void kryo3xx_poll_cache_errors(struct edac_device_ctl_info *edev_ctl)
{
	int cpu;

	if (edev_ctl == NULL)
		edev_ctl = panic_handler_drvdata->edev_ctl;

	kryo3xx_check_l3_scu_error(edev_ctl);
	for_each_possible_cpu(cpu)
		smp_call_function_single(cpu, kryo3xx_check_l1_l2_ecc,
			edev_ctl, 0);
}
#endif

static irqreturn_t kryo3xx_l1_l2_handler(int irq, void *drvdata)
{
	kryo3xx_check_l1_l2_ecc(panic_handler_drvdata->edev_ctl);
	return IRQ_HANDLED;
}

static irqreturn_t kryo3xx_l3_scu_handler(int irq, void *drvdata)
{
	struct erp_drvdata *drv = drvdata;
	struct edac_device_ctl_info *edev_ctl = drv->edev_ctl;

	kryo3xx_check_l3_scu_error(edev_ctl);
	return IRQ_HANDLED;
}

static void initialize_registers(void *info)
{
	set_errxctlr_el1();
	set_errxmisc_overflow();
}

static void init_regs_on_cpu(bool all_cpus)
{
	int cpu;

	write_errselr_el1(0);
	if (all_cpus) {
		for_each_possible_cpu(cpu)
			smp_call_function_single(cpu, initialize_registers,
						NULL, 1);
	} else
		initialize_registers(NULL);

	write_errselr_el1(1);
	initialize_registers(NULL);
}

static int kryo3xx_pmu_cpu_pm_notify(struct notifier_block *self,
				unsigned long action, void *v)
{
	switch (action) {
	case CPU_PM_EXIT:
		init_regs_on_cpu(false);
		kryo3xx_check_l3_scu_error(panic_handler_drvdata->edev_ctl);
		kryo3xx_check_l1_l2_ecc(panic_handler_drvdata->edev_ctl);
		break;
	}

	return NOTIFY_OK;
}

static int arm64_edac_dbg_part_notifier_callback(
	struct notifier_block *nfb, unsigned long action, void *data)
{
	switch (action) {
	case DBG_PART_DRV_INIT_DONE:
		p_health = ap_health_data_read();
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static struct notifier_block arm64_edac_dbg_part_notifier = {
	.notifier_call = arm64_edac_dbg_part_notifier_callback,
};


static int kryo3xx_cpu_erp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct erp_drvdata *drv;
	int rc = 0;
	int fail = 0;

	init_regs_on_cpu(true);

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);

	if (!drv)
		return -ENOMEM;

	drv->edev_ctl = edac_device_alloc_ctl_info(0, "cpu",
					num_possible_cpus(), "L", 3, 1, NULL, 0,
					edac_device_alloc_index());

	if (!drv->edev_ctl)
		return -ENOMEM;

	#ifdef CONFIG_EDAC_KRYO3XX_ARM64_POLL
	drv->edev_ctl->edac_check = kryo3xx_poll_cache_errors;
	drv->edev_ctl->poll_msec = poll_msec;
	drv->edev_ctl->defer_work = 1;
	#endif

	drv->edev_ctl->dev = dev;
	drv->edev_ctl->mod_name = dev_name(dev);
	drv->edev_ctl->dev_name = dev_name(dev);
	drv->edev_ctl->ctl_name = "cache";
	drv->edev_ctl->panic_on_ce = panic_on_ce;
	drv->edev_ctl->panic_on_ue = ARM64_ERP_PANIC_ON_UE;
	drv->nb_pm.notifier_call = kryo3xx_pmu_cpu_pm_notify;
	platform_set_drvdata(pdev, drv);

	rc = edac_device_add_device(drv->edev_ctl);
	if (rc)
		goto out_mem;

	panic_handler_drvdata = drv;

	if (request_erp_irq(pdev, "l1-l2-faultirq",
			"KRYO3XX L1-L2 ECC FAULTIRQ",
			kryo3xx_l1_l2_handler, drv, 1))
		fail++;

	if (request_erp_irq(pdev, "l3-scu-faultirq",
			"KRYO3XX L3-SCU ECC FAULTIRQ",
			kryo3xx_l3_scu_handler, drv, 0))
		fail++;

	if (fail == of_irq_count(dev->of_node)) {
		pr_err("KRYO3XX ERP: Could not request any IRQs. Giving up.\n");
		rc = -ENODEV;
		goto out_dev;
	}

	cpu_pm_register_notifier(&(drv->nb_pm));

	dbg_partition_notifier_register(&arm64_edac_dbg_part_notifier);

	return 0;

out_dev:
	edac_device_del_device(dev);
out_mem:
	edac_device_free_ctl_info(drv->edev_ctl);
	return rc;
}

static int kryo3xx_cpu_erp_remove(struct platform_device *pdev)
{
	struct erp_drvdata *drv = dev_get_drvdata(&pdev->dev);
	struct edac_device_ctl_info *edac_ctl = drv->edev_ctl;


	if (drv->erp_cpu_drvdata != NULL) {
		free_percpu_irq(drv->ppi, drv->erp_cpu_drvdata);
		free_percpu(drv->erp_cpu_drvdata);
	}

	edac_device_del_device(edac_ctl->dev);
	edac_device_free_ctl_info(edac_ctl);

	return 0;
}

static const struct of_device_id kryo3xx_cpu_erp_match_table[] = {
	{ .compatible = "arm,arm64-kryo3xx-cpu-erp" },
	{ }
};

static struct platform_driver kryo3xx_cpu_erp_driver = {
	.probe = kryo3xx_cpu_erp_probe,
	.remove = kryo3xx_cpu_erp_remove,
	.driver = {
		.name = "kryo3xx_cpu_cache_erp",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(kryo3xx_cpu_erp_match_table),
	},
};

static int __init kryo3xx_cpu_erp_init(void)
{
	return platform_driver_register(&kryo3xx_cpu_erp_driver);
}
module_init(kryo3xx_cpu_erp_init);

static void __exit kryo3xx_cpu_erp_exit(void)
{
	platform_driver_unregister(&kryo3xx_cpu_erp_driver);
}
module_exit(kryo3xx_cpu_erp_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Kryo3xx EDAC driver");
