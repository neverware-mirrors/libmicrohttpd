/*
  This file is part of libmicrohttpd
  Copyright (C) 2016 Karlson2k (Evgeny Grin)

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/**
 * @file microhttpd/mhd_options.h
 * @brief  additional automatic macros for MHD_config.h
 * @author Karlson2k (Evgeny Grin)
 *
 * This file includes MHD_config.h and adds automatic macros based on values
 * in MHD_config.h, compiler built-in macros and commandline-defined macros
 * (but not based on values defined in other headers). Works also as a guard
 * to prevent double inclusion of MHD_config.h
 */

#ifndef MHD_OPTIONS_H
#define MHD_OPTIONS_H 1

#include "MHD_config.h"


#ifndef _MHD_EXTERN
#if defined(BUILDING_MHD_LIB) && defined(_WIN32) && \
    (defined(DLL_EXPORT) || defined(MHD_W32DLL))
#define _MHD_EXTERN __declspec(dllexport) extern
#else   /* !BUILDING_MHD_LIB || !_WIN32 || (!DLL_EXPORT && !MHD_W32DLL) */
#define _MHD_EXTERN extern
#endif  /* !BUILDING_MHD_LIB || !_WIN32 || (!DLL_EXPORT && !MHD_W32DLL) */
#endif  /* ! _MHD_EXTERN */

/* Some platforms (FreeBSD, Solaris, W32) allow to override
   default FD_SETSIZE by defining it before including
   headers. */
#ifdef FD_SETSIZE
/* FD_SETSIZE defined in command line or in MHD_config.h */
/* Use function to retrieve system default FD_SETSIZE value. */
#define _MHD_SYS_DEFAULT_FD_SETSIZE get_system_fdsetsize_value()
#elif defined(_WIN32) && !defined(__CYGWIN__)
/* Platform with WinSock and without overridden FD_SETSIZE */
#define FD_SETSIZE 2048 /* Override default small value */
/* Use function to retrieve system default FD_SETSIZE value. */
#define _MHD_SYS_DEFAULT_FD_SETSIZE get_system_fdsetsize_value()
#else  /* !FD_SETSIZE && !WinSock*/
/* System default value of FD_SETSIZE is used */
#define _MHD_SYS_DEFAULT_FD_SETSIZE FD_SETSIZE
#define _MHD_FD_SETSIZE_IS_DEFAULT 1
#endif /* !FD_SETSIZE && !WinSock*/

#define _XOPEN_SOURCE_EXTENDED  1
#if OS390
#define _OPEN_THREADS
#define _OPEN_SYS_SOCK_IPV6
#define _OPEN_MSGQ_EXT
#define _LP64
#endif

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#else /* _WIN32_WINNT */
#if _WIN32_WINNT < 0x0501
#error "Headers for Windows XP or later are required"
#endif /* _WIN32_WINNT < 0x0501 */
#endif /* _WIN32_WINNT */
#ifndef WIN32_LEAN_AND_MEAN
/* Do not include unneeded parts of W32 headers. */
#define WIN32_LEAN_AND_MEAN 1
#endif /* !WIN32_LEAN_AND_MEAN */
#endif /* _WIN32 */

#if LINUX+0 && (defined(HAVE_SENDFILE64) || defined(HAVE_LSEEK64)) && ! defined(_LARGEFILE64_SOURCE)
/* On Linux, special macro is required to enable definitions of some xxx64 functions */
#define _LARGEFILE64_SOURCE 1
#endif

#ifdef HAVE_C11_GMTIME_S
/* Special macro is required to enable C11 definition of gmtime_s() function */
#define __STDC_WANT_LIB_EXT1__ 1
#endif /* HAVE_C11_GMTIME_S */

#endif /* MHD_OPTIONS_H */