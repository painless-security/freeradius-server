dnl Look up the OS codename, docker base image etc before including
dnl the main Dockerfile template.
dnl
divert(`-1')
changequote(`[', `]')
define([p_SET], [
	define([PKG_TYPE],	[$1])
	define([OS_NAME],	[$2])
	define([OS_VER],	[$3])
	define([OS_CODENAME],	[$4])
	define([DOCKER_IMAGE],	[$5])
])
dnl		D_NAME		PKG_TYPE      OS_NAME	OS_VER	OS_CODENAME	DOCKER_IMAGE
ifelse(
	D_NAME, [debian10],	[p_SET([deb], [debian],	[10],	[buster],	[debian:buster])],
	D_NAME, [debian11],	[p_SET([deb], [debian],	[11],	[bullseye],	[debian:bullseye])],
	D_NAME, [debian12],	[p_SET([deb], [debian],	[12],	[bookworm],	[debian:bookworm])],
	D_NAME, [ubuntu18],	[p_SET([deb], [ubuntu],	[18],	[bionic],	[ubuntu:18.04])],
	D_NAME, [ubuntu20],	[p_SET([deb], [ubuntu],	[20],	[focal],	[ubuntu:20.04])],
	D_NAME, [ubuntu22],	[p_SET([deb], [ubuntu],	[22],	[jammy],	[ubuntu:22.04])],
	D_NAME, [centos7],	[p_SET([rpm], [centos],	[7],	[7],		[centos:7])],
	D_NAME, [centos8],	[p_SET([rpm], [centos],	[8],	[8],		[centos:8])],
	D_NAME, [rocky8],	[p_SET([rpm], [rocky],	[8],	[8],		[rockylinux/rockylinux:8])],
	D_NAME, [rocky9],	[p_SET([rpm], [rocky],	[9],	[9],		[rockylinux/rockylinux:9])],
	[errprint(error: OS 'D_NAME' not defined[,] see __file__
)m4exit(1)]
)
undefine([p_SET])
divert[]dnl
[#] Auto generated for D_NAME
[#] from scripts/crossbuild/m4/Dockerfile.PKG_TYPE.m4
[#]
[#] Rebuild this file with `make crossbuild.D_NAME.regen`
[#]
changequote([`], ['])dnl
include(Dockerfile.PKG_TYPE.m4)dnl
