% Full disk encryption, including `/boot`: Unlocking LUKS devices from GRUB

Introduction
============

So called “full disk encryption” is often a misnomer, because there is
typically a separate plaintext partition holding `/boot`.  For instance
the Debian Installer does this in its “encrypted LVM” partitioning method.
Since not all bootloaders are able to unlock LUKS devices, a plaintext
`/boot` is the only solution that works for all of them.

However, GRUB2 is (since Jessie) able to unlock LUKS devices with its
[`cryptomount`](https://www.gnu.org/software/grub/manual/grub/html_node/cryptomount.html)
command, which therefore enables encryption of the `/boot` partition as
well: using that feature reduces the amount of plaintext data written to
disk.  It is especially interesting when GRUB is installed to a read-only
media, for instance as [coreboot payload](https://doc.coreboot.org/payloads.html#grub2)
flashed to a write-protected chip.  On the other hand, it is *incompatible*
with some other features that only enabled later at initramfs stage, such
as slash screens or remote unlocking.

Since enabling unlocking LUKS devices from GRUB [isn't exposed to the d-i
interface](https://bugs.debian.org/814798) (as of Buster), people have
come up with various custom workarounds.  But as of Buster [`cryptsetup`(8)]
defaults to a new [LUKS header format version](https://gitlab.com/cryptsetup/LUKS2-docs),
which isn't supported by GRUB as of 2.04.  **Hence the pre-Buster
workarounds won't work anymore**.  Until LUKS *version 2* support is
[added to GRUB2](https://savannah.gnu.org/bugs/?55093), the device(s)
holding `/boot` needs to be in *LUKS format version 1* to be unlocked from
the boot loader.

This document describes a generic way to unlock LUKS devices from GRUB
for Debian Buster.


Encrypting the device holding `/boot`
=====================================

There are two alternatives here:

  * Either format an existing `/boot` partition to LUKS1; or
  * Move `/boot` to the root file system.  The root device(s) needs to
    use LUKS version 1, but existing LUKS2 devices can be *converted*
    (in-place) to LUKS1.

These two alternatives are described in the two following sub-sections.


Formatting the existing `/boot` partition to LUKS1
--------------------------------------------------

Since the installer creates a separate (plaintext) `/boot` partition by
default in its “encrypted LVM” partitioning method, the simplest
solution is arguably to re-format it as LUKS1, especially if the root
device is in LUKS2 format.

That way other partitions, including the one holding the root file
system, can remain in LUKS2 format and benefit from the *stronger
security guaranties* and *convenience features* of the newer version:
more secure (memory-hard) Key Derivation Function, backup header,
ability to offload the volume key to the kernel keyring (thus preventing
access from userspace), custom sector size, persistent flags, unattended
unlocking via kernel keyring tokens, etc.

Furthermore every command in this sub-section can be run from the main
system: no need to reboot into a live CD or an initramfs shell.

 1. Before copying content of the `/boot` directory, remount it read-only
    to make sure data is not modified while it's being copied.

        root@debian:~$ mount -oremount,ro /boot

 2. Archive the directory elsewhere (on another device), and unmount it
    afterwards.

        root@debian:~$ install -m0600 /dev/null /tmp/boot.tar
    <!-- -->
        root@debian:~$ tar -C /boot --acls --xattrs -cf /tmp/boot.tar .
    <!-- -->
        root@debian:~$ umount /boot

 3. Optionally, wipe out the underlying block device (assumed to be
    `/dev/sda1` in the rest of this sub-section).

        root@debian:~$ dd if=/dev/urandom of=/dev/sda1 bs=1M
        dd: error writing '/dev/sda1': No space left on device
        244+0 records in
        243+0 records out
        254803968 bytes (255 MB, 243 MiB) copied, 1.70967 s, 149 MB/s

 4. Format the underlying block device to LUKS1.  (Note the `--type luks1`
    in the command below, as Buster's [`cryptsetup`(8)] defaults to LUKS
    version 2 for `luksFormat`.)

        root@debian:~$ cryptsetup luksFormat --type luks1 /dev/sda1

        WARNING!
        ========
        This will overwrite data on /dev/sda1 irrevocably.

        Are you sure? (Type uppercase yes): YES
        Enter passphrase for /dev/sda1:
        Verify passphrase:

 5. Add a corresponding entry to [`crypttab`(5)] with mapped device name
    `boot_crypt`, and open it afterwards.

        root@debian:~$ uuid="$(blkid -o value -s UUID /dev/sda1)"
    <!-- -->
        root@debian:~$ echo "boot_crypt UUID=$uuid none luks" | tee -a /etc/crypttab
    <!-- -->
        root@debian:~$ cryptdisks_start boot_crypt
        Starting crypto disk...boot_crypt (starting)...
        Please unlock disk boot_crypt:  ********
        boot_crypt (started)...done.

 6. Create a file system on the mapped device.  Assuming source device for
    `/boot` is specified by its UUID in the [`fstab`(5)] -- which the
    Debian Installer does by default -- reusing the old UUID avoids
    editing the file.

        root@debian:~$ grep /boot /etc/fstab
        # /boot was on /dev/sda1 during installation
        UUID=c104749f-a0fa-406c-9e9a-3fc01f8e2f78 /boot           ext2    defaults        0       2
    <!-- -->
        root@debian:~$ mkfs.ext2 -m0 -U c104749f-a0fa-406c-9e9a-3fc01f8e2f78 /dev/mapper/boot_crypt
        mke2fs 1.44.5 (15-Dec-2018)
        Creating filesystem with 246784 1k blocks and 61752 inodes
        Filesystem UUID: c104749f-a0fa-406c-9e9a-3fc01f8e2f78
        […]

 7. Finally, mount `/boot` again from [`fstab`(5)], and copy the saved
    tarball to the new (and now encrypted) file system.

        root@debian:~$ mount -v /boot
        mount: /dev/mapper/boot_crypt mounted on /boot.
    <!-- -->
        root@debian:~$ tar -C /boot --acls --xattrs -xf /tmp/boot.tar

You can skip the next sub-section and go directly to [Enabling
`cryptomount` in GRUB2].  Note that `init`(1) needs to unlock the
`/boot` partition *again* during the boot process.  See [Avoiding the
extra password prompt] for details and a proposed workaround.  (Only
steps 1-3 from that section are relevant here; no need to copy the key
file to the initramfs image since `/boot` can be unlocked and mounted
later during the boot process.)


Moving `/boot` to the root file system
--------------------------------------

The [previous sub-section][Formatting the existing `/boot` partition to LUKS1]
described how to to re-format the `/boot` partition as LUKS1.
Alternatively, it can be moved to the root file system, assuming the
latter is not held by any LUKS2 device.  (As shown below, LUKS2 devices
created with default parameters can be “downgraded” to LUKS1.)

The advantage of this method is that the original `/boot` partition can
be preserved and used in case of *disaster recovery* (if for some reason
the GRUB image is lacking the `cryptodisk` module and the original
plaintext `/boot` partition is lost, you'd need to reboot into a live CD
to recover).  Moreover increasing the number of partitions *increases
usage pattern visibility*: a separate `/boot` partition, even encrypted,
will likely leak the fact that a kernel update took place to an attacker
with access to both pre- and post-update snapshots.

On the other hand, the downside of that method is that the root file
system can't benefit from the nice LUKS2 improvements over LUKS1, some
of which were listed above.  Another (minor) downside is that space
occupied by the former `/boot` partition (typically 256MiB) becomes
unused and can't easily be reclaimed by the root file system.

### Downgrading LUKS2 to LUKS1 ###

Check the LUKS format version on the root device (assumed to be
`/dev/sda3` in the rest of this sub-section):

    root@debian:~$ cryptsetup luksDump /dev/sda3 | grep -A1 "^LUKS"
    LUKS header information
    Version:        2

Here the LUKS format version is 2, so the device needs to be *converted*
to LUKS *version 1* to be able to unlock from GRUB.  Unlike the rest of
this document, conversion can't be done on an open device, so you'll
need reboot into a live CD or an [initramfs shell].  (The `(initramfs)`
prompt strings in this sub-section indicates commands that are executed
from an initramfs shell.)  Also, if you have valuable data in the root
partition, then *make sure you have a backup* (at least of the LUKS
header)!

[initramfs shell]: https://wiki.debian.org/InitramfsDebug#Rescue_shell_.28also_known_as_initramfs_shell.29

Run `cryptsetup convert --type luks1 DEVICE` to downgrade.  However if
the device was created with the default parameters then in-place
conversion will fail:

    (initramfs) cryptsetup convert --type luks1 /dev/sda3

    WARNING!
    ========
    This operation will convert /dev/sda3 to LUKS1 format.


    Are you sure? (Type uppercase yes): YES
    Cannot convert to LUKS1 format - keyslot 0 is not LUKS1 compatible.

This is because its first key slot uses Argon2 as Password-Based Key
Derivation Function (PBKDF) algorithm:

    (initramfs) cryptsetup luksDump /dev/sda3 | grep "PBKDF:"
            PBKDF:      argon2i

Argon2 is a *memory-hard* function that was selected as the winner of
the Password-Hashing Competition; LUKS2 devices use it by default for
key slots, but LUKS1's only supported PBKDF algorithm is PBKDF2.  Hence
the key slot has to be converted to PBKDF2 prior to LUKS format version
downgrade.

    (initramfs) cryptsetup luksChangeKey --key-slot 0 --pbkdf pbkdf2 /dev/sda3
    Enter passphrase to be changed:
    Enter new passphrase:
    Verify passphrase:

(You can reuse the existing passphrase in the above prompts.)  Now that
all key slots use the PBKDF2 algorithm, the device shouldn't have any
LUKS2-only features left, and can be converted to LUKS1.

    (initramfs) cryptsetup luksDump /dev/sda3 | grep "PBKDF:"
            PBKDF:      pbkdf2
<!-- -->
    (initramfs) cryptsetup convert --type luks1 /dev/sda3

    WARNING!
    ========
    This operation will convert /dev/sda3 to LUKS1 format.


    Are you sure? (Type uppercase yes): YES
<!-- -->
    (initramfs) cryptsetup luksDump /dev/sda3 | grep -A1 ^LUKS
    LUKS header information

### Moving `/boot` to the root file system ###

(The moving operation can be done from the normal system.  No need to
reboot into a live CD or an initramfs shell if the root file system
resides in a LUK1 device.)

 1. To ensure data is not modified while it's being copied, remount
    `/boot` read-only.

        root@debian:~$ mount -oremount,ro /boot

 2. Recursively copy the directory to the root file system, and replace
    the old `/boot` mountpoint with the new directory.

    <!-- -->
        root@debian:~$ cp -aT /boot /boot.tmp
    <!-- -->
        root@debian:~$ umount /boot
    <!-- -->
        root@debian:~$ rmdir /boot
    <!-- -->
        root@debian:~$ mv -T /boot.tmp /boot

 3. Comment out the [`fstab`(5)] entry for the `/boot` mountpoint.
    Otherwise at reboot `init`(1) will mount it and therefore shadow data
    in the new `/boot` directory with data from the old plaintext
    partition.

        root@debian:~$ grep /boot /etc/fstab
        ## /boot was on /dev/sda1 during installation
        #UUID=c104749f-a0fa-406c-9e9a-3fc01f8e2f78 /boot           ext2    defaults        0       2


Enabling `cryptomount` in GRUB2
===============================

Enable the feature and update the GRUB image:

    root@debian:~$ echo "GRUB_ENABLE_CRYPTODISK=y" >>/etc/default/grub
<!-- -->
    root@debian:~$ update-grub
<!-- -->
    root@debian:~$ grub-install /dev/sda

If everything went well, `/boot/grub/grub.cfg` should contain `insmod
cryptodisk` (and also `insmod lvm` if `/boot` is on a Logical Volume).

*Note*: The PBKDF parameters are determined via benchmark upon key slot
creation (or update).  Thus they only makes sense if the environment in
which the LUKS device is opened matches (same CPU, same RAM size, etc.)
the one in which it's been formated.  Unlocking from GRUB does count as
an environment mismatch, because GRUB operates under tighter memory
constraints and doesn't take advantage of all crypto-related CPU
instructions.  Concretely, that means unlocking a LUKS device from GRUB
might take *a lot* longer than doing it from the normal system.  Since
GRUB's LUKS implementation isn't able to benchmark, you'll need to
manually tune the PBKDF parameters.  It's easier for PBKDF2 as there is
a single parameter to play with (iteration count) — while Argon2 has two
(iteration count and memory) — and changing the iteration count
linearly affects unlocking time.  For instance, if the “slow” keyslot
has iteration count 1000000, halving it would speed up the unlocking
by a factor of two. (And of course, making it twice as easy to
brute-force, too.  Use your judgment when making manual benchmarks and
trying to find the fine line between convenience and security.)

    root@debian:~$ cryptsetup luksDump /dev/sda2 | grep -B1 Iterations:
    Key Slot 0: ENABLED
        Iterations:             1000000
<!-- -->
    root@debian:~$ cryptsetup luksChangeKey --key-slot=0 --pbkdf-force-iterations=500000 /dev/sda2
    Enter passphrase to be changed:
    Enter new passphrase:
    Verify passphrase:


Avoiding the extra password prompt
==================================

The device holding the kernel (and the initramfs image) is unlocked by
GRUB, but the root device needs to be *unlocked again* at initramfs
stage, regardless whether it's the same device or not.  This is because
GRUB boots with the given `vmlinuz` and initramfs images, but there is
currently no way to securely pass cryptographic material (or Device
Mapper information) to the kernel.  Hence the Device Mapper table is
initially empty at initramfs stage; in other words, all devices are
locked, and the root device needs to be unlocked again.

To avoid extra passphrase prompts at initramfs stage, a workaround is
to *unlock via key files stored into the initramfs image*.  Since the
initramfs image now resides on an encrypted device, this still provides
protection for data at rest.  After all for LUK1 the volume key can
already be found by userspace in the Device Mapper table, so one could
argue that including key files to the initramfs image -- created with
restrictive permissions -- doesn't change the threat model for LUKS1
devices.  Please note however that for LUKS2 the volume key is normally
*offloaded to the kernel keyring* (hence no longer readable by
userspace), while key files lying on disk are of course readable by
userspace.

 1. Generate the shared secret (here with 512 bits of entropy as it's also
    the size of the volume key) inside a new file.

        root@debian:~$ mkdir -m0700 /etc/keys
    <!-- -->
        root@debian:~$ ( umask 0077 && dd if=/dev/urandom bs=1 count=64 of=/etc/keys/root.key conv=excl,fsync )
        64+0 records in
        64+0 records out
        64 bytes copied, 0.000698363 s, 91.6 kB/s

 2. Create a new key slot with that key file.

        root@debian:~$ cryptsetup luksAddKey /dev/sda3 /etc/keys/root.key
        Enter any existing passphrase:
    <!-- -->
        root@debian:~$ cryptsetup luksDump /dev/sda3 | grep "^Key Slot"
        Key Slot 0: ENABLED
        Key Slot 1: ENABLED
        Key Slot 2: DISABLED
        Key Slot 3: DISABLED
        Key Slot 4: DISABLED
        Key Slot 5: DISABLED
        Key Slot 6: DISABLED
        Key Slot 7: DISABLED

 3. Edit the [`crypttab`(5)] and set the third column to the key file path
    for the root device entry.

        root@debian:~$ cat /etc/crypttab
        root_crypt UUID=… /etc/keys/root.key luks,discard,key-slot=1

    The unlock logic normally runs the PBKDF algorithm through each key
    slot sequentially until a match is found.  Since the key file is
    explicitly targeting the second key slot, its index is specified with
    `key-slot=1` in the [`crypttab`(5)] to save useless expensive PBKDF
    computations and *reduce boot time*.

 4. In `/etc/cryptsetup-initramfs/conf-hook`, set `KEYFILE_PATTERN` to a
    `glob`(7) expanding to the key path names to include to the initramfs
    image.

        root@debian:~$ echo "KEYFILE_PATTERN=\"/etc/keys/*.key\"" >>/etc/cryptsetup-initramfs/conf-hook

 5. In `/etc/initramfs-tools/initramfs.conf`, set `UMASK` to a restrictive
    value to avoid leaking key material.  See [`initramfs.conf`(5)] for
    details.

        root@debian:~$ echo UMASK=0077 >>/etc/initramfs-tools/initramfs.conf

 6. Finally re-generate the initramfs image, and double-check that it
    1/ has restrictive permissions; and 2/ includes the key.

        root@debian:~$ update-initramfs -u
        update-initramfs: Generating /boot/initrd.img-4.19.0-4-amd64
    <!-- -->
        root@debian:~$ stat -L -c "%A  %n" /initrd.img
        -rw-------  /initrd.img
    <!-- -->
        root@debian:~$ lsinitramfs /initrd.img | grep "^cryptroot/keyfiles/"
        cryptroot/keyfiles/root_crypt.key

    (`cryptsetup-initramfs` normalises and renames key files inside the
    initramfs, hence the new file name.)

Should be safe to reboot now :-)  If all went well you should see a
single passphrase prompt.

[`cryptsetup`(8)]: https://manpages.debian.org/cryptsetup.8.en.html
[`crypttab`(5)]: https://manpages.debian.org/crypttab.5.en.html
[`fstab`(5)]: https://manpages.debian.org/fstab.5.en.html
[`initramfs.conf`(5)]: https://manpages.debian.org/initramfs.conf.5.en.html

 -- Guilhem Moulin <guilhem@debian.org>, Sun, 09 Jun 2019 16:35:20 +0200
