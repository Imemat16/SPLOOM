TARGET = SploomTerminal
OBJS = main.o

PSP_EBOOT_TITLE = SPLOOM TERMINAL
PSP_EBOOT_ICON = ICON0.PNG

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspgum -lpspgu -lpsputility -lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lm

EXTRA_TARGETS = EBOOT.PBP

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak