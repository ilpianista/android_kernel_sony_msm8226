/* Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>
#include <linux/iommu.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/syscalls.h>

#include "mdss_fb.h"
#include "mdss_mdp.h"
#include "splash.h"
#include "mdss_mdp_splash_logo.h"

#define INVALID_PIPE_INDEX 0xFFFF
#define MAX_FRAME_DONE_COUNT_WAIT 2

#define fb_width(fb)	((fb)->var.xres)
#define fb_linewidth(fb) \
	((fb)->fix.line_length / (fb_depth(fb) == 2 ? 2 : 4))
#define fb_height(fb)	((fb)->var.yres)
#define fb_depth(fb)	((fb)->var.bits_per_pixel >> 3)
#define fb_size(fb)	(fb_width(fb) * fb_height(fb) * fb_depth(fb))
#define INIT_IMAGE_FILE "/logo.rle"
static int splash_image_width;
static int splash_image_height;
static int splash_image_bpp;
static unsigned char *orig_logo_bits;

static void memset16(void *_ptr, unsigned short val, unsigned count)
{
	unsigned short *ptr = _ptr;
	count >>= 1;
	while (count--)
		*ptr++ = val;
}

static void memset24(void *_ptr, unsigned int val, unsigned count)
{
	unsigned char *ptr = _ptr;
	count >>= 2;
	while (count--) {
		*ptr++ = val & 0xff;
		*ptr++ = (val & 0xff00) >> 8;
		*ptr++ = (val & 0xff0000) >> 16;
	}
}

/* 565RLE image format: [count(2 bytes), rle(2 bytes)] */
static int load_565rle_to_bgr888(char *filename)
{
	struct fb_info *info;
	int fd, err = 0;
	unsigned count, max, width, stride, line_pos = 0;
	unsigned short *data, *ptr;
	unsigned char *bits;

	info = registered_fb[0];
	if (!info) {
		printk(KERN_WARNING "%s: Can not access framebuffer\n",
			__func__);
		return -ENODEV;
	}

	fd = sys_open(filename, O_RDONLY, 0);
	if (fd < 0) {
		printk(KERN_WARNING "%s: Can not open %s\n",
			__func__, filename);
		return -ENOENT;
	}
	count = sys_lseek(fd, (off_t)0, 2);
	if (count <= 0) {
		err = -EIO;
		goto err_logo_close_file;
	}
	sys_lseek(fd, (off_t)0, 0);
	data = kmalloc(count, GFP_KERNEL);
	if (!data) {
		printk(KERN_WARNING "%s: Can not alloc data\n", __func__);
		err = -ENOMEM;
		goto err_logo_close_file;
	}
	if (sys_read(fd, (char *)data, count) != count) {
		printk(KERN_WARNING "%s: Can not read data\n", __func__);
		err = -EIO;
		goto err_logo_free_data;
	}
	splash_image_width = width = fb_width(info);
	splash_image_height = fb_height(info);
	splash_image_bpp = 3;/*BGR888, not ABGR, not fb_depth(info)*/
	stride = fb_width(info);/*stride = fb_linewidth(info);*/
	max = width * fb_height(info);
	ptr = data;
	bits = orig_logo_bits = kmalloc(max * splash_image_bpp,
		GFP_KERNEL);/*BGR888*/
	if (!bits) {
		printk(KERN_WARNING "%s: Can not alloc bits\n", __func__);
		err = -ENOMEM;
		goto err_logo_free_data;
	}
	printk(KERN_INFO "%s:width %d, height %d, BPP %d\n", __func__,
		fb_width(info), fb_height(info), fb_depth(info));
	while (count > 3) {
		int n = ptr[0];

		if (n > max)
			break;
		max -= n;
		while (n > 0) {
			unsigned int j =
				(line_pos + n > width ? width-line_pos : n);

			if (fb_depth(info) == 2)
				memset16(bits, swab16(ptr[1]), j << 1);
			else {
				unsigned int widepixel = ptr[1];
				/*
				 * Format is RGBA, but fb is big
				 * endian so we should make widepixel
				 * as ABGR.
				 */
				widepixel =
					/* red :   f800 -> 000000f8 */
					(widepixel & 0xf800) >> 8 |
					/* green : 07e0 -> 0000fc00 */
					(widepixel & 0x07e0) << 5 |
					/* blue :  001f -> 00f80000 */
					(widepixel & 0x001f) << 19;
				memset24(bits, widepixel, j << 2);/*BGR*/
			}
			bits += j * splash_image_bpp;
			line_pos += j;
			n -= j;
			if (line_pos == width) {
				bits += (stride-width) * splash_image_bpp;
				line_pos = 0;
			}
		}
		ptr += 2;
		count -= 4;/*for RLE format*/
	}
err_logo_free_data:
	kfree(data);
err_logo_close_file:
	sys_close(fd);

	return err;
}

static int mdss_mdp_splash_alloc_memory(struct msm_fb_data_type *mfd,
							uint32_t size)
{
	int rc;
	struct msm_fb_splash_info *sinfo;
	unsigned long buf_size = size;
	struct mdss_data_type *mdata;

	if (!mfd || !size)
		return -EINVAL;

	mdata = mfd_to_mdata(mfd);
	sinfo = &mfd->splash_info;

	if (!mdata || !mdata->iclient || sinfo->splash_buffer)
		return -EINVAL;

	sinfo->ion_handle = ion_alloc(mdata->iclient, size, SZ_4K,
				ION_HEAP(ION_SYSTEM_HEAP_ID), 0);
	if (IS_ERR_OR_NULL(sinfo->ion_handle)) {
		pr_err("ion memory allocation failed\n");
		rc = PTR_RET(sinfo->ion_handle);
		goto end;
	}

	rc = ion_map_iommu(mdata->iclient, sinfo->ion_handle,
			mdss_get_iommu_domain(MDSS_IOMMU_DOMAIN_UNSECURE),
			0, SZ_4K, 0, (unsigned long *)&sinfo->iova,
				(unsigned long *)&buf_size, 0, 0);
	if (rc) {
		pr_err("ion memory map failed\n");
		goto imap_err;
	}

	sinfo->splash_buffer = ion_map_kernel(mdata->iclient,
						sinfo->ion_handle);
	if (IS_ERR_OR_NULL(sinfo->splash_buffer)) {
		pr_err("ion kernel memory mapping failed\n");
		rc = IS_ERR(sinfo->splash_buffer);
		goto kmap_err;
	}

	return rc;

kmap_err:
	ion_unmap_iommu(mdata->iclient, sinfo->ion_handle,
			mdss_get_iommu_domain(MDSS_IOMMU_DOMAIN_UNSECURE), 0);
imap_err:
	ion_free(mdata->iclient, sinfo->ion_handle);
end:
	return rc;
}

static void mdss_mdp_splash_free_memory(struct msm_fb_data_type *mfd)
{
	struct msm_fb_splash_info *sinfo;
	struct mdss_data_type *mdata;

	if (!mfd)
		return;

	sinfo = &mfd->splash_info;
	mdata = mfd_to_mdata(mfd);

	if (!mdata || !mdata->iclient || !sinfo->ion_handle)
		return;

	ion_unmap_kernel(mdata->iclient, sinfo->ion_handle);

	ion_unmap_iommu(mdata->iclient, sinfo->ion_handle,
			mdss_get_iommu_domain(MDSS_IOMMU_DOMAIN_UNSECURE), 0);

	ion_free(mdata->iclient, sinfo->ion_handle);
	sinfo->splash_buffer = NULL;
}

static int mdss_mdp_splash_iommu_attach(struct msm_fb_data_type *mfd)
{
	struct iommu_domain *domain;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int rc, ret;

	/*
	 * iommu dynamic attach for following conditions.
	 * 1. it is still not attached
	 * 2. MDP hardware version supports the feature
	 * 3. configuration is with valid splash buffer
	 */
	if (is_mdss_iommu_attached() ||
		!mfd->panel_info->cont_splash_enabled ||
		!mdss_mdp_iommu_dyn_attach_supported(mdp5_data->mdata) ||
		!mdp5_data->splash_mem_addr ||
		!mdp5_data->splash_mem_size) {
		pr_err("dynamic attach is not supported\n");
		return -EPERM;
	}

	domain = msm_get_iommu_domain(mdss_get_iommu_domain(
						MDSS_IOMMU_DOMAIN_UNSECURE));
	if (!domain) {
		pr_err("mdss iommu domain get failed\n");
		return -EINVAL;
	}

	rc = iommu_map(domain, mdp5_data->splash_mem_addr,
				mdp5_data->splash_mem_addr,
				mdp5_data->splash_mem_size, IOMMU_READ);
	if (rc) {
		pr_err("iommu memory mapping failed rc=%d\n", rc);
	} else {
		ret = mdss_iommu_ctrl(1);
		if (IS_ERR_VALUE(ret)) {
			pr_err("mdss iommu attach failed\n");
			iommu_unmap(domain, mdp5_data->splash_mem_addr,
						mdp5_data->splash_mem_size);
		} else {
			mfd->splash_info.iommu_dynamic_attached = true;
		}
	}

	return rc;
}

static void mdss_mdp_splash_unmap_splash_mem(struct msm_fb_data_type *mfd)
{
	struct iommu_domain *domain;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	if (mfd->splash_info.iommu_dynamic_attached) {
		domain = msm_get_iommu_domain(mdss_get_iommu_domain(
						MDSS_IOMMU_DOMAIN_UNSECURE));
		if (!domain) {
			pr_err("mdss iommu domain get failed\n");
			return;
		}

		iommu_unmap(domain, mdp5_data->splash_mem_addr,
						mdp5_data->splash_mem_size);
		mdss_iommu_ctrl(0);

		mfd->splash_info.iommu_dynamic_attached = false;
	}
}

void mdss_mdp_release_splash_pipe(struct msm_fb_data_type *mfd)
{
	struct msm_fb_splash_info *sinfo;

	if (!mfd || !mfd->splash_info.splash_pipe_allocated)
		return;

	sinfo = &mfd->splash_info;

	if (sinfo->pipe_ndx[0] != INVALID_PIPE_INDEX)
		mdss_mdp_overlay_release(mfd, sinfo->pipe_ndx[0]);
	if (sinfo->pipe_ndx[1] != INVALID_PIPE_INDEX)
		mdss_mdp_overlay_release(mfd, sinfo->pipe_ndx[1]);
	sinfo->splash_pipe_allocated = false;
}

int mdss_mdp_splash_cleanup(struct msm_fb_data_type *mfd,
					bool use_borderfill)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	int rc = 0;

	if (!mfd || !mdp5_data)
		return -EINVAL;

	if (mfd->splash_info.iommu_dynamic_attached ||
			!mfd->panel_info->cont_splash_enabled)
		goto end;

	if (use_borderfill && mdp5_data->handoff) {
		/*
		 * Set up border-fill on the handed off pipes.
		 * This is needed to ensure that there are no memory
		 * accesses prior to attaching iommu during continuous
		 * splash screen case. However, for command mode
		 * displays, this is not necessary since the panels can
		 * refresh from their internal memory if no data is sent
		 * out on the dsi lanes.
		 */
		if (mdp5_data->handoff && ctl && ctl->is_video_mode) {
			rc = mdss_mdp_display_commit(ctl, NULL);
			if (!IS_ERR_VALUE(rc)) {
				mdss_mdp_display_wait4comp(ctl);
			} else {
				/*
				 * Since border-fill setup failed, we
				 * need to ensure that we turn off the
				 * MDP timing generator before attaching
				 * iommu
				 */
				pr_err("failed to set BF at handoff\n");
				mdp5_data->handoff = false;
			}
		}
	}

	if (rc || mdp5_data->handoff) {
		/* Add all the handed off pipes to the cleanup list */
		mdss_mdp_handoff_cleanup_pipes(mfd, MDSS_MDP_PIPE_TYPE_RGB);
		mdss_mdp_handoff_cleanup_pipes(mfd, MDSS_MDP_PIPE_TYPE_VIG);
		mdss_mdp_handoff_cleanup_pipes(mfd, MDSS_MDP_PIPE_TYPE_DMA);
	}

	mdss_mdp_ctl_splash_finish(ctl, mdp5_data->handoff);

	if (mdp5_data->splash_mem_addr) {
		/* Give back the reserved memory to the system */
		memblock_free(mdp5_data->splash_mem_addr,
					mdp5_data->splash_mem_size);
		free_bootmem_late(mdp5_data->splash_mem_addr,
				 mdp5_data->splash_mem_size);
	}

	mdss_mdp_footswitch_ctrl_splash(0);
end:
	return rc;
}

static struct mdss_mdp_pipe *mdss_mdp_splash_get_pipe(
					struct msm_fb_data_type *mfd,
					struct mdp_overlay *req)
{
	struct mdss_mdp_pipe *pipe;
	int ret;
	struct mdss_mdp_data *buf;
	uint32_t image_size = splash_image_width * splash_image_height
						* splash_image_bpp;

	ret = mdss_mdp_overlay_pipe_setup(mfd, req, &pipe);
	if (ret)
		return NULL;

	if (mdss_mdp_pipe_map(pipe)) {
		pr_err("unable to map base pipe\n");
		return NULL;
	}

	buf = &pipe->back_buf;
	buf->p[0].addr = mfd->splash_info.iova;
	buf->p[0].len = image_size;
	buf->num_planes = 1;
	mdss_mdp_pipe_unmap(pipe);

	return pipe;
}

static int mdss_mdp_splash_kickoff(struct msm_fb_data_type *mfd,
				struct mdss_mdp_img_rect *src_rect,
				struct mdss_mdp_img_rect *dest_rect)
{
	struct mdss_mdp_pipe *pipe;
	struct fb_info *fbi;
	struct mdp_overlay req;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_data_type *mdata;
	struct mdss_mdp_mixer *mixer;
	int ret;
	bool use_single_pipe = false;
	struct msm_fb_splash_info *sinfo;

	if (!mfd)
		return -EINVAL;

	fbi = mfd->fbi;
	mdp5_data = mfd_to_mdp5_data(mfd);
	mdata = mfd_to_mdata(mfd);
	sinfo = &mfd->splash_info;

	if (!mdp5_data || !mdp5_data->ctl)
		return -EINVAL;

	if (mutex_lock_interruptible(&mdp5_data->ov_lock))
		return -EINVAL;

	ret = mdss_mdp_overlay_start(mfd);
	if (ret) {
		pr_err("unable to start overlay %d (%d)\n", mfd->index, ret);
		goto end;
	}

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		pr_err("unable to retrieve mixer\n");
		ret = -EINVAL;
		goto end;
	}

	memset(&req, 0, sizeof(struct mdp_overlay));
	/*
	 * use single pipe for
	 * 1. split display disabled
	 * 2. splash image is only on one side of panel
	 */
	use_single_pipe =
		!mfd->split_display ||
		(mfd->split_display &&
		((dest_rect->x + dest_rect->w) < mfd->split_fb_left ||
		dest_rect->x > mfd->split_fb_left));

	req.src.width = src_rect->w;
	if (use_single_pipe)
		req.src_rect.w = src_rect->w;
	else
		req.src_rect.w = min_t(u16, mixer->width, src_rect->w >> 1);
	req.dst_rect.w = req.src_rect.w;
	req.src.height = req.dst_rect.h = req.src_rect.h =
			src_rect->h;
	req.src.format = SPLASH_IMAGE_FORMAT;
	req.id = MSMFB_NEW_REQUEST;
	req.z_order = MDSS_MDP_STAGE_0;
	req.alpha = 0xff;
	req.transp_mask = MDP_TRANSP_NOP;
	req.dst_rect.x = dest_rect->x;
	req.dst_rect.y = dest_rect->y;

	pipe = mdss_mdp_splash_get_pipe(mfd, &req);
	if (!pipe) {
		pr_err("unable to allocate base pipe\n");
		ret = -EINVAL;
		goto end;
	}

	sinfo->pipe_ndx[0] = pipe->ndx;

	if (!use_single_pipe) {
		req.id = MSMFB_NEW_REQUEST;
		req.src_rect.x = src_rect->x + min_t(u16, mixer->width,
					src_rect->w - req.src_rect.w);
		req.dst_rect.x = mixer->width;
		pipe = mdss_mdp_splash_get_pipe(mfd, &req);
		if (!pipe) {
			pr_err("unable to allocate right base pipe\n");
			mdss_mdp_overlay_release(mfd, sinfo->pipe_ndx[0]);
			ret = -EINVAL;
			goto end;
		}
		sinfo->pipe_ndx[1] = pipe->ndx;
	}
	mutex_unlock(&mdp5_data->ov_lock);

	ret = mfd->mdp.kickoff_fnc(mfd, NULL);
	if (ret) {
		pr_err("error in displaying image\n");
		mdss_mdp_overlay_release(mfd, sinfo->pipe_ndx[0] |
					sinfo->pipe_ndx[1]);
	}

	return ret;
end:
	sinfo->pipe_ndx[0] = INVALID_PIPE_INDEX;
	sinfo->pipe_ndx[1] = INVALID_PIPE_INDEX;
	mutex_unlock(&mdp5_data->ov_lock);
	return ret;
}

static int mdss_mdp_display_splash_image(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct fb_info *fbi;
	uint32_t image_len;
	struct mdss_mdp_img_rect src_rect, dest_rect;
	struct msm_fb_splash_info *sinfo;

	load_565rle_to_bgr888(INIT_IMAGE_FILE);
	image_len = splash_image_width * splash_image_height
						* splash_image_bpp;
	if (!mfd || !mfd->fbi || !orig_logo_bits) {
		pr_err("invalid input parameter\n");
		rc = -EINVAL;
		goto end;
	}

	fbi = mfd->fbi;
	sinfo = &mfd->splash_info;

	if (splash_image_width > fbi->var.xres ||
		  splash_image_height > fbi->var.yres ||
		  splash_image_bpp > (fbi->var.bits_per_pixel >> 3)) {
		pr_err("invalid splash parameter configuration\n");
		rc = -EINVAL;
		goto end;
	}

	sinfo->pipe_ndx[0] = INVALID_PIPE_INDEX;
	sinfo->pipe_ndx[1] = INVALID_PIPE_INDEX;

	src_rect.x = 0;
	src_rect.y = 0;
	dest_rect.w = src_rect.w = splash_image_width;
	dest_rect.h = src_rect.h = splash_image_height;
	dest_rect.x = (fbi->var.xres >> 1) - (splash_image_width >> 1);
	dest_rect.y = (fbi->var.yres >> 1) - (splash_image_height >> 1);

	rc = mdss_mdp_splash_alloc_memory(mfd, image_len);
	if (rc) {
		pr_err("splash buffer allocation failed\n");
		goto end;
	}

	memcpy(sinfo->splash_buffer, orig_logo_bits, image_len);

	rc = mdss_mdp_splash_iommu_attach(mfd);
	if (rc)
		pr_err("iommu dynamic attach failed\n");

	rc = mdss_mdp_splash_kickoff(mfd, &src_rect, &dest_rect);
	if (rc)
		pr_err("splash image display failed\n");
	else
		sinfo->splash_pipe_allocated = true;
end:
	kfree(orig_logo_bits);
	return rc;
}

static int mdss_mdp_splash_ctl_cb(struct notifier_block *self,
					unsigned long event, void *data)
{
	struct msm_fb_splash_info *sinfo = container_of(self,
					struct msm_fb_splash_info, notifier);
	struct msm_fb_data_type *mfd;

	if (!sinfo)
		goto done;

	mfd = container_of(sinfo, struct msm_fb_data_type, splash_info);

	if (!mfd)
		goto done;

	if (event != MDP_NOTIFY_FRAME_DONE)
		goto done;

	if (!sinfo->frame_done_count) {
		mdss_mdp_splash_unmap_splash_mem(mfd);
	/* wait for 2 frame done events before releasing memory */
	} else if (sinfo->frame_done_count > MAX_FRAME_DONE_COUNT_WAIT &&
			sinfo->splash_thread) {
		complete(&sinfo->frame_done);
		sinfo->splash_thread = NULL;
	}

	/* increase frame done count after pipes are staged from other client */
	if (!sinfo->splash_pipe_allocated)
		sinfo->frame_done_count++;
done:
	return NOTIFY_OK;
}

static int mdss_mdp_splash_thread(void *data)
{
	struct msm_fb_data_type *mfd = data;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret = -EINVAL;

	if (!mfd) {
		pr_err("invalid input parameter\n");
		goto end;
	}

	lock_fb_info(mfd->fbi);
	ret = fb_blank(mfd->fbi, FB_BLANK_UNBLANK);

        pr_info("LCM reboot for truly panel");
        usleep(1000);
	    ret = fb_blank(mfd->fbi, FB_BLANK_POWERDOWN);
        usleep(1000);
	    ret = fb_blank(mfd->fbi, FB_BLANK_UNBLANK);

	if (ret) {
		pr_err("can't turn on fb!\n");
		goto end;
	}
	unlock_fb_info(mfd->fbi);

	mutex_lock(&mfd->bl_lock);
	mfd->bl_updated = true;
	mdss_fb_set_backlight(mfd, mfd->panel_info->bl_max >> 1);
	mutex_unlock(&mfd->bl_lock);

	init_completion(&mfd->splash_info.frame_done);

	mfd->splash_info.notifier.notifier_call = mdss_mdp_splash_ctl_cb;
	mdss_mdp_ctl_notifier_register(mdp5_data->ctl,
				&mfd->splash_info.notifier);

	ret = mdss_mdp_display_splash_image(mfd);
	if (ret) {
		/*
		 * keep thread alive to release dynamically allocated
		 * resources
		 */
		pr_err("splash image display failed\n");
	}

	/* wait for second display complete to release splash resources */
	ret = wait_for_completion_killable(&mfd->splash_info.frame_done);

	mdss_mdp_splash_free_memory(mfd);

	mdss_mdp_ctl_notifier_unregister(mdp5_data->ctl,
				&mfd->splash_info.notifier);
end:
	return ret;
}

static __ref int mdss_mdp_splash_parse_dt(struct msm_fb_data_type *mfd)
{
	struct platform_device *pdev = mfd->pdev;
	struct mdss_overlay_private *mdp5_mdata = mfd_to_mdp5_data(mfd);
	int len = 0, rc = 0;
	u32 offsets[2];

	mfd->splash_info.splash_logo_enabled =
				of_property_read_bool(pdev->dev.of_node,
				"qcom,mdss-fb-splash-logo-enabled");

	of_find_property(pdev->dev.of_node, "qcom,memblock-reserve", &len);
	if (len < 1) {
		pr_err("mem reservation for splash screen fb not present\n");
		rc = -EINVAL;
		goto error;
	}

	len = len / sizeof(u32);

	rc = of_property_read_u32_array(pdev->dev.of_node,
			"qcom,memblock-reserve", offsets, len);
	if (rc) {
		pr_err("error reading mem reserve settings for fb\n");
		goto error;
	}

	if (!memblock_is_reserved(offsets[0])) {
		pr_err("failed to reserve memory for fb splash\n");
		rc = -EINVAL;
		goto error;
	}

	mdp5_mdata->splash_mem_addr = offsets[0];
	mdp5_mdata->splash_mem_size = offsets[1];
	pr_info("memaddr=%x size=%x\n", mdp5_mdata->splash_mem_addr,
		mdp5_mdata->splash_mem_size);

error:
	if (!rc && !mfd->panel_info->cont_splash_enabled &&
		mdp5_mdata->splash_mem_addr) {
		pr_err("mem reservation not reqd if cont splash disabled\n");
		memblock_free(mdp5_mdata->splash_mem_addr,
					mdp5_mdata->splash_mem_size);
		free_bootmem_late(mdp5_mdata->splash_mem_addr,
				 mdp5_mdata->splash_mem_size);
	} else if (rc && mfd->panel_info->cont_splash_enabled) {
		pr_err("no rsvd mem found in DT for splash screen\n");
	} else {
		rc = 0;
	}

	return rc;
}

int mdss_mdp_splash_init(struct msm_fb_data_type *mfd)
{
	int rc;

	if (!mfd) {
		rc = -EINVAL;
		goto end;
	}

	rc = mdss_mdp_splash_parse_dt(mfd);
	if (rc) {
		pr_err("splash memory reserve failed\n");
		goto end;
	}

    pr_info("mdss_mdp_splash_init() splash_logo_enabled=%d",mfd->splash_info.splash_logo_enabled);

	if (!mfd->splash_info.splash_logo_enabled) {
		rc = -EINVAL;
		goto end;
	}

	mfd->splash_info.splash_thread = kthread_run(mdss_mdp_splash_thread,
							mfd, "mdss_fb_splash");

	if (IS_ERR(mfd->splash_info.splash_thread)) {
		pr_err("unable to start splash thread %d\n", mfd->index);
		mfd->splash_info.splash_thread = NULL;
	}

end:
	return rc;
}
