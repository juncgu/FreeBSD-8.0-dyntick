# $FreeBSD: src/sys/modules/cmx/Makefile,v 1.1.4.1.2.1 2009/10/25 01:10:29 kensmith Exp $
#
MAINTAINER=	daniel@roe.ch

.PATH:	${.CURDIR}/../../dev/cmx

KMOD=	cmx
SRCS=	cmx.c cmxvar.h cmxreg.h \
	cmx_pccard.c \
	pccarddevs.h device_if.h bus_if.h card_if.h

.include <bsd.kmod.mk>
