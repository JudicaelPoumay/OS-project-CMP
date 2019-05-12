# The ext4 File System

This ext4 is from the Linux kernel 4.4.0 which is used for Ubuntu 16.04.
To try this, no kernel update is necessary if your OS is Ubuntu 16.04.

The names of files and functions are replaced with ext42 in order to avoid name space conflict.
The unmodified version is found at the fist commit log and the other modifications are also included in the commit logs.

# How to use

The following instruction shows how to use this ext4 file system kernel module
with a disk named ```/dev/sdb``` whose mount point is ```/mnt```.

If you are using a VM for testing, you should be able to add a new disk from the VM console.

## Step1: Prepare a Disk

CAUTION!!!: This operation may delete your important data!!! Please perform the command carefully.
The target disk ( in this case ```/dev/sdb``` ) must not contain any data you need.
After you type the following command, all data on the disk will be lost.

``mkfs``` command initializes a disk.

```
$ sudo mkfs.ext4 /dev/sdb
```

## Step2: Compile the Kernel Module

Please type ```make``` in this directory.

```
$ make
```

After the compilation finishes, we will have a file named ```ext42.ko```.

## Step3: Installing the Kernel Module

We can use the ```insmod``` command to install the module.

```
$ sudo insmod ext42.ko
```

## Step4: Mount the ext4 File System

```mount``` command allows us to mount a disk on a specific
mount point ( ```/mnt``` directory in this example ).

```
$ sudo mount -t ext42 /dev/sdb /mnt
```

Now the installation is completed.
All the file operations under the directory under ```/mnt``` are performed by this kernel module.

## Unload the Kernel Module

When we want to update the program, we need to reload the kernel module.
Initially we need to unload the existing kernel module.
The following commands will uninstall the currently installed module.

```
$ sudo umount /mnt
$ sudo rmmod ext42
```
