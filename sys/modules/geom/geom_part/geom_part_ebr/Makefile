# $FreeBSD: src/sys/modules/geom/geom_part/geom_part_ebr/Makefile,v 1.2.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $

.PATH: ${.CURDIR}/../../../../geom/part

KMOD=	geom_part_ebr
SRCS=	g_part_ebr.c

SRCS+=	bus_if.h device_if.h g_part_if.h
SRCS+=	opt_geom.h

MFILES=	kern/bus_if.m kern/device_if.m geom/part/g_part_if.m

.include <bsd.kmod.mk>
