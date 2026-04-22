PKGNAME=	librdkafka-oauthbearer-aws
LIBNAME=	rdkafka-oauthbearer-aws
LIBVER=		1

-include ../Makefile.config

SRCS=		plugin.c

OBJS=		$(SRCS:.c=.o)

# Pick up rdkafka.h from the sibling C library build tree.
CPPFLAGS+=-I../src
LDFLAGS+=-L../src
LIBS+=-lrdkafka

# No linker script / symbol hiding at M0 — revisited in M6 alongside
# -static-libstdc++ once aws-sdk-cpp lands in M1.
DISABLE_LDS=y

all: lib

include ../mklove/Makefile.base

clean: lib-clean

-include $(DEPS)
