sudo rmmod nvmev

sudo dmesg -c ;sudo insmod ./nvmev.ko memmap_start=6G memmap_size=4G cpus=2,3 ; sudo rmmod nvmev; sudo dmesg> dmesg

sudo insmod ./nvmev.ko memmap_start=6G memmap_size=4G cpus=2,3

