# $FreeBSD: src/sys/modules/ip_mroute_mod/Makefile,v 1.20.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $

.include <bsd.own.mk>

.PATH: ${.CURDIR}/../../netinet ${.CURDIR}/../../netinet6

KMOD=	ip_mroute

SRCS=	ip_mroute.c
SRCS+=	opt_inet.h opt_mrouting.h

.if !defined(KERNBUILDDIR)
opt_inet.h:
	echo "#define INET 1" > ${.TARGET}
opt_mrouting.h:
	echo "#define MROUTING 1" > ${.TARGET}
.endif

.include <bsd.kmod.mk>
