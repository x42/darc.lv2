#!/usr/bin/make -f

# these can be overridden using make variables. e.g.
#   make CFLAGS=-O2
#   make install DESTDIR=$(CURDIR)/debian/darc.lv2 PREFIX=/usr
#
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
# see http://lv2plug.in/pages/filesystem-hierarchy-standard.html, don't use libdir
LV2DIR ?= $(PREFIX)/lib/lv2

CFLAGS ?= -Wall -g -Wno-unused-function

PKG_CONFIG?=pkg-config
STRIP?= strip

BUILDOPENGL?=yes
BUILDJACKAPP?=yes
INLINEDISPLAY?=yes

darc_VERSION ?= $(shell (git describe --tags HEAD || echo "0") | sed 's/-g.*$$//;s/^v//')
RW ?= robtk/

###############################################################################

MACHINE=$(shell uname -m)
ifneq (,$(findstring x64,$(MACHINE)))
  HAVE_SSE=yes
endif
ifneq (,$(findstring 86,$(MACHINE)))
  HAVE_SSE=yes
endif

ifeq ($(HAVE_SSE),yes)
  OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only -DNDEBUG
else
  OPTIMIZATIONS ?= -fomit-frame-pointer -O3 -fno-finite-math-only -DNDEBUG
endif

###############################################################################

BUILDDIR = build/
APPBLD   = x42/

###############################################################################

LV2NAME=darc
LV2GUI=darcUI_gl
BUNDLE=darc.lv2
targets=

LOADLIBES=-lm
LV2UIREQ=
GLUICFLAGS=-I.

UNAME=$(shell uname)
ifeq ($(UNAME),Darwin)
  LV2LDFLAGS=-dynamiclib
  LIB_EXT=.dylib
  EXE_EXT=
  UI_TYPE=ui:CocoaUI
  PUGL_SRC=$(RW)pugl/pugl_osx.mm
  PKG_GL_LIBS=
  GLUICFLAGS+=-DROBTK_UPSCALE
  GLUILIBS=-framework Cocoa -framework OpenGL -framework CoreFoundation
  STRIPFLAGS=-u -r -arch all -s $(RW)lv2syms
  EXTENDED_RE=-E
else
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed
  LIB_EXT=.so
  EXE_EXT=
  UI_TYPE=ui:X11UI
  PUGL_SRC=$(RW)pugl/pugl_x11.c
  PKG_GL_LIBS=glu gl
  GLUILIBS=-lX11
  GLUICFLAGS+=`$(PKG_CONFIG) --cflags glu`
  STRIPFLAGS= -s
  EXTENDED_RE=-r
endif

ifneq ($(XWIN),)
  CC=$(XWIN)-gcc
  CXX=$(XWIN)-g++
  STRIP=$(XWIN)-strip
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed
  LIB_EXT=.dll
  EXE_EXT=.exe
  PUGL_SRC=$(RW)pugl/pugl_win.cpp
  PKG_GL_LIBS=
  UI_TYPE=ui:WindowsUI
  GLUILIBS=-lws2_32 -lwinmm -lopengl32 -lglu32 -lgdi32 -lcomdlg32 -lpthread
  GLUICFLAGS=-I.
  override LDFLAGS += -static-libgcc -static-libstdc++
endif

ifeq ($(EXTERNALUI), yes)
  UI_TYPE=
endif

ifeq ($(UI_TYPE),)
  UI_TYPE=kx:Widget
  LV2UIREQ+=lv2:requiredFeature kx:Widget;
  override CFLAGS += -DXTERNAL_UI
endif

targets+=$(BUILDDIR)$(LV2NAME)$(LIB_EXT)

UITTL=
ifneq ($(BUILDOPENGL), no)
  targets+=$(BUILDDIR)$(LV2GUI)$(LIB_EXT)
  UITTL=ui:ui $(LV2NAME):ui_gl ;
endif

###############################################################################
# extract versions
LV2VERSION=$(darc_VERSION)
include git2lv2.mk

###############################################################################
# check for build-dependencies

ifeq ($(shell $(PKG_CONFIG) --exists lv2 || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell $(PKG_CONFIG) --atleast-version=1.6.0 lv2 || echo no), no)
  $(error "LV2 SDK needs to be version 1.6.0 or later")
endif

ifneq ($(BUILDOPENGL)$(BUILDJACKAPP), nono)
 ifeq ($(shell $(PKG_CONFIG) --exists pango cairo $(PKG_GL_LIBS) || echo no), no)
  $(error "This plugin requires cairo pango $(PKG_GL_LIBS)")
 endif
endif

ifneq ($(BUILDJACKAPP), no)
 ifeq ($(shell $(PKG_CONFIG) --exists jack || echo no), no)
  $(warning *** libjack from http://jackaudio.org is required)
  $(error   Please install libjack-dev or libjack-jackd2-dev)
 endif
 JACKAPP=$(APPBLD)x42-darc$(EXE_EXT)
endif

# check for lv2_atom_forge_object  new in 1.8.1 deprecates lv2_atom_forge_blank
ifeq ($(shell $(PKG_CONFIG) --atleast-version=1.8.1 lv2 && echo yes), yes)
  override CFLAGS += -DHAVE_LV2_1_8
endif

ifeq ($(shell $(PKG_CONFIG) --atleast-version=1.18.6 lv2 && echo yes), yes)
  override CFLAGS += -DHAVE_LV2_1_18_6
endif

ifneq ($(BUILDOPENGL)$(BUILDJACKAPP), nono)
 ifneq ($(MAKECMDGOALS), submodules)
  ifeq ($(wildcard $(RW)robtk.mk),)
    $(warning "**********************************************************")
    $(warning This plugin needs https://github.com/x42/robtk)
    $(warning "**********************************************************")
    $(info )
    $(info set the RW environment variale to the location of the robtk headers)
    ifeq ($(wildcard .git),.git)
      $(info or run 'make submodules' to initialize robtk as git submodule)
    endif
    $(info )
    $(warning "**********************************************************")
    $(error robtk not found)
  endif
 endif
endif

# LV2 idle >= lv2-1.6.0
GLUICFLAGS+=-DHAVE_IDLE_IFACE
LV2UIREQ+=lv2:requiredFeature ui:idleInterface; lv2:extensionData ui:idleInterface;

# add library dependent flags and libs
override CFLAGS += $(OPTIMIZATIONS) -DVERSION="\"$(darc_VERSION)\""
override CFLAGS += `$(PKG_CONFIG) --cflags lv2`
ifeq ($(XWIN),)
override CFLAGS += -fPIC -fvisibility=hidden
else
override CFLAGS += -DPTW32_STATIC_LIB
endif

ifneq ($(INLINEDISPLAY),no)
  override CFLAGS += `$(PKG_CONFIG) --cflags cairo pangocairo pango` -I$(RW) -DDISPLAY_INTERFACE
  override LOADLIBES += `$(PKG_CONFIG) $(PKG_UI_FLAGS) --libs cairo pangocairo pango`
  ifneq ($(XWIN),)
    override LOADLIBES += -lpthread -lusp10
  endif
endif

GLUICFLAGS+=`$(PKG_CONFIG) --cflags cairo pango` $(CFLAGS)
GLUILIBS+=`$(PKG_CONFIG) $(PKG_UI_FLAGS) --libs cairo pango pangocairo $(PKG_GL_LIBS)`

ifneq ($(XWIN),)
GLUILIBS+=-lpthread -lusp10
endif

GLUICFLAGS+=$(LIC_CFLAGS)
GLUILIBS+=$(LIC_LOADLIBES)

#GLUICFLAGS+=-DUSE_GUI_THREAD
ifeq ($(GLTHREADSYNC), yes)
  GLUICFLAGS+=-DTHREADSYNC
endif

ifneq ($(LIC_CFLAGS),)
  LV2SIGN=, <http:\\/\\/harrisonconsoles.com\\/lv2\\/license\#interface>
  override CFLAGS += -I$(RW)
endif

ROBGL+= Makefile

JACKCFLAGS=-I. $(CFLAGS) $(LIC_CFLAGS)
JACKCFLAGS+=`$(PKG_CONFIG) --cflags jack lv2 pango pangocairo $(PKG_GL_LIBS)`
JACKLIBS=-lm $(GLUILIBS) $(LOADLIBES)


###############################################################################
# build target definitions
default: all

submodule_pull:
	-test -d .git -a .gitmodules -a -f Makefile.git && $(MAKE) -f Makefile.git submodule_pull

submodule_update:
	-test -d .git -a .gitmodules -a -f Makefile.git && $(MAKE) -f Makefile.git submodule_update

submodule_check:
	-test -d .git -a .gitmodules -a -f Makefile.git && $(MAKE) -f Makefile.git submodule_check

submodules:
	-test -d .git -a .gitmodules -a -f Makefile.git && $(MAKE) -f Makefile.git submodules

all: submodule_check $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(targets) $(JACKAPP)

$(BUILDDIR)manifest.ttl: lv2ttl/manifest.ttl.in lv2ttl/manifest.gui.in Makefile
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/g;s/@LIB_EXT@/$(LIB_EXT)/" \
		lv2ttl/manifest.ttl.in > $(BUILDDIR)manifest.ttl
ifneq ($(BUILDOPENGL), no)
	sed "s/@LV2NAME@/$(LV2NAME)/g;s/@LIB_EXT@/$(LIB_EXT)/;s/@UI_TYPE@/$(UI_TYPE)/;s/@LV2GUI@/$(LV2GUI)/g" \
		lv2ttl/manifest.gui.in >> $(BUILDDIR)manifest.ttl
endif

$(BUILDDIR)$(LV2NAME).ttl: Makefile lv2ttl/$(LV2NAME).ttl.in lv2ttl/$(LV2NAME).gui.in \
	lv2ttl/$(LV2NAME).ports.ttl.in lv2ttl/$(LV2NAME).mono.ttl.in lv2ttl/$(LV2NAME).stereo.ttl.in
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/g" \
	    lv2ttl/$(LV2NAME).ttl.in > $(BUILDDIR)$(LV2NAME).ttl
ifneq ($(BUILDOPENGL), no)
	sed "s/@LV2NAME@/$(LV2NAME)/g;s/@UI_TYPE@/$(UI_TYPE)/;s/@UI_REQ@/$(LV2UIREQ)/" \
	    lv2ttl/$(LV2NAME).gui.in >> $(BUILDDIR)$(LV2NAME).ttl
endif
	sed "s/@LV2NAME@/$(LV2NAME)/g;s/@URISUFFIX@/mono/;s/@NAMESUFFIX@/ Mono/;s/@CTLSIZE@/1024/;s/@SIGNATURE@/$(LV2SIGN)/;s/@VERSION@/lv2:microVersion $(LV2MIC) ;lv2:minorVersion $(LV2MIN) ;/g;s/@UITTL@/$(UITTL)/" \
	    lv2ttl/$(LV2NAME).ports.ttl.in >> $(BUILDDIR)$(LV2NAME).ttl
	cat lv2ttl/$(LV2NAME).mono.ttl.in >> $(BUILDDIR)$(LV2NAME).ttl
	sed "s/@LV2NAME@/$(LV2NAME)/g;s/@URISUFFIX@/stereo/;s/@NAMESUFFIX@/ Stereo/;s/@CTLSIZE@/1024/;s/@SIGNATURE@/$(LV2SIGN)/;s/@VERSION@/lv2:microVersion $(LV2MIC) ;lv2:minorVersion $(LV2MIN) ;/g;s/@UITTL@/$(UITTL)/" \
	    lv2ttl/$(LV2NAME).ports.ttl.in >> $(BUILDDIR)$(LV2NAME).ttl
	cat lv2ttl/$(LV2NAME).stereo.ttl.in >> $(BUILDDIR)$(LV2NAME).ttl

DSP_SRC = src/lv2.c
DSP_DEPS = $(DSP_SRC) src/darc.h
GUI_DEPS = gui/$(LV2NAME).c src/darc.h

$(BUILDDIR)$(LV2NAME)$(LIB_EXT): $(DSP_DEPS) Makefile
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIC_CFLAGS) -std=c99 \
	  -o $(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(DSP_SRC) \
	  -shared $(LV2LDFLAGS) $(LDFLAGS) $(LOADLIBES) $(LIC_LOADLIBES)
	$(STRIP) $(STRIPFLAGS) $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

jackapps: $(JACKAPP)

$(eval x42_darc_JACKSRC = -DX42_MULTIPLUGIN src/lv2.c)
x42_darc_JACKGUI = gui/darc.c
x42_darc_LV2HTTL = lv2ttl/plugins.h
x42_darc_JACKDESC = lv2ui_descriptor
$(APPBLD)x42-darc$(EXE_EXT): $(DSP_DEPS) $(GUI_DEPS) \
	        $(x42_darc_JACKGUI) $(x42_darc_LV2HTTL)

ifneq ($(BUILDOPENGL)$(BUILDJACKAPP), nono)
 -include $(RW)robtk.mk
endif

$(BUILDDIR)$(LV2GUI)$(LIB_EXT): $(GUI_DEPS)

###############################################################################
# install/uninstall/clean target definitions

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

install-bin: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
ifneq ($(BUILDOPENGL), no)
	install -m755 $(BUILDDIR)$(LV2GUI)$(LIB_EXT) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
endif
ifneq ($(BUILDJACKAPP), no)
	install -d $(DESTDIR)$(BINDIR)
	install -m755 $(APPBLD)x42-darc$(EXE_EXT) $(DESTDIR)$(BINDIR)
endif

uninstall-bin:
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/manifest.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME).ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME)$(LIB_EXT)
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2GUI)$(LIB_EXT)
	rm -f $(DESTDIR)$(BINDIR)/x42-darc$(EXE_EXT)
	-rmdir $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	-rmdir $(DESTDIR)$(BINDIR)

install-man:
ifneq ($(BUILDJACKAPP), no)
	install -d $(DESTDIR)$(MANDIR)
	install -m644 x42-darc.1 $(DESTDIR)$(MANDIR)
endif

uninstall-man:
	rm -f $(DESTDIR)$(MANDIR)/x42-darc.1
	-rmdir $(DESTDIR)$(MANDIR)

man: $(APPBLD)x42-darc
	help2man -N -o x42-darc.1 -n "x42-darc JACK Dynamic Audio Compressor" $(APPBLD)x42-darc

clean:
	rm -f $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl \
	  $(BUILDDIR)$(LV2NAME)$(LIB_EXT) \
	  $(BUILDDIR)$(LV2GUI)$(LIB_EXT)
	rm -rf $(BUILDDIR)*.dSYM
	rm -rf $(APPBLD)x42-*
	-test -d $(APPBLD) && rmdir $(APPBLD) || true
	-test -d $(BUILDDIR) && rmdir $(BUILDDIR) || true

distclean: clean
	rm -f cscope.out cscope.files tags

.PHONY: clean all install uninstall distclean jackapps man \
        install-bin uninstall-bin install-man uninstall-man \
        submodule_check submodules submodule_update submodule_pull
