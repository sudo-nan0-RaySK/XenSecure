#################################
 __  __            _  _    ___  
 \ \/ /___ _ __   | || |  / _ \ 
  \  // _ \ '_ \  | || |_| | | |
  /  \  __/ | | | |__   _| |_| |
 /_/\_\___|_| |_|    |_|(_)___/ 

#################################

http://www.xen.org/

What is Xen?
============

Xen is a Virtual Machine Monitor (VMM) originally developed by the
Systems Research Group of the University of Cambridge Computer
Laboratory, as part of the UK-EPSRC funded XenoServers project.  Xen
is freely-distributable Open Source software, released under the GNU
GPL. Since its initial public release, Xen has grown a large
development community, spearheaded by xen.org (http://www.xen.org).

The 4.0 release offers excellent performance, hardware support and
enterprise-grade features such as x86_32-PAE, x86_64, SMP guests and
live relocation of VMs. Ports to Linux, NetBSD, FreeBSD and Solaris
are available from the community.

This file contains some quick-start instructions to install Xen on
your system. For full documentation, see the Xen User Manual. If this
is a pre-built release then you can find the manual at:
 dist/install/usr/share/doc/xen/pdf/user.pdf
If you have a source release, then 'make -C docs' will build the
manual at docs/pdf/user.pdf.

Quick-Start Guide
=================

First, there are a number of prerequisites for building a Xen source
release. Make sure you have all the following installed, either by
visiting the project webpage or installing a pre-built package
provided by your Linux distributor:
    * GCC v3.4 or later
    * GNU Make
    * GNU Binutils
    * Development install of zlib (e.g., zlib-dev)
    * Development install of Python v2.3 or later (e.g., python-dev)
    * Development install of curses (e.g., libncurses-dev)
    * Development install of openssl (e.g., openssl-dev)
    * Development install of x11 (e.g. xorg-x11-dev)
    * bridge-utils package (/sbin/brctl)
    * iproute package (/sbin/ip)
    * hotplug or udev
    * GNU bison and GNU flex

[NB. Unless noted otherwise, all the following steps should be
performed with root privileges.]

1. Download and untar the source tarball file. This will be a
   file named xen-unstable-src.tgz, or xen-$version-src.tgz.
   You can also pull the current version from the mercurial
   repository at http://xenbits.xensource.com/

    # tar xzf xen-unstable-src.tgz

   Assuming you are using the unstable tree, this will
   untar into xen-unstable. The rest of the instructions
   use the unstable tree as an example, substitute the
   version for unstable.

2. cd to xen-unstable (or whatever you sensibly rename it to).

On Linux:

3. For the very first build, or if you want to destroy existing
   .configs and build trees, perform the following steps:

    # make world
    # make install

   This will create and install onto the local machine. It will build
   the xen binary (xen.gz), and a linux kernel and modules that can be
   used in both dom0 and an unprivileged guest kernel (vmlinuz-2.6.x-xen),
   the tools and the documentation.

   You can override the destination for make install by setting DESTDIR
   to some value.

   The make command line defaults to building the kernel vmlinuz-2.6.x-xen.
   You can override this default by specifying KERNELS=kernelname. For
   example, you can make two kernels - linux-2.6-xen0
   and linux-2.6-xenU - which are smaller builds containing only selected
   modules, intended primarily for developers that don't like to wait
   for a full -xen kernel to build. The -xenU kernel is particularly small,
   as it does not contain any physical device drivers, and hence is
   only useful for guest domains.

   To make these two kernels, simply specify

   KERNELS="linux-2.6-xen0 linux-2.6-xenU"

   in the make command line.

4. To rebuild an existing tree without modifying the config:
    # make dist

   This will build and install xen, kernels, tools, and
   docs into the local dist/ directory.

   You can override the destination for make install by setting DISTDIR
   to some value.

   make install and make dist differ in that make install does the
   right things for your local machine (installing the appropriate
   version of hotplug or udev scripts, for example), but make dist
   includes all versions of those scripts, so that you can copy the dist
   directory to another machine and install from that distribution.

5. To rebuild a kernel with a modified config:

    # make linux-2.6-xen-config CONFIGMODE=menuconfig     (or xconfig)
    # make linux-2.6-xen-build
    # make linux-2.6-xen-install

   Depending on your config, you may need to use 'mkinitrd' to create
   an initial ram disk, just like a native system e.g.
    # depmod 2.6.18-xen
    # mkinitrd -v -f --with=aacraid --with=sd_mod --with=scsi_mod initrd-2.6.18-xen.img 2.6.18-xen

   Other systems may requires the use of 'mkinitramfs' to create the
   ram disk.
    # depmod 2.6.18-xen
    # mkinitramfs -o initrd-2.6.18-xen.img 2.6.18-xen


Python Runtime Libraries
========================

Xend (the Xen daemon) has the following runtime dependencies:

    * Python 2.3 or later.
      In many distros, the XML-aspects to the standard library
      (xml.dom.minidom etc) are broken out into a separate python-xml package.
      This is also required.

          URL:    http://www.python.org/
          Debian: python, python-xml

    * For optional SSL support, pyOpenSSL:
          URL:    http://pyopenssl.sourceforge.net/
          Debian: python-pyopenssl

    * For optional PAM support, PyPAM:
          URL:    http://www.pangalactic.org/PyPAM/
          Debian: python-pam

    * For optional XenAPI support in XM, PyXML:
          URL:    http://pyxml.sourceforge.net
          YUM:    PyXML


Intel(R) Trusted Execution Technology Support
=============================================

Intel's technology for safer computing, Intel(R) Trusted Execution Technology
(Intel(R) TXT), defines platform-level enhancements that provide the building
blocks for creating trusted platforms.  For more information, see
http://www.intel.com/technology/security/.

Intel(R) TXT support is provided by the Trusted Boot (tboot) module in
conjunction with minimal logic in the Xen hypervisor.

Tboot is an open source, pre- kernel/VMM module that uses Intel(R) TXT to
perform a measured and verified launch of an OS kernel/VMM.

The Trusted Boot module is available from
http://sourceforge.net/projects/tboot.  This project hosts the code in a
mercurial repo at http://tboot.sourceforge.net/hg/tboot.hg and contains
tarballs of the source.  Instructions in the tboot README describe how
to modify grub.conf to use tboot to launch Xen.

There are optional targets as part of Xen's top-level makefile that will
download and build tboot: install-tboot, build-tboot, dist-tboot, clean-tboot.
These will download the latest tar file from the SourceForge site using wget,
then build/install/dist according to Xen's settings.
