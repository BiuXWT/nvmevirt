# NVMeVirt

## Introduction

NVMeVirt is a versatile software-defined virtual NVMe device. It is implemented as a Linux kernel module providing the system with a virtual NVMe device of various kinds. Currently, NVMeVirt supports conventional SSDs, NVM SSDs, ZNS SSDs, etc. The device is emulated at the PCI layer, presenting a native NVMe device to the entire system. Thus, NVMeVirt has the capability not only to function as a standard storage device, but also to be utilized in advanced storage configurations, such as NVMe-oF target offloading, kernel bypassing, and PCI peer-to-peer communication.

Further details on the design and implementation of NVMeVirt can be found in the following papers.
- [NVMeVirt: A Versatile Software-defined Virtual NVMe Device (FAST 2023)](https://www.usenix.org/conference/fast23/presentation/kim-sang-hoon)
- [Empowering Storage Systems Research with NVMeVirt: A Comprehensive NVMe Device Emulator (Transactions on Storage 2023)](https://dl.acm.org/doi/full/10.1145/3625006)

Please feel free to contact us at [nvmevirt@gmail.com](mailto:nvmevirt@gmail.com) if you have any questions or suggestions. Also you can raise an issue anytime for bug reports or discussions.

We encourage you to cite our paper at FAST 2023 as follows:
```
@InProceedings{NVMeVirt:FAST23,
  author = {Sang-Hoon Kim and Jaehoon Shim and Euidong Lee and Seongyeop Jeong and Ilkueon Kang and Jin-Soo Kim},
  title = {{NVMeVirt}: A Versatile Software-defined Virtual {NVMe} Device},
  booktitle = {Proceedings of the 21st USENIX Conference on File and Storage Technologies (USENIX FAST)},
  address = {Santa Clara, CA},
  month = {February},
  year = {2023},
}
```


## Installation

### Linux kernel requirement

The recommended Linux kernel version is v5.15.x and higher (tested on Linux vanilla kernel v5.15.37 and Ubuntu kernel v5.15.0-58-generic).

### Reserving physical memory

A part of the main memory should be reserved for the storage of the emulated NVMe device. To reserve a chunk of physical memory, add the following option to `GRUB_CMDLINE_LINUX` in `/etc/default/grub` as follows:

```bash
GRUB_CMDLINE_LINUX="memmap=64G\\\$128G"
```
从128G的位置开始保留64G的内存用作虚拟设备存储

This example will reserve 64GiB of physical memory chunk (out of the total 192GiB physical memory) starting from the 128GiB memory offset. You may need to adjust those values depending on the available physical memory size and the desired storage capacity.

After changing the `/etc/default/grub` file, you are required to run the following commands to update `grub` and reboot your system.

```bash
$ sudo update-grub
$ sudo reboot
```

### Compiling `nvmevirt`

Please download the latest version of `nvmevirt` from Github:

```bash
$ git clone https://github.com/snu-csl/nvmevirt
```

`nvmevirt` is implemented as a Linux kernel module. Thus, the kernel headers should be installed in the `/lib/modules/$(shell uname -r)` directory to compile `nvmevirt`.

Currently, you need to select the target device type by manually editing the `Kbuild`. You may find the following lines in the `Kbuild`, which imply that NVMeVirt is currently configured for emulating NVM(Non-Volatile Memory) SSD (such as Intel Optane SSD). You may uncomment other one to change the target device type. Note that you can select one device type at a time.

```Makefile
# Select one of the targets to build
CONFIG_NVMEVIRT_NVM := y
#CONFIG_NVMEVIRT_SSD := y
#CONFIG_NVMEVIRT_ZNS := y
#CONFIG_NVMEVIRT_KV := y
```

You may find the detailed configuration parameters for conventional SSD and ZNS SSD from `ssd_config.h`.

Build the kernel module by running the `make` command in the `nvmevirt` source directory.
```bash
$ make
make -C /lib/modules/5.15.37/build M=/path/to/nvmev modules
make[1]: Entering directory '/path/to/linux-5.15.37'
  CC [M]  /path/to/nvmev/main.o
  CC [M]  /path/to/nvmev/pci.o
  CC [M]  /path/to/nvmev/admin.o
  CC [M]  /path/to/nvmev/io.o
  CC [M]  /path/to/nvmev/dma.o
  CC [M]  /path/to/nvmev/simple_ftl.o
  LD [M]  /path/to/nvmev/nvmev.o
  MODPOST /path/to/nvmev/Module.symvers
  CC [M]  /path/to/nvmev/nvmev.mod.o
  LD [M]  /path/to/nvmev/nvmev.ko
  BTF [M] /path/to/nvmev/nvmev.ko
make[1]: Leaving directory '/path/to/linux-5.15.37'
$
```

### Using `nvmevirt`

`nvmevirt` is configured to emulate the NVM SSD by default. You can attach an emulated NVM SSD in your system by loading the `nvmevirt` kernel module as follows:

```bash
$ sudo insmod ./nvmev.ko \
  memmap_start=128G \       # e.g., 1M, 4G, 8T
  memmap_size=64G   \       # e.g., 1M, 4G, 8T
  cpus=7,8                  # List of CPU cores to process I/O requests (should have at least 2)
```

In the above example, `memmap_start` and `memmap_size` indicate the relative offset and the size of the reserved memory, respectively. Those values should match the configurations specified in the `/etc/default/grub` file shown earlier. In addition, the `cpus` option specifies the id of cores on which I/O dispatcher and I/O worker threads run. You have to specify at least two cores for this purpose: one for the I/O dispatcher thread, and one or more cores for the I/O worker thread(s).

It is highly recommended to use the `isolcpus` Linux command-line configuration to avoid schedulers putting tasks on the CPUs that NVMeVirt uses:

```bash
GRUB_CMDLINE_LINUX="memmap=64G\\\$128G isolcpus=7,8"
```

When you are successfully load the `nvmevirt` module, you can see something like these from the system message.

```log
$ sudo dmesg
[  144.812917] nvme nvme0: pci function 0001:10:00.0
[  144.812975] NVMeVirt: Successfully created virtual PCI bus (node 1)
[  144.813911] NVMeVirt: nvmev_proc_io_0 started on cpu 7 (node 1)
[  144.813972] NVMeVirt: Successfully created Virtual NVMe device
[  144.814032] NVMeVirt: nvmev_dispatcher started on cpu 8 (node 1)
[  144.822075] nvme nvme0: 48/0/0 default/read/poll queues
```

If you encounter a kernel panic in `__pci_enable_msix()` or in `nvme_hwmon_init()` during `insmod`, it is because the current implementation of `nvmevirt` is not compatible with IOMMU. In this case, you can either turn off Intel VT-d or IOMMU in BIOS, or disable the interrupt remapping using the grub option as shown below:

```bash
GRUB_CMDLINE_LINUX="memmap=64G\\\$128G intremap=off"
```

Now the emulated `nvmevirt` device is ready to be used as shown below. The actual device number (`/dev/nvme0`) can vary depending on the number of real NVMe devices in your system.


```bash
$ ls -l /dev/nvme*
crw------- 1 root root 242, 0 Feb 22 14:13 /dev/nvme0
brw-rw---- 1 root disk 259, 5 Feb 22 14:13 /dev/nvme0n1
```

## Contributing
When contributing to this repository, please first discuss the change you wish to make via [issues](https://github.com/snu-csl/nvmevirt/issues) or email(nvmevirt@gmail.com) before making a change.

### Pull Requests
1. Create a personal fork of the project on Github.
2. Clone the fork on your local machine.
3. Implement/fix your feature, comment your code.
4. Follow the code style of this project, including indentation.
5. Run tests using [nvmev-evaluation](https://github.com/snu-csl/nvmev-evaluation).
6. From your fork open a pull request in our `main` branch!
7. Please wait for the maintainer's review.


## License

NVMeVirt is offered under the terms of the GNU General Public License version 2 as published by the Free Software Foundation. More information about this license can be found [here](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).

Priority queue implementation [`pqueue/`](pqueue/) is offered under the terms of the BSD 2-clause license (GPL-compatible). (Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>. All rights reserved.)


## testing

### configs
修改编辑/etc/default/grub
```bash
GRUB_CMDLINE_LINUX="memmap=4G\\\6G isolcpus=2,3"

#生效grub
update-grub
#重启
reboot
```

从6G的位置开始保留4G的空间

### 运行NVMeVirt
```bash
sudo insmod ./nvmev.ko memmap_start=6G memmap_size=4G cpus=2,3
```

查看kernel logs
```log
[  326.781688] nvmev: loading out-of-tree module taints kernel.
[  326.781705] nvmev: module verification failed: signature and/or required key missing - tainting kernel
[  326.786150] NVMeVirt: Version 1.10 for >> Samsung 970 Pro SSD <<
[  326.786163] NVMeVirt: Storage: 0x180100000-0x280000000 (4095 MiB)
[  326.796294] NVMeVirt: Total Capacity(GiB,MiB)=1,1024 chs=2 luns=4 lines=8192 blk-size(MiB,KiB)=0,32 line-size(MiB,KiB)=0,128
[  326.808814] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.825516] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.825567] NVMeVirt: [chmodel_init] bandwidth 3360 max_credits 110 tx_time 36
[  326.827359] NVMeVirt: pqueue: Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>. All rights reserved.
[  326.827447] NVMeVirt: Init FTL instance with 2 channels (262144 pages)
[  326.837045] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.851049] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.851070] NVMeVirt: [chmodel_init] bandwidth 3360 max_credits 110 tx_time 36
[  326.852586] NVMeVirt: Init FTL instance with 2 channels (262144 pages)
[  326.862741] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.872369] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.872391] NVMeVirt: [chmodel_init] bandwidth 3360 max_credits 110 tx_time 36
[  326.873937] NVMeVirt: Init FTL instance with 2 channels (262144 pages)
[  326.883591] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.892983] NVMeVirt: [chmodel_init] bandwidth 800 max_credits 26 tx_time 152
[  326.893025] NVMeVirt: [chmodel_init] bandwidth 3360 max_credits 110 tx_time 36
[  326.897986] NVMeVirt: Init FTL instance with 2 channels (262144 pages)
[  326.898022] NVMeVirt: FTL physical space: 4293918720, logical space: 4013008149 (physical/logical * 100 = 107)
[  326.898024] NVMeVirt: ns 0/1: size 3827 MiB
[  326.899118] PCI host bridge to bus 0001:10
[  326.899128] pci_bus 0001:10: root bus resource [io  0x0000-0xffff]
[  326.899131] pci_bus 0001:10: root bus resource [mem 0x00000000-0x1fffffffffff]
[  326.899135] pci_bus 0001:10: root bus resource [bus 00-ff]
[  326.899153] pci 0001:10:00.0: [0c51:0110] type 00 class 0x010802 PCIe Endpoint
[  326.899159] pci 0001:10:00.0: BAR 0 [mem 0x180000000-0x180003fff 64bit]
[  326.899163] pci 0001:10:00.0: enabling Extended Tags
[  326.911778] NVMeVirt: Virtual PCI bus created (node 0)
[  326.913593] NVMeVirt: nvmev_io_worker_0 started on cpu 3 (node 0)
[  326.915738] NVMeVirt: nvmev_dispatcher started on cpu 2 (node 0)
[  326.916158] nvme nvme1: pci function 0001:10:00.0
[  326.925504] nvme nvme1: 72/0/0 default/read/poll queues
[  326.942828] NVMeVirt: Virtual NVMe device created
```

创建成功

## 代码分析
初始化NVMeV虚拟设备

```mermaid
flowchart TD
    A[开始] --> B[打印基础配置]
    B --> C[初始化虚拟设备]
    C -->|失败| R1[返回 -EINVAL]
    C --> D[加载配置信息]
    D -->|失败| R2[跳转到错误处理]
    D --> E[初始化存储]
    E --> F[初始化命名空间]
    F --> G{是否使用 DMA}
    G -->|是| H[设置 DMA 引擎]
    H -->|失败| I[回退到 memcpy]
    G -->|否| J[初始化 PCI]
    H --> J
    I --> J
    J -->|失败| R3[跳转到错误处理]
    J --> K[打印性能配置]
    K --> L[初始化 I/O 工作者]
    L --> M[初始化调度器]
    M --> N[添加 PCI 总线设备]
    N --> O[返回 0]
    R2 --> P[释放资源]
    R3 --> P
    P --> Q[返回 -EIO]
```


`struct nvmev_dev *VDEV_INIT(void)`函数的流程图如下：
```mermaid
flowchart TD
    A[开始] --> B[分配 nvmev_dev 结构体]
    B --> C[分配一页大小的虚拟设备内存]
    C --> D[设置 PCI 配置空间和扩展功能指针]
    D --> E[初始化管理队列为 NULL]
    E --> F[返回初始化后的设备结构体]
```
`static bool __load_configs(struct nvmev_config *config)`函数的流程图如下
```mermaid
flowchart TD
    A[开始] --> B{验证配置}
    B -->|失败| F[返回 false]
    B -->|成功| C[调整内存映射大小]
    C --> D[设置存储空间参数]
    D --> E[初始化读写参数]
    E --> G[解析CPU列表]
    G --> H{是否为第一个CPU}
    H -->|是| I[设置调度器CPU编号]
    H -->|否| J[设置工作线程CPU编号]
    J --> K[增加工作线程计数]
    K --> L{是否有更多CPU}
    L -->|是| G
    L -->|否| M[返回 true]

```
初始化一个NVMe虚拟设备的存储模块
```mermaid
flowchart TD
    A([开始]) --> B[打印存储配置信息]
    B --> C[分配IO单元统计内存]
    C --> D{映射存储内存是否成功}
    D -->|Yes| E[创建proc文件系统节点]
    D -->|No| F[记录映射失败错误]
    E --> G([结束])
    F --> G
```

初始化NVMe虚拟设备命名空间 `NVMeV_init` :
```mermaid
flowchart TD
    A[开始] --> B{遍历命名空间}
    B -->|是| C[计算命名空间大小]
    C --> D{判断SSD类型}
    D -->|NVM| E[初始化NVM命名空间]
    D -->|CONV| F[初始化CONV命名空间]
    D -->|ZNS| G[初始化ZNS命名空间]
    D -->|KV| H[初始化KV命名空间]
    D -->|未知类型| I[错误处理]
    E --> J[更新剩余容量和地址]
    F --> J
    G --> J
    H --> J
    I --> J
    J --> K[记录命名空间信息]
    K --> L[设置设备命名空间信息]
    L --> M[结束]
```

初始化conv命名空间 `conv_init_namespace` :
```mermaid
flowchart TD
    A[开始] --> B[初始化 SSD 和转换参数]
    B --> C[分配并初始化多个 SSD 分区及其 FTL]
    C --> D[共享 PCIe 和写缓冲区]
    D --> E[设置命名空间属性]
    E --> F[注册 I/O 命令处理函数]
    F --> G[结束]
```

初始化SSD参数 `ssd_init_params` ：
- 设置基本参数如扇区大小、页面大小等。
- 根据通道数和分区数调整通道数和容量。
- 计算块、平面、LUN 和通道的数量及大小。
- 设置读写延迟和其他性能参数。
- 计算总容量和各层级的大小，并输出相关信息。
```mermaid
flowchart TD
    A[开始] --> B[初始化基本参数]
    B --> C{BLKS_PER_PLN 是否大于0}
    C -->|Yes| D[根据容量计算块大小]
    C -->|No| E[使用固定块大小]
    D --> F[计算其他参数]
    E --> F
    F --> G[设置读写延迟等性能参数]
    G --> H[计算总容量和各层级大小]
    H --> I[输出信息]
    I --> J[结束]
```
PCIe 写缓存
```mermaid
flowchart TD
    A[开始] --> B{初始化 i=1}
    B --> C{是否 i < nr_parts?}
    C -->|Yes| D[释放 perf_model 内存]
    D --> E[释放 pcie 内存]
    E --> F[释放 write_buffer 内存]
    F --> G[设置 pcie 指针]
    G --> H[设置 write_buffer 指针]
    H --> I{i++}
    I --> C
    C -->|No| J[结束]
```


初始化虚拟NVMe设备的IO工作线程
```mermaid
flowchart TD
    A[开始] --> B[分配并初始化IO工作线程数组]
    B --> C{遍历每个工作线程}
    C -->|是| D[初始化当前工作线程的工作队列]
    D --> E[设置工作队列的链表结构]
    E --> F[设置工作线程的初始状态]
    F --> G[创建并启动工作线程]
    G --> H[绑定工作线程到指定CPU核心]
    H --> I[唤醒工作线程]
    I --> J{继续下一个工作线程?}
    J -->|是| C
    J -->|否| K[结束]
```
IO工作线程：
```mermaid
flowchart TD
    A[初始化并打印线程信息] --> B{是否停止线程?}
    B -->|No| C[获取当前时间戳]
    C --> D[获取当前I/O序列号]
    D --> E{当前I/O序列号是否为-1?}
    E -->|No| F[获取I/O工作项]
    F --> G{是否已完成?}
    G -->|Yes| H[继续下一个I/O工作项]
    G -->|No| I{是否已复制?}
    I -->|No| J[执行I/O操作]
    J --> K[标记为已复制]
    K --> L{是否到达目标时间?}
    L -->|Yes| M[处理完成的I/O请求]
    M --> N[继续下一个I/O工作项]
    E -->|Yes| O[检查并处理中断队列]
    O --> P{是否有空闲超时配置?}
    P -->|Yes| Q[检查是否超时]
    Q -->|是| R[休眠]
    Q -->|否| S[重新调度]
    P -->|No| S
    B -->|Yes| T[退出线程]
```

```mermaid 
flowchart TD
    A[开始遍历设备] --> B{获取设备资源}
    B --> C[设置资源父级]
    C --> D[初始化设备中断和配置]
    D --> E[初始化NVMe控制器寄存器]
    E --> F[分配并复制旧门铃内存]
    F --> G[分配并复制旧BAR内存]
    G --> H[内存映射MSI-X表并清零]
    H --> I[继续遍历下一个设备]
```

```plantuml
@startuml
class pci_pm_cap {
  - struct pid
  - struct pc
  - struct pmcs
  - u8[2] ext
}

class pid {
  - u8 cid
  - u8 next
}

class pc {
  - u16 vs : 3
  - u16 pmec : 1
  - u16 resv : 1
  - u16 dsi : 1
  - u16 auxc : 3
  - u16 d1s : 1
  - u16 d2s : 1
  - u16 psup : 5
}

class pmcs {
  - u16 ps : 2
  - u16 rsvd01 : 1
  - u16 nsfrst : 1
  - u16 rsvd02 : 4
  - u16 pmee : 1
  - u16 dse : 4
  - u16 dsc : 2
  - u16 pmes : 1
}

pci_pm_cap "1" *-- "1" pid
pci_pm_cap "1" *-- "1" pc
pci_pm_cap "1" *-- "1" pmcs
@enduml
```