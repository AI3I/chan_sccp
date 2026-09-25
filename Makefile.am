# FILE:			AutoMake Makefile for chan-sccp
# COPYRIGHT:		http://chan-sccp.github.io/chan-sccp/ group 2009
# CREATED BY:		Diederik de Groot <ddegroot@sourceforge.net>
# LICENSE:		This program is free software and may be modified and distributed under the terms of the GNU Public License.
# 			See the LICENSE file at the top of the source tree.
# NOTE:			Process this file with automake to produce a makefile.in script.
AUTOMAKE_OPTIONS	= gnu foreign
EXTRA_DIST 		= .version tools conf doc
SUBDIRS 		= src
ACLOCAL_AMFLAGS		= -I autoconf
AM_CPPFLAGS		= $(LTDLINCL)

distdir 		= $(PACKAGE)-$(VERSION)
	
# Update libtool if necessary	
libtool: 		@LIBTOOL_DEPS@
			cd $(srcdir) && \
			$(SHELL) ./config.status --recheck
	
FORCE:

# Update ChangeLog file from SVN Log
ChangeLog:
	@if test -d "$(top_srcdir)/.git"; then \
	  $(GIT) -C "$(top_srcdir)" log -100 --pretty=medium > $@; \
	else \
	  echo 'See NEWS and the repository history for changes.' > $@; \
	fi

isntall: install

rmcoverage:
	[ -d coverage ] && rm -rf coverage;

coverage: src/.libs/chan_sccp.so
	mkdir coverage; \
	lcov --directory src/.libs -b src -c --o coverage/chan-sccp.info ; \
	genhtml -o coverage -f -t chan-sccp coverage/chan-sccp.info
	
coverage-zero: rmcoverage
	lcov --directory src/.libs -b src -z;

check-local:
	CC="$(CC)" $(SHELL) $(top_srcdir)/tools/run-tests.sh

.PHONY: test
test: check


install-data-local: chan_sccp-en_US.xml
	@test -d $(DESTDIR)$(PBX_ETC) || $(mkinstalldirs) $(DESTDIR)$(PBX_ETC);
	@if [ ! -f $(DESTDIR)$(PBX_ETC)/sccp.conf ]; then										\
		echo "Installing $(DESTDIR)$(PBX_ETC)/sccp.conf...";									\
		$(INSTALL) -m 644 $(top_srcdir)/conf/sccp.conf $(DESTDIR)$(PBX_ETC);							\
	fi;
	@echo "Installing chan_sccp-en_US.xml to $(DESTDIR)$(PBX_DATADIR)/documentation/thirdparty/..."					
	@test -d $(DESTDIR)$(PBX_DATADIR)/documentation/thirdparty || $(mkinstalldirs) $(DESTDIR)$(PBX_DATADIR)/documentation/thirdparty;
	@$(INSTALL) -m 644 chan_sccp-en_US.xml "$(DESTDIR)$(PBX_DATADIR)/documentation/thirdparty";

XML_chan_sccp_en_US=$(top_srcdir)/src/sccp_appfunctions.c $(top_srcdir)/src/sccp_management.c $(top_srcdir)/src/sccp_cli.c $(top_srcdir)/src/sccp_conference.c $(top_srcdir)/src/sccp_config.c
chan_sccp-en_US.xml: $(top_builddir)/src/stamp-h1 $(XML_chan_sccp_en_US)
	@printf "Building Documentation..."
	@echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" > $@
	@echo "<!DOCTYPE docs SYSTEM \"appdocsxml.dtd\">" >> $@
	@echo "<?xml-stylesheet type=\"text/xsl\" href=\"../appdocsxml.xslt\"?>" >> $@
	@echo "<docs xmlns:xi=\"http://www.w3.org/2001/XInclude\">" >> $@
	@for x in $(XML_chan_sccp_en_US);do 												\
		$(AWK) -f $(top_srcdir)/tools/get_documentation $$x >> $@;									\
	done;
	@echo
	@echo "</docs>" >> $@

unload:	
	cd src;make unload
	
load:	
	cd src;make load
	
reload:
	cd src;make reload

#install-exec-hook:
#	@if test "$(DEBUG)" == "1" && test -n "$(GDB)" && test tools/chan-sccp_backtrace -nt /usr/bin/chan-sccp_backtrace; then	\
#		$(INSTALL) -C tools/chan-sccp_backtrace /usr/bin/chan-sccp_backtrace;						\
#	fi
