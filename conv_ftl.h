// SPDX-License-Identifier: GPL-2.0-only
// CONV FTL: 传统闪存转换层, 一种传统的经典的FTL实现方式. 区别于其他更复杂或优化过的FTL设计(如:分区FTL, 混合FTL)
#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

// 页映射

/*
victim line的概念: 
	line: line通常是一个逻辑概念,描述一组相关的块, 作用是简化垃圾回收和磨损均衡. 通过将多个块组成一个line, FTL可以高效的管理数据的分布和移动.
	定义：逻辑管理单元，通常指跨多个Plane或Die的Block集合（称为Super Block）。
	作用：提升并行性，例如同时擦除或写入多个Block。
	关系：Line由控制器动态组合，用于优化性能和垃圾回收。
	当系统需要新的空闲行来写入数据时，如果当前没有足够的空闲行，就需要选择一些行进行垃圾回收。这些被选中的行就称为Victim Line。
	
选择Victim Line的策略
	论文说明:FTL 在运行时动态确定数据放置，并在可用块数量低于阈值时执行垃圾回收。目前，FTL 根据贪婪策略选择牺牲块。
	优先队列（Priority Queue）：victim_line_pq 是一个优先队列，用于管理候选的Victim Line。优先队列根据一定的规则（如有效页面数量、最近最少使用等）对行进行排序，选择最优的行作为Victim Line。
	有效页面和无效页面：每条行记录了其有效页面数（vpc）和无效页面数（ipc）。优先选择有效页面较少的行作为Victim Line，因为这样的行在垃圾回收时需要复制的有效页面较少，效率更高。
*/
//垃圾回收参数
struct convparams {
	uint32_t gc_thres_lines;//垃圾回收线数量(常规) 线:指的是存储设备中的物理单元，每条线可以包含多个页面. 当空闲线的数量减少到2条或以下时，系统将触发垃圾回收操作，以确保有足够的空闲线供主机写入和垃圾回收使用。
	uint32_t gc_thres_lines_high;//垃圾回收线数量(高优先级)线:指的是存储设备中的物理单元，每条线可以包含多个页面. 当空闲线的数量减少到2条或以下时，系统将触发垃圾回收操作，以确保有足够的空闲线供主机写入和垃圾回收使用。
	bool enable_gc_delay;// 启用垃圾回收延迟

	double op_area_pcent;//预留空间百分比
	int pba_pcent; /*物理空间与逻辑空间的比例(百分比): (physical space / logical space) * 100*/
};

struct line {
	int id; /*行的标识符，与对应的块标识符相同。 line id, the same as corresponding block id */
	int ipc; /*该行中无效页面的数量。 invalid page count in this line */
	int vpc; /*该行中有效页面的数量。 valid page count in this line */
	struct list_head entry;//用于将该行插入到链表中的节点。
	/* position in the priority queue for victim lines */
	size_t pos;//该行在优先队列中的位置，用于选择牺牲行。
};

/*记录下一个写入地址 wp: record next write addr */
struct write_pointer {
	struct line *curline;// 当前行指针
	uint32_t ch;// 通道号
	uint32_t lun;// Die
	uint32_t pl;// Plane
	uint32_t blk;// Block
	uint32_t pg;// Page
};

/* 行管理 */
struct line_mgmt {
	struct line *lines;// 行数组

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;// 空闲行链表
	pqueue_t *victim_line_pq;//  牺牲行优先队列, 这个队列如何管理?
	struct list_head full_line_list;// 满行链表

	uint32_t tt_lines;// 总行数
	uint32_t free_line_cnt;// 空闲行数量
	uint32_t victim_line_cnt;// 牺牲行数量
	uint32_t full_line_cnt;// 满行数量
};

/* 写流量控制 */
struct write_flow_control {
	uint32_t write_credits;// 写信用额度
	uint32_t credits_to_refill; // 需要补充的信用额度
};

struct conv_ftl {
	struct ssd *ssd;// 指向SSD的指针

	struct convparams cp;// 垃圾回收参数
	struct ppa *maptbl; /*页级映射表(逻辑地址到物理地址的映射关系) page level mapping table */
	uint64_t *rmap; /*反向映射表(从物理地址反向查找对应的逻辑地址)，假设存储在OOB（带外数据区） reverse mapptbl, assume it's stored in OOB */
	struct write_pointer wp;// 写指针
	struct write_pointer gc_wp;// 垃圾回收写指针
	struct line_mgmt lm;// 行管理结构
	struct write_flow_control wfc;// 写流量控制结构
};
/*
带外存储器是指NAND闪存中除了主数据区域之外的一小部分额外存储空间。
这部分空间通常用于存储元数据、错误校正码（ECC）、wear leveling信息等。
*/

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void conv_remove_namespace(struct nvmev_ns *ns);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
