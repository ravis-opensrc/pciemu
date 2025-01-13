<div align="center">

<h1>pciemu</h1>

![License](https://img.shields.io/github/license/luizinhosuraty/pciemu)
![Build](https://img.shields.io/github/actions/workflow/status/luizinhosuraty/pciemu/linux-ci.yml?label=tests)
 
</div>

pciemu provides an example of PCIe Device Emulation in QEMU.

The idea is to help those willing to explore PCIe devices but do not have access
to a real hardware. Or maybe someone with research ideas for a new PCIe device
or capability who want to easily test those ideas.

## Setup and Compiling QEMU

The [setup script](setup.sh) will modify build files and configure QEMU to
properly compile the pciemu device along with all other QEMU files. Thus,
once all preparation is finished, run the following:

```bash
$ git submodule update --init --remote --merge
$ ./setup.sh
```

Note here for those running locally: though normally not necessary, you may
need to change the arguments of the ```./configure``` command inside the
[setup script](setup.sh) to better translate them to your system requirements.

## Running

Once all dependencies are installed and QEMU is properly compiled, all you have
to do is run QEMU

### Inside the VM

Later, it's all standard kernel module compilation and insertion into the kernel:

```bash
$ cd src/sw/kernel/
$ make
$ sudo insmod pcie-bar-hmem.ko
```
Don't forget that ```lspci``` is your friend when it comes to PCIe devices.
