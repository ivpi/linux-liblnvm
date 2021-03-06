/*
 * nvme-lightnvm.c - LightNVM NVMe device
 *
 * Copyright (C) 2014-2015 IT University of Copenhagen
 * Initial release: Matias Bjorling <mb@lightnvm.io>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
 */

#include "nvme.h"

#include <linux/nvme.h>
#include <linux/bitops.h>
#include <linux/lightnvm.h>
#include <linux/vmalloc.h>
#include <linux/sched/sysctl.h>

enum nvme_nvm_admin_opcode {
	nvme_nvm_admin_identity		= 0xe2,
	nvme_nvm_admin_get_l2p_tbl	= 0xea,
	nvme_nvm_admin_get_bb_tbl	= 0xf2,
	nvme_nvm_admin_set_bb_tbl	= 0xf1,
};

struct nvme_nvm_hb_rw {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__u64			rsvd2;
	__le64			metadata;
	__le64			prp1;
	__le64			prp2;
	__le64			spba;
	__le16			length;
	__le16			control;
	__le32			dsmgmt;
	__le64			slba;
};

struct nvme_nvm_ph_rw {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__u64			rsvd2;
	__le64			metadata;
	__le64			prp1;
	__le64			prp2;
	__le64			spba;
	__le16			length;
	__le16			control;
	__le32			dsmgmt;
	__le64			resv;
};

struct nvme_nvm_identity {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__u64			rsvd[2];
	__le64			prp1;
	__le64			prp2;
	__le32			chnl_off;
	__u32			rsvd11[5];
};

struct nvme_nvm_l2ptbl {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__le32			cdw2[4];
	__le64			prp1;
	__le64			prp2;
	__le64			slba;
	__le32			nlb;
	__le16			cdw14[6];
};

struct nvme_nvm_getbbtbl {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__u64			rsvd[2];
	__le64			prp1;
	__le64			prp2;
	__le64			spba;
	__u32			rsvd4[4];
};

struct nvme_nvm_setbbtbl {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__le64			rsvd[2];
	__le64			prp1;
	__le64			prp2;
	__le64			spba;
	__le16			nlb;
	__u8			value;
	__u8			rsvd3;
	__u32			rsvd4[3];
};

struct nvme_nvm_erase_blk {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__u64			rsvd[2];
	__le64			prp1;
	__le64			prp2;
	__le64			spba;
	__le16			length;
	__le16			control;
	__le32			dsmgmt;
	__le64			resv;
};

struct nvme_nvm_command {
	union {
		struct nvme_common_command common;
		struct nvme_nvm_identity identity;
		struct nvme_nvm_hb_rw hb_rw;
		struct nvme_nvm_ph_rw ph_rw;
		struct nvme_nvm_l2ptbl l2p;
		struct nvme_nvm_getbbtbl get_bb;
		struct nvme_nvm_setbbtbl set_bb;
		struct nvme_nvm_erase_blk erase;
	};
};

struct nvme_nvm_completion {
	__le64	result;		/* Used by LightNVM to return ppa completions */
	__le16	sq_head;	/* how much of this queue may be reclaimed */
	__le16	sq_id;		/* submission queue that generated this entry */
	__u16	command_id;	/* of the command which completed */
	__le16	status;		/* did the command fail, and if so, why? */
};

#define NVME_NVM_LP_MLC_PAIRS 886
struct nvme_nvm_lp_mlc {
	__le16			num_pairs;
	__u8			pairs[NVME_NVM_LP_MLC_PAIRS];
};

struct nvme_nvm_lp_tbl {
	__u8			id[8];
	struct nvme_nvm_lp_mlc	mlc;
};

struct nvme_nvm_id_group {
	__u8			mtype;
	__u8			fmtype;
	__le16			res16;
	__u8			num_ch;
	__u8			num_lun;
	__u8			num_pln;
	__u8			rsvd1;
	__le16			num_blk;
	__le16			num_pg;
	__le16			fpg_sz;
	__le16			csecs;
	__le16			sos;
	__le16			rsvd2;
	__le32			trdt;
	__le32			trdm;
	__le32			tprt;
	__le32			tprm;
	__le32			tbet;
	__le32			tbem;
	__le32			mpos;
	__le32			mccap;
	__le16			cpar;
	__u8			reserved[10];
	struct nvme_nvm_lp_tbl lptbl;
} __packed;

struct nvme_nvm_addr_format {
	__u8			ch_offset;
	__u8			ch_len;
	__u8			lun_offset;
	__u8			lun_len;
	__u8			pln_offset;
	__u8			pln_len;
	__u8			blk_offset;
	__u8			blk_len;
	__u8			pg_offset;
	__u8			pg_len;
	__u8			sect_offset;
	__u8			sect_len;
	__u8			res[4];
} __packed;

struct nvme_nvm_id {
	__u8			ver_id;
	__u8			vmnt;
	__u8			cgrps;
	__u8			res;
	__le32			cap;
	__le32			dom;
	struct nvme_nvm_addr_format ppaf;
	__u8			resv[228];
	struct nvme_nvm_id_group groups[4];
} __packed;

struct nvme_nvm_bb_tbl {
	__u8	tblid[4];
	__le16	verid;
	__le16	revid;
	__le32	rvsd1;
	__le32	tblks;
	__le32	tfact;
	__le32	tgrown;
	__le32	tdresv;
	__le32	thresv;
	__le32	rsvd2[8];
	__u8	blk[0];
};

/*
 * Check we didn't inadvertently grow the command struct
 */
static inline void _nvme_nvm_check_size(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_nvm_identity) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_hb_rw) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_ph_rw) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_getbbtbl) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_setbbtbl) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_l2ptbl) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_erase_blk) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_id_group) != 960);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_addr_format) != 128);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_id) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_nvm_bb_tbl) != 512);
}

static int init_grps(struct nvm_id *nvm_id, struct nvme_nvm_id *nvme_nvm_id)
{
	struct nvme_nvm_id_group *src;
	struct nvm_id_group *dst;
	int i, end;

	end = min_t(u32, 4, nvm_id->cgrps);

	for (i = 0; i < end; i++) {
		src = &nvme_nvm_id->groups[i];
		dst = &nvm_id->groups[i];

		dst->mtype = src->mtype;
		dst->fmtype = src->fmtype;
		dst->num_ch = src->num_ch;
		dst->num_lun = src->num_lun;
		dst->num_pln = src->num_pln;

		dst->num_pg = le16_to_cpu(src->num_pg);
		dst->num_blk = le16_to_cpu(src->num_blk);
		dst->fpg_sz = le16_to_cpu(src->fpg_sz);
		dst->csecs = le16_to_cpu(src->csecs);
		dst->sos = le16_to_cpu(src->sos);

		dst->trdt = le32_to_cpu(src->trdt);
		dst->trdm = le32_to_cpu(src->trdm);
		dst->tprt = le32_to_cpu(src->tprt);
		dst->tprm = le32_to_cpu(src->tprm);
		dst->tbet = le32_to_cpu(src->tbet);
		dst->tbem = le32_to_cpu(src->tbem);
		dst->mpos = le32_to_cpu(src->mpos);
		dst->mccap = le32_to_cpu(src->mccap);

		dst->cpar = le16_to_cpu(src->cpar);

		if (dst->fmtype == NVM_ID_FMTYPE_MLC) {
			memcpy(dst->lptbl.id, src->lptbl.id, 8);
			dst->lptbl.mlc.num_pairs =
					le16_to_cpu(src->lptbl.mlc.num_pairs);

			if (dst->lptbl.mlc.num_pairs > NVME_NVM_LP_MLC_PAIRS) {
				pr_err("nvm: number of MLC pairs not supported\n");
				return -EINVAL;
			}

			memcpy(dst->lptbl.mlc.pairs, src->lptbl.mlc.pairs,
						dst->lptbl.mlc.num_pairs);
		}
	}

	return 0;
}

static int nvme_nvm_identity(struct nvm_dev *nvmdev, struct nvm_id *nvm_id)
{
	struct nvme_ns *ns = nvmdev->q->queuedata;
	struct nvme_nvm_id *nvme_nvm_id;
	struct nvme_nvm_command c = {};
	int ret;

	c.identity.opcode = nvme_nvm_admin_identity;
	c.identity.nsid = cpu_to_le32(ns->ns_id);
	c.identity.chnl_off = 0;

	nvme_nvm_id = kmalloc(sizeof(struct nvme_nvm_id), GFP_KERNEL);
	if (!nvme_nvm_id)
		return -ENOMEM;

	ret = nvme_submit_sync_cmd(ns->ctrl->admin_q, (struct nvme_command *)&c,
				nvme_nvm_id, sizeof(struct nvme_nvm_id));
	if (ret) {
		ret = -EIO;
		goto out;
	}

	nvm_id->ver_id = nvme_nvm_id->ver_id;
	nvm_id->vmnt = nvme_nvm_id->vmnt;
	nvm_id->cgrps = nvme_nvm_id->cgrps;
	nvm_id->cap = le32_to_cpu(nvme_nvm_id->cap);
	nvm_id->dom = le32_to_cpu(nvme_nvm_id->dom);
	memcpy(&nvm_id->ppaf, &nvme_nvm_id->ppaf,
					sizeof(struct nvme_nvm_addr_format));

	ret = init_grps(nvm_id, nvme_nvm_id);
out:
	kfree(nvme_nvm_id);
	return ret;
}

static int nvme_nvm_get_l2p_tbl(struct nvm_dev *nvmdev, u64 slba, u32 nlb,
				nvm_l2p_update_fn *update_l2p, void *priv)
{
	struct nvme_ns *ns = nvmdev->q->queuedata;
	struct nvme_nvm_command c = {};
	u32 len = queue_max_hw_sectors(ns->ctrl->admin_q) << 9;
	u32 nlb_pr_rq = len / sizeof(u64);
	u64 cmd_slba = slba;
	void *entries;
	int ret = 0;

	c.l2p.opcode = nvme_nvm_admin_get_l2p_tbl;
	c.l2p.nsid = cpu_to_le32(ns->ns_id);
	entries = kmalloc(len, GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	while (nlb) {
		u32 cmd_nlb = min(nlb_pr_rq, nlb);

		c.l2p.slba = cpu_to_le64(cmd_slba);
		c.l2p.nlb = cpu_to_le32(cmd_nlb);

		ret = nvme_submit_sync_cmd(ns->ctrl->admin_q,
				(struct nvme_command *)&c, entries, len);
		if (ret) {
			dev_err(ns->ctrl->device,
				"L2P table transfer failed (%d)\n", ret);
			ret = -EIO;
			goto out;
		}

		if (update_l2p(cmd_slba, cmd_nlb, entries, priv)) {
			ret = -EINTR;
			goto out;
		}

		cmd_slba += cmd_nlb;
		nlb -= cmd_nlb;
	}

out:
	kfree(entries);
	return ret;
}

static int nvme_nvm_get_bb_tbl(struct nvm_dev *nvmdev, struct ppa_addr ppa,
								u8 *blks)
{
	struct request_queue *q = nvmdev->q;
	struct nvme_ns *ns = q->queuedata;
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_nvm_command c = {};
	struct nvme_nvm_bb_tbl *bb_tbl;
	int nr_blks = nvmdev->blks_per_lun * nvmdev->plane_mode;
	int tblsz = sizeof(struct nvme_nvm_bb_tbl) + nr_blks;
	int ret = 0;

	c.get_bb.opcode = nvme_nvm_admin_get_bb_tbl;
	c.get_bb.nsid = cpu_to_le32(ns->ns_id);
	c.get_bb.spba = cpu_to_le64(ppa.ppa);

	bb_tbl = kzalloc(tblsz, GFP_KERNEL);
	if (!bb_tbl)
		return -ENOMEM;

	ret = nvme_submit_sync_cmd(ctrl->admin_q, (struct nvme_command *)&c,
								bb_tbl, tblsz);
	if (ret) {
		dev_err(ctrl->device, "get bad block table failed (%d)\n", ret);
		ret = -EIO;
		goto out;
	}

	if (bb_tbl->tblid[0] != 'B' || bb_tbl->tblid[1] != 'B' ||
		bb_tbl->tblid[2] != 'L' || bb_tbl->tblid[3] != 'T') {
		dev_err(ctrl->device, "bbt format mismatch\n");
		ret = -EINVAL;
		goto out;
	}

	if (le16_to_cpu(bb_tbl->verid) != 1) {
		ret = -EINVAL;
		dev_err(ctrl->device, "bbt version not supported\n");
		goto out;
	}

	if (le32_to_cpu(bb_tbl->tblks) != nr_blks) {
		ret = -EINVAL;
		dev_err(ctrl->device,
				"bbt unsuspected blocks returned (%u!=%u)",
				le32_to_cpu(bb_tbl->tblks), nr_blks);
		goto out;
	}

	memcpy(blks, bb_tbl->blk, nvmdev->blks_per_lun * nvmdev->plane_mode);
out:
	kfree(bb_tbl);
	return ret;
}

static int nvme_nvm_set_bb_tbl(struct nvm_dev *nvmdev, struct ppa_addr *ppas,
							int nr_ppas, int type)
{
	struct nvme_ns *ns = nvmdev->q->queuedata;
	struct nvme_nvm_command c = {};
	int ret = 0;

	c.set_bb.opcode = nvme_nvm_admin_set_bb_tbl;
	c.set_bb.nsid = cpu_to_le32(ns->ns_id);
	c.set_bb.spba = cpu_to_le64(ppas->ppa);
	c.set_bb.nlb = cpu_to_le16(nr_ppas - 1);
	c.set_bb.value = type;

	ret = nvme_submit_sync_cmd(ns->ctrl->admin_q, (struct nvme_command *)&c,
								NULL, 0);
	if (ret)
		dev_err(ns->ctrl->device, "set bad block table failed (%d)\n",
									ret);
	return ret;
}

static inline void nvme_nvm_rqtocmd(struct request *rq, struct nvm_rq *rqd,
				struct nvme_ns *ns, struct nvme_nvm_command *c)
{
	c->ph_rw.opcode = rqd->opcode;
	c->ph_rw.nsid = cpu_to_le32(ns->ns_id);
	c->ph_rw.spba = cpu_to_le64(rqd->ppa_addr.ppa);
	c->ph_rw.metadata = cpu_to_le64(rqd->dma_meta_list);
	c->ph_rw.control = cpu_to_le16(rqd->flags);
	c->ph_rw.length = cpu_to_le16(rqd->nr_ppas - 1);

	if (rqd->opcode == NVM_OP_HBWRITE || rqd->opcode == NVM_OP_HBREAD)
		c->hb_rw.slba = cpu_to_le64(nvme_block_nr(ns,
					rqd->bio->bi_iter.bi_sector));
}

static void nvme_nvm_end_io(struct request *rq, int error)
{
	struct nvm_rq *rqd = rq->end_io_data;
	struct nvme_nvm_completion *cqe = rq->special;

	if (cqe)
		rqd->ppa_status = le64_to_cpu(cqe->result);

	nvm_end_io(rqd, error);

	kfree(rq->cmd);
	blk_mq_free_request(rq);
}

static int nvme_nvm_submit_io(struct nvm_dev *dev, struct nvm_rq *rqd)
{
	struct request_queue *q = dev->q;
	struct nvme_ns *ns = q->queuedata;
	struct request *rq;
	struct bio *bio = rqd->bio;
	struct nvme_nvm_command *cmd;

	rq = blk_mq_alloc_request(q, bio_data_dir(bio), 0);
	if (IS_ERR(rq))
		return -ENOMEM;

	cmd = kzalloc(sizeof(struct nvme_nvm_command) +
				sizeof(struct nvme_nvm_completion), GFP_KERNEL);
	if (!cmd) {
		blk_mq_free_request(rq);
		return -ENOMEM;
	}

	rq->cmd_type = REQ_TYPE_DRV_PRIV;
	rq->ioprio = bio_prio(bio);

	if (bio_has_data(bio))
		rq->nr_phys_segments = bio_phys_segments(q, bio);

	rq->__data_len = bio->bi_iter.bi_size;
	rq->bio = rq->biotail = bio;

	nvme_nvm_rqtocmd(rq, rqd, ns, cmd);

	rq->cmd = (unsigned char *)cmd;
	rq->cmd_len = sizeof(struct nvme_nvm_command);
	rq->special = cmd + 1;

	rq->end_io_data = rqd;

	blk_execute_rq_nowait(q, NULL, rq, 0, nvme_nvm_end_io);

	return 0;
}

static void nvme_nvm_end_user_io(struct request *rq, int error)
{
	struct completion *waiting = rq->end_io_data;

	complete(waiting);
}

static int nvme_nvm_submit_user_io(struct nvm_dev *dev, struct nvm_rq *rqd,
					void __user *data, unsigned len)
{
	struct request_queue *q = dev->q;
	struct nvme_ns *ns = q->queuedata;
	struct request *rq;
	struct nvme_nvm_command *cmd;
	DECLARE_COMPLETION_ONSTACK(wait);
	unsigned long hang_check;
	struct nvme_nvm_completion *cqe;
	struct bio *bio = NULL;

	rq = blk_mq_alloc_request(q, rqd->opcode & 1, 0);
	if (IS_ERR(rq))
		return -ENOMEM;

	cmd = kzalloc(sizeof(struct nvme_nvm_command) +
				sizeof(struct nvme_nvm_completion), GFP_KERNEL);
	if (!cmd) {
		blk_mq_free_request(rq);
		return -ENOMEM;
	}

	rq->cmd_type = REQ_TYPE_DRV_PRIV;
	if (data) {
		if (blk_rq_map_user(q, rq, NULL, data, len, GFP_KERNEL)) {
			blk_mq_free_request(rq);
			kfree(cmd);
			return -ENOMEM;
		}
		bio = rq->bio;
		bio_get(bio);
	}

	nvme_nvm_rqtocmd(rq, rqd, ns, cmd);

	rq->cmd = (unsigned char *)cmd;
	rq->cmd_len = sizeof(struct nvme_nvm_command);
	rq->special = cmd + 1;

	rq->end_io_data = &wait;

	blk_execute_rq_nowait(q, NULL, rq, 0, nvme_nvm_end_user_io);

	/* Prevent hang_check timer from firing at us during very long I/O */
	hang_check = 0;//sysctl_hung_task_timeout_secs;
	if (hang_check)
		while (!wait_for_completion_io_timeout(&wait,
							hang_check * (HZ/2)));
	else
		wait_for_completion_io(&wait);

	cqe = rq->special;
	if (cqe)
		rqd->ppa_status = le64_to_cpu(cqe->result);

	rqd->error = rq->errors;

	if (bio) {
		blk_rq_unmap_user(bio);
		bio_put(bio);
	}
	kfree(rq->cmd);
	blk_mq_free_request(rq);

	return 0;
}

static int nvme_nvm_erase_block(struct nvm_dev *dev, struct nvm_rq *rqd)
{
	struct request_queue *q = dev->q;
	struct nvme_ns *ns = q->queuedata;
	struct nvme_nvm_command c = {};

	c.erase.opcode = NVM_OP_ERASE;
	c.erase.nsid = cpu_to_le32(ns->ns_id);
	c.erase.spba = cpu_to_le64(rqd->ppa_addr.ppa);
	c.erase.length = cpu_to_le16(rqd->nr_ppas - 1);

	return nvme_submit_sync_cmd(q, (struct nvme_command *)&c, NULL, 0);
}

static void *nvme_nvm_create_dma_pool(struct nvm_dev *nvmdev, char *name)
{
	struct nvme_ns *ns = nvmdev->q->queuedata;

	return dma_pool_create(name, ns->ctrl->dev, PAGE_SIZE, PAGE_SIZE, 0);
}

static void nvme_nvm_destroy_dma_pool(void *pool)
{
	struct dma_pool *dma_pool = pool;

	dma_pool_destroy(dma_pool);
}

static void *nvme_nvm_dev_dma_alloc(struct nvm_dev *dev, void *pool,
				    gfp_t mem_flags, dma_addr_t *dma_handler)
{
	return dma_pool_alloc(pool, mem_flags, dma_handler);
}

static void nvme_nvm_dev_dma_free(void *pool, void *addr,
							dma_addr_t dma_handler)
{
	dma_pool_free(pool, addr, dma_handler);
}

static struct nvm_dev_ops nvme_nvm_dev_ops = {
	.identity		= nvme_nvm_identity,

	.get_l2p_tbl		= nvme_nvm_get_l2p_tbl,

	.get_bb_tbl		= nvme_nvm_get_bb_tbl,
	.set_bb_tbl		= nvme_nvm_set_bb_tbl,

	.submit_io		= nvme_nvm_submit_io,
	.submit_user_io		= nvme_nvm_submit_user_io,
	.erase_block		= nvme_nvm_erase_block,

	.create_dma_pool	= nvme_nvm_create_dma_pool,
	.destroy_dma_pool	= nvme_nvm_destroy_dma_pool,
	.dev_dma_alloc		= nvme_nvm_dev_dma_alloc,
	.dev_dma_free		= nvme_nvm_dev_dma_free,

	.max_phys_sect		= 64,
};

int nvme_nvm_register(struct nvme_ns *ns, char *disk_name, int node,
		      const struct attribute_group *attrs)
{
	struct request_queue *q = ns->queue;
	struct nvm_dev *dev;
	int ret;

	dev = nvm_alloc_dev(node);
	if (!dev)
		return -ENOMEM;

	dev->q = q;
	memcpy(dev->name, disk_name, DISK_NAME_LEN);
	dev->ops = &nvme_nvm_dev_ops;
	dev->parent_dev = ns->ctrl->device;
	dev->private_data = ns;
	ns->ndev = dev;

	ret = nvm_register(dev);

	ns->lba_shift = ilog2(dev->sec_size) - 9;

	if (sysfs_create_group(&dev->dev.kobj, attrs))
		pr_warn("%s: failed to create sysfs group for identification\n",
			disk_name);
	return ret;
}

void nvme_nvm_unregister(struct nvme_ns *ns)
{
	nvm_unregister(ns->ndev);
}

/* move to shared place when used in multiple places. */
#define PCI_VENDOR_ID_CNEX 0x1d1d
#define PCI_DEVICE_ID_CNEX_WL 0x2807
#define PCI_DEVICE_ID_CNEX_QEMU 0x1f1f

int nvme_nvm_ns_supported(struct nvme_ns *ns, struct nvme_id_ns *id)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	/* XXX: this is poking into PCI structures from generic code! */
	struct pci_dev *pdev = to_pci_dev(ctrl->dev);

	/* QEMU NVMe simulator - PCI ID + Vendor specific bit */
	if (pdev->vendor == PCI_VENDOR_ID_CNEX &&
				pdev->device == PCI_DEVICE_ID_CNEX_QEMU &&
							id->vs[0] == 0x1)
		return 1;

	/* CNEX Labs - PCI ID + Vendor specific bit */
	if (pdev->vendor == PCI_VENDOR_ID_CNEX &&
				pdev->device == PCI_DEVICE_ID_CNEX_WL &&
							id->vs[0] == 0x1)
		return 1;

	/* CNEX Labs - PCI ID + Vendor specific bit */
	if (pdev->vendor == PCI_VENDOR_ID_CNEX &&
				pdev->device == 0x0e01 &&
							id->vs[0] == 0x1)
		return 1;


	return 0;
}

static void _peek_pread(struct nvme_nvm_command *cmd)
{
	int flags = le16_to_cpu(cmd->ph_rw.control);

	if (flags != (NVM_IO_SNGL_ACCESS | NVM_IO_SUSPEND))
		pr_err("R ERROR - flags:%d\n", flags);
}

static void _peek_pwrite(struct nvme_nvm_command *cmd)
{
	int flags = le16_to_cpu(cmd->ph_rw.control);
	int nppas = le16_to_cpu(cmd->ph_rw.length) + 1;

	int ppa_off = 0;
	int i;
	u64 *ppa_list, ppa1, ppa2, mask1, mask2, exp, exp2;

	ppa_list = (u64*) phys_to_virt(le64_to_cpu(cmd->ph_rw.spba));

	if (flags != NVM_IO_QUAD_ACCESS)
		pr_err("W ERROR - flags:%d\n", flags);

	if (((nppas) % 16))
		pr_err("W ERROR - nppas:%d\n", nppas);

next:
	mask1 = 0xF0;
	mask2 = 0xFFFFFF0F;

	exp2 = ppa_list[ppa_off] & mask2;

	for (i = ppa_off; i < ppa_off + 16; i++) {
		exp = i % 16;
		ppa1 = (ppa_list[i] & mask1) >> 4;
		ppa2 = ppa_list[i] & mask2;

		if (ppa2 != exp2) {
			int j;

			pr_err("W ERROR - exp:%llu, ppa2:%llu\n", exp2, ppa2);

			for (j = ppa_off; j < ppa_off + 16; j++)
				pr_err("dev[%d]:%llx\n", j, ppa_list[j]);
		}

		if (ppa1 != exp) {
			int j;

			pr_err("W ERROR - exp:%llu, ppa1:%llu\n", exp, ppa1);

			for (j = ppa_off; j < ppa_off + 16; j++)
				pr_err("dev[%d]:%llx\n", j, ppa_list[j]);
		}

		if (ppa_list[i] >> 31) {
			int j;

			pr_err("W ERROR - corrupted\b");

			for (j = ppa_off; j < ppa_off + 16; j++)
				pr_err("dev[%d]:%llx\n", j, ppa_list[j]);
		}
	}

	ppa_off += 16;
	if (ppa_off < nppas)
		goto next;
}

void _peek_erase(struct nvme_nvm_command *cmd)
{
	int flags = le16_to_cpu(cmd->ph_rw.control);
	int nppas = le16_to_cpu(cmd->ph_rw.length) + 1;

	u64 *ppa_list, ppa, mask, exp;
	int i;

	ppa_list = (u64*) phys_to_virt(le64_to_cpu(cmd->ph_rw.spba));

	if (flags != NVM_IO_QUAD_ACCESS)
		pr_err("E ERROR - flags:%d\n", flags);

	if (nppas != 4)
		pr_err("E ERROR - nppas:%d\n", flags);

	/* Planes */
	mask = 0xC0;
	for (i = 0; i < nppas; i++) {
		exp = i;
		ppa = (ppa_list[i] & mask) >> 6;

		if (ppa != exp) {
			int j;

			pr_err("E ERROR - exp:%llu, ppa:%llu\n",
					exp, ppa);

			for (j = 0; j < nppas; j++)
				pr_err("dev[%d]:%llx\n",
						j, ppa_list[j]);
		}
	}
}

void _peek_setbb(struct nvme_nvm_command *cmd)
{
	uint64_t *ppa_list;
	int i;

	ppa_list = (uint64_t*) phys_to_virt(le64_to_cpu(cmd->ph_rw.spba));

	pr_info("set_bb0{ opcode(%d), flags(%d), command_id(%d), nsid(%d) }\n",
		cmd->set_bb.opcode, cmd->set_bb.flags,
		cmd->set_bb.command_id, cmd->set_bb.nsid);
	pr_info("set_bb1{ spba(%llu), nlb(%d), value(%d) }\n",
		cmd->set_bb.spba,
		cmd->set_bb.nlb,
		cmd->set_bb.value
		);
	pr_info("set_bb2{ ppa_list(%p) }", ppa_list);
	for (i = 0; i < cmd->set_bb.nlb+1; ++i) {
		pr_info("set_bb3{ ppa_list[%d] = 0x%016llx }\n", i, ppa_list[i]);
	}

}

#ifdef CONFIG_NVM_DEBUG
void nvm_nvmecmd_peek(void *data)
{
	struct nvme_nvm_command *cmd = data;

	switch (cmd->common.opcode) {
		case 0xf1:
			_peek_setbb(cmd);
			break;
		case NVM_OP_PWRITE:
			_peek_pwrite(cmd);
			break;
		case NVM_OP_PREAD:
			_peek_pread(cmd);
			break;
		case NVM_OP_ERASE:
			_peek_erase(cmd);
			break;
		default:
			pr_info("opcode(%d)\n",
				cmd->common.opcode);
			break;
	}
}
#else
void nvm_nvmecmd_peek(void *data) { }
#endif	/* CONFIG_NVM_DEBUG */
