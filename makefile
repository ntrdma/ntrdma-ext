ifndef KSRC
KSRC		= /lib/modules/$(shell uname -r)/build
endif

# Build the ntrdma kernel module
LINUXCONFIG	+= DEBUG=1
LINUXCONFIG	+= CONFIG_NTC=m
LINUXCONFIG	+= CONFIG_NTRDMA=m

MAKE_TARGETS	= all modules modules_install clean help
MAKE_OPTIONS	= -C $(KSRC) M=$(CURDIR) $(LINUXCONFIG)

install: modules_install

$(MAKE_TARGETS):
	$(MAKE) $(MAKE_OPTIONS) $@

.PHONY: $(MAKE_TARGETS)

