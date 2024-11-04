#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/soc/hailo/scmi_hailo_ops.h>

#define	SCMI_HAILO_BOOT_SUCCESS_AP_SOFTWARE  1
#define	SCMI_HAILO_BOOT_SUCCESS_SW_UPDATE 99

static const struct scmi_hailo_ops *hailo_ops;

static const struct of_device_id hailo_soc_of_match[] = {
	{ .compatible = "hailo,hailo15" },
	{ .compatible = "hailo,hailo15l" },
	{}
};

struct __attribute__((packed)) hailo_fuse_file {
	u8 user_fuse_array[SCMI_HAILO_PROTOCOL_USER_FUSE_DATA_SIZE];
	u32 active_clusters;
};

struct hailo_soc {
	struct hailo_fuse_file fuse_file;
	struct scmi_hailo_get_boot_info_p2a boot_info;
	struct kernfs_node *fuse_kn;
	struct soc_device *soc_dev;
};

#define H15__SCU_BOOT_BIT_MASK (3)
static const char *hailo15_boot_options[] = { "Flash", "UART", "PCIe", "EMMC",
					      "N/A" };

static ssize_t boot_success_scu_bl_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.boot_status_bitmap.boot_success_scu_bl);
}

static ssize_t boot_success_scu_fw_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.boot_status_bitmap.boot_success_scu_fw);
}

static ssize_t boot_success_ap_bootloader_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.boot_status_bitmap.boot_success_ap_bootloader);
}

static ssize_t boot_success_ap_software_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.boot_status_bitmap.boot_success_ap_software);
}

static ssize_t boot_success_ap_software_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);
	struct scmi_hailo_boot_success_indication_a2p params;
	u8 val;
	int ret;

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	hailo_ops = scmi_hailo_get_ops();
	if (IS_ERR(hailo_ops))
		return PTR_ERR(hailo_ops);

	// value == 1 indicates that linux has booted successfully
	if(val == SCMI_HAILO_BOOT_SUCCESS_AP_SOFTWARE)
	{
		hailo_soc->boot_info.boot_status_bitmap.boot_success_ap_software = val;

		pr_info("SCU booted from:                %s",
			hailo15_boot_options[
				hailo_soc->boot_info.active_boot_image_storage & H15__SCU_BOOT_BIT_MASK]);

		// Send SCMI message to SCU FW, indicating linux boot success
		params.component = SCMI_HAILO_BOOT_SUCCESS_COMPONENT_AP_SOFTWARE;
		ret = hailo_ops->send_boot_success_ind(&params);
		if (ret) {
			dev_err(dev, "Failed to send boot success indication\n");
			return ret;
		}
	}
	
	// value == 99 indicates swupdate procedure has just completed
	else if(val == SCMI_HAILO_BOOT_SUCCESS_SW_UPDATE)
	{
		// send SCMI message to SCU FW, indicating swupdate procedure has completed
		ret = hailo_ops->send_swupdate_ind();
		if (ret) {
			dev_err(dev, "Failed to send swupdate indication\n");
			return ret;
		}	
	}

	else
	{
		dev_err(dev, "boot_success_ap_software_store: invalid value (=%d)\n", val);
		return -EINVAL;
	}

	return count;
}

static ssize_t boot_count_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.boot_count);
}

static ssize_t active_image_desc_index_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.active_image_desc_index);
}

static ssize_t active_boot_image_storage_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.active_boot_image_storage);
}

static ssize_t active_boot_image_offset_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "0x%08X\n",
		       hailo_soc->boot_info.active_boot_image_offset);
}

static ssize_t bootstrap_image_storage_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       hailo_soc->boot_info.bootstrap_image_storage);
}

static DEVICE_ATTR_RO(boot_success_scu_bl);
static DEVICE_ATTR_RO(boot_success_scu_fw);
static DEVICE_ATTR_RO(boot_success_ap_bootloader);
static DEVICE_ATTR_RW(boot_success_ap_software);
static DEVICE_ATTR_RO(boot_count);
static DEVICE_ATTR_RO(active_image_desc_index);
static DEVICE_ATTR_RO(active_boot_image_storage);
static DEVICE_ATTR_RO(active_boot_image_offset);
static DEVICE_ATTR_RO(bootstrap_image_storage);

static struct attribute *hailo_boot_info_attrs[] = {
	&dev_attr_boot_success_scu_bl.attr,
	&dev_attr_boot_success_scu_fw.attr,
	&dev_attr_boot_success_ap_bootloader.attr,
	&dev_attr_boot_success_ap_software.attr,
	&dev_attr_boot_count.attr,
	&dev_attr_active_image_desc_index.attr,
	&dev_attr_active_boot_image_storage.attr,
	&dev_attr_active_boot_image_offset.attr,
	&dev_attr_bootstrap_image_storage.attr,
	NULL,
};

static const struct attribute_group hailo_boot_info_group = { .name = "boot_info", .attrs = hailo_boot_info_attrs, };

static ssize_t fuse_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	memcpy(buf, &hailo_soc->fuse_file, sizeof(hailo_soc->fuse_file));
	return sizeof(hailo_soc->fuse_file);
}

static DEVICE_ATTR_RO(fuse);

static struct attribute *hailo_attrs[] = { &dev_attr_fuse.attr, NULL };

ATTRIBUTE_GROUPS(hailo);

static int hailo_soc_fill_fuse_file(struct hailo_fuse_file *fuse_file)
{
	struct scmi_hailo_get_fuse_info_p2a fuse_info;
	int ret;

	ret = hailo_ops->get_fuse_info(&fuse_info);
	if (ret) {
		return ret;
	}

	memcpy(&fuse_file->user_fuse_array, &fuse_info.user_fuse, sizeof(struct scmi_hailo_user_fuse));

	fuse_file->active_clusters = fuse_info.active_clusters;

	return 0;
}

static int hailo_soc_probe(struct platform_device *pdev)
{
	struct soc_device *soc_dev;
	struct device *dev;
	struct soc_device_attribute *soc_dev_attr;
	struct hailo_soc *hailo_soc;

	int ret;

	hailo_ops = scmi_hailo_get_ops();
	if (IS_ERR(hailo_ops)) {
		return PTR_ERR(hailo_ops);
	}

	hailo_soc = kzalloc(sizeof(*hailo_soc), GFP_KERNEL);
	if (!hailo_soc)
		return -ENOMEM;

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	soc_dev_attr->family = "Hailo15";
	soc_dev_attr->custom_attr_group = hailo_groups[0];

	ret = hailo_soc_fill_fuse_file(&hailo_soc->fuse_file);
	if (ret) {
		dev_err(&pdev->dev, "Failed to fill fuse info\n");
		return ret;
	}

	ret = hailo_ops->get_boot_info(&hailo_soc->boot_info);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get boot info\n");
		return ret;
	}

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		kfree(soc_dev_attr);
		return -ENODEV;
	}

	dev = soc_device_to_device(soc_dev);

	// Create attribute group "boot_info" under hailo soc device
	ret = sysfs_create_group(&dev->kobj, &hailo_boot_info_group);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create boot_info group\n");
		return ret;
	}

	hailo_soc->soc_dev = soc_dev;
	dev_set_drvdata(dev, hailo_soc);

	return 0;
}

static int hailo_soc_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hailo_soc *hailo_soc = dev_get_drvdata(dev);

	soc_device_unregister(hailo_soc->soc_dev);

	return 0;
}

static struct platform_driver hailo_soc_driver = {
	.probe = hailo_soc_probe,
	.remove = hailo_soc_remove,
	.driver = {
		.name = "hailo-soc",
		.of_match_table = hailo_soc_of_match,
	},
};
builtin_platform_driver(hailo_soc_driver);
