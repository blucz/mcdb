/*
 * mcdb_makefn - create temp file for mcdb creation and atomically install
 *
 * Copyright (c) 2011, Glue Logic LLC. All rights reserved. code()gluelogic.com
 *
 *  This file is part of mcdb.
 *
 *  mcdb is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  mcdb is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with mcdb.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * mcdb is originally based upon the Public Domain cdb-0.75 by Dan Bernstein
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _XOPEN_SOURCE /* >= 500 on Linux for mkstemp(), fchmod(), fdatasync() */
#define _XOPEN_SOURCE 700
#endif
/* large file support needed for stat(),fstat() input file > 2 GB */
#define PLASMA_FEATURE_ENABLE_LARGEFILE
#ifdef _AIX
#define PLASMA_FEATURE_ENABLE_LARGEFILE64 /* mkstemp64() */
#endif

#include "mcdb_makefn.h"
#include "mcdb_make.h"
#include "mcdb_error.h"
#include "nointr.h"
#include "plasma/plasma_stdtypes.h"

#include <errno.h>
#include <sys/stat.h>  /* fchmod() umask() */
#include <stdlib.h>    /* mkstemp() EXIT_SUCCESS */
#include <string.h>    /* memcpy() strlen() */
#include <stdio.h>     /* rename() */
#include <unistd.h>    /* unlink() */

#if defined(__APPLE__) && defined(__MACH__)
#include <sys/syscall.h>
int syscall(int, ...); /* or omit -ansi compile flag, define _DARWIN_C_SOURCE */
#define fdatasync(fd) syscall(SYS_fdatasync, (fd))
#endif

int
mcdb_makefn_start (struct mcdb_make * const restrict m,
                   const char * const restrict fname,
                   void * (* const fn_malloc)(size_t),
                   void (* const fn_free)(void *))
{
    struct stat st;
    const size_t len = strlen(fname);
    char * restrict fntmp;

    m->head[0] = NULL;
    m->fntmp   = NULL;
    m->fd      = -1;

    /* preserve permission modes if previous mcdb exists; else make read-only
     * (since mcdb is *constant* -- not modified -- after creation) */
    if (stat(fname, &st) != 0) {
        st.st_mode = S_IRUSR;
        if (errno != ENOENT)
            return -1;
    }
    else if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    fntmp = fn_malloc((len<<1) + 9);
    if (fntmp == NULL)
        return -1;
    memcpy(fntmp, fname, len);
    memcpy(fntmp+len, ".XXXXXX", 8);
    memcpy(fntmp+len+8, fname, len+1);

    m->st_mode   = st.st_mode;
    m->fn_malloc = fn_malloc;
    m->fn_free   = fn_free;
    /* POSIX.1-2008 adds requirement that mkstemp() create file with mode 0600;
     * glibc <= 2.06 (old) mkstemp() created files with unsafe mode 0666.
     * Note: setting umask() is global, affects file creation in other threads*/
  #if defined(__GLIBC__) \
   && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ <= 6))
    st.st_mode   = umask(S_IRWXG|S_IRWXO); /* 0077; octal 077 */
  #endif
    /* coverity[secure_temp : FALSE] */
    m->fd        = mkstemp(fntmp);
  #if defined(__GLIBC__) \
   && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ <= 6))
    umask(st.st_mode);                     /* restore prior umask */
  #endif
    if (m->fd != -1) {
        m->fntmp = fntmp;
        m->fname = fntmp+len+8;
        return EXIT_SUCCESS;
    }
    else {
        fn_free(fntmp);
        return -1;
    }
}

int
mcdb_makefn_finish (struct mcdb_make * const restrict m, const bool datasync)
{
    return fchmod(m->fd, m->st_mode) == 0
        && (!datasync || fdatasync(m->fd) == 0)
        && nointr_close(m->fd) == 0     /* NFS might report write errors here */
        && (m->fd = -2, rename(m->fntmp, m->fname) == 0) /*(fd=-2 flag closed)*/
      ? (m->fd = -1, EXIT_SUCCESS)
      : -1;
    /* mcdb_makefn_cleanup() is not called unconditionally here since fsync
     * may take a long time and contrib/python-mcdb/ releases a global lock
     * around call to mcdb_makefn_finish().  However, Python global lock must
     * be held when PyMem_Free() is called, and m->fn_free() is PyMem_Free(),
     * so mcdb_makefn_cleanup() must not be called here */
}

int
mcdb_makefn_cleanup (struct mcdb_make * const restrict m)
{
    const int errsave = errno;
    if (m->fd != -1) {                       /* (fd == -1 if mkstemp() fails) */
        unlink(m->fntmp);
        if (m->fd >= 0)
            (void) nointr_close(m->fd);
        m->fd = -1;
    }
    if (m->fntmp != NULL) {
        m->fn_free(m->fntmp);
        m->fntmp = NULL;
    }
    if (errsave != 0)
        errno = errsave;
    return -1;
}
