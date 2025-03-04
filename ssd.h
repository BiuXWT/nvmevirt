// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_SSD_H
#define _NVMEVIRT_SSD_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "channel_model.h"
/*
    Default malloc size (when sector size is 512B)
    Channel = 40 * 8 = 320
    LUN     = 40 * 8 = 320
    Plane   = 16 * 1 = 16
    Block   = 32 * 256 = 8192
    Page    = 16 * 256 = 4096
    Sector  = 4 * 8 = 32

    Line    = 40 * 256 = 10240
    maptbl  = 8 * 4194304 = 33554432
    rmap    = 8 * 4194304 = 33554432
*/

#define INVALID_PPA (~(0ULL))
#define INVALID_LPN (~(0ULL))
#define UNMAPPED_PPA (~(0ULL))

enum {
	NAND_READ = 0,
	NAND_WRITE = 1,
	NAND_ERASE = 2,
	NAND_NOP = 3,
};

enum {
	USER_IO = 0,
	GC_IO = 1,
};

enum {
	SEC_FREE = 0,
	SEC_INVALID = 1,
	SEC_VALID = 2,

	PG_FREE = 0,
	PG_INVALID = 1,
	PG_VALID = 2
};

/* Cell type */
enum { CELL_TYPE_LSB, CELL_TYPE_MSB, CELL_TYPE_CSB, MAX_CELL_TYPES };

#define TOTAL_PPA_BITS (64) //
#define BLK_BITS (16)
#define PAGE_BITS (16) //65535
#define PL_BITS (8) //256
#define LUN_BITS (8)
#define CH_BITS (8)
#define RSB_BITS (TOTAL_PPA_BITS - (BLK_BITS + PAGE_BITS + PL_BITS + LUN_BITS + CH_BITS)) //8

/*物理页地址 describe a physical page addr */
struct ppa {
	union {
		struct {
			uint64_t pg : PAGE_BITS; // pg == 4KB
			uint64_t blk : BLK_BITS;
			uint64_t pl : PL_BITS;
			uint64_t lun : LUN_BITS;
			uint64_t ch : CH_BITS;
			uint64_t rsv : RSB_BITS;// 保留位
		} g;

		struct {
			uint64_t : PAGE_BITS;
			uint64_t blk_in_ssd : BLK_BITS + PL_BITS + LUN_BITS + CH_BITS;
			uint64_t rsv : RSB_BITS;
		} h;

		uint64_t ppa;
	};
};

typedef int nand_sec_status_t;

struct nand_page {
	nand_sec_status_t *sec; // 扇面状态
	int nsecs; //Sector 数量
	int status;
};

struct nand_block {
	struct nand_page *pg; //Page
	int npgs;//Page（页面）数量
	int ipc; /*失效页数量 invalid page count */
	int vpc; /*有效页数量 valid page count */
	int erase_cnt;
	int wp; /* current write pointer */
};

struct nand_plane {
	struct nand_block *blk; // Block
	uint64_t next_pln_avail_time;
	int nblks;// Block（块）数量
};

struct nand_lun {
	struct nand_plane *pl; //Plane
	int npls;// Plane（面）数量
	uint64_t next_lun_avail_time;
	bool busy;
	uint64_t gc_endtime;// 垃圾回收结束时间
};

struct ssd_channel {
	struct nand_lun *lun; //Die chip
	int nluns; //LUN（Die芯片）数量
	uint64_t gc_endtime;
	struct channel_model *perf_model;
};

struct ssd_pcie {
	struct channel_model *perf_model;
};

struct nand_cmd {
	int type;
	int cmd;
	uint64_t xfer_size; // byte
	uint64_t stime; /* Coperd: request arrival time */
	bool interleave_pci_dma;
	struct ppa *ppa;
};

struct buffer {
	size_t size;
	size_t remaining;
	spinlock_t lock;
};

/*
pg (page): Mapping unit (4KB)
flashpg (flash page) : Nand sensing unit , tR
oneshotpg (oneshot page) : Nand program unit, tPROG, (eg. flashpg * 3 (TLC))
blk (block): Nand erase unit
lun (die) : Nand operation unit
ch (channel) : Nand <-> SSD controller data transfer unit
*/
/*
pg（页面（逻辑层面））：映射单元（4KB）
flashpg（闪存页面（物理层面））：NAND 感测单元，tR（读取时间）
oneshotpg（一次性页面）：NAND 编程单元，tPROG（编程时间）（例如：flashpg × 3（TLC））
blk（块）：NAND 擦除单元
lun（die芯片）：NAND 操作单元
ch（通道）：NAND 与 SSD 控制器之间的数据传输单元
*/
struct ssdparams {
    int secsz; /* 扇区大小，以字节为单位 */
    int secs_per_pg; /* 每个页面的扇区数量 */
    int pgsz; /*页大小,逻辑页 映射单元大小，以字节为单位 */
    int pgs_per_flashpg; /* 每个闪存页面的逻辑页面数量 */
    int flashpgs_per_blk; /* 每个块的闪存页面数量 */
    int pgs_per_oneshotpg; /* 每个单次写入页面的页面数量 */
    int oneshotpgs_per_blk; /* 每个块的单次写入页面数量 */
    int pgs_per_blk; /* 每个block的page数量 */
    int blks_per_pl; /* 每个plane的block数量 */
    int pls_per_lun; /* 每个LUN（Die芯片）的plane数量 */
    int luns_per_ch; /* 每个通道的LUN数量 */
    int nchs; /* SSD中的通道数量 */
    int cell_mode; /* 单元模式SLC MLC TLC QLC  */

    /* NVMe写入命令的单位大小
       传输大小应为该值的倍数 */
    int write_unit_size;
    bool write_early_completion; /* 写入操作是否支持提前完成 */

    int pg_4kb_rd_lat[MAX_CELL_TYPES]; /* NAND页面4KB读取延迟，以纳秒为单位。感测时间（半tR） */
    int pg_rd_lat[MAX_CELL_TYPES]; /* NAND页面读取延迟，以纳秒为单位。感测时间（tR） */
    int pg_wr_lat; /* NAND页面编程延迟，以纳秒为单位。编程时间（tPROG） */
    int blk_er_lat; /* NAND块擦除延迟，以纳秒为单位。擦除时间（tERASE） */
    int max_ch_xfer_size; /* 通道最大传输大小 */

    int fw_4kb_rd_lat; /* 4KB读取的固件开销，以纳秒为单位 */
    int fw_rd_lat; /* 读取操作的固件开销，以纳秒为单位 */
    int fw_wbuf_lat0; /* 写入缓冲区的固件开销0，以纳秒为单位 */
    int fw_wbuf_lat1; /* 写入缓冲区的固件开销1，以纳秒为单位 */
    int fw_ch_xfer_lat; /* NAND通道数据传输（4KB）的固件开销，以纳秒为单位 */

    uint64_t ch_bandwidth; /* NAND通道的最大带宽，以MiB/s为单位 */
    uint64_t pcie_bandwidth; /* PCIe的最大带宽，以MiB/s为单位 */

    /* 以下是计算得出的值 */
    unsigned long secs_per_blk; /* 每个块的扇区数量 */
    unsigned long secs_per_pl; /* 每个平面的扇区数量 */
    unsigned long secs_per_lun; /* 每个LUN的扇区数量 */
    unsigned long secs_per_ch; /* 每个通道的扇区数量 */
    unsigned long tt_secs; /* SSD中的总扇区数量 */

    unsigned long pgs_per_pl; /* 每个平面的页面数量 */
    unsigned long pgs_per_lun; /* 每个LUN的页面数量 */
    unsigned long pgs_per_ch; /* 每个通道的页面数量 */
    unsigned long tt_pgs; /* SSD中的总页面数量 */

    unsigned long blks_per_lun; /* 每个LUN的块数量 */
    unsigned long blks_per_ch; /* 每个通道的块数量 */
    unsigned long tt_blks; /* SSD中的总块数量 */

    unsigned long secs_per_line;
    unsigned long pgs_per_line;
    unsigned long blks_per_line;
    unsigned long tt_lines;

    unsigned long pls_per_ch; /* 每个通道的平面数量 */
    unsigned long tt_pls; /* SSD中的总平面数量 */

    unsigned long tt_luns; /* SSD中的总LUN数量 */

    unsigned long long write_buffer_size; /* 写入缓冲区大小 */
};

// struct ssdparams {
// 	int secsz; /* sector size in bytes */
// 	int secs_per_pg; /* # of sectors per page */
// 	int pgsz; /* mapping unit size in bytes*/
// 	int pgs_per_flashpg; /* # of pgs per flash page */
// 	int flashpgs_per_blk; /* # of flash pages per block */
// 	int pgs_per_oneshotpg; /* # of pgs per oneshot page */
// 	int oneshotpgs_per_blk; /* # of oneshot pages per block */
// 	int pgs_per_blk; /* # of pages per block */
// 	int blks_per_pl; /* # of blocks per plane */
// 	int pls_per_lun; /* # of planes per LUN (Die) */
// 	int luns_per_ch; /* # of LUNs per channel */
// 	int nchs; /* # of channels in the SSD */
// 	int cell_mode;

// 	/* Unit size of NVMe write command
//        Transfer size should be multiple of it */
// 	int write_unit_size;
// 	bool write_early_completion;

// 	int pg_4kb_rd_lat
// 		[MAX_CELL_TYPES]; /* NAND page 4KB read latency in nanoseconds. sensing time (half tR) */
// 	int pg_rd_lat[MAX_CELL_TYPES]; /* NAND page read latency in nanoseconds. sensing time (tR) */
// 	int pg_wr_lat; /* NAND page program latency in nanoseconds. pgm time (tPROG)*/
// 	int blk_er_lat; /* NAND block erase latency in nanoseconds. erase time (tERASE) */
// 	int max_ch_xfer_size;

// 	int fw_4kb_rd_lat; /* Firmware overhead of 4KB read of read in nanoseconds */
// 	int fw_rd_lat; /* Firmware overhead of read of read in nanoseconds */
// 	int fw_wbuf_lat0; /* Firmware overhead0 of write buffer in nanoseconds */
// 	int fw_wbuf_lat1; /* Firmware overhead1 of write buffer in nanoseconds */
// 	int fw_ch_xfer_lat; /* Firmware overhead of nand channel data transfer(4KB) in nanoseconds */

// 	uint64_t ch_bandwidth; /*NAND CH Maximum bandwidth in MiB/s*/
// 	uint64_t pcie_bandwidth; /*PCIE Maximum bandwidth in MiB/s*/

// 	/* below are all calculated values */
// 	unsigned long secs_per_blk; /* # of sectors per block */
// 	unsigned long secs_per_pl; /* # of sectors per plane */
// 	unsigned long secs_per_lun; /* # of sectors per LUN */
// 	unsigned long secs_per_ch; /* # of sectors per channel */
// 	unsigned long tt_secs; /* # of sectors in the SSD */

// 	unsigned long pgs_per_pl; /* # of pages per plane */
// 	unsigned long pgs_per_lun; /* # of pages per LUN (Die) */
// 	unsigned long pgs_per_ch; /* # of pages per channel */
// 	unsigned long tt_pgs; /* total # of pages in the SSD */

// 	unsigned long blks_per_lun; /* # of blocks per LUN */
// 	unsigned long blks_per_ch; /* # of blocks per channel */
// 	unsigned long tt_blks; /* total # of blocks in the SSD */

// 	unsigned long secs_per_line;
// 	unsigned long pgs_per_line;
// 	unsigned long blks_per_line;
// 	unsigned long tt_lines;

// 	unsigned long pls_per_ch; /* # of planes per channel */
// 	unsigned long tt_pls; /* total # of planes in the SSD */

// 	unsigned long tt_luns; /* total # of LUNs in the SSD */

// 	unsigned long long write_buffer_size;
// };


//SSD实例，本项目中仿真的ssd： SSD主要参数，通道参数，PCIE 参数，写缓冲区
struct ssd {
	struct ssdparams sp;//SSD参数
	struct ssd_channel *ch;// SSD 的逻辑结构 ch->lun->pl->blk->pg->sec
	struct ssd_pcie *pcie;//  PCIe 实例
	struct buffer *write_buffer;
	unsigned int cpu_nr_dispatcher;
};

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
	return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
	struct ssd_channel *ch = get_ch(ssd, ppa);
	return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
	struct nand_lun *lun = get_lun(ssd, ppa);
	return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
	struct nand_plane *pl = get_pl(ssd, ppa);
	return &(pl->blk[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
	struct nand_block *blk = get_blk(ssd, ppa);
	return &(blk->pg[ppa->g.pg]);
}

static inline uint32_t get_cell(struct ssd *ssd, struct ppa *ppa)
{
	struct ssdparams *spp = &ssd->sp;
	return (ppa->g.pg / spp->pgs_per_flashpg) % (spp->cell_mode);
}

void ssd_init_params(struct ssdparams *spp, uint64_t capacity, uint32_t nparts);
void ssd_init(struct ssd *ssd, struct ssdparams *spp, uint32_t cpu_nr_dispatcher);
void ssd_remove(struct ssd *ssd);

uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd);
uint64_t ssd_advance_pcie(struct ssd *ssd, uint64_t request_time, uint64_t length);
uint64_t ssd_advance_write_buffer(struct ssd *ssd, uint64_t request_time, uint64_t length);
uint64_t ssd_next_idle_time(struct ssd *ssd);

void buffer_init(struct buffer *buf, size_t size);
uint32_t buffer_allocate(struct buffer *buf, size_t size);
bool buffer_release(struct buffer *buf, size_t size);
void buffer_refill(struct buffer *buf);

void adjust_ftl_latency(int target, int lat);
#endif
