// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef CONFIG_H
#define CONFIG_H

#ifndef __linux__
#define CONFIG_HAS_ARC4RANDOM_BUF 1
#else
#define CONFIG_HAS_ARC4RANDOM_BUF 0
#endif

#ifdef __FreeBSD__
#define CONFIG_HAS_BINDAT_SOCKADDR 1
#else
#define CONFIG_HAS_BINDAT_SOCKADDR 0
#endif

#ifdef __CloudABI__
#define CONFIG_HAS_BINDAT_STRING 1
#else
#define CONFIG_HAS_BINDAT_STRING 0
#endif

#if defined(__CloudABI__)
#define CONFIG_HAS_CAP_ENTER 1
#else
#define CONFIG_HAS_CAP_ENTER 0
#endif

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#define CONFIG_HAS_CLOCK_NANOSLEEP 1
#else
#define CONFIG_HAS_CLOCK_NANOSLEEP 0
#endif

#ifdef __FreeBSD__
#define CONFIG_HAS_CONNECTAT_SOCKADDR 1
#else
#define CONFIG_HAS_CONNECTAT_SOCKADDR 0
#endif

#ifdef __CloudABI__
#define CONFIG_HAS_CONNECTAT_STRING 1
#else
#define CONFIG_HAS_CONNECTAT_STRING 0
#endif

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#define CONFIG_HAS_FDATASYNC 1
#else
#define CONFIG_HAS_FDATASYNC 0
#endif

#if !defined(__CloudABI__)
#define CONFIG_HAS_GETPEERNAME 1
#else
#define CONFIG_HAS_GETPEERNAME 0
#endif

#ifndef __CloudABI__
#define CONFIG_HAS_ISATTY 1
#else
#define CONFIG_HAS_ISATTY 0
#endif

#ifndef __APPLE__
#define CONFIG_HAS_MKFIFOAT 1
#else
#define CONFIG_HAS_MKFIFOAT 0
#endif

#if defined(__APPLE__) || defined(__CloudABI__) || defined(__FreeBSD__)
#define CONFIG_HAS_KQUEUE 1
#else
#define CONFIG_HAS_KQUEUE 0
#endif

#if defined(__CloudABI__)
#define CONFIG_HAS_PDFORK 1
#else
#define CONFIG_HAS_PDFORK 0
#endif

#ifndef __APPLE__
#define CONFIG_HAS_POSIX_FALLOCATE 1
#else
#define CONFIG_HAS_POSIX_FALLOCATE 0
#endif

#ifndef __APPLE__
#define CONFIG_HAS_PREADV 1
#else
#define CONFIG_HAS_PREADV 0
#endif

#ifndef __APPLE__
#define CONFIG_HAS_PTHREAD_CONDATTR_SETCLOCK 1
#else
#define CONFIG_HAS_PTHREAD_CONDATTR_SETCLOCK 0
#endif

#ifndef __APPLE__
#define CONFIG_HAS_PWRITEV 1
#else
#define CONFIG_HAS_PWRITEV 0
#endif

#ifdef __APPLE__
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#endif

#ifndef __linux__
#define CONFIG_HAS_STRLCPY 1
#else
#define CONFIG_HAS_STRLCPY 0
#endif

#ifdef __APPLE__
#define CONFIG_TLS_USE_GSBASE 1
#else
#define CONFIG_TLS_USE_GSBASE 0
#endif

#endif
