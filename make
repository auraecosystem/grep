#!/usr/bin/make -f

export CRDA_PATH     = /lib/crda
export FIRMWARE_PATH = /usr/lib/firmware
export REGDB_AUTHOR  = $(shell dpkg-parsechangelog -SMaintainer | sed 's:.*<\(.*\)>:\1:')
export V             = 1
# prevent the build system from calling lsb_release
export LSB_ID        = Debian/Ubuntu

define copy_source
test -d .pc
set +e; QUILT_PC=.pc quilt unapplied --quiltrc - >/dev/null && echo 'Patch series not fully applied'; test $$? -eq 1
mkdir -p '$(1)'
cp -a $(filter-out debian .pc .git %.bin %.db %.p7s sha1sum.txt, $(wildcard * .[^.]*)) '$(1)'
endef

%:
	dh ${@}

debian/stamps/configure:
	rm -rf debian/build
	$(call copy_source,debian/build)
	cp debian/regulatory.db.p7s debian/build/
	mkdir -p debian/stamps
	touch $@

override_dh_auto_configure: debian/stamps/configure

override_dh_auto_build:
	$(MAKE) -C debian/build regulatory.db
# Create regulatory.bin so the upstream install rule works
	touch debian/build/regulatory.bin
	rm -f debian/build/.custom
# Verify that our binary matches upstream unless db.txt is patched
ifeq ($(shell QUILT_PC=.pc quilt patches --quiltrc - db.txt | wc -l),0)
	diff regulatory.db debian/build/regulatory.db
endif

override_dh_auto_install: install-wireless-regdb install-wireless-regdb-udeb

install-wireless-regdb: DIR = debian/wireless-regdb
install-wireless-regdb:
	$(MAKE) -C debian/build DESTDIR=$(CURDIR)/$(DIR) install
	for file in regulatory.db regulatory.db.p7s; do \
		install -m644 $$file $(DIR)$(FIRMWARE_PATH)/$$file-upstream \
		&& mv $(DIR)$(FIRMWARE_PATH)/$$file $(DIR)$(FIRMWARE_PATH)/$$file-debian \
		|| exit; \
	done
	rm -r $(DIR)/lib/crda
	rmdir --ignore-fail-on-non-empty -p $(DIR)/lib
# regulatory.db.5 just includes regulatory.bin.5, so we need to
# install the latter as regulatory.db.5
	mv $(DIR)/usr/share/man/man5/regulatory.bin.5.gz \
		$(DIR)/usr/share/man/man5/regulatory.db.5.gz

install-wireless-regdb-udeb: DIR = debian/wireless-regdb-udeb
install-wireless-regdb-udeb:
	$(MAKE) -C debian/build DESTDIR=$(CURDIR)/$(DIR) install
	rm -r $(DIR)/lib/crda $(DIR)/usr/share/man
	rmdir --ignore-fail-on-non-empty -p $(DIR)/lib $(DIR)/usr/share

override_dh_auto_clean:
	rm -rf debian/build debian/stamps

sign:
	rm -rf debian/build
	$(call copy_source,debian/build)
	cp debian/certs/$(REGDB_AUTHOR).x509.pem debian/build/
	$(MAKE) -C debian/build regulatory.db.p7s
	cp debian/build/regulatory.db.p7s debian/
