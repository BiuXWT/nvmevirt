// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"

static inline uint64_t __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}

void buffer_init(struct buffer *buf, size_t size)
{
	spin_lock_init(&buf->lock);
	buf->size = size;
	buf->remaining = size;
}

uint32_t buffer_allocate(struct buffer *buf, size_t size)
{
	NVMEV_ASSERT(size <= buf->size);

	while (!spin_trylock(&buf->lock)) {
		cpu_relax();
	}

	if (buf->remaining < size) {
		size = 0;
	}

	buf->remaining -= size;

	spin_unlock(&buf->lock);
	return size;
}

bool buffer_release(struct buffer *buf, size_t size)
{
	while (!spin_trylock(&buf->lock))
		;
	buf->remaining += size;
	spin_unlock(&buf->lock);

	return true;
}

void buffer_refill(struct buffer *buf)
{
	while (!spin_trylock(&buf->lock))
		;
	buf->remaining = buf->size;
	spin_unlock(&buf->lock);
}

static void check_params(struct ssdparams *spp)
{
	/*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

	//ftl_assert(is_power_of_2(spp->luns_per_ch));
	//ftl_assert(is_power_of_2(spp->nchs));
}

void ssd_init_params(struct ssdparams *spp, uint64_t capacity, uint32_t nparts)
{
	//page->block->plane->Die chip(lun)
	NVMEV_INFO("file: [%s]-[%d]-[%s] start\n", __FILE__, __LINE__, __FUNCTION__);
	NVMEV_INFO("capacity=%llu,nparts=%u", capacity, nparts);//8GB-1MB,4
	uint64_t blk_size, total_size;

	/**
	 * 页面是物理存储单元，扇区是逻辑存储单元
	 *		页面是SSD内部的实际存储单元，由闪存芯片的物理结构决定。
	 *		扇区是逻辑上的存储单元，用于与操作系统和文件系统交互。
	 */
	//扇区,在SSD中称为页(page)
	spp->secsz = LBA_SIZE; // 扇区大小512
	spp->secs_per_pg = 4096 / LBA_SIZE; // pg == 4KB
	spp->pgsz =
		spp->secsz *
		spp->secs_per_pg; //SSD的一个逻辑页(page)大小4K有8个扇区,每个扇区512字节 , 写入时,需要将扇区合并为页进行写入
	NVMEV_INFO("secsz=%d,secs_per_pg=%d,pgsz=%d", //secsz=512,secs_per_pg=8,pgsz=4096
		   spp->secsz, spp->secs_per_pg, spp->pgsz);

	/**
	* LUN(Logical Unit Number): SSD内部的逻辑单元,通常对应一个闪存芯片(Die chip)
	*/
	spp->nchs = NAND_CHANNELS; //8 8个通道
	spp->pls_per_lun = PLNS_PER_LUN; //1 每个Die chip有1个plane
	spp->luns_per_ch = LUNS_PER_NAND_CH;//2  一个通道连接2个LUN(Die chip)
	spp->cell_mode = CELL_MODE;
	NVMEV_INFO("nchs[%d],pls_per_lun[%d],luns_per_ch[%d],cell_mode[%d]", spp->nchs,
		   spp->pls_per_lun, spp->luns_per_ch, spp->cell_mode);
		   //   nchs[8],pls_per_lun=1,luns_per_ch=2,cell_mode=2

	/* partitioning SSD by dividing channel 通过划分通道（channel）来分区固态硬盘（SSD）*/
	NVMEV_ASSERT((spp->nchs % nparts) == 0);
	spp->nchs /= nparts;// 2通道
	capacity /= nparts;
	NVMEV_INFO("after devide: nchs[%d],capacity[%llu]\n", spp->nchs, capacity); 
			//after devide: nchs[2],capacity[2147221504]

	if (BLKS_PER_PLN > 0) { //BLKS_PER_PLN 8192
		/* flashpgs_per_blk depends on capacity */
		spp->blks_per_pl = BLKS_PER_PLN;//8192 ;plane有8192个block
		blk_size = DIV_ROUND_UP(capacity, spp->blks_per_pl * spp->pls_per_lun *
							  spp->luns_per_ch * spp->nchs);//块大小 = 容量/(通道数2*每个通道对应的Die数量2*plane数量1*块数量8192)
		NVMEV_INFO("BLKS_PER_PLN[%d],blk_size=%llu", BLKS_PER_PLN,
			   blk_size); //BLKS_PER_PLN[8192],blk_size=65528
	} else {
		NVMEV_ASSERT(BLK_SIZE > 0);
		blk_size = BLK_SIZE;
		spp->blks_per_pl = DIV_ROUND_UP(capacity, blk_size * spp->pls_per_lun *
								  spp->luns_per_ch * spp->nchs);
		NVMEV_INFO("blk_size=%llu,blks_per_pl=%d", blk_size, spp->blks_per_pl); //
	}

	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % spp->pgsz) == 0 && (FLASH_PAGE_SIZE % spp->pgsz) == 0);
	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

	spp->pgs_per_oneshotpg = ONESHOT_PAGE_SIZE / (spp->pgsz);// 每次写入 8 页
	spp->oneshotpgs_per_blk = DIV_ROUND_UP(blk_size, ONESHOT_PAGE_SIZE);
	NVMEV_INFO("ONESHOT_PAGE_SIZE[%d],pgs_per_oneshotpg=%d,oneshotpgs_per_blk=%d",
		   ONESHOT_PAGE_SIZE, spp->pgs_per_oneshotpg, spp->oneshotpgs_per_blk);
	//ONESHOT_PAGE_SIZE[32768,32K],pgs_per_oneshotpg=8,oneshotpgs_per_blk=2

	/* 通常闪存页大于逻辑页, 实际场景中若干个逻辑页写在一个物理页: 逻辑页实际和子物理页一一对应 */
	spp->pgs_per_flashpg = FLASH_PAGE_SIZE / (spp->pgsz); // 闪存页大小(32KB)÷逻辑页大小(4KB)=8
	spp->flashpgs_per_blk = (ONESHOT_PAGE_SIZE / FLASH_PAGE_SIZE) * spp->oneshotpgs_per_blk;
	spp->pgs_per_blk = spp->pgs_per_oneshotpg * spp->oneshotpgs_per_blk;// 8
	NVMEV_INFO("FLASH_PAGE_SIZE[%d],pgs_per_flashpg[%d],flashpgs_per_blk[%d],pgs_per_blk[%d]\n",
				FLASH_PAGE_SIZE, spp->pgs_per_flashpg, spp->flashpgs_per_blk, spp->pgs_per_blk);
				//FLASH_PAGE_SIZE[32768],pgs_per_flashpg[8],flashpgs_per_blk[2],pgs_per_blk[16]

	spp->write_unit_size = WRITE_UNIT_SIZE;

	spp->pg_4kb_rd_lat[CELL_TYPE_LSB] = NAND_4KB_READ_LATENCY_LSB;
	spp->pg_4kb_rd_lat[CELL_TYPE_MSB] = NAND_4KB_READ_LATENCY_MSB;
	spp->pg_4kb_rd_lat[CELL_TYPE_CSB] = NAND_4KB_READ_LATENCY_CSB;
	spp->pg_rd_lat[CELL_TYPE_LSB] = NAND_READ_LATENCY_LSB;
	spp->pg_rd_lat[CELL_TYPE_MSB] = NAND_READ_LATENCY_MSB;
	spp->pg_rd_lat[CELL_TYPE_CSB] = NAND_READ_LATENCY_CSB;
	spp->pg_wr_lat = NAND_PROG_LATENCY;
	spp->blk_er_lat = NAND_ERASE_LATENCY;
	spp->max_ch_xfer_size = MAX_CH_XFER_SIZE;//通道最大传输大小16KB

	spp->fw_4kb_rd_lat = FW_4KB_READ_LATENCY;
	spp->fw_rd_lat = FW_READ_LATENCY;
	spp->fw_ch_xfer_lat = FW_CH_XFER_LATENCY;
	spp->fw_wbuf_lat0 = FW_WBUF_LATENCY0;
	spp->fw_wbuf_lat1 = FW_WBUF_LATENCY1;

	spp->ch_bandwidth = NAND_CHANNEL_BANDWIDTH;
	spp->pcie_bandwidth = PCIE_BANDWIDTH;

	spp->write_buffer_size = GLOBAL_WB_SIZE;
	spp->write_early_completion = WRITE_EARLY_COMPLETION;
	NVMEV_INFO("ch_bandwidth=%llu,pcie_bandwidth=%llu,write_buffer_size=%llu",
			   spp->ch_bandwidth, spp->pcie_bandwidth, spp->write_buffer_size);

	/* calculated values */
	spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;//8*16=128
	spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;//128*8192=1048576
	spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;//1048576
	spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;//1048576*2=2097152
	spp->tt_secs = spp->secs_per_ch * spp->nchs;//2097152*2=4194304
	NVMEV_INFO("secs_per_blk=%lu,secs_per_pl=%lu,secs_per_lun=%lu,secs_per_ch=%lu,tt_secs=%lu",
		   spp->secs_per_blk, spp->secs_per_pl, spp->secs_per_lun, spp->secs_per_ch, spp->tt_secs);
			//secs_per_blk=128,secs_per_pl=1048576,secs_per_lun=1048576,secs_per_ch=2097152,tt_secs=4194304

	spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;//16*8192=131072
	spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;//131072*1
	spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;//131072*2=262144
	spp->tt_pgs = spp->pgs_per_ch * spp->nchs;//262144*2=524288
	NVMEV_INFO("pgs_per_pl=%lu,pgs_per_lun=%lu,pgs_per_ch=%lu,tt_pgs=%lu", spp->pgs_per_pl,
		   spp->pgs_per_lun, spp->pgs_per_ch, spp->tt_pgs);
			//pgs_per_pl=131072,pgs_per_lun=131072,pgs_per_ch=262144,tt_pgs=524288

	spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;//8192*1
	spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;//8192*2=16384
	spp->tt_blks = spp->blks_per_ch * spp->nchs;//16384*2=32768
	NVMEV_INFO("blks_per_lun=%lu,blks_per_ch=%lu,tt_blks=%lu", 
			spp->blks_per_lun, spp->blks_per_ch, spp->tt_blks);
			//blks_per_lun=8192,blks_per_ch=16384,tt_blks=32768

	spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
	spp->tt_pls = spp->pls_per_ch * spp->nchs;
	NVMEV_INFO("pls_per_ch=%lu,tt_pls=%lu", spp->pls_per_ch, spp->tt_pls);
			  //pls_per_ch=2,tt_pls=4

	spp->tt_luns = spp->luns_per_ch * spp->nchs;//2*2
	NVMEV_INFO("tt_luns=%lu", spp->tt_luns);//4
	
	/* line is special, put it at the end */
	spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
	spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;//4*16=64
	spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
	spp->tt_lines = spp->blks_per_lun;
	NVMEV_INFO("blks_per_line=%lu,pgs_per_line=%lu,secs_per_line=%lu,tt_lines=%lu",
		   spp->blks_per_line, spp->pgs_per_line, spp->secs_per_line, spp->tt_lines);
		   //blks_per_line=4,pgs_per_line=64,secs_per_line=512,tt_lines=8192
	/* TODO: to fix under multiplanes */ // lun size is super-block(line) size

	check_params(spp);

	total_size = (unsigned long)spp->tt_luns * spp->blks_per_lun * spp->pgs_per_blk *
		     spp->secsz * spp->secs_per_pg;//4*8192*16*4*512*8=2147483648
	blk_size = spp->pgs_per_blk * spp->secsz * spp->secs_per_pg;//16*512*8=65536
	NVMEV_INFO("total_size[%llu],blk_size[%llu]",total_size, blk_size);
			//total_size[2147483648],blk_size[65536]
	NVMEV_INFO(
		"Total Capacity(GiB,MiB)=%llu,%llu chs=%u luns=%lu lines=%lu blk-size(MiB,KiB)=%u,%u line-size(MiB,KiB)=%lu,%lu",
		BYTE_TO_GB(total_size), BYTE_TO_MB(total_size), 
		spp->nchs, 
		spp->tt_luns,
		spp->tt_lines, 
		BYTE_TO_MB(spp->pgs_per_blk * spp->pgsz), BYTE_TO_KB(spp->pgs_per_blk * spp->pgsz),
		BYTE_TO_MB(spp->pgs_per_line * spp->pgsz), BYTE_TO_KB(spp->pgs_per_line * spp->pgsz));
	/*
		Total Capacity(GiB,MiB)=2,2048 chs=2 luns=4 lines=8192 blk-size(MiB,KiB)=0,64 line-size(MiB,KiB)=0,256
	*/
	/*
	1 sector = 512 bit
	8 sectors=1 page;
	8 (logical)pages = 1 flash page;
	2 flash page = 1block; 16 pages = 1 block;
	8192 blocks = 1 plane
	1 plane = 1 Die Chip
	4 Die Chips = 1 SSD


	1 channels has 2 Die chip(lun)

	nParts: 4
	Channels: 8 
	Die Chips: 4
	planes: 4
	blocks: 32768
	flash pages: 
	pages: 524288
	sectors: 4194304
	*/
	NVMEV_INFO("file: [%s]-[%d]-[%s] end\n", __FILE__, __LINE__, __FUNCTION__);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
	int i;
	pg->nsecs = spp->secs_per_pg;//8
	pg->sec = kmalloc(sizeof(nand_sec_status_t) * pg->nsecs, GFP_KERNEL);
	for (i = 0; i < pg->nsecs; i++) {
		pg->sec[i] = SEC_FREE;//扇面空
	}
	pg->status = PG_FREE;//页面空
}

static void ssd_remove_nand_page(struct nand_page *pg)
{
	kfree(pg->sec);
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
	int i;
	blk->npgs = spp->pgs_per_blk;//16
	blk->pg = kmalloc(sizeof(struct nand_page) * blk->npgs, GFP_KERNEL);
	for (i = 0; i < blk->npgs; i++) {
		ssd_init_nand_page(&blk->pg[i], spp);
	}
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt = 0;
	blk->wp = 0;
}

static void ssd_remove_nand_blk(struct nand_block *blk)
{
	int i;

	for (i = 0; i < blk->npgs; i++)
		ssd_remove_nand_page(&blk->pg[i]);

	kfree(blk->pg);
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
	int i;
	pl->nblks = spp->blks_per_pl;//8192
	pl->blk = kmalloc(sizeof(struct nand_block) * pl->nblks, GFP_KERNEL);
	for (i = 0; i < pl->nblks; i++) {
		ssd_init_nand_blk(&pl->blk[i], spp);
	}
}

static void ssd_remove_nand_plane(struct nand_plane *pl)
{
	int i;

	for (i = 0; i < pl->nblks; i++)
		ssd_remove_nand_blk(&pl->blk[i]);

	kfree(pl->blk);
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
	int i;
	lun->npls = spp->pls_per_lun;//1
	lun->pl = kmalloc(sizeof(struct nand_plane) * lun->npls, GFP_KERNEL);
	for (i = 0; i < lun->npls; i++) {
		ssd_init_nand_plane(&lun->pl[i], spp);
	}
	lun->next_lun_avail_time = 0;
	lun->busy = false;
}

static void ssd_remove_nand_lun(struct nand_lun *lun)
{
	int i;

	for (i = 0; i < lun->npls; i++)
		ssd_remove_nand_plane(&lun->pl[i]);

	kfree(lun->pl);
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
	int i;
	ch->nluns = spp->luns_per_ch;//2
	ch->lun = kmalloc(sizeof(struct nand_lun) * ch->nluns, GFP_KERNEL);
	for (i = 0; i < ch->nluns; i++) {
		ssd_init_nand_lun(&ch->lun[i], spp);
	}

	ch->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	NVMEV_INFO("init ssd ch perf model");
	chmodel_init(ch->perf_model, spp->ch_bandwidth);//通道模型实例化

	/* Add firmware overhead */
	ch->perf_model->xfer_lat += (spp->fw_ch_xfer_lat * UNIT_XFER_SIZE / KB(4));
}

static void ssd_remove_ch(struct ssd_channel *ch)
{
	int i;

	kfree(ch->perf_model);

	for (i = 0; i < ch->nluns; i++)
		ssd_remove_nand_lun(&ch->lun[i]);

	kfree(ch->lun);
}

static void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
	pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	NVMEV_INFO("init pcie ch perf model");
	chmodel_init(pcie->perf_model, spp->pcie_bandwidth);
}

static void ssd_remove_pcie(struct ssd_pcie *pcie)
{
	kfree(pcie->perf_model);
}

void ssd_init(struct ssd *ssd, struct ssdparams *spp, uint32_t cpu_nr_dispatcher)
{
	NVMEV_INFO("file: [%s]-[%d]-[%s] start\n", __FILE__, __LINE__, __FUNCTION__);
	uint32_t i;
	/* copy spp */
	ssd->sp = *spp;

	/* initialize conv_ftl internal layout architecture */
	ssd->ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); // 40 * 8 = 320
	for (i = 0; i < spp->nchs; i++) {
		ssd_init_ch(&(ssd->ch[i]), spp);
	}

	/* Set CPU number to use same cpuclock as io.c */
	ssd->cpu_nr_dispatcher = cpu_nr_dispatcher;

	ssd->pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
	ssd_init_pcie(ssd->pcie, spp);

	ssd->write_buffer = kmalloc(sizeof(struct buffer), GFP_KERNEL);
	buffer_init(ssd->write_buffer, spp->write_buffer_size);

	NVMEV_INFO("file: [%s]-[%d]-[%s] end\n", __FILE__, __LINE__, __FUNCTION__);
	return;
}

void ssd_remove(struct ssd *ssd)
{
	NVMEV_INFO("file: [%s]-[%d]-[%s] start\n", __FILE__, __LINE__, __FUNCTION__);
	uint32_t i;

	kfree(ssd->write_buffer);
	if (ssd->pcie) {
		kfree(ssd->pcie->perf_model);
		kfree(ssd->pcie);
	}

	for (i = 0; i < ssd->sp.nchs; i++) {
		ssd_remove_ch(&(ssd->ch[i]));
	}

	kfree(ssd->ch);
	NVMEV_INFO("file: [%s]-[%d]-[%s] end\n", __FILE__, __LINE__, __FUNCTION__);
}

uint64_t ssd_advance_pcie(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	struct channel_model *perf_model = ssd->pcie->perf_model;
	return chmodel_request(perf_model, request_time, length);
}

/* Write buffer Performance Model
  Y = A + (B * X)
  Y : latency (ns)
  X : transfer size (4KB unit)
  A : fw_wbuf_lat0
  B : fw_wbuf_lat1 + pcie dma transfer
*/
uint64_t ssd_advance_write_buffer(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	uint64_t nsecs_latest = request_time;
	struct ssdparams *spp = &ssd->sp;

	nsecs_latest += spp->fw_wbuf_lat0;
	nsecs_latest += spp->fw_wbuf_lat1 * DIV_ROUND_UP(length, KB(4));

	nsecs_latest = ssd_advance_pcie(ssd, nsecs_latest, length);

	return nsecs_latest;
}

uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd)
{
	int c = ncmd->cmd;
	uint64_t cmd_stime = (ncmd->stime == 0) ? __get_ioclock(ssd) : ncmd->stime;
	uint64_t nand_stime, nand_etime;
	uint64_t chnl_stime, chnl_etime;
	uint64_t remaining, xfer_size, completed_time;
	struct ssdparams *spp;
	struct nand_lun *lun;
	struct ssd_channel *ch;
	struct ppa *ppa = ncmd->ppa;
	uint32_t cell;
	NVMEV_DEBUG(
		"SSD: %p, Enter stime: %lld, ch %d lun %d blk %d page %d command %d ppa 0x%llx\n",
		ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);

	if (ppa->ppa == UNMAPPED_PPA) {
		NVMEV_ERROR("Error ppa 0x%llx\n", ppa->ppa);
		return cmd_stime;
	}

	spp = &ssd->sp;
	lun = get_lun(ssd, ppa);
	ch = get_ch(ssd, ppa);
	cell = get_cell(ssd, ppa);
	remaining = ncmd->xfer_size;

	switch (c) {
	case NAND_READ:
		/* read: perform NAND cmd first */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);

		if (ncmd->xfer_size == 4096) {
			nand_etime = nand_stime + spp->pg_4kb_rd_lat[cell];
		} else {
			nand_etime = nand_stime + spp->pg_rd_lat[cell];
		}

		/* read: then data transfer through channel */
		chnl_stime = nand_etime;

		while (remaining) {
			xfer_size = min(remaining, (uint64_t)spp->max_ch_xfer_size);
			chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size);

			if (ncmd->interleave_pci_dma) { /* overlap pci transfer with nand ch transfer*/
				completed_time = ssd_advance_pcie(ssd, chnl_etime, xfer_size);
			} else {
				completed_time = chnl_etime;
			}

			remaining -= xfer_size;
			chnl_stime = chnl_etime;
		}

		lun->next_lun_avail_time = chnl_etime;
		break;

	case NAND_WRITE:
		/* write: transfer data through channel first */
		chnl_stime = max(lun->next_lun_avail_time, cmd_stime);

		chnl_etime = chmodel_request(ch->perf_model, chnl_stime, ncmd->xfer_size);

		/* write: then do NAND program */
		nand_stime = chnl_etime;
		nand_etime = nand_stime + spp->pg_wr_lat;
		lun->next_lun_avail_time = nand_etime;
		completed_time = nand_etime;
		break;

	case NAND_ERASE:
		/* erase: only need to advance NAND status */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		nand_etime = nand_stime + spp->blk_er_lat;
		lun->next_lun_avail_time = nand_etime;
		completed_time = nand_etime;
		break;

	case NAND_NOP:
		/* no operation: just return last completed time of lun */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		lun->next_lun_avail_time = nand_stime;
		completed_time = nand_stime;
		break;

	default:
		NVMEV_ERROR("Unsupported NAND command: 0x%x\n", c);
		return 0;
	}

	return completed_time;
}

uint64_t ssd_next_idle_time(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
	uint32_t i, j;
	uint64_t latest = __get_ioclock(ssd);

	for (i = 0; i < spp->nchs; i++) {
		struct ssd_channel *ch = &ssd->ch[i];

		for (j = 0; j < spp->luns_per_ch; j++) {
			struct nand_lun *lun = &ch->lun[j];
			latest = max(latest, lun->next_lun_avail_time);
		}
	}

	return latest;
}

void adjust_ftl_latency(int target, int lat)
{
/* TODO ..*/
#if 0
    struct ssdparams *spp;
    int i;

    for (i = 0; i < SSD_PARTITIONS; i++) {
        spp = &(g_conv_ftls[i].sp);
        NVMEV_INFO("Before latency: %d %d %d, change to %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat, lat);
        switch (target) {
            case NAND_READ:
                spp->pg_rd_lat = lat;
                break;

            case NAND_WRITE:
                spp->pg_wr_lat = lat;
                break;

            case NAND_ERASE:
                spp->blk_er_lat = lat;
                break;

            default:
                NVMEV_ERROR("Unsupported NAND command\n");
        }
        NVMEV_INFO("After latency: %d %d %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat);
    }
#endif
}
