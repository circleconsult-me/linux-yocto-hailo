#include <linux/module.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/of_graph.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ctrls.h>
#include <isp_ctrl/hailo15_isp_ctrl.h>
#include <stdbool.h>
#include "hailo15-isp.h"
#include "hailo15-isp-hw.h"
#include "hailo15-isp-events.h"
#include "hailo15-media.h"
#include "common.h"


#define HAILO15_ISP_NAME_SIZE 10
#define HAILO15_ISP_ADDR_SPACE_MAX_SIZE 0x10000
#define HAILO15_ISP_RMEM_SIZE (32 * 1024 * 1024)
#define HAILO15_ISP_MCM_MODE_RMEM_EXT_SIZE (2 * 3840 * 2160 * 2)
#define HAILO15_ISP_FBUF_SIZE (3840 * 2160 * 2)
#define HAILO15_ISP_FAKEBUF_SIZE (3840 * 2160 * 3)

#define VID_GRP_TO_ISP_PATH(vid_grp)                                           \
	({                                                                     \
		int __path;                                                    \
		do {                                                           \
			switch (vid_grp) {                                     \
			case VID_GRP_ISP_MP:                                   \
				__path = ISP_MP;                               \
				break;                                         \
			case VID_GRP_ISP_SP:                                   \
				__path = ISP_SP2;                              \
				break;                                         \
			case VID_GRP_MCM_IN:                                   \
				__path = ISP_MCM_IN;                           \
				break;                                         \
			default:                                               \
				__path = -EINVAL;                              \
				break;                                         \
			}                                                      \
		} while (0);                                                   \
		__path;                                                        \
	})

#define ISP_PATH_TO_VID_GRP(isp_path)                                          \
	({                                                                     \
		int __path;                                                    \
		do {                                                           \
			switch (isp_path) {                                    \
			case ISP_MP:                                           \
				__path = VID_GRP_ISP_MP;                       \
				break;                                         \
			case ISP_SP2:                                          \
				__path = VID_GRP_ISP_SP;                       \
				break;                                         \
			case ISP_MCM_IN:                                       \
			        __path = VID_GRP_MCM_IN;                       \
				break;                                         \
			default:                                               \
				__path = -EINVAL;                              \
				break;                                         \
			}                                                      \
		} while (0);                                                   \
		__path;                                                        \
	})

extern uint32_t hailo15_isp_read_reg(struct hailo15_isp_device *, uint32_t);
extern void hailo15_isp_write_reg(struct hailo15_isp_device *, uint32_t,
				  uint32_t);
extern void hailo15_isp_configure_frame_base(struct hailo15_isp_device *,
						dma_addr_t *, unsigned int);
extern void hailo15_isp_configure_frame_size(struct hailo15_isp_device *, int);
extern irqreturn_t hailo15_isp_irq_process(struct hailo15_isp_device *);
extern void hailo15_isp_handle_afm_int(struct work_struct *);
extern int hailo15_isp_dma_set_enable(struct hailo15_isp_device *, int, int);
extern void hailo15_fe_get_dev(struct vvcam_fe_dev** dev);
extern void hailo15_fe_set_address_space_base(struct vvcam_fe_dev* fe_dev,void* base);

struct hailo15_af_kevent af_kevent;
EXPORT_SYMBOL(af_kevent);

struct vvcam_fe_dev* fe_dev = NULL;

static irqreturn_t hailo15_isp_irq_handler(int irq, void *isp_dev)
{
    return hailo15_isp_irq_process(isp_dev);
}

static struct hailo15_isp_device* isp_dev_from_v4l2_subdev(struct v4l2_subdev *sd)
{
    struct hailo15_dma_ctx *ctx = v4l2_get_subdevdata(sd);
	return ctx->dev;
}

static inline struct fwnode_handle *
hailo15_isp_pad_get_ep(struct hailo15_isp_device *isp_dev, int pad_nr)
{
	return fwnode_graph_get_endpoint_by_id(dev_fwnode(isp_dev->dev), pad_nr,
						   0, FWNODE_GRAPH_ENDPOINT_NEXT);
}

static inline int hailo15_isp_put_ep(struct fwnode_handle *ep)
{
	fwnode_handle_put(ep);
	return 0;
}

static inline void
hailo15_isp_configure_buffer(struct hailo15_isp_device *isp_dev,
				 struct hailo15_buffer *buf)
{
	// Validate that buffer address is in a specific range? 
	int isp_path;
	unsigned long flags;

	if(!buf)
		return;

	isp_path = VID_GRP_TO_ISP_PATH(buf->grp_id);
	if (isp_path < 0 || isp_path >= ISP_MAX_PATH)
		return;
	if(isp_path == ISP_MCM_IN){
		if(!isp_dev->rdma_enable){
			pr_err("trying to configure mcm buffer with rdma disabled!\n");
			return;
		}
		spin_lock_irqsave(&isp_dev->mcm_lock, flags);
		if(isp_dev->cur_buf[isp_path]){
			list_add_tail(&buf->irqlist, &isp_dev->mcm_queue);
			spin_unlock_irqrestore(&isp_dev->mcm_lock, flags);
			return;
		}

		isp_dev->cur_buf[isp_path] = buf;
		spin_unlock_irqrestore(&isp_dev->mcm_lock, flags);
	}else{
		isp_dev->cur_buf[isp_path] = buf;
	}
	hailo15_isp_configure_frame_base(isp_dev, buf->dma, isp_path);
}

static int hailo15_isp_requbufs(struct v4l2_subdev *sd, void *arg)
{
    struct hailo15_reqbufs *pad_requbufs = (struct hailo15_reqbufs *)arg;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);

    return hailo15_isp_post_event_requebus(isp_dev, pad_requbufs->pad, pad_requbufs->num_buffers);
}

static int hailo15_isp_pad_s_stream(struct v4l2_subdev *sd, void *arg)
{
    struct hailo15_pad_stream_status *pad_stream = (struct hailo15_pad_stream_status *)arg;
    struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);

    isp_dev->pad_data[pad_stream->pad].stream = pad_stream->status;

    /* This function does not actually start the stream on the selected pad at the moment
     * It is only used to set the stream status on the pad_data structure.
     * The stream is started by the hailo15_isp_s_stream_event function,
     * which currently always uses pad 0 */

    return 0;
}

static int hailo15_isp_mcm_extract_mode(struct v4l2_subdev *sd, void *arg)
{
    struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
    uint32_t *mcm_mode = (uint32_t *)arg;

    *mcm_mode = isp_dev->mcm_mode;
    return 0;
}

static int hailo15_isp_mcm_set_mode(struct v4l2_subdev *sd, void *arg)
{
    struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
    uint32_t *mcm_mode = (uint32_t *)arg;

    isp_dev->mcm_mode = *mcm_mode;
    return 0;
}


/******************************/
/* VSI infrastructure support */
/******************************/
static int hailo15_vsi_isp_qcap(struct v4l2_subdev *sd, void *arg)
{
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct v4l2_capability *cap = (struct v4l2_capability *)arg;
	/* SHOULD BE COPY_TO_USER */
	strlcpy((char *)cap->driver, HAILO15_ISP_NAME, sizeof(cap->driver));
	cap->bus_info[0] = isp_dev->id;
	return 0;
}

static int hailo15_isp_queryctrl(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_queryctrl *pad_querctrl =
		(struct hailo15_pad_queryctrl *)arg;
	ret = v4l2_queryctrl(&isp_dev->ctrl_handler, pad_querctrl->query_ctrl);

	return ret;
}

static int hailo15_isp_query_ext_ctrl(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_query_ext_ctrl *pad_quer_ext_ctrl =
		(struct hailo15_pad_query_ext_ctrl *)arg;
	ret = v4l2_query_ext_ctrl(&isp_dev->ctrl_handler,
				  pad_quer_ext_ctrl->query_ext_ctrl);

	return ret;
}

static int hailo15_isp_querymenu(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_querymenu *pad_quermenu =
		(struct hailo15_pad_querymenu *)arg;
	ret = v4l2_querymenu(&isp_dev->ctrl_handler, pad_quermenu->querymenu);

	return ret;
}

static int hailo15_isp_g_ctrl(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_control *pad_ctrl =
		(struct hailo15_pad_control *)arg;

	mutex_lock(&isp_dev->ctrl_lock);
	isp_dev->ctrl_pad = pad_ctrl->pad;
	ret = v4l2_g_ctrl(&isp_dev->ctrl_handler, pad_ctrl->control);
	mutex_unlock(&isp_dev->ctrl_lock);

	return ret;
}

static int hailo15_isp_s_ctrl(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_control *pad_ctrl =
		(struct hailo15_pad_control *)arg;

	mutex_lock(&isp_dev->ctrl_lock);
	isp_dev->ctrl_pad = pad_ctrl->pad;
	ret = v4l2_s_ctrl(NULL, &isp_dev->ctrl_handler, pad_ctrl->control);
	mutex_unlock(&isp_dev->ctrl_lock);

	return ret;
}

static int hailo15_isp_g_ext_ctrls(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_ext_controls *pad_ext_ctrls =
		(struct hailo15_pad_ext_controls *)arg;

	mutex_lock(&isp_dev->ctrl_lock);
	isp_dev->ctrl_pad = pad_ext_ctrls->pad;
	ret = v4l2_g_ext_ctrls(&isp_dev->ctrl_handler, sd->devnode,
				   sd->v4l2_dev->mdev, pad_ext_ctrls->ext_controls);
	mutex_unlock(&isp_dev->ctrl_lock);

	return ret;
}

static int hailo15_isp_s_ext_ctrls(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_ext_controls *pad_ext_ctrls =
		(struct hailo15_pad_ext_controls *)arg;

	mutex_lock(&isp_dev->ctrl_lock);
	isp_dev->ctrl_pad = pad_ext_ctrls->pad;
	ret = v4l2_s_ext_ctrls(NULL, &isp_dev->ctrl_handler, sd->devnode,
				   sd->v4l2_dev->mdev, pad_ext_ctrls->ext_controls);
	mutex_unlock(&isp_dev->ctrl_lock);

	return ret;
}

static int hailo15_isp_try_ext_ctrls(struct v4l2_subdev *sd, void *arg)
{
	int ret;
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct hailo15_pad_ext_controls *pad_ext_ctrls =
		(struct hailo15_pad_ext_controls *)arg;
	ret = v4l2_try_ext_ctrls(&isp_dev->ctrl_handler, sd->devnode,
				 sd->v4l2_dev->mdev,
				 pad_ext_ctrls->ext_controls);

	return ret;
}

static int hailo15_isp_stat_subscribe(struct v4l2_subdev *sd, void *arg)
{
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
    struct hailo15_pad_stat_subscribe *pad_sub = (struct hailo15_pad_stat_subscribe*)arg;
    struct hailo15_isp_pad_data *cur_pad = &isp_dev->pad_data[pad_sub->pad];
    int ret = 0;

    if (pad_sub->type != HAILO15_UEVENT_ISP_STAT) {
        return -EINVAL;
    }

    if (pad_sub->id >= HAILO15_UEVENT_ISP_STAT_MAX) {
        ret = -EINVAL;
    } else {
        if (cur_pad->stat_sub[pad_sub->id].type) {
            ret = -EINVAL;
        } else {
            cur_pad->stat_sub[pad_sub->id] = *pad_sub;
        }
    }

    if (ret == 0)
        pr_info("subscribed to isp stat event %d on pad %d\n", pad_sub->id, pad_sub->pad);

    return ret;
}

static int hailo15_isp_stat_unsubscribe(struct v4l2_subdev *sd, void *arg)
{
	int ret = 0;
    struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
    struct hailo15_pad_stat_subscribe *pad_sub = (struct hailo15_pad_stat_subscribe *)arg;
    struct hailo15_isp_pad_data *cur_pad = &isp_dev->pad_data[pad_sub->pad];

    if (pad_sub->type != HAILO15_UEVENT_ISP_STAT) {
        return -EINVAL;
    }

    if (pad_sub->id >= HAILO15_UEVENT_ISP_STAT_MAX) {
        ret = -EINVAL;
    } else {
        if (cur_pad->stat_sub[pad_sub->id].type == 0) {
            pr_err("%s: stat event %d is not subscribed\n", __func__, pad_sub->id);
            return -EINVAL;
        }

        memset(&cur_pad->stat_sub[pad_sub->id], 0, sizeof(*pad_sub));
        pr_info("unsubscribed from isp stat event %d on pad %d\n", pad_sub->id, pad_sub->pad);
    }

    return ret;
}

static long hailo15_vsi_isp_priv_ioctl(struct v4l2_subdev *sd, unsigned int cmd,
					   void *arg)
{
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct isp_reg_data isp_reg;
	int ret = -EINVAL;
	int path;

	switch (cmd) {
	case VIDIOC_QUERYCAP:
		ret = hailo15_vsi_isp_qcap(sd, arg);
		break;
	case ISPIOC_V4L2_READ_REG:
		mutex_lock(&isp_dev->mlock);
		memcpy(&isp_reg, arg, sizeof(struct isp_reg_data));
		isp_reg.value = hailo15_isp_read_reg(isp_dev, isp_reg.reg);
		memcpy(arg, &isp_reg, sizeof(struct isp_reg_data));
		mutex_unlock(&isp_dev->mlock);
		ret = 0;
		break;
	case ISPIOC_V4L2_WRITE_REG:
		mutex_lock(&isp_dev->mlock);
		memcpy(&isp_reg, arg, sizeof(struct isp_reg_data));
		hailo15_isp_write_reg(isp_dev, isp_reg.reg, isp_reg.value);
		mutex_unlock(&isp_dev->mlock);
		ret = 0;
		break;
	case ISPIOC_V4L2_RMEM:
		isp_dev->rmem.size = HAILO15_ISP_RMEM_SIZE;
		if(isp_dev->mcm_mode){
			isp_dev->rmem.size += HAILO15_ISP_MCM_MODE_RMEM_EXT_SIZE;
		}
		if(!isp_dev->rmem_vaddr){
			isp_dev->rmem_vaddr = dma_alloc_coherent(
			isp_dev->dev, isp_dev->rmem.size, &isp_dev->rmem.addr, GFP_KERNEL);
			if (!isp_dev->rmem_vaddr) {
				dev_err(isp_dev->dev, "can't allocate rmem buffer\n");
				ret = -ENOMEM;
				break;
			}
		}

		memcpy(arg, &isp_dev->rmem, sizeof(isp_dev->rmem));
		ret = 0;
		break;
	case ISPIOC_V4L2_MI_START:
		mutex_lock(&isp_dev->mlock);
		for (path = ISP_MP; path < ISP_MAX_PATH; ++path) {
			if(isp_dev->mcm_mode && path == ISP_MCM_IN){
				hailo15_isp_configure_frame_size(isp_dev, path);
			}else if (isp_dev->cur_buf[path] &&
				isp_dev->mi_stopped[path] &&
				hailo15_isp_is_path_enabled(isp_dev, path)) {
				hailo15_isp_configure_frame_size(isp_dev, path);
				hailo15_isp_configure_buffer(
					isp_dev, isp_dev->cur_buf[path]);
				isp_dev->mi_stopped[path] = 0;
			}
		}
		mutex_unlock(&isp_dev->mlock);
		ret = 0;
		break;
	case ISPIOC_V4L2_MI_STOP:
		mutex_lock(&isp_dev->mlock);
		for (path = ISP_MP; path < ISP_MAX_PATH; ++path) {
			if (!hailo15_isp_is_path_enabled(isp_dev, path) &&
				!isp_dev->mi_stopped[path])
				isp_dev->mi_stopped[path] = 1;
		}
		mutex_unlock(&isp_dev->mlock);
		ret = 0;
		break;
	case ISPIOC_V4L2_SET_MCM_MODE:
        ret = hailo15_isp_mcm_set_mode(sd, arg);
		break;
	case ISPIOC_V4L2_MCM_MODE:
        ret = hailo15_isp_mcm_extract_mode(sd, arg);
		break;
	case ISPIOC_V4L2_REQBUFS:
		ret = hailo15_isp_requbufs(sd, arg);
		break;
	case ISPIOC_V4L2_SET_INPUT_FORMAT:
		memcpy(&isp_dev->input_fmt, (void *)arg,
			   sizeof(struct v4l2_subdev_format));
		ret = 0;
		break;
	case ISPIOC_V4L2_GET_NULL_ADDR:
		*((uint32_t*)arg) = isp_dev->null_addr;
		ret = 0;
		break;
	case ISPIOC_S_MIV_INFO:
	case ISPIOC_S_MIS_IRQADDR:
	case ISPIOC_S_MP_34BIT:
	case ISPIOC_RST_QUEUE:
	case ISPIOC_D_MIS_IRQADDR:
		ret = 0;
		break;

    case HAILO15_PAD_S_STREAM:
        ret = hailo15_isp_pad_s_stream(sd, arg);
        break;
	case HAILO15_PAD_QUERYCTRL:
		ret = hailo15_isp_queryctrl(sd, arg);
		break;
	case HAILO15_PAD_QUERY_EXT_CTRL:
		ret = hailo15_isp_query_ext_ctrl(sd, arg);
		break;
	case HAILO15_PAD_G_CTRL:
		ret = hailo15_isp_g_ctrl(sd, arg);
		break;
	case HAILO15_PAD_S_CTRL:
		ret = hailo15_isp_s_ctrl(sd, arg);
		break;
	case HAILO15_PAD_G_EXT_CTRLS:
		ret = hailo15_isp_g_ext_ctrls(sd, arg);
		break;
	case HAILO15_PAD_S_EXT_CTRLS:
		ret = hailo15_isp_s_ext_ctrls(sd, arg);
		break;
	case HAILO15_PAD_TRY_EXT_CTRLS:
		ret = hailo15_isp_try_ext_ctrls(sd, arg);
		break;
	case HAILO15_PAD_QUERYMENU:
		ret = hailo15_isp_querymenu(sd, arg);
		break;
	case HAILO15_PAD_STAT_SUBSCRIBE:
		ret = hailo15_isp_stat_subscribe(sd, arg);
		break;
	case HAILO15_PAD_STAT_UNSUBSCRIBE:
		ret = hailo15_isp_stat_unsubscribe(sd, arg);
		break;
	default:
		ret = -EINVAL;
		pr_debug("unsupported isp command %x.\n", cmd);
		break;
	}

	return ret;
}

static int hailo15_vsi_isp_init_events(struct hailo15_isp_device *isp_dev)
{
	size_t size = HAILO15_EVENT_RESOURCE_DATA_SIZE + offsetof(struct hailo15_isp_event_pkg, data);
	size_t size_page_align = PAGE_ALIGN(size);

	mutex_init(&isp_dev->event_resource.event_lock);
	isp_dev->event_resource.virt_addr = kmalloc(size_page_align, GFP_KERNEL);
	if (!isp_dev->event_resource.virt_addr)
		return -ENOMEM;

	isp_dev->event_resource.phy_addr =
		virt_to_phys(isp_dev->event_resource.virt_addr);
	isp_dev->event_resource.size = size;
	memset(isp_dev->event_resource.virt_addr, 0,
		   isp_dev->event_resource.size);
	return 0;
}

/*@TODO add clean events function*/

static int hailo15_vsi_isp_subscribe_event(struct v4l2_subdev *subdev,
					   struct v4l2_fh *fh,
					   struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subdev_subscribe_event(subdev, fh, sub);
	case HAILO15_DAEMON_ISP_EVENT:
	case HAILO15_ISP_EVENT_IRQ:
		return v4l2_event_subscribe(fh, sub, 8, NULL);
	default:
		return v4l2_event_subscribe(fh, sub,
						HAILO15_ISP_EVENT_QUEUE_SIZE, NULL);
	}
}

static int
hailo15_vsi_isp_unsubscribe_event(struct v4l2_subdev *subdev,
				  struct v4l2_fh *fh,
				  struct v4l2_event_subscription *sub)
{
	return v4l2_event_unsubscribe(fh, sub);
}

static int hailo15_isp_enable_clocks(struct hailo15_isp_device *isp_dev)
{
	int ret;

	ret = clk_prepare_enable(isp_dev->p_clk);
	if (ret) {
		pr_err("Couldn't prepare and enable P clock\n");
		return ret;
	}
	isp_dev->is_p_clk_enabled = 1;

	ret = clk_prepare_enable(isp_dev->ip_clk);
	if (ret) {
		pr_err("Couldn't prepare and enable IP clock\n");
		clk_disable_unprepare(isp_dev->p_clk);
		isp_dev->is_p_clk_enabled = 0;
		return ret;
	}
	isp_dev->is_ip_clk_enabled = 1;

	return ret;
}

static void hailo15_isp_disable_clocks(struct hailo15_isp_device *isp_dev)
{
	if (isp_dev->is_ip_clk_enabled) {
		clk_disable_unprepare(isp_dev->ip_clk);
		isp_dev->is_ip_clk_enabled = 0;
	}
	if (isp_dev->is_p_clk_enabled) {
		clk_disable_unprepare(isp_dev->p_clk);
		isp_dev->is_p_clk_enabled = 0;
	}
}

static struct v4l2_subdev_core_ops hailo15_isp_core_ops = {
	.ioctl = hailo15_vsi_isp_priv_ioctl,
	.subscribe_event = hailo15_vsi_isp_subscribe_event,
	.unsubscribe_event = hailo15_vsi_isp_unsubscribe_event,
};

static int hailo15_isp_refcnt_inc_enable(struct hailo15_isp_device *isp_dev)
{
	mutex_lock(&isp_dev->mlock);

	isp_dev->refcnt++;
	pm_runtime_get_sync(isp_dev->dev);

	if (isp_dev->refcnt == 1) {
		hailo15_isp_enable_clocks(isp_dev);
		hailo15_config_isp_wrapper(isp_dev);
	}

	mutex_unlock(&isp_dev->mlock);
	return 0;
}

static int hailo15_isp_refcnt_dec_disable(struct hailo15_isp_device *isp_dev)
{
	int ret;

	ret = 0;
	mutex_lock(&isp_dev->mlock);
	if (isp_dev->refcnt == 0) {
		ret = -EINVAL;
		goto out;
	}

	isp_dev->refcnt--;

	if (isp_dev->refcnt == 0) {
		pm_runtime_put_sync(isp_dev->dev);
		hailo15_isp_disable_clocks(isp_dev);
		goto out;
	}

out:
	mutex_unlock(&isp_dev->mlock);
	return ret;
}

/****************************/
/* v4l2 subdevice video ops */
/****************************/
static int hailo15_isp_s_stream(struct v4l2_subdev *sd, int enable)
{
	int ret;
	int path;
	unsigned long flags;
	struct v4l2_subdev *subdev;
	struct media_pad *pad;
	struct hailo15_buffer *pos, *npos;
	struct hailo15_miv2_mis *miv_pos, *miv_npos;
	struct hailo15_isp_device *isp_dev =
		container_of(sd, struct hailo15_isp_device, sd);
	struct hailo15_dma_ctx *ctx = v4l2_get_subdevdata(&isp_dev->sd);

	if (enable) {
		path = VID_GRP_TO_ISP_PATH(sd->grp_id);
		if (path < 0 || path >= ISP_MAX_PATH)
			return -EINVAL;
		if(path == ISP_MCM_IN){
			isp_dev->mcm_mode = 1;
			isp_dev->rdma_enable = 1;
			isp_dev->fe_enable = 1;
			isp_dev->dma_ready = 0;
			isp_dev->frame_end = 0;
			isp_dev->fe_ready = 0;
			return 0;
		}

		ret = hailo15_isp_refcnt_inc_enable(isp_dev);
		if (ret)
			return ret;

		hailo15_config_isp_wrapper(isp_dev);

		isp_dev->queue_empty[path] = 0;
		isp_dev->current_vsm_index[path] = -1;
		ret = hailo15_isp_post_event_start_stream(isp_dev);
		if (ret) {
			pr_warn("%s - start stream event failed with %d\n", __func__, ret);
			return ret;
		}
	}
	if(!isp_dev->rdma_enable){
		pad = &isp_dev->pads[HAILO15_ISP_SINK_PAD_S0];
		if (pad)
			pad = media_entity_remote_pad(pad);

		if (pad && is_media_entity_v4l2_subdev(pad->entity)) {
			subdev = media_entity_to_v4l2_subdev(pad->entity);
			subdev->grp_id = sd->grp_id;
			v4l2_subdev_call(subdev, video, s_stream, enable);
		}
	}

	// disable clocks only after disabling the stream for all the subdevs
	if (!enable) {
		path = VID_GRP_TO_ISP_PATH(sd->grp_id);
		if (path < 0 || path >= ISP_MAX_PATH)
			return -EINVAL;
		if(path == ISP_MCM_IN){
			isp_dev->rdma_enable = 0;
			isp_dev->dma_ready = 0;
			isp_dev->frame_end = 0;
			isp_dev->fe_ready = 0;
			isp_dev->fe_enable = 0;
			spin_lock_irqsave(&isp_dev->mcm_lock, flags);
			list_for_each_entry_safe(pos, npos, &isp_dev->mcm_queue, irqlist){
				hailo15_dma_buffer_done(ctx, ISP_PATH_TO_VID_GRP(path), pos);
				list_del(&pos->irqlist);
			}
			hailo15_dma_buffer_done(ctx, ISP_PATH_TO_VID_GRP(path), isp_dev->cur_buf[ISP_MCM_IN]);
			isp_dev->cur_buf[ISP_MCM_IN] = NULL;
			spin_unlock_irqrestore(&isp_dev->mcm_lock, flags);

			return 0;
		}
		isp_dev->mcm_mode = 0;
		isp_dev->frame_count[path] = 0;
		isp_dev->mi_stopped[path] = 1;
		isp_dev->cur_buf[path] = NULL;
		memset(&isp_dev->input_fmt, 0, sizeof(isp_dev->input_fmt));
		ret = hailo15_isp_post_event_stop_stream(isp_dev);
		if(isp_dev->rmem_vaddr){
			dma_free_coherent(isp_dev->dev, isp_dev->rmem.size,
				  isp_dev->rmem_vaddr, isp_dev->rmem.addr);
			isp_dev->rmem_vaddr = 0;
		}

		hailo15_isp_reset_hw(isp_dev);
		hailo15_isp_refcnt_dec_disable(isp_dev);
		spin_lock_irqsave(&isp_dev->miv2_mis_lock, flags);
		list_for_each_entry_safe(miv_pos, miv_npos, &isp_dev->miv2_mis_queue, list){
			list_del(&miv_pos->list);
			kfree(miv_pos);
		}
		spin_unlock_irqrestore(&isp_dev->miv2_mis_lock, flags);
		return ret;
	}
	
	if(isp_dev->mcm_mode){
		hailo15_isp_configure_frame_size(isp_dev, path);
		hailo15_isp_configure_buffer(isp_dev, isp_dev->cur_buf[path]);
	}

	if(isp_dev->rdma_enable)
		hailo15_isp_configure_frame_size(isp_dev, ISP_MCM_IN);
	return ret;
}

static struct v4l2_subdev_video_ops hailo15_isp_video_ops = {
	.s_stream = hailo15_isp_s_stream,
};

/**************************/
/* v4l2 subdevice pad ops */
/**************************/
static int hailo15_isp_set_fmt(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *format)

{
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	struct v4l2_subdev *sensor_sd;
	int ret = -EINVAL;
	struct media_pad *pad;
	int isp_path;
	unsigned int sink_pad_idx = HAILO15_ISP_SINK_PAD_MAX;
	struct v4l2_subdev *subdev;
	int max_width = INPUT_WIDTH;
	int max_height = INPUT_HEIGHT;

	if (isp_dev->input_fmt.format.width != 0 &&
		isp_dev->input_fmt.format.height != 0) {
		max_width = isp_dev->input_fmt.format.width;
		max_height = isp_dev->input_fmt.format.height;
	}

	/*We support only downscaling*/
	if (format->format.width > max_width ||
		format->format.height > max_height) {
		pr_debug("Unsupported resolution %dx%d\n", format->format.width,
			 format->format.height);
		return -EINVAL;
	}

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		return 0;
	}

	isp_path = VID_GRP_TO_ISP_PATH(sd->grp_id);
	if (isp_path < 0 || isp_path >= ISP_MAX_PATH)
		return -EINVAL;

	memcpy(&isp_dev->fmt[isp_path], format,
		   sizeof(struct v4l2_subdev_format));
	ret = hailo15_isp_post_event_set_fmt(
		isp_dev, isp_dev->pads[HAILO15_ISP_SOURCE_PAD_MP_S0].index,
		&(format->format));
	if (ret) {
		pr_warn("%s - set_fmt event failed with %d\n", __func__, ret);
		return ret;
	}
	
	if(isp_dev->rdma_enable){
		return 0;
	}
	
	sink_pad_idx = (int)(format->pad/2);
	pad = &isp_dev->pads[sink_pad_idx];
	if (pad) {
		pad = media_entity_remote_pad(pad);
	}
	
	if (pad && is_media_entity_v4l2_subdev(pad->entity)) {
		subdev = media_entity_to_v4l2_subdev(pad->entity);
		subdev->grp_id = sd->grp_id;
		ret = v4l2_subdev_call(subdev, pad, set_fmt, NULL, format);
		if (ret)
			return ret;
	}

	// if input_format was not set - no need to update the sensor
	if (isp_dev->input_fmt.format.width == 0 ||
		isp_dev->input_fmt.format.height == 0) {
		pr_debug("%s - input format was not set - no need to update the sensor\n", __func__);
		return 0;
	}
 
	sensor_sd = hailo15_get_sensor_subdev(isp_dev->sd.v4l2_dev->mdev);
	if (!sensor_sd) {
		pr_warn("%s - failed to get sensor subdev\n", __func__);
		return -EINVAL;
	}

	return v4l2_subdev_call(sensor_sd, pad, set_fmt, NULL,
				&isp_dev->input_fmt);

	return ret;
}

static const struct v4l2_subdev_pad_ops hailo15_isp_pad_ops = {
	.set_fmt = hailo15_isp_set_fmt,
};


/**********************/
/* v4l2 subdevice ops */
/**********************/

/* This is the parent structure that contains all supported v4l2 subdevice operations */
struct v4l2_subdev_ops hailo15_isp_subdev_ops = {
	.core = &hailo15_isp_core_ops,
	.video = &hailo15_isp_video_ops,
	.pad = &hailo15_isp_pad_ops,
};

/**********************/
/* media entity ops   */
/**********************/
static int hailo15_isp_link_setup(struct media_entity *entity,
				  const struct media_pad *local,
				  const struct media_pad *remote, u32 flags)
{
	return 0;
}

static const struct media_entity_operations hailo15_isp_entity_ops = {
	.link_setup = hailo15_isp_link_setup,
};


/********************************/
/*  v4l2 subdevice internal ops */
/********************************/
static int hailo15_isp_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);

	return hailo15_isp_refcnt_inc_enable(isp_dev);
}

static int hailo15_isp_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct hailo15_isp_device *isp_dev = isp_dev_from_v4l2_subdev(sd);
	return hailo15_isp_refcnt_dec_disable(isp_dev);
}

static int hailo15_isp_registered(struct v4l2_subdev* sd){
	struct hailo15_dma_ctx *ctx = v4l2_get_subdevdata(sd);
	struct hailo15_isp_device *isp_dev = ctx->dev;
	return hailo15_media_create_links(isp_dev->dev, &sd->entity, -1);
}

static struct v4l2_subdev_internal_ops hailo15_isp_internal_ops = {
	.open = hailo15_isp_open,
	.close = hailo15_isp_close,
	.registered = hailo15_isp_registered,
};

/**********************************************************/
/* Hailo15 buffer operations.                             */
/* this ops are defining the API between the video device */
/* and the isp device                                     */
/**********************************************************/
inline void hailo15_isp_buffer_done(struct hailo15_isp_device *isp_dev,
					int path)
{
	struct hailo15_buffer *buf;
	struct hailo15_dma_ctx *ctx = v4l2_get_subdevdata(&isp_dev->sd);
	unsigned long flags;

	if (path < 0 || path >= ISP_MAX_PATH)
		return;

	++isp_dev->frame_count[path];
	
	if(path == ISP_MCM_IN){
		spin_lock_irqsave(&isp_dev->mcm_lock, flags);
		buf = isp_dev->cur_buf[path];
		isp_dev->cur_buf[path] = list_first_entry_or_null(&isp_dev->mcm_queue, struct hailo15_buffer, irqlist);
		if(!buf){
			spin_unlock_irqrestore(&isp_dev->mcm_lock, flags);
			return;
		}
		if(isp_dev->cur_buf[path]){
			list_del(&isp_dev->cur_buf[path]->irqlist);
			hailo15_isp_configure_frame_base(isp_dev, buf->dma, path);
		}
		spin_unlock_irqrestore(&isp_dev->mcm_lock, flags);
	} else{

		buf = isp_dev->cur_buf[path];
		isp_dev->cur_buf[path] = NULL;

		if (isp_dev->current_vsm_index[path] >= 0 &&
			isp_dev->current_vsm_index[path] < HAILO15_MAX_BUFFERS) {
			isp_dev->vsm_list[path][isp_dev->current_vsm_index[path]] =
				isp_dev->current_vsm;
			memset(&isp_dev->current_vsm, 0, sizeof(struct hailo15_vsm));
		}

		if (buf) {
			isp_dev->current_vsm_index[path] = buf->vb.vb2_buf.index;
		} else {
			isp_dev->current_vsm_index[path] = -1;
		}
	}

	hailo15_dma_buffer_done(ctx, ISP_PATH_TO_VID_GRP(path), buf);
}

static int hailo15_isp_buffer_process(struct hailo15_dma_ctx *ctx,
					  struct hailo15_buffer *buf)
{
	struct v4l2_subdev *sd;
	struct hailo15_isp_device *isp_dev;
	int path;

	sd = buf->sd;
	isp_dev = container_of(sd, struct hailo15_isp_device, sd);

	/*configure buffer to hw*/
	hailo15_isp_configure_buffer(isp_dev, buf);

	path = VID_GRP_TO_ISP_PATH(buf->grp_id);
	if (path < 0 || path >= ISP_MAX_PATH)
		return -EINVAL;

	if (isp_dev->queue_empty[path]) {
		hailo15_isp_dma_set_enable(isp_dev, path, 1);
		isp_dev->queue_empty[path] = 0;
	}

	return 0;
}

static int hailo15_isp_get_frame_count(struct hailo15_dma_ctx *dma_ctx, int grp_id,
					   uint64_t *fc)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)dma_ctx->dev;
	int path;
	if (!fc)
		return -EINVAL;

	path = VID_GRP_TO_ISP_PATH(grp_id);
	if (path < 0 || path >= ISP_MAX_PATH)
		return -EINVAL;

	*fc = isp_dev->frame_count[path];
	return 0;
}

static int hailo15_isp_get_rmem(struct hailo15_dma_ctx *dma_ctx,
				struct hailo15_rmem *rmem)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)dma_ctx->dev;
	memcpy(rmem, &isp_dev->rmem, sizeof(struct hailo15_rmem));
	return 0;
}

static int
hailo15_isp_get_event_resource(struct hailo15_dma_ctx *dma_ctx,
				   struct hailo15_event_resource *resource)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)dma_ctx->dev;
	memcpy(resource, &isp_dev->event_resource,
		   sizeof(struct hailo15_event_resource));
	return 0;
}

static int hailo15_isp_get_fbuf(struct hailo15_dma_ctx *ctx,
				struct v4l2_framebuffer *fbuf)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)ctx->dev;
	fbuf->base = (void *)isp_dev->fbuf_phys;
	fbuf->fmt.sizeimage = HAILO15_ISP_FBUF_SIZE;
	return 0;
}

static int hailo15_isp_set_private_data(struct hailo15_dma_ctx *ctx, int grp_id,
					void *data)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)ctx->dev;
	int path;

	path = VID_GRP_TO_ISP_PATH(grp_id);
	if (path < 0 || path >= ISP_MAX_PATH)
		return -EINVAL;

	isp_dev->private_data[path] = data;
	return 0;
}

static int hailo15_isp_get_private_data(struct hailo15_dma_ctx *ctx, int grp_id,
					void **data)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)ctx->dev;
	int path;

	path = VID_GRP_TO_ISP_PATH(grp_id);

	if (!data)
		return -EINVAL;

	if (path < 0 || path >= ISP_MAX_PATH)
		return -EINVAL;

	*data = isp_dev->private_data[path];
	return 0;
}
static int hailo15_isp_get_vsm(struct hailo15_dma_ctx *ctx, int grp_id,
				   int index, struct hailo15_vsm *vsm)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)ctx->dev;
	int isp_path;
	if (index < 0 || index >= HAILO15_MAX_BUFFERS)
		return -EINVAL;

	isp_path = VID_GRP_TO_ISP_PATH(grp_id);
	if (isp_path < 0 || isp_path >= ISP_MAX_PATH) {
		return -EINVAL;
	}

	memcpy(vsm, &isp_dev->vsm_list[isp_path][index],
		   sizeof(struct hailo15_vsm));
	return 0;
}

static int hailo15_isp_queue_empty(struct hailo15_dma_ctx *ctx, int path)
{
	struct hailo15_isp_device *isp_dev =
		(struct hailo15_isp_device *)ctx->dev;
	dma_addr_t fakebuf_arr[FMT_MAX_PLANES];
	int i;

	if (path < 0 || path >= ISP_MAX_PATH)
		return -EINVAL;

	for (i = 0; i < FMT_MAX_PLANES; ++i) {
		fakebuf_arr[i] = isp_dev->fakebuf_phys;
	}

	hailo15_isp_configure_frame_base(isp_dev, fakebuf_arr, path);
	hailo15_isp_dma_set_enable(isp_dev, path, 0);
	isp_dev->queue_empty[path] = 1;
	return 0;
}

static struct hailo15_buf_ops hailo15_isp_buf_ops = {
	.buffer_process =
		hailo15_isp_buffer_process, /*should it return a value once we validate things?*/
	.get_frame_count = hailo15_isp_get_frame_count,
	.get_rmem = hailo15_isp_get_rmem,
	.get_event_resource = hailo15_isp_get_event_resource,
	.get_fbuf = hailo15_isp_get_fbuf,
	.set_private_data = hailo15_isp_set_private_data,
	.get_private_data = hailo15_isp_get_private_data,
	.get_vsm = hailo15_isp_get_vsm,
	.queue_empty = hailo15_isp_queue_empty,
};

/* Perform any platform device related initialization.      */
/* These include irq line request and address space mapping */
static int hailo15_isp_init_platdev(struct hailo15_isp_device *isp_dev)
{
	int ret;
	struct device *dev = isp_dev->dev;
	struct platform_device *pdev;
	struct resource *mem_res;
	struct resource *wrapper_mem_res;
	pdev = container_of(isp_dev->dev, struct platform_device, dev);

	mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	wrapper_mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);

	if (!mem_res) {
		dev_err(dev, "can't get memory resource for isp\n");
		return -ENOENT;
	}

	if (!wrapper_mem_res) {
		dev_err(dev, "can't get memory resource for isp wrapper\n");
		kfree(mem_res);
		return -ENOENT;
	}


	isp_dev->base = devm_ioremap_resource(&pdev->dev, mem_res);
	if (IS_ERR(isp_dev->base)) {
		dev_err(dev, "can't get reg mem resource for isp\n");
		return -ENOMEM;
	}

	isp_dev->wrapper_base =
		devm_ioremap_resource(&pdev->dev, wrapper_mem_res);
	if (IS_ERR(isp_dev->wrapper_base)) {
		dev_err(dev, "can't get reg mem resource for isp wrapper\n");
		return -ENOMEM;
	}

	isp_dev->p_clk = devm_clk_get(&pdev->dev, "p_clk");
	if (IS_ERR(isp_dev->p_clk)) {
		dev_err(&pdev->dev, "Couldn't get p clock\n");
		return PTR_ERR(isp_dev->p_clk);
	}

	isp_dev->ip_clk = devm_clk_get(&pdev->dev, "ip_clk");
	if (IS_ERR(isp_dev->ip_clk)) {
		dev_err(&pdev->dev, "Couldn't get ip clock\n");
		return PTR_ERR(isp_dev->ip_clk);
	}

	isp_dev->irq = platform_get_irq(pdev, HAILO15_ISP_IRQ_EVENT_ISP_MIS);
	if (isp_dev->irq < 0) {
		dev_err(dev, "can't get isp irq resource\n");
		return -ENXIO;
	}

	ret = devm_request_irq(dev, isp_dev->irq, hailo15_isp_irq_handler, 0,
				   dev_name(dev), isp_dev);
	if (ret) {
		dev_err(dev, "request isp irq error\n");
		return ret;
	}

	return 0;
}

static void hailo15_isp_destroy_platdev(struct hailo15_isp_device *isp_dev)
{
	devm_free_irq(isp_dev->dev, isp_dev->irq, isp_dev);
	hailo15_isp_disable_clocks(isp_dev);
	if(isp_dev->rmem_vaddr){
		dma_free_coherent(isp_dev->dev, isp_dev->rmem.size,
			  isp_dev->rmem_vaddr, isp_dev->rmem.addr);
		isp_dev->rmem_vaddr = 0;
	}
}

/* Perform v4l2 subdevice specific initializations.            */
/* This function must be called before the actual registration */
static void hailo15_isp_init_v4l2_subdev(struct v4l2_subdev *sd)
{
	struct hailo15_isp_device *isp_dev =
		container_of(sd, struct hailo15_isp_device, sd);
	v4l2_subdev_init(sd, &hailo15_isp_subdev_ops);
	sd->dev = isp_dev->dev;
	sd->fwnode = dev_fwnode(isp_dev->dev);
	sd->internal_ops = &hailo15_isp_internal_ops;
	snprintf(sd->name, HAILO15_ISP_NAME_SIZE, "%s", HAILO15_ISP_NAME);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->flags |= V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->owner = THIS_MODULE;

	sd->entity.obj_type = MEDIA_ENTITY_TYPE_V4L2_SUBDEV;
	sd->entity.function = MEDIA_ENT_F_IO_V4L;

	sd->entity.ops = &hailo15_isp_entity_ops;
}

/* Initializes media pads type and pad handles. */
static inline int
hailo15_isp_init_pads(struct hailo15_isp_device *isp_dev)
{
	int pad, ret;

    // Initialize pad flags for sink and source pads
	for (pad = 0; pad < HAILO15_ISP_SINK_PAD_MAX; ++pad) {
		isp_dev->pads[pad].flags = MEDIA_PAD_FL_SINK;
	}

	for (; pad < HAILO15_ISP_SOURCE_PAD_MAX; ++pad) {
		isp_dev->pads[pad].flags = MEDIA_PAD_FL_SOURCE;
	}

	ret = media_entity_pads_init(&isp_dev->sd.entity, HAILO15_ISP_PADS_NR,
					 isp_dev->pads);
	if (ret) {
		dev_err(isp_dev->dev, "media entity init error\n");
	}

	return ret;
}

static inline void
hailo15_isp_destroy_media_pads(struct hailo15_isp_device *isp_dev)
{
	hailo15_media_entity_clean(&isp_dev->sd.entity);
}

static int hailo15_isp_parse_null_addr(struct hailo15_isp_device* isp_dev){
	struct fwnode_handle *ep = NULL;
	int ret = -EINVAL;

	ep = dev_fwnode(isp_dev->dev);;

	if(!ep){
		return -EINVAL;
	}

	ret = fwnode_property_read_u32(ep, "null-addr", &isp_dev->null_addr);

	return ret;
}


/* Init the isp device.                               */
/* These include any previous initialization function */
static int hailo15_init_isp_device(struct hailo15_isp_device *isp_dev)
{
	int ret;
	int path;

	if (!isp_dev)
		return -EINVAL;

	ret = hailo15_isp_parse_null_addr(isp_dev);
	if(ret){
		dev_err(isp_dev->dev, "can't parse null address\n");
		return ret;
	}

	mutex_init(&isp_dev->mlock);
	mutex_init(&isp_dev->ctrl_lock);

	ret = hailo15_isp_init_platdev(isp_dev);
	if (ret) {
		dev_err(isp_dev->dev, "cannot parse platform device\n");
		goto err_init_platdev;
	}

	hailo15_isp_init_v4l2_subdev(&isp_dev->sd);

	ret = hailo15_isp_init_pads(isp_dev);
	if (ret) {
		dev_err(isp_dev->dev, "cannot initialize media pads\n");
		goto err_init_media_pads;
	}

	pm_runtime_enable(isp_dev->dev);
	isp_dev->fbuf_vaddr =
		dma_alloc_coherent(isp_dev->dev, HAILO15_ISP_FBUF_SIZE,
				   &isp_dev->fbuf_phys, GFP_USER);
	if (!isp_dev->fbuf_vaddr) {
		dev_err(isp_dev->dev, "cannot allocate framebuffer\n");
		goto err_alloc_fbuf;
	}

	for (path = ISP_MP; path < ISP_MAX_PATH; ++path)
		isp_dev->mi_stopped[path] = 1;

	isp_dev->fakebuf_vaddr =
		dma_alloc_coherent(isp_dev->dev, HAILO15_ISP_FAKEBUF_SIZE,
				   &isp_dev->fakebuf_phys, GFP_USER);
	if (!isp_dev->fakebuf_vaddr) {
		dev_err(isp_dev->dev, "cannot allocate fakebuf\n");
		goto err_alloc_fakebuf;
	}

	for (path = ISP_MP; path < ISP_MAX_PATH; ++path)
		isp_dev->mi_stopped[path] = 1;
	/*queue_empty field should not be set to 1 as the software assumes empty queue on initialization*/
	INIT_LIST_HEAD(&isp_dev->mcm_queue);
	spin_lock_init(&isp_dev->mcm_lock);
	INIT_LIST_HEAD(&isp_dev->miv2_mis_queue);
	spin_lock_init(&isp_dev->miv2_mis_lock);
	isp_dev->miv2_mis_wq = alloc_ordered_workqueue("miv2_mis_wq", WQ_HIGHPRI);
	INIT_WORK(&isp_dev->miv2_mis_w, hailo15_isp_handle_frame_rx);

	tasklet_init(&isp_dev->fe_tasklet, mcm_fe_irq_tasklet, (unsigned long)isp_dev);

	goto out;
	

err_alloc_fakebuf:
	dma_free_coherent(isp_dev->dev, HAILO15_ISP_FBUF_SIZE,
			  isp_dev->fbuf_vaddr, isp_dev->fbuf_phys);
err_alloc_fbuf:
	pm_runtime_disable(isp_dev->dev);
err_init_media_pads:
	hailo15_isp_destroy_platdev(isp_dev);
err_init_platdev:
	mutex_destroy(&isp_dev->mlock);
	mutex_destroy(&isp_dev->ctrl_lock);
out:
	return ret;
}

static void hailo15_clean_isp_device(struct hailo15_isp_device *isp_dev)
{
	dma_free_coherent(isp_dev->dev, HAILO15_ISP_FAKEBUF_SIZE,
			  isp_dev->fakebuf_vaddr, isp_dev->fakebuf_phys);
	dma_free_coherent(isp_dev->dev, HAILO15_ISP_FBUF_SIZE,
			  isp_dev->fbuf_vaddr, isp_dev->fbuf_phys);
	pm_runtime_disable(isp_dev->dev);
	hailo15_isp_destroy_media_pads(isp_dev);
	v4l2_device_unregister_subdev(&isp_dev->sd);
	hailo15_isp_destroy_platdev(isp_dev);
	mutex_destroy(&isp_dev->mlock);
	mutex_destroy(&isp_dev->ctrl_lock);
	mutex_destroy(&isp_dev->af_kevent->data_lock);
	destroy_workqueue(isp_dev->af_wq);
}

/* Initialize the dma context.                                                  */
/* The dma context holds the required information for proper buffer management. */
static int hailo15_init_dma_ctx(struct hailo15_dma_ctx *ctx,
				struct hailo15_isp_device *isp_dev)
{
	int path, index;
	int ret = 0;

	ctx->dev = (void *)isp_dev;
	for(path = 0; path < ISP_MAX_PATH; ++path){
		index = ISP_PATH_TO_VID_GRP(path);
		if(index < 0 || index >= VID_GRP_MAX){
			ret = -EINVAL;
			goto err_invalid_path;
		}

		ctx->buf_ctx[index].ops = kzalloc(sizeof(struct hailo15_buf_ops), GFP_KERNEL);
		if (!ctx->buf_ctx[index].ops){
			ret = -ENOMEM;
			goto err_alloc_ops;
		}

		memcpy(ctx->buf_ctx[index].ops, &hailo15_isp_buf_ops,
			   sizeof(struct hailo15_buf_ops));
	}
	v4l2_set_subdevdata(&isp_dev->sd, ctx);
err_alloc_ops:
err_invalid_path:
	path--;
	for(;path >= 0; --path){
		index = ISP_PATH_TO_VID_GRP(path);
		if(index < 0 || index >= VID_GRP_MAX)
			continue;

		kfree(isp_dev->buf_ctx[index]);
	}
	return ret;
}

static void hailo15_clean_dma_ctx(struct hailo15_dma_ctx *ctx)
{
	int index;
	int path;
	for(path = 0; path < ISP_MAX_PATH; ++path){
		index = ISP_PATH_TO_VID_GRP(path);
		if(index < 0 || index >= VID_GRP_MAX)
			continue;

		kfree(ctx->buf_ctx[index].ops);
	}

	return;
}

static int hailo15_isp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hailo15_isp_device *isp_dev;
	struct hailo15_dma_ctx *dma_ctx;
	int ret;

	ret = hailo15_media_get_endpoints_status(&pdev->dev);
	if(ret){
		return ret;
	}
	

	isp_dev = devm_kzalloc(&pdev->dev, sizeof(struct hailo15_isp_device),
				   GFP_KERNEL);
	if (!isp_dev)
		return -ENOMEM;

	isp_dev->dev = dev;
	platform_set_drvdata(pdev, isp_dev);

	ret = hailo15_init_isp_device(isp_dev);
	if (ret) {
		dev_err(dev, "can't init isp device\n");
		goto err_init_isp_dev;
	}

	dma_ctx = devm_kzalloc(&pdev->dev, sizeof(struct hailo15_dma_ctx),
				   GFP_KERNEL);
	if (!dma_ctx) {
		ret = -ENOMEM;
		goto err_alloc_dma_ctx;
	}

	ret = hailo15_init_dma_ctx(dma_ctx, isp_dev);
	if (ret) {
		dev_err(dev, "can't init dma context\n");
		goto err_init_dma_ctx;
	}

	ret = hailo15_vsi_isp_init_events(isp_dev);
	if (ret) {
		dev_err(dev, "can't init isp events memory\n");
		goto err_init_events;
	}

	ret = hailo15_isp_ctrl_init(isp_dev);
	if (ret) {
		dev_err(dev, "can't init isp ctrls\n");
		goto err_init_ctrl;
	}

	init_waitqueue_head(&af_kevent.wait_q);
	mutex_init(&af_kevent.data_lock);
	isp_dev->af_kevent = &af_kevent;

	isp_dev->af_wq = alloc_ordered_workqueue("af_wq", WQ_HIGHPRI);
	if (!isp_dev->af_wq) {
		dev_err(dev, "can't create af workqueue\n");
		goto err_create_wq;
	}

	INIT_WORK(&isp_dev->af_w, hailo15_isp_handle_afm_int);
	
	hailo15_fe_get_dev(&isp_dev->fe_dev);
	hailo15_fe_set_address_space_base(isp_dev->fe_dev, isp_dev->base);
	
	ret = hailo15_media_create_connections(isp_dev->dev, &isp_dev->sd);
	if(ret){
		dev_err(isp_dev->dev, "can't create media connections\n");
		/*@TODO seperate err label (destroy wq)*/
		goto err_create_wq;
	}

	dev_info(dev, "hailo15 isp driver probed\n");
	goto out;

err_create_wq:
	mutex_destroy(&af_kevent.data_lock);
	hailo15_isp_ctrl_destroy(isp_dev);
err_init_ctrl:
err_init_events:
	hailo15_clean_dma_ctx(dma_ctx);
err_init_dma_ctx:
	kfree(dma_ctx);
err_alloc_dma_ctx:
	hailo15_clean_isp_device(isp_dev);
err_init_isp_dev:
	kfree(isp_dev);
out:
	return ret;
}

static int hailo15_isp_remove(struct platform_device *pdev)
{
	struct hailo15_isp_device *isp_dev = platform_get_drvdata(pdev);
	struct hailo15_dma_ctx *ctx = v4l2_get_subdevdata(&isp_dev->sd);
	hailo15_clean_dma_ctx(
		ctx); /* need to stop interrupts before doing thath */
	hailo15_clean_isp_device(isp_dev);
	kfree(ctx);
	kfree(isp_dev);
	dev_info(isp_dev->dev, "hailo15 isp driver removed\n");
	return 0;
}

static int hailo15_isp_hal_pad_stat_done(struct hailo15_isp_device *isp_dev,
        struct hailo15_pad_stat *pad_stat)
{
    struct media_pad *pad;
    struct video_device *video;
    struct v4l2_event event;

    static_assert(sizeof(event.u.data) == sizeof(pad_stat->reserved),
        "event.u.data and pad_stat->reserved are not the same size");

    pad = media_entity_remote_pad(&isp_dev->pads[pad_stat->pad]);
    if (!pad)
        return -EINVAL;

    if (!is_media_entity_v4l2_video_device(pad->entity)) {
        pr_err("received unexpected pad entity type %d\n", pad->entity->obj_type);
        return -EINVAL;
    }

    video = media_entity_to_video_device(pad->entity);
    event.id = pad_stat->id;
    event.type = pad_stat->type;
    memcpy(event.u.data, pad_stat->reserved, sizeof(pad_stat->reserved));

    v4l2_event_queue(video, &event);
    return 0;
}

static bool hailo15_isp_pad_is_sink(struct hailo15_isp_device *isp_dev, int pad)
{
    bool by_flags = (isp_dev->pads[pad].flags & MEDIA_PAD_FL_SINK);
    bool by_index = (pad < HAILO15_ISP_SINK_PAD_END);

    if (by_flags != by_index)
        pr_err("pad %d bad sink pad check! by_flags %d, by_index %d\n",
            pad, by_flags, by_index);
    
    return by_index;
}


static int hailo15_process_pad_stat_sub(
    struct hailo15_isp_device *isp_dev, uint8_t pad, uint32_t mis_reg,
    enum hailo15_event_stat_id stat_id, uint32_t mis_mask,
    void *data, size_t data_size)
{
    struct hailo15_pad_stat_subscribe *stat_sub = &isp_dev->pad_data[pad].stat_sub[stat_id];
    struct hailo15_pad_stat pad_stat;

    switch (stat_sub->type) {
        case HAILO15_UEVENT_ISP_STAT:
            break; // stat is subscribed, continue
        case 0:
            return 0; // stat is not subscribed
        default:
            pr_err("received unexpected stat sub type %x for pad %d\n",
                stat_sub->type, pad);
            return -EINVAL;
    }

    if (stat_sub->id != stat_id) {
        pr_err("received unexpected stat sub id %d for pad %d, expected stat_id %d\n",
            stat_sub->id, pad, stat_id);
        return -EINVAL;
    }
    
    if ((mis_reg & mis_mask) == 0)
        return 0; // stat is not ready


    memset(&pad_stat, 0, sizeof(pad_stat));
    pad_stat.pad = pad;
    pad_stat.type = HAILO15_UEVENT_ISP_STAT;
    pad_stat.id = stat_id;

    if (data) {
        if (data_size == 0 || data_size > sizeof(pad_stat.reserved)) {
            pr_err("unexpected data size %ld for pad %d\n", data_size, pad);
            return -EINVAL;
        }

        memcpy(pad_stat.reserved, data, data_size);
    }

    return hailo15_isp_hal_pad_stat_done(isp_dev, &pad_stat);
}

static int hailo15_process_isp_pad_stat_sub(
    struct hailo15_isp_device *isp_dev, uint8_t pad, uint32_t mis_reg,
    enum hailo15_event_stat_id stat_id, uint32_t mis_mask)
{
    uint32_t *sequence = &isp_dev->pad_data[pad].sequence;
    
    return hailo15_process_pad_stat_sub(isp_dev, pad, mis_reg,
        stat_id, mis_mask, sequence, sizeof(*sequence));
}

static int hailo15_process_sensor_dataloss_stat_sub(
    struct hailo15_isp_device *isp_dev, uint8_t pad, uint32_t mis_reg)
{
    int ret = 0;
    uint32_t sensor_index = 0;
    
    for (sensor_index = 0; sensor_index < ISP_MIS_SENSORS_COUNT; sensor_index++) {
        uint32_t sensor_dataloss_mask = ISP_MIS_SENSOR_DATALOSS_FIRST_BIT << sensor_index;
        ret |= hailo15_process_pad_stat_sub(
            isp_dev, pad, mis_reg, HAILO15_UEVENT_SENSOR_DATALOSS_STAT,
            sensor_dataloss_mask, &sensor_index, sizeof(sensor_index));
    }

    return ret;
}

static int hailo15_isp_irq_stat_process(struct hailo15_isp_device *isp_dev,
                uint32_t id, uint32_t mis)
{
    uint8_t pad = 0;
    int ret = 0;

    if (id == HAILO15_ISP_IRQ_EVENT_ISP_MIS) {
        for (pad = 0; pad < HAILO15_ISP_PADS_NR; pad++) {
            // Skip sink pads and pads without streams
            if (hailo15_isp_pad_is_sink(isp_dev, pad) ||
                !isp_dev->pad_data[pad].stream)
                continue;

            ret |= hailo15_process_isp_pad_stat_sub(isp_dev, pad, mis,
                HAILO15_UEVENT_ISP_EXP_STAT, ISP_MIS_EXP_END_MASK);
            ret |= hailo15_process_isp_pad_stat_sub(isp_dev, pad, mis,
                HAILO15_UEVENT_ISP_HIST_STAT, ISP_MIS_HIST_MEASURE_RDY_MASK);
            ret |= hailo15_process_isp_pad_stat_sub(isp_dev, pad, mis,
                HAILO15_UEVENT_ISP_AWB_STAT, ISP_MIS_AWB_DONE_MASK);
            ret |= hailo15_process_isp_pad_stat_sub(isp_dev, pad, mis,
                HAILO15_UEVENT_ISP_AFM_STAT, ISP_MIS_AFM_FIN_MASK);
            ret |= hailo15_process_isp_pad_stat_sub(isp_dev, pad, mis,
                HAILO15_UEVENT_VSM_DONE_STAT, ISP_MIS_VSM_DONE);

            ret |= hailo15_process_sensor_dataloss_stat_sub(isp_dev, pad, mis);
        }
    } else if (id == HAILO15_ISP_IRQ_EVENT_MI_MIS) {
        for (pad = 0; pad < HAILO15_ISP_PADS_NR; pad++) {
            // Skip sink pads and pads without streams
            if (hailo15_isp_pad_is_sink(isp_dev, pad) ||
                !isp_dev->pad_data[pad].stream)
                continue;

            ret |= hailo15_process_isp_pad_stat_sub(isp_dev, pad, mis,
                HAILO15_UEVENT_ISP_EXPV2_STAT, ISP_MP_JDP_FRAME_END_MASK);
        }
    }

    return ret;
}

irqreturn_t hailo15_process_irq_stats_events(struct hailo15_isp_device *isp_dev,
    int event_id, uint32_t mis)
{
    int ret = 0;

    if (mis == 0) {
        return IRQ_NONE;
    }

    ret = hailo15_isp_irq_stat_process(isp_dev, event_id, mis);
    if (ret) {
        pr_err("hailo15_isp_irq_stat_process with id %d and mis %x failed with ret %d\n",
            event_id, mis, ret);
    }

    return IRQ_HANDLED;
}


/********************/
/* PM subsystem ops */
/********************/
static int isp_system_suspend(struct device *dev)
{
	return pm_runtime_force_suspend(dev);
}

static int isp_system_resume(struct device *dev)
{
	return pm_runtime_force_resume(dev);
	;
}

static int isp_runtime_suspend(struct device *dev)
{
	return 0;
}

static int isp_runtime_resume(struct device *dev)
{
	return 0;
}
static const struct dev_pm_ops hailo15_isp_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(isp_system_suspend, isp_system_resume)
		SET_RUNTIME_PM_OPS(isp_runtime_suspend, isp_runtime_resume,
				   NULL)
};

static const struct of_device_id hailo15_isp_of_match[] = {
	{
		.compatible = "hailo,isp",
	},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, hailo15_isp_of_match);

static struct platform_driver
	hailo15_isp_driver = { .probe = hailo15_isp_probe,
				   .remove = hailo15_isp_remove,
				   .driver = {
					   .name = HAILO15_ISP_NAME,
					   .owner = THIS_MODULE,
					   .of_match_table = hailo15_isp_of_match,
					   .pm = &hailo15_isp_pm_ops,
				   } };

module_platform_driver(hailo15_isp_driver);

MODULE_DESCRIPTION("Hailo 15 isp driver");
MODULE_AUTHOR("Hailo Imaging SW Team");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("Hailo15-ISP");
