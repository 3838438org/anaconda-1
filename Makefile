#
# Makefile
#
# Copyright (C) 1998, 1999, 2000, 2001, 2002  Red Hat, Inc.
# All rights reserved.
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
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

include Makefile.inc
VERSION := $(shell awk '/Version:/ { print $$2 }' anaconda.spec)
RELEASE := $(shell awk '/Release:/ { print $$2 }' anaconda.spec)
CVSROOT ?= ${CVSROOT:-$(shell cat CVS/Root 2>/dev/null)}

SUBDIRS = isys wlite stubs loader2 po \
	    textw utils scripts bootdisk installclasses \
	    iw pixmaps command-stubs ui docs
# fonts aren't on s390/s390x
ifeq (,$(filter s390 s390x, $(ARCH)))
SUBDIRS += fonts
endif

ifneq (,$(filter ppc ppc64 i386 x86_64,$(ARCH)))
# we only do the liveinst bits on i386/x86_64 for now
SUBDIRS += liveinst
endif
ifneq (,$(filter i386 x86_64,$(ARCH)))
# gptsync only on x86 for mactels right now
SUBDIRS += gptsync
endif

PYCHECKERPATH=isys:textw:iw:installclasses:/usr/lib/booty:/usr/share/system-config-date
PYCHECKEROPTS=-F pycheckrc-for-anaconda

CATALOGS = po/anaconda.pot

PYFILES = $(wildcard *.py)

all:  subdirs mini-wm xutils.so $(CATALOGS) lang-table lang-names

lang-names: lang-table subdirs
	PYTHONPATH="." $(PYTHON) scripts/getlangnames.py > lang-names

mini-wm: mini-wm.c
	gcc -o mini-wm mini-wm.c `pkg-config gtk+-x11-2.0 --cflags --libs` $(CFLAGS) $(LDFLAGS)

xutils.so: xutils.c
	gcc -ggdb -Wall -o xutils.o -fno-strict-aliasing -fPIC -I/usr/X11R6/include -I$(PYTHONINCLUDE) -I $(PYTHONINCLUDE) -c xutils.c $(CFLAGS) `pkg-config --cflags gdk-2.0`
	gcc -o xutils.so -shared xutils.o -ggdb -L/usr/X11R6/$(LIBDIR) -lX11 `pkg-config --libs gdk-2.0` $(LDFLAGS)

depend:
	rm -f *.o *.so *.pyc
	for d in $(SUBDIRS); do make -C $$d depend; done

clean:
	rm -f *.o *.so *.pyc lang-names mini-wm ChangeLog netinst.iso outiso
	for d in $(SUBDIRS); do make -C $$d clean; done

subdirs:
	for d in $(SUBDIRS); do make -C $$d; [ $$? = 0 ] || exit 1; done

testiso: install
	@if [ "$(REPO)" = "" ]; then echo "ERROR: Need a repo to pull packages from!" ; exit 1 ; fi
	@pushd scripts ; sudo ./buildinstall --version $(VERSION) --product anaconda --release $(ANACONDA)-$(VERSION) --output $(shell pwd)/outiso --updates $(DESTDIR) $(REPO) ; popd ; cp outiso/images/boot.iso ./boot.iso ; sudo rm -rf outiso
	@echo
	@echo "Test iso is located at ./boot.iso"

install: 
	@if [ "$(DESTDIR)" = "" ]; then \
		echo " "; \
		echo "ERROR: A destdir is required"; \
		exit 1; \
	fi

	mkdir -p $(DESTDIR)/usr/bin
	mkdir -p $(DESTDIR)/usr/sbin
	mkdir -p $(DESTDIR)/etc/rc.d/init.d
	mkdir -p $(DESTDIR)/$(PYTHONLIBDIR)
	mkdir -p $(DESTDIR)/$(RUNTIMEDIR)
	mkdir -p $(DESTDIR)/$(ANACONDADATADIR)

	install -m 755 anaconda $(DESTDIR)/usr/sbin/anaconda
	install -m 755 mini-wm $(DESTDIR)/usr/bin/mini-wm

	cp -var $(PYFILES) $(DESTDIR)/$(PYTHONLIBDIR)
	cp -a lang-table $(DESTDIR)/$(PYTHONLIBDIR)
	cp -a lang-names $(DESTDIR)/$(PYTHONLIBDIR)
	./py-compile --basedir $(DESTDIR)/$(PYTHONLIBDIR) $(PYFILES)
	cp -a *.so $(DESTDIR)/$(PYTHONLIBDIR)
	strip $(DESTDIR)/$(PYTHONLIBDIR)/*.so
	for d in $(SUBDIRS); do make DESTDIR=`cd $(DESTDIR); pwd` -C $$d install; [ $$? = 0 ] || exit 1; done

tag:
	@git tag -a -m "Tag as anaconda-$(VERSION)-$(RELEASE)" -f anaconda-$(VERSION)-$(RELEASE)
	@echo "Tagged as anaconda-$(VERSION)-$(RELEASE)"

ChangeLog:
	(GIT_DIR=.git git log > .changelog.tmp && mv .changelog.tmp ChangeLog; rm -f .changelog.tmp) || (touch ChangeLog; echo 'git directory not found: installing possibly empty changelog.' >&2)

archive: tag
	@rm -f ChangeLog docs/kickstart-docs.txt docs/command-line.txt
	@make ChangeLog
	@make -C docs kickstart-docs.txt command-line.txt
	@git archive --format=tar --prefix=anaconda-$(VERSION)/ HEAD > anaconda-$(VERSION).tar
	@mkdir -p anaconda-$(VERSION)/docs/
	@cp docs/kickstart-docs.txt docs/command-line.txt anaconda-$(VERSION)/docs/
	@cp ChangeLog anaconda-$(VERSION)/
	@tar --append -f anaconda-$(VERSION).tar anaconda-$(VERSION)
	@bzip2 -f anaconda-$(VERSION).tar
	@rm -rf anaconda-$(VERSION)

src: archive
	@rpmbuild -ts --nodeps anaconda-$(VERSION).tar.bz2 || exit 1
	@rm -f anaconda-$(VERSION).tar.bz2

pycheck:
	PYTHONPATH=$(PYCHECKERPATH) pychecker $(PYCHECKEROPTS) *.py textw/*.py iw/*.py installclasses/*.py | grep -v "__init__() not called" 

pycheck-file:
	PYTHONPATH=.:$(PYCHECKERPATH) pychecker $(PYCHECKEROPTS) $(CHECK) | grep -v "__init__() not called" 

api:
	doxygen docs/api.cfg

rpmlog:
	@git log --pretty="format:- %s (%ae)" anaconda-$(VERSION)-$(RELEASE).. |sed -e 's/@.*)/)/'
	@echo

bumpver:
	@NEWSUBVER=$$((`echo $(VERSION) |cut -d . -f 4` + 1)) ; \
	NEWVERSION=`echo $(VERSION).$$NEWSUBVER |cut -d . -f 1-3,5` ; \
	DATELINE="* `date "+%a %b %d %Y"` `git config user.name` <`git config user.email`> - $$NEWVERSION-1"  ; \
	cl=`grep -n %changelog anaconda.spec |cut -d : -f 1` ; \
	tail --lines=+$$(($$cl + 1)) anaconda.spec > speclog ; \
	(head -n $$cl anaconda.spec ; echo "$$DATELINE" ; make --quiet rpmlog 2>/dev/null ; echo ""; cat speclog) > anaconda.spec.new ; \
	mv anaconda.spec.new anaconda.spec ; rm -f speclog ; \
	sed -i "s/Version: $(VERSION)/Version: $$NEWVERSION/" anaconda.spec
