# Makefile for Mini vMac PS2
# Redirects to Makefile.ps2

all:
	@$(MAKE) -f Makefile.ps2

clean:
	@$(MAKE) -f Makefile.ps2 clean

.PHONY: all clean
