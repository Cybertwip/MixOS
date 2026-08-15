// SPDX-License-Identifier: GPL-2.0-only
/*
 * J36 Ultra -- a dma-buf over the framebuffer the bootloader handed us.
 *
 * WHAT PROBLEM THIS SOLVES. The GPU works. eglprobe brings up an ES 2.0 context
 * on lima's render node and draws a cube at a sensible rate, and every frame of
 * it is then thrown away and re-created on the CPU: glReadPixels() drags
 * 640x480x4 bytes out of a tiled render target into a malloc'd buffer, and a
 * write() pushes the same 1.2 MB into /dev/fb0. That is 2.4 MB of memory
 * traffic per frame that the hardware never needed, on a board where the same
 * two copies are also what makes the media player's decode look slow -- ffmpeg
 * hands 1.2 MB per frame down a 64 KiB pipe, Qt wraps it in a QImage, paints
 * it, and linuxfb blits it. The copies are the cost, not the decode.
 *
 * The fix in both cases is the same and it is one sentence: let the thing that
 * produces pixels write them where the display controller is already looking.
 * This driver is what makes that address nameable from userspace.
 *
 * WHERE THE DISPLAY IS ALREADY LOOKING. The MVII LK programs the whole display
 * path before it jumps to the kernel -- OVL0 -> RDMA0 -> COLOR0 -> DSI0 -- and
 * points OVL0 at a carveout at 0x82700000: 640x480, 32 bpp, 2560-byte pitch,
 * the numbers in mvii-board/mt6592_board_j36.h. Linux never re-programs it.
 * mediatek-drm binds and stops there, because CONFIG_DRM_FBDEV_EMULATION=n
 * means nothing asks it for a modeset, and the device tree's simple-framebuffer
 * over that same carveout is what /dev/fb0 is. The proof that the DDP is still
 * scanning it is that writes to /dev/fb0 appear on the glass.
 *
 * So the carveout is live scanout, continuously, with no modeset involved.
 * A GPU that renders into it is on screen, immediately, and -- this is the part
 * that matters on this board -- without taking the screen away from anything
 * else. eglprobe -p and -c ask for consent with -y for exactly that reason: they
 * SETCRTC, so for as long as they run the panel is theirs and the dashboard is
 * not on it. (They used to cost far more than that: a client that modeset and
 * exited left the panel dark until reboot, until preserve_lk_state in
 * 0002-drm-mediatek-mt6592.patch made the CRTC disable restore the LK's overlay
 * instead of tearing the pipe down.) Rendering into the LK's buffer touches no
 * CRTC at all.
 *
 * WHY A DRIVER RATHER THAN /dev/mem. Because the consumer is lima, and lima
 * takes buffers as dma-bufs, not as addresses. Handing Mesa a physical address
 * is not a thing the API allows; handing it a dma-buf fd is
 * EGL_EXT_image_dma_buf_import, which is one of the extensions the payload
 * already ships. This driver is the exporter that makes such an fd exist.
 *
 * THE ARM32 DETAIL THAT DICTATES THE WHOLE IMPLEMENTATION. The carveout is a
 * reserved-memory region with `no-map', and no-map on ARM32 means the pages are
 * not in the linear map, which means pfn_valid() is false over the entire
 * region -- it resolves to memblock_is_map_memory(), and this memory was never
 * mapped. THERE ARE NO struct pages HERE. Not "there are pages we should not
 * touch": there are none, and asking for one gets a NULL or a wild pointer.
 *
 * A normal exporter builds its sg_table with sg_set_page() over real pages and
 * lets dma_map_sgtable() fill in the DMA addresses. This one cannot. What saves
 * it is that the importer does not need pages either:
 *
 *	lima_drv.c:  .gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table
 *	             ...which only stores the sgt.
 *	lima_vm.c:   for_each_sgtable_dma_page(sgt, &iter, 0)
 *	                 lima_vm_map_page(vm, sg_page_iter_dma_address(&iter), va);
 *
 * That walk reads sg_dma_len() and sg_dma_address() and nothing else. So the
 * sg_table below is one entry, with a NULL page, a DMA address written in by
 * hand, and sgt->nents = 1. It is deliberately not passed through
 * dma_map_sgtable(), because there is nothing to map: the region is already
 * device-visible physical memory, and the MT6592 has no IOMMU and no dma-ranges
 * anywhere in its device tree, so the bus address of a physical address is that
 * physical address.
 *
 * AND no-map HAS TO STAY. It is tempting to drop it and get struct pages back,
 * and it would break two things at once. ARM32's ioremap() refuses to map
 * memory that is in the linear map -- deliberately, so that two mappings of one
 * page cannot disagree about cacheability -- so simplefb's ioremap_wc() of the
 * carveout would start failing and /dev/fb0 would disappear. And the page
 * allocator would own pixels the display controller is reading.
 *
 * WHAT IT DOES NOT DO. It does not ioremap the region: it never needs a kernel
 * mapping of it, and taking one would be a second cacheability opinion beside
 * simplefb's write-combining one. It does not touch a single display register,
 * so it cannot disturb the LK's pipe or race mediatek-drm. It does not flip,
 * because there is one buffer -- a client rendering into it tears exactly as
 * much as a client writing /dev/fb0 tears today, which is to say the same, and
 * the carveout would have to grow to change that.
 *
 * THE INTERFACE. /dev/j36fb, two ioctls:
 *
 *	J36FB_IOC_INFO	 fills in geometry and the physical address, so a client
 *			 can size its render target without parsing a device
 *			 tree or trusting a compiled-in constant.
 *	J36FB_IOC_EXPORT returns a dma-buf fd for the whole region.
 *
 * plus mmap(), write-combining, for a client that wants the pixels from the CPU
 * without going through fbdev.
 */

#include <linux/dma-buf.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>

/*
 * ── The ABI ───────────────────────────────────────────────────────────────────
 *
 * THIS BLOCK IS THE AUTHORITY. tools/j36-eglprobe.c declares the same two
 * numbers and the same two structures by hand, the way it declares EGL, GBM and
 * the DRM uapi by hand, so that the probe needs no headers beyond libc. If
 * anything here changes, that file changes with it -- grep for J36FB_IOC.
 *
 * Everything is fixed-width and naturally aligned, so the layout is the same
 * for a 32-bit caller as it is here, and there is no compat handler to write.
 */
#define J36FB_IOC_MAGIC		'j'

struct j36fb_info {
	__u64 phys;		/* where the display controller is reading */
	__u64 size;		/* bytes in the carveout */
	__u32 width;
	__u32 height;
	__u32 stride;		/* bytes per line, not pixels */
	__u32 bpp;		/* bits, so 32 for the only format this board uses */
	__u32 fourcc;		/* DRM fourcc, for EGL_LINUX_DRM_FOURCC_EXT */
	__u32 reserved;		/* zeroed by the kernel; must be zero from userspace */
};

struct j36fb_export {
	__u32 flags;		/* in: O_CLOEXEC and/or O_RDONLY|O_WRONLY|O_RDWR */
	__s32 fd;		/* out: the dma-buf */
};

#define J36FB_IOC_INFO		_IOR(J36FB_IOC_MAGIC, 1, struct j36fb_info)
#define J36FB_IOC_EXPORT	_IOWR(J36FB_IOC_MAGIC, 2, struct j36fb_export)

/*
 * DRM fourccs, spelled out rather than pulled in from <uapi/drm/drm_fourcc.h>.
 * Three constants against a uapi header this module has no other use for, and
 * a fourcc is four characters in a u32 with the first character in the low
 * byte -- there is nothing here that can be got subtly wrong.
 */
#define J36FB_FOURCC(a, b, c, d) \
	((__u32)(a) | ((__u32)(b) << 8) | ((__u32)(c) << 16) | ((__u32)(d) << 24))

#define J36FB_FOURCC_XRGB8888	J36FB_FOURCC('X', 'R', '2', '4')
#define J36FB_FOURCC_ARGB8888	J36FB_FOURCC('A', 'R', '2', '4')
#define J36FB_FOURCC_RGB565	J36FB_FOURCC('R', 'G', '1', '6')

struct j36fb {
	struct device		*dev;
	struct miscdevice	misc;

	phys_addr_t		phys;
	size_t			size;

	u32			width;
	u32			height;
	u32			stride;
	u32			bpp;
	u32			fourcc;
};

/*
 * The format strings are simple-framebuffer's, because the device tree already
 * describes this same carveout to simplefb with them and two spellings of one
 * pixel layout in one device tree would be a trap. The list is short on
 * purpose: it is what the LK can hand over, not what the DDP can be programmed
 * for.
 */
static const struct {
	const char *name;
	u32	    fourcc;
	u32	    bpp;
} j36fb_formats[] = {
	{ "x8r8g8b8", J36FB_FOURCC_XRGB8888, 32 },
	{ "a8r8g8b8", J36FB_FOURCC_ARGB8888, 32 },
	{ "r5g6b5",   J36FB_FOURCC_RGB565,   16 },
};

/* ── The dma-buf ─────────────────────────────────────────────────────────── */

/*
 * ONE ENTRY, NO PAGE, A DMA ADDRESS WRITTEN IN BY HAND.
 *
 * The reasoning is in the file header and it is worth having in front of the
 * code as well, because this function looks wrong to anybody who has written an
 * exporter before. sg_set_page(sg, NULL, ...) is not a mistake and it is not a
 * placeholder: on this region pfn_valid() is false, so there is no struct page
 * to put there, and phys_to_page() would hand back a pointer into a memmap hole.
 * sg_assign_page() accepts NULL -- it only rejects the two low tag bits -- and
 * every consumer this buffer is built for reads the DMA side of the entry.
 *
 * There is no dma_map_sgtable() here for the same reason there is no page: the
 * memory is already where the device will read it. MT6592 has no IOMMU in front
 * of the GPU and its device tree carries no dma-ranges, so the bus address of a
 * physical address on this SoC is that physical address. Writing it in is
 * honest; calling dma_map_sgtable() on a NULL page would not survive
 * CONFIG_DMA_API_DEBUG and would compute the same number when it did work.
 *
 * A consequence to know about: this sg_table cannot be handed to an importer
 * that wants CPU access through it, because sg_page() is NULL and kmap of that
 * is a crash. Importers that want the pixels from the CPU must use mmap()
 * below, which goes through remap_pfn_range() and needs no pages either.
 */
static struct sg_table *j36fb_map_dma_buf(struct dma_buf_attachment *attach,
					  enum dma_data_direction dir)
{
	struct j36fb *fb = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	sg_set_page(sgt->sgl, NULL, fb->size, 0);
	sg_dma_address(sgt->sgl) = (dma_addr_t)fb->phys;
	sg_dma_len(sgt->sgl) = fb->size;

	/*
	 * nents is the count of DMA-mapped entries and orig_nents the count of
	 * allocated ones. sg_alloc_table() set orig_nents; nothing set nents,
	 * because nothing called dma_map_sgtable(). for_each_sgtable_dma_page()
	 * walks nents, so it has to be set here or the walk ends before it
	 * starts and lima maps a zero-page buffer.
	 */
	sgt->nents = 1;

	return sgt;
}

static void j36fb_unmap_dma_buf(struct dma_buf_attachment *attach,
				struct sg_table *sgt, enum dma_data_direction dir)
{
	sg_free_table(sgt);
	kfree(sgt);
}

/*
 * WRITE-COMBINING, TO MATCH simplefb. Two mappings of one physical page with
 * different cacheability is the thing ARM32 spends real effort making
 * impossible, and the reason the no-map above exists at all. simplefb maps this
 * carveout with ioremap_wc(); anything else that maps it has to agree, so this
 * is pgprot_writecombine() and not pgprot_noncached() and not the default.
 *
 * remap_pfn_range() rather than vm_insert_page() because -- again -- there is
 * no page. VM_PFNMAP is what tells the rest of mm not to go looking for one.
 */
static int j36fb_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct j36fb *fb = dmabuf->priv;
	unsigned long len = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff)
		return -EINVAL;
	if (len > fb->size)
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	return remap_pfn_range(vma, vma->vm_start,
			       (unsigned long)(fb->phys >> PAGE_SHIFT),
			       len, vma->vm_page_prot);
}

/*
 * Nothing to free. The carveout outlives every reference to it -- it outlives
 * the kernel, it is the bootloader's -- so release() exists only because
 * dma_buf_export() refuses an ops table without one.
 */
static void j36fb_dmabuf_release(struct dma_buf *dmabuf)
{
}

static const struct dma_buf_ops j36fb_dmabuf_ops = {
	.map_dma_buf	= j36fb_map_dma_buf,
	.unmap_dma_buf	= j36fb_unmap_dma_buf,
	.mmap		= j36fb_mmap,
	.release	= j36fb_dmabuf_release,
};

/*
 * A FRESH dma_buf PER EXPORT, rather than one cached in the driver and handed
 * out repeatedly. The cached form reads better and gets the module refcount
 * wrong: DEFINE_DMA_BUF_EXPORT_INFO() sets .owner = THIS_MODULE and
 * dma_buf_export() takes a reference on it, so a dma_buf the driver holds for
 * its own lifetime pins the module forever and it can never be unloaded. Made
 * per-call, the reference is held exactly while some fd is open, which is the
 * correct answer, and it costs an allocation on a path that runs once per
 * client.
 *
 * The two dma_bufs from two exports alias -- they are the same physical memory
 * and neither knows about the other. That is intended. This is not an
 * allocator; it is a name for the one buffer the display is scanning.
 */
static int j36fb_do_export(struct j36fb *fb, u32 flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp);
	struct dma_buf *dmabuf;
	int fd;

	exp.ops	  = &j36fb_dmabuf_ops;
	exp.size  = fb->size;
	exp.flags = O_RDWR;
	exp.priv  = fb;

	dmabuf = dma_buf_export(&exp);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, flags);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}

/* ── The character device ────────────────────────────────────────────────── */

/*
 * misc_open() leaves the struct miscdevice in file->private_data before it
 * calls any open() the driver supplies, so a driver that supplies none can
 * recover itself from there. That is why there is no .open below.
 */
static struct j36fb *j36fb_from_file(struct file *file)
{
	struct miscdevice *misc = file->private_data;

	return container_of(misc, struct j36fb, misc);
}

static long j36fb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct j36fb *fb = j36fb_from_file(file);
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case J36FB_IOC_INFO: {
		struct j36fb_info info = { };

		info.phys   = fb->phys;
		info.size   = fb->size;
		info.width  = fb->width;
		info.height = fb->height;
		info.stride = fb->stride;
		info.bpp    = fb->bpp;
		info.fourcc = fb->fourcc;

		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case J36FB_IOC_EXPORT: {
		struct j36fb_export req;
		int fd;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;

		/*
		 * Only the flags get_unused_fd_flags() understands, and it is
		 * worth rejecting the rest rather than ignoring them: a client
		 * that passes something meaning "and also map it read-only"
		 * should be told the kernel did not do that.
		 */
		if (req.flags & ~(u32)(O_CLOEXEC | O_ACCMODE))
			return -EINVAL;

		fd = j36fb_do_export(fb, req.flags);
		if (fd < 0)
			return fd;

		req.fd = fd;
		if (copy_to_user(uarg, &req, sizeof(req))) {
			put_unused_fd(fd);
			return -EFAULT;
		}
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

/*
 * The same mapping the dma-buf offers, straight off the character device, for a
 * client that wants the pixels and not a GPU. mixdash's software paths and any
 * shell one-liner can use this instead of /dev/fb0 without the fbdev ioctls.
 */
static int j36fb_chr_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct j36fb *fb = j36fb_from_file(file);
	unsigned long len = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff)
		return -EINVAL;
	if (len > fb->size)
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	return remap_pfn_range(vma, vma->vm_start,
			       (unsigned long)(fb->phys >> PAGE_SHIFT),
			       len, vma->vm_page_prot);
}

static const struct file_operations j36fb_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= j36fb_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.mmap		= j36fb_chr_mmap,
	.llseek		= noop_llseek,
};

/*
 * One line, for the log dump and for anybody holding a serial console who wants
 * to know whether the address in front of them is the one the LK named. Nothing
 * parses it; it exists so that "is /dev/j36fb pointing at the right memory" is
 * a cat and not a debugging session.
 */
static ssize_t info_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	/*
	 * misc_register() hands the struct miscdevice itself to
	 * device_create_with_groups() as the drvdata, so this is what is there --
	 * and it is there before the attribute exists, which a value the driver
	 * set after misc_register() returned would not be.
	 */
	struct miscdevice *misc = dev_get_drvdata(dev);
	struct j36fb *fb = container_of(misc, struct j36fb, misc);

	return sysfs_emit(buf,
			  "phys=0x%08llx size=%zu %ux%u stride=%u bpp=%u fourcc=%c%c%c%c\n",
			  (unsigned long long)fb->phys, fb->size,
			  fb->width, fb->height, fb->stride, fb->bpp,
			  (char)(fb->fourcc & 0xff), (char)((fb->fourcc >> 8) & 0xff),
			  (char)((fb->fourcc >> 16) & 0xff),
			  (char)((fb->fourcc >> 24) & 0xff));
}
static DEVICE_ATTR_RO(info);

static struct attribute *j36fb_attrs[] = {
	&dev_attr_info.attr,
	NULL,
};
ATTRIBUTE_GROUPS(j36fb);

/* ── Probe ───────────────────────────────────────────────────────────────── */

/*
 * The region is found through the memory-region phandle rather than a reg of
 * our own, so that there is one place in the device tree that says where the
 * LK's framebuffer is and everything else points at it. of_reserved_mem_lookup()
 * is the direct answer and it also proves the region was actually reserved --
 * a phandle to a reserved-memory node the kernel skipped would return NULL
 * here, which is worth knowing before handing the address to a GPU.
 *
 * of_address_to_resource() is the fallback because it reads the same reg
 * property without needing the region to be registered, which keeps this
 * working if the node is ever moved out of /reserved-memory.
 */
static int j36fb_resolve_region(struct j36fb *fb, struct device_node *np)
{
	struct device_node *mem;
	struct reserved_mem *rmem;
	struct resource r;
	int ret;

	mem = of_parse_phandle(np, "memory-region", 0);
	if (!mem) {
		dev_err(fb->dev, "no memory-region phandle: nothing to export\n");
		return -EINVAL;
	}

	rmem = of_reserved_mem_lookup(mem);
	if (rmem) {
		fb->phys = rmem->base;
		fb->size = rmem->size;
		of_node_put(mem);
		return 0;
	}

	ret = of_address_to_resource(mem, 0, &r);
	of_node_put(mem);
	if (ret) {
		dev_err(fb->dev, "memory-region has neither a reservation nor a translatable reg\n");
		return ret;
	}

	dev_warn(fb->dev,
		 "memory-region was not reserved by the kernel: using its reg, but something else may own these pages\n");
	fb->phys = r.start;
	fb->size = resource_size(&r);

	return 0;
}

static int j36fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const char *format = NULL;
	struct j36fb *fb;
	size_t need;
	unsigned int i;
	int ret;

	fb = devm_kzalloc(dev, sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return -ENOMEM;
	fb->dev = dev;

	ret = j36fb_resolve_region(fb, np);
	if (ret)
		return ret;

	if (of_property_read_u32(np, "width", &fb->width) ||
	    of_property_read_u32(np, "height", &fb->height)) {
		dev_err(dev, "width and height are required\n");
		return -EINVAL;
	}

	fb->fourcc = J36FB_FOURCC_XRGB8888;
	fb->bpp = 32;
	if (!of_property_read_string(np, "format", &format)) {
		for (i = 0; i < ARRAY_SIZE(j36fb_formats); i++) {
			if (strcmp(format, j36fb_formats[i].name))
				continue;
			fb->fourcc = j36fb_formats[i].fourcc;
			fb->bpp = j36fb_formats[i].bpp;
			break;
		}
		if (i == ARRAY_SIZE(j36fb_formats)) {
			dev_err(dev, "format \"%s\" is not one this board can hand over\n",
				format);
			return -EINVAL;
		}
	}

	if (of_property_read_u32(np, "stride", &fb->stride))
		fb->stride = fb->width * (fb->bpp / 8);

	/*
	 * The three ways this node can be wrong are all ways that end with a
	 * GPU writing outside the carveout, so they are checked rather than
	 * trusted. A device tree is not a more reliable source than a driver
	 * just because it is data.
	 */
	if (fb->stride < fb->width * (fb->bpp / 8)) {
		dev_err(dev, "stride %u is narrower than %u pixels at %u bpp\n",
			fb->stride, fb->width, fb->bpp);
		return -EINVAL;
	}

	need = (size_t)fb->stride * fb->height;
	if (!fb->size || fb->size < need) {
		dev_err(dev, "the region is %zu bytes and %ux%u at stride %u needs %zu\n",
			fb->size, fb->width, fb->height, fb->stride, need);
		return -EINVAL;
	}

	if (fb->phys & ~PAGE_MASK) {
		dev_err(dev, "the region starts at 0x%08llx, which is not page aligned\n",
			(unsigned long long)fb->phys);
		return -EINVAL;
	}

	fb->misc.minor  = MISC_DYNAMIC_MINOR;
	fb->misc.name   = "j36fb";
	fb->misc.fops   = &j36fb_fops;
	fb->misc.groups = j36fb_groups;
	fb->misc.parent = dev;

	ret = misc_register(&fb->misc);
	if (ret) {
		dev_err(dev, "misc_register: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, fb);

	/*
	 * Said at info level and not debug, because this address is the one
	 * fact that has to agree between the LK, the device tree's
	 * simple-framebuffer and whatever ends up rendering -- and a boot log
	 * showing all three saying 0x82700000 is how that gets confirmed
	 * without a debugger.
	 */
	dev_info(dev,
		 "/dev/j36fb over the LK framebuffer at 0x%08llx, %zu bytes, %ux%u stride %u, %u bpp\n",
		 (unsigned long long)fb->phys, fb->size,
		 fb->width, fb->height, fb->stride, fb->bpp);

	return 0;
}

static void j36fb_remove(struct platform_device *pdev)
{
	struct j36fb *fb = platform_get_drvdata(pdev);

	misc_deregister(&fb->misc);
}

static const struct of_device_id j36fb_of_match[] = {
	{ .compatible = "j36,lk-framebuffer" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, j36fb_of_match);

static struct platform_driver j36fb_driver = {
	.probe = j36fb_probe,
	.remove = j36fb_remove,
	.driver = {
		.name = "j36-fbmem",
		.of_match_table = j36fb_of_match,
	},
};
module_platform_driver(j36fb_driver);

MODULE_DESCRIPTION("J36 Ultra dma-buf exporter for the LK framebuffer carveout");
/*
 * dma_buf_export(), dma_buf_fd() and dma_buf_put() are EXPORT_SYMBOL_NS_GPL
 * into DMA_BUF, so modpost refuses the module without this. Bare token and not
 * a string: 6.12's MODULE_IMPORT_NS() runs its argument through __stringify(),
 * and the quoted form only became correct in 6.13.
 */
MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL v2");
