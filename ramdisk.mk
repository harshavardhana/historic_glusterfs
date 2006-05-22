# Copyright (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
#  
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#  
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#  
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#  

%.ramdisk: %.live
	cd $(BUILD_LIVE)/$(PACKAGE_NAME) ; \
	export $(LIVE_INSTALL_ENV) ; \
	$(call LIVE_INSTALL_CMD,$(DESTDIR_RAMDISK))

ramdisk-template: $(APPS_RAMDISK:%=%.ramdisk)
	$(MAKE) -C ramdisk all
	$(MAKE) -C ramdisk install DESTDIR=$(DESTDIR_RAMDISK)

	-if [ -d $(TOOL_BASE)/$(CROSS)/sys-root ] ;\
	then \
		cd $(TOOL_BASE)/$(CROSS)/sys-root && \
		(find . | grep -v '/usr/include' \
			| grep -v '/lib.*\.a$$' \
			| grep -v '/lib.*\.old$$' \
			| grep -v '/man/'  \
			| grep -v '/usr/share/' \
			| grep -v '/usr/info/' \
		| cpio -puvd $(DESTDIR_RAMDISK)); \
	fi
	-cp -a $(TOOL_BASE)/$(CROSS)/lib/{*.so,*.so.*} \
		$(DESTDIR_RAMDISK)/lib;
	-cp -a $(TOOL_BASE)/$(CROSS)/lib64/{*.so,*.so.*} \
			$(DESTDIR_RAMDISK)/lib64;

	cp $(abs_top_srcdir)/ramdisk/linuxrc $(DESTDIR_RAMDISK)
	$(abs_top_srcdir)/cleanup.sh $(DESTDIR_RAMDISK) STRIP=$(CROSS)-strip

initrd.slave: $(GENEXT2FS_NATIVE_IF_YES) ramdisk-template
	rm -rf $(top_builddir)/ramdisk.slave
	cp -af $(DESTDIR_RAMDISK) $(top_builddir)/ramdisk.slave
	cd $(abs_top_srcdir)/ramdisk/skel-slave && \
		(find . | grep -Ev 'CVS|~|.arch-ids' | \
		cpio -puvd $(abs_top_builddir)/ramdisk.slave)
	genext2fs -b 65536 -e 0 -D $(abs_top_srcdir)/ramdisk/devices.txt \
		-d $(top_builddir)/ramdisk.slave \
		$(top_builddir)/initrd.slave.img
	mkdir -p $(top_builddir)/iso_fs/boot
	gzip -9 < $(top_builddir)/initrd.slave.img > \
		$(top_builddir)/iso_fs/boot/initrd.slave.gz

initrd.master: $(GENEXT2FS_NATIVE_IF_YES) ramdisk-template
	mkdir -p $(top_builddir)/ramdisk.master
	cp -af $(DESTDIR_RAMDISK)/* $(top_builddir)/ramdisk.master
	cd $(abs_top_srcdir)/ramdisk/skel-master && \
		(find . | grep -Ev 'CVS|~|.arch-ids' | \
		cpio -puvd $(abs_top_builddir)/ramdisk.master)
	genext2fs -b 65536 -e 0 -D $(abs_top_srcdir)/ramdisk/devices.txt \
		-d $(top_builddir)/ramdisk.master \
		$(top_builddir)/initrd.master.img
	mkdir -p $(top_builddir)/iso_fs/boot
	gzip -9 < $(top_builddir)/initrd.master.img > \
		$(top_builddir)/iso_fs/boot/initrd.master.gz


initrds: initrd.slave initrd.master

clean.ramdisk:
	rm -rf $(top_builddir)/{initrd.slave.img,initrd.master.img,ramdisk.slave,ramdisk.master}
	rm -rf $(DESTDIR_RAMDISK)

clean: clean.ramdisk

