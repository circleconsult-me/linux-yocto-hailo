#include <linux/module.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_graph.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>
#include "hailo15-events.h"
#include "hailo15-vid-cap.h"
#include "hailo15-isp.h"
#include "hailo15-video-events.h"

#define MAX_WAIT_ITERATIONS 200000
#define WAIT_MICRO_SEC_BOTTOM_RANGE 5
#define WAIT_MICRO_SEC_TOP_RANGE 10

static int hailo15_video_event_subscribed(struct video_device *vdev,
					  uint32_t type, uint32_t id)
{
	struct v4l2_fh *fh;
	unsigned long flags;
	struct v4l2_subscribed_event *sev;
	int subscribed = 0;

	spin_lock_irqsave(&vdev->fh_lock, flags);

	list_for_each_entry (fh, &vdev->fh_list, list) {
		list_for_each_entry (sev, &fh->subscribed, list) {
			if (sev->type == type && sev->id == id) {
				subscribed = 1;
				break;
			}
		}
		if (subscribed)
			break;
	}

	spin_unlock_irqrestore(&vdev->fh_lock, flags);
	return subscribed;
}

static int hailo15_video_event_wait_complete(
	struct hailo15_event_resource *event_resource,
	uint8_t complete_idx)
{
	struct hailo15_video_event_pkg *event_shm;
	int i = 0;

	event_shm = event_resource->virt_addr;
	for (i = 0; i < MAX_WAIT_ITERATIONS; i++) {
		usleep_range(WAIT_MICRO_SEC_BOTTOM_RANGE,
				 WAIT_MICRO_SEC_TOP_RANGE);
		if (event_shm->complete == complete_idx)
			return 0;
	}

	event_shm->complete++;
	pr_warn("%s - return EAGAIN\n", __func__);
	return -EAGAIN;
}

int hailo15_video_post_event(struct video_device *vdev,
				 hailo15_daemon_event_meta_t event_meta,
				 struct hailo15_event_resource *event_resource,
				 int pad, void *data, size_t data_size)
{
	struct v4l2_event event;
	struct hailo15_video_event_pkg_head *event_data;
	struct hailo15_video_event_pkg *event_shm;
	uint8_t cur_complete;
	int ret = 0;

	if (event_meta.event_type != HAILO15_DEAMON_VIDEO_EVENT) {
		pr_err("%s - event type is %d, not video\n", __func__, event_meta.event_type);
		return -EINVAL;
	}

	if (hailo15_video_event_subscribed(vdev, event_meta.event_type,
					   event_meta.event_id)) {
		event.type = event_meta.event_type;
		event.id = event_meta.event_id;
		event_data = (struct hailo15_video_event_pkg_head *)event.u.data;
		memset(event_data, 0, sizeof(*event_data));

		event_data->phy_addr = event_resource->phy_addr;
		event_data->size = event_resource->size;
		event_shm = event_resource->virt_addr;

		mutex_lock(&event_resource->event_lock);
		event_shm->result = 0;
		cur_complete = event_shm->complete;

		if (data) {
			if (data_size > 0 && data_size <= HAILO15_EVENT_RESOURCE_DATA_SIZE) {
				memset(event_shm, 0, event_resource->size);
				event_shm->complete = cur_complete;
				event_data->data_size = data_size;
				memcpy(event_shm->data, data, data_size);
			} else {
				pr_err("%s - got data with bad data size: %ld\n", __func__, data_size);
				mutex_unlock(&event_resource->event_lock);
				return -EINVAL;
			}
		}

		v4l2_event_queue(vdev, &event);
		ret = hailo15_video_event_wait_complete(event_resource,
							(uint8_t)(cur_complete + 1));
		if (ret) {
			pr_err("%s: post event id: %d timeout\n", __func__,
				   event_meta.event_id);
		} else {
			ret = event_shm->result;
		}

		mutex_unlock(&event_resource->event_lock);
	} else {
		pr_debug("%s: event id: %d not subscribed\n", __func__,
			event_meta.event_id);
	}

	return ret;
}


int hailo15_video_post_event_create_pipeline(struct hailo15_video_node *vid_node)
{
	hailo15_daemon_event_meta_t meta = {
		HAILO15_DEAMON_VIDEO_EVENT,
		HAILO15_DAEMON_VIDEO_EVENT_CREATE_PIPELINE
	};
	return hailo15_video_post_event(vid_node->video_dev, meta,
					&(vid_node->event_resource),
					vid_node->pad.index, NULL, 0);
}

int hailo15_video_post_event_release_pipeline(
	struct hailo15_video_node *vid_node)
{
	hailo15_daemon_event_meta_t meta = {
		HAILO15_DEAMON_VIDEO_EVENT,
		HAILO15_DAEMON_VIDEO_EVENT_DESTROY_PIPELINE
	};
	return hailo15_video_post_event(vid_node->video_dev, meta,
					&(vid_node->event_resource),
					vid_node->pad.index, NULL, 0);
}

MODULE_LICENSE("GPL v2");
