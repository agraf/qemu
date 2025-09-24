#!/bin/bash

set -ex

WIN11IMG=~/Documents/VMs/windows/win11.qcow2

if [ ! -f tpm.sock ]; then
	swtpm socket --ctrl type=unixio,path=tpm.sock,terminate --tpmstate backend-uri=file://tpm.data --tpm2 &
	sleep 1
fi

~/git/qemu/build/qemu-system-aarch64				\
	-M virt,accel=hvf					\
	-device ramfb						\
	-bios build/pc-bios/edk2-aarch64-code.fd		\
	-m 4G							\
	-smp 2							\
	-cpu host \
	-drive file=$WIN11IMG,format=qcow2,if=none,id=d,cache=writeback \
	-device nvme,drive=d,serial=1234			\
	-device qemu-xhci					\
	-device usb-tablet					\
	-device usb-kbd						\
	-net none						\
	-device virtio-net-pci,netdev=n				\
	-netdev user,id=n,hostfwd=tcp:127.0.0.1:3387-:3389	\
	-serial mon:stdio					\
	-chardev socket,id=chrtpm0,path=tpm.sock		\
	-tpmdev emulator,id=tpm0,chardev=chrtpm0		\
	-device tpm-crb-device,tpmdev=tpm0			\
	"$@"

