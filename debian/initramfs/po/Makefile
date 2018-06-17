XGETTEXT = xgettext --width=72
MSGFMT   = msgfmt
MSGMERGE = msgmerge --width=72

LOCALEDIR = /usr/share/locale

.SUFFIXES: .po .mo .pot

%.mo: %.po
	$(MSGFMT) -o $@ $<

PO = $(wildcard *.po)
LANG = $(basename $(PO))
MO = $(addsuffix .mo,$(LANG))
SOURCES = ../cryptroot-script ../cryptroot-unlock

all: update $(MO)
update: cryptsetup_cryptroot.pot
	-@for po in $(PO); do \
	echo -n "Updating $$po"; \
	$(MSGMERGE) -U $$po cryptsetup_cryptroot.pot; \
	done;

cryptsetup_cryptroot.pot: $(SOURCES)
	$(XGETTEXT) -c -L Shell -kgtx \
	--msgid-bugs-address=pkg-cryptsetup-devel@alioth-lists.debian.net \
	-o $@ $(SOURCES)

install: all
	for i in $(MO) ; do \
	  t=$(DESTDIR)/$(LOCALEDIR)/`basename $$i .mo`/LC_MESSAGES ;\
	  install -d $$t ;\
	  install -m 644 $$i $$t/cryptsetup_cryptroot.mo ;\
	done

clean:
	$(RM) $(MO) *~

.PHONY: update
