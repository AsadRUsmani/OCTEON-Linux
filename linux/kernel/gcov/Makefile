#
# Makefile to build GCOV utility.
#
#    Copyright IBM Corp. 2009
#    Author(s): Peter Oberparleiter <oberpar@linux.vnet.ibm.com>
#

EXTRA_CFLAGS := -DSRCTREE='"$(srctree)"' -DOBJTREE='"$(objtree)"'

obj-$(CONFIG_GCOV_KERNEL) := base.o fs.o gcc_3_4.o
