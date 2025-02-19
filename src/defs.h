/**
 * bookmarkfs/src/defs.h
 * ----
 *
 * Copyright (C) 2024  CismonX <admin@cismon.net>
 *
 * This file is part of BookmarkFS.
 *
 * BookmarkFS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BookmarkFS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BookmarkFS.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef BOOKMARKFS_DEFS_H_
#define BOOKMARKFS_DEFS_H_

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#  error "unsupported compiler"
#elif __STDC_VERSION__ >= 202311L
#  define HAVE_STDC_23  1
#elif __STDC_VERSION__ >= 201112L
#  define HAVE_STDC_11  1
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_COLD
#  define FUNCATTR_COLD  __attribute__((cold))
#else
#  define FUNCATTR_COLD
#endif  /* defined(HAVE_FUNC_ATTRIBUTE_COLD) */

#ifdef HAVE_FUNC_ATTRIBUTE_DESTRUCTOR
#  define FUNCATTR_DTOR  __attribute__((destructor))
#else
#  define FUNCATTR_DTOR
#endif  /* defined(HAVE_FUNC_ATTRIBUTE_DESTRUCTOR) */

#ifdef HAVE_FUNC_ATTRIBUTE_MALLOC
#  define FUNCATTR_MALLOC  __attribute__((malloc))
#else
#  define FUNCATTR_MALLOC
#endif  /* defined(HAVE_FUNC_ATTRIBUTE_MALLOC) */

#if defined(HAVE_STDC_23)
#  define FUNCATTR_NORETURN  [[noreturn]]
#elif defined(HAVE_STDC_11)
#  define FUNCATTR_NORETURN  _Noreturn
#else
#  ifdef HAVE_FUNC_ATTRIBUTE_NORETURN
#    define FUNCATTR_NORETURN  __attribute__((noreturn))
#  else
#    define FUNCATTR_NORETURN
#  endif  /* defined(HAVE_FUNC_ATTRIBUTE_NORETURN) */
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL
#  define FUNCATTR_RETURNS_NONNULL  __attribute__((returns_nonnull))
#else
#  define FUNCATTR_RETURNS_NONNULL
#endif  /* defined(HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL) */

#ifdef HAVE_FUNC_ATTRIBUTE_VISIBILITY
#  define BOOKMARKFS_API       __attribute__((visibility("default")))
#  define BOOKMARKFS_INTERNAL  __attribute__((visibility("hidden")))
#else
#  define BOOKMARKFS_API
#  define BOOKMARKFS_INTERNAL
#endif  /* defined(HAVE_FUNC_ATTRIBUTE_VISIBILITY) */

#if defined(HAVE_STDC_23)
#  define UNUSED_VAR(name)
#else
#  ifdef HAVE_VAR_ATTRIBUTE_UNUSED
#    define VARATTR_UNUSED_  __attribute__((unused))
#  else
#    define VARATTR_UNUSED_
#  endif  /* defined(HAVE_VAR_ATTRIBUTE_UNUSED) */
#  define UNUSED_VAR(name)  name##_unused_ VARATTR_UNUSED_
#endif  /* defined(HAVE_STDC_23) */

#if defined(HAVE_STDC_23)
#  define BOOKMARKFS_TLS  thread_local
#elif defined(HAVE_STDC_11)
#  define BOOKMARKFS_TLS  _Thread_local
// Incomprehensive list.  Add more if needed.
#elif defined(__GNUC__) || defined(__clang__)
#  define BOOKMARKFS_TLS  __thread
#endif

#if defined(__FreeBSD__) || (defined(__linux__) && defined(_GNU_SOURCE))
#  define HAVE_PIPE2  1
#endif

#ifndef __FreeBSD__
#  define O_RESOLVE_BENEATH  0
#  define PROT_MAX(prot)     0
#endif

#ifdef __linux__
#  define ENOATTR  ENODATA
#endif

#ifdef __FILE_NAME__
#  define FILE_NAME_  __FILE_NAME__
#else
#  ifdef BOOKMARKFS_SRCDIR
#    define FILE_NAME_OFFSET_  sizeof(BOOKMARKFS_SRCDIR)
#  else
#    define FILE_NAME_OFFSET_  0
#  endif
#  define FILE_NAME_  ( __FILE__ + FILE_NAME_OFFSET_ )
#endif

#define BOOKMARKFS_HOMEPAGE_URL  "https://www.nongnu.org/bookmarkfs/"
#define BOOKMARKFS_XATTR_PREFIX  "user.bookmarkfs."

#endif  /* !defined(BOOKMARKFS_DEFS_H_) */
