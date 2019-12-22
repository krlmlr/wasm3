//
//  m3_api_wasi.c
//
//  Created by Volodymyr Shymanskyy on 11/20/19.
//  Copyright © 2019 Volodymyr Shymanskyy. All rights reserved.
//

#define _POSIX_C_SOURCE 199309L

#include "m3_api_wasi.h"

#include "m3_api_defs.h"
#include "m3_env.h"
#include "m3_module.h"
#include "m3_exception.h"

#if defined(d_m3HasWASI)

typedef uint32_t __wasi_size_t;
#include "extra/wasi_core.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

#if defined(__wasi__) || defined(__APPLE__) || defined(__ANDROID_API__) || defined(__OpenBSD__) || defined(__linux__)
#  include <unistd.h>
#  include <sys/uio.h>
#  include <sys/random.h>
#  include <fcntl.h>
#  define HAS_IOVEC
#elif defined(_WIN32)
#  include <Windows.h>
// See http://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx
#  define SystemFunction036 NTAPI SystemFunction036
#  include <NTSecAPI.h>
#  undef SystemFunction036
#  define ssize_t SSIZE_T
#endif

struct wasi_iovec
{
    __wasi_size_t iov_base;
    __wasi_size_t iov_len;
};

static inline
uint32_t addr2offset(IM3Runtime m, void *addr) {
    return (u8*)addr - m->memory.wasmPages;
}

static inline
void *offset2addr(IM3Runtime m, uint32_t offset) {
    return m->memory.wasmPages + offset;
}

#define PREOPEN_CNT   5

typedef struct Preopen {
    int         fd;
    char*       path;
} Preopen;

Preopen preopen[PREOPEN_CNT] = {
    {  0, "<stdin>"  },
    {  1, "<stdout>" },
    {  2, "<stderr>" },
    { -1, "./"       },
    { -1, "../"      },
};

static
__wasi_errno_t errno_to_wasi(int errnum) {
    switch (errnum) {
    case EPERM:   return __WASI_EPERM;   break;
    case ENOENT:  return __WASI_ENOENT;  break;
    case ESRCH:   return __WASI_ESRCH;   break;
    case EINTR:   return __WASI_EINTR;   break;
    case EIO:     return __WASI_EIO;     break;
    case ENXIO:   return __WASI_ENXIO;   break;
    case E2BIG:   return __WASI_E2BIG;   break;
    case ENOEXEC: return __WASI_ENOEXEC; break;
    case EBADF:   return __WASI_EBADF;   break;
    case ECHILD:  return __WASI_ECHILD;  break;
    case EAGAIN:  return __WASI_EAGAIN;  break;
    case ENOMEM:  return __WASI_ENOMEM;  break;
    case EACCES:  return __WASI_EACCES;  break;
    case EFAULT:  return __WASI_EFAULT;  break;
    case EBUSY:   return __WASI_EBUSY;   break;
    case EEXIST:  return __WASI_EEXIST;  break;
    case EXDEV:   return __WASI_EXDEV;   break;
    case ENODEV:  return __WASI_ENODEV;  break;
    case ENOTDIR: return __WASI_ENOTDIR; break;
    case EISDIR:  return __WASI_EISDIR;  break;
    case EINVAL:  return __WASI_EINVAL;  break;
    case ENFILE:  return __WASI_ENFILE;  break;
    case EMFILE:  return __WASI_EMFILE;  break;
    case ENOTTY:  return __WASI_ENOTTY;  break;
    case ETXTBSY: return __WASI_ETXTBSY; break;
    case EFBIG:   return __WASI_EFBIG;   break;
    case ENOSPC:  return __WASI_ENOSPC;  break;
    case ESPIPE:  return __WASI_ESPIPE;  break;
    case EROFS:   return __WASI_EROFS;   break;
    case EMLINK:  return __WASI_EMLINK;  break;
    case EPIPE:   return __WASI_EPIPE;   break;
    case EDOM:    return __WASI_EDOM;    break;
    case ERANGE:  return __WASI_ERANGE;  break;
    default:      return __WASI_EINVAL;
    }
}

#if defined(_WIN32)

static inline
int clock_gettime(int clk_id, struct timespec *spec)
{
    __int64 wintime; GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime      -= 116444736000000000i64;           //1jan1601 to 1jan1970
    spec->tv_sec  = wintime / 10000000i64;           //seconds
    spec->tv_nsec = wintime % 10000000i64 *100;      //nano-seconds
    return 0;
}

static inline
int clock_getres(int clk_id, struct timespec *spec) {
    return -1; // Defaults to 1000000
}

static inline
int convert_clockid(__wasi_clockid_t in) {
    return 0;
}

#else // _WIN32

static inline
int convert_clockid(__wasi_clockid_t in) {
    switch (in) {
    case __WASI_CLOCK_MONOTONIC:            return CLOCK_MONOTONIC;
    case __WASI_CLOCK_PROCESS_CPUTIME_ID:   return CLOCK_PROCESS_CPUTIME_ID;
    case __WASI_CLOCK_REALTIME:             return CLOCK_REALTIME;
    case __WASI_CLOCK_THREAD_CPUTIME_ID:    return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}

#endif // _WIN32

static inline
__wasi_timestamp_t convert_timespec(const struct timespec *ts) {
    if (ts->tv_sec < 0)
        return 0;
    if ((__wasi_timestamp_t)ts->tv_sec >= UINT64_MAX / 1000000000)
        return UINT64_MAX;
    return (__wasi_timestamp_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

#if defined(HAS_IOVEC)

static
void copy_iov_to_host(struct iovec* host_iov, IM3Runtime runtime, uint32_t iov_offset, int32_t iovs_len)
{
    // Convert wasi_memory offsets to host addresses
    struct wasi_iovec *wasi_iov = offset2addr(runtime, iov_offset);
    for (int i = 0; i < iovs_len; i++) {
        host_iov[i].iov_base = offset2addr(runtime, wasi_iov[i].iov_base);
        host_iov[i].iov_len  = wasi_iov[i].iov_len;
    }
}

#endif

/*
 * WASI API implementation
 */

m3ApiRawFunction(m3_wasi_unstable_args_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (u32 *                , argv_offset)
    m3ApiGetArgMem   (u8 *                 , argv_buf_offset)

    if (runtime == NULL) { m3ApiReturn(__WASI_EINVAL); }

    for (u32 i = 0; i < runtime->argc; ++i)
    {
        argv_offset [i] = addr2offset (runtime, argv_buf_offset);
        
        size_t len = strlen (runtime->argv [i]);
        memcpy (argv_buf_offset, runtime->argv [i], len);
        argv_buf_offset += len;
        * argv_buf_offset++ = 0;
    }

    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_args_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (__wasi_size_t*       , argc)
    m3ApiGetArgMem   (__wasi_size_t*       , argv_buf_size)

    if (runtime == NULL) { m3ApiReturn(__WASI_EINVAL); }

    *argc = runtime->argc;
    *argv_buf_size = 0;
    for (int i = 0; i < runtime->argc; ++i)
    {
        * argv_buf_size += strlen (runtime->argv [i]) + 1;
    }
    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_environ_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t             , environ_ptrs_offset)
    m3ApiGetArg      (uint32_t             , environ_strs_offset)

    if (runtime == NULL) { m3ApiReturn(__WASI_EINVAL); }
    // TODO
    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_environ_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (__wasi_size_t*       , environ_count)
    m3ApiGetArgMem   (__wasi_size_t*       , environ_buf_size)

    if (runtime == NULL) { m3ApiReturn(__WASI_EINVAL); }
    // TODO
    *environ_count = 0;
    *environ_buf_size = 0;
    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_fd_prestat_dir_name)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (char*                , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)

    if (runtime == NULL) { m3ApiReturn(__WASI_EINVAL); }
    if (fd < 3 || fd >= PREOPEN_CNT) { m3ApiReturn(__WASI_EBADF); }
    int size = min(strlen(preopen[fd].path), path_len);
    memcpy(path, preopen[fd].path, size);
    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_fd_prestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (uint32_t*            , buf)  // TODO: use actual struct

    if (runtime == NULL) { m3ApiReturn(__WASI_EINVAL); }
    if (fd < 3 || fd >= PREOPEN_CNT) { m3ApiReturn(__WASI_EBADF); }
    *(buf)   = __WASI_PREOPENTYPE_DIR;
    *(buf+1) = strlen(preopen[fd].path);
    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_fd_fdstat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (__wasi_fdstat_t*     , fdstat)

    if (runtime == NULL || fdstat == NULL) { m3ApiReturn(__WASI_EINVAL); }

#ifdef _WIN32
    fdstat->fs_filetype = 0;
    fdstat->fs_flags = 0;
    fdstat->fs_rights_base = (uint64_t)-1; // all rights
    fdstat->fs_rights_inheriting = (uint64_t)-1; // all rights
    m3ApiReturn(__WASI_ESUCCESS);
#else
    struct stat fd_stat;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { m3ApiReturn(errno_to_wasi(errno)); }
    fstat(fd, &fd_stat);
    int mode = fd_stat.st_mode;
    fdstat->fs_filetype = (S_ISBLK(mode)   ? __WASI_FILETYPE_BLOCK_DEVICE     : 0) |
                          (S_ISCHR(mode)   ? __WASI_FILETYPE_CHARACTER_DEVICE : 0) |
                          (S_ISDIR(mode)   ? __WASI_FILETYPE_DIRECTORY        : 0) |
                          (S_ISREG(mode)   ? __WASI_FILETYPE_REGULAR_FILE     : 0) |
                          //(S_ISSOCK(mode)  ? __WASI_FILETYPE_SOCKET_STREAM    : 0) |
                          (S_ISLNK(mode)   ? __WASI_FILETYPE_SYMBOLIC_LINK    : 0);
    fdstat->fs_flags = ((fl & O_APPEND)    ? __WASI_FDFLAG_APPEND    : 0) |
                       //((fl & O_DSYNC)     ? __WASI_FDFLAG_DSYNC     : 0) |
                       ((fl & O_NONBLOCK)  ? __WASI_FDFLAG_NONBLOCK  : 0) |
                       //((fl & O_RSYNC)     ? __WASI_FDFLAG_RSYNC     : 0) |
                       ((fl & O_SYNC)      ? __WASI_FDFLAG_SYNC      : 0);
    fdstat->fs_rights_base = (uint64_t)-1; // all rights
    fdstat->fs_rights_inheriting = (uint64_t)-1; // all rights
    m3ApiReturn(__WASI_ESUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_unstable_fd_seek)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filedelta_t   , offset)
    m3ApiGetArg      (__wasi_whence_t      , whence)
    m3ApiGetArgMem   (__wasi_filesize_t*   , result)

    if (runtime == NULL || result == NULL) { m3ApiReturn(__WASI_EINVAL); }

    int wasi_whence = whence == __WASI_WHENCE_END ? SEEK_END :
                                __WASI_WHENCE_CUR ? SEEK_CUR : 0;
    int64_t ret;
#if defined(M3_COMPILER_MSVC)
    ret = _lseeki64(fd, offset, wasi_whence);
#else
    ret = lseek(fd, offset, wasi_whence);
#endif
    if (ret < 0) { m3ApiReturn(errno_to_wasi(errno)); }
    *result = ret;
    m3ApiReturn(__WASI_ESUCCESS);
}


m3ApiRawFunction(m3_wasi_unstable_path_open)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , dirfd)
    m3ApiGetArg      (__wasi_lookupflags_t , dirflags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (uint32_t             , path_len)
    m3ApiGetArg      (__wasi_oflags_t      , oflags)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_base)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_inheriting)
    m3ApiGetArg      (__wasi_fdflags_t     , fs_flags)
    m3ApiGetArgMem   (__wasi_fd_t *        , fd)

    if (path_len >= 512)
    {
        m3ApiReturn(__WASI_EINVAL);
    }

#ifdef _WIN32
    // TODO
#else
    // copy path so we can ensure it is NULL terminated
    char host_path [path_len+1];

    memcpy (host_path, path, path_len);
    host_path [path_len] = '\0'; // NULL terminator

    // translate o_flags and fs_flags into flags and mode
    int flags = ((oflags & __WASI_O_CREAT)             ? O_CREAT     : 0) |
                //((oflags & __WASI_O_DIRECTORY)         ? O_DIRECTORY : 0) |
                ((oflags & __WASI_O_EXCL)              ? O_EXCL      : 0) |
                ((oflags & __WASI_O_TRUNC)             ? O_TRUNC     : 0) |
                ((fs_flags & __WASI_FDFLAG_APPEND)     ? O_APPEND    : 0) |
                //((fs_flags & __WASI_FDFLAG_DSYNC)      ? O_DSYNC     : 0) |
                ((fs_flags & __WASI_FDFLAG_NONBLOCK)   ? O_NONBLOCK  : 0) |
                //((fs_flags & __WASI_FDFLAG_RSYNC)      ? O_RSYNC     : 0) |
                ((fs_flags & __WASI_FDFLAG_SYNC)       ? O_SYNC      : 0);
    if ((fs_rights_base & __WASI_RIGHT_FD_READ) &&
        (fs_rights_base & __WASI_RIGHT_FD_WRITE)) {
        flags |= O_RDWR;
    } else if ((fs_rights_base & __WASI_RIGHT_FD_WRITE)) {
        flags |= O_WRONLY;
    } else if ((fs_rights_base & __WASI_RIGHT_FD_READ)) {
        flags |= O_RDONLY; // no-op because O_RDONLY is 0
    }
    int mode = 0644;
    int host_fd = openat (dirfd, host_path, flags, mode);

    if (host_fd < 0)
    {
        m3ApiReturn(errno_to_wasi (errno));
    }
    else
    {
        * fd = host_fd;
        m3ApiReturn(__WASI_ESUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_unstable_fd_read)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (uint32_t             , iovs_offset)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArgMem   (__wasi_size_t*       , nread)

    if (runtime == NULL || nread == NULL) { m3ApiReturn(__WASI_EINVAL); }

#if defined(HAS_IOVEC)
    struct iovec iovs[iovs_len];
    copy_iov_to_host(iovs, runtime, iovs_offset, iovs_len);

    ssize_t ret = readv(fd, iovs, iovs_len);
    if (ret < 0) { m3ApiReturn(errno_to_wasi(errno)); }
    *nread = ret;
    m3ApiReturn(__WASI_ESUCCESS);
#else
    ssize_t res = 0;
    struct wasi_iovec *wasi_iov = offset2addr(runtime, iovs_offset);
    for (int i = 0; i < iovs_len; i++) {
        void* addr = offset2addr(runtime, wasi_iov[i].iov_base);
        size_t len = wasi_iov[i].iov_len;
        if (len == 0) continue;

        //fprintf(stderr, "R %d %d\n", i, len);

        int ret = _read (fd, addr, len);
        if (ret < 0) m3ApiReturn(errno_to_wasi(errno));
        res += ret;
        if (ret < len) break;
    }
    *nread = res;
    m3ApiReturn(__WASI_ESUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_unstable_fd_write)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (uint32_t             , iovs_offset)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArgMem   (__wasi_size_t*       , nwritten)

    if (runtime == NULL || nwritten == NULL) { m3ApiReturn(__WASI_EINVAL); }

#if defined(HAS_IOVEC)
    struct iovec iovs[iovs_len];
    copy_iov_to_host(iovs, runtime, iovs_offset, iovs_len);

    ssize_t ret = writev(fd, iovs, iovs_len);
    if (ret < 0) { m3ApiReturn(errno_to_wasi(errno)); }
    *nwritten = ret;
    m3ApiReturn(__WASI_ESUCCESS);
#else
    ssize_t res = 0;
    struct wasi_iovec *wasi_iov = offset2addr(runtime, iovs_offset);
    for (int i = 0; i < iovs_len; i++) {
        void* addr = offset2addr(runtime, wasi_iov[i].iov_base);
        size_t len = wasi_iov[i].iov_len;
        if (len == 0) continue;

        //fprintf(stderr, "W %d %d\n", i, len);

        int ret = _write (fd, addr, len);
        if (ret < 0) m3ApiReturn(errno_to_wasi(errno));
        res += ret;
        if (ret < len) break;
    }
    *nwritten = res;
    m3ApiReturn(__WASI_ESUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_unstable_fd_close)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

    int ret = close(fd);
    m3ApiReturn(ret == 0 ? __WASI_ESUCCESS : ret);
}

m3ApiRawFunction(m3_wasi_unstable_fd_datasync)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

#if defined(_WIN32)
    int ret = fflush(fd);
    m3ApiReturn(ret == 0 ? __WASI_ESUCCESS : ret);
#else
    int ret = fdatasync(fd);
    m3ApiReturn(ret == 0 ? __WASI_ESUCCESS : ret);
#endif
}

m3ApiRawFunction(m3_wasi_unstable_random_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint8_t*             , buf)
    m3ApiGetArg      (__wasi_size_t        , buflen)

    while (1) {
        ssize_t retlen = 0;

#if defined(__wasi__) || defined(__APPLE__) || defined(__ANDROID_API__) || defined(__OpenBSD__)
        size_t reqlen = min(buflen, 256);
        if (getentropy(buf, reqlen) < 0) {
            retlen = -1;
        } else {
            retlen = reqlen;
        }
#elif defined(__FreeBSD__) || defined(__linux__)
        retlen = getrandom(buf, buflen, 0);
#elif defined(_WIN32)
        if (RtlGenRandom(buf, buflen) == TRUE) {
            m3ApiReturn(__WASI_ESUCCESS);
        }
#else
        m3ApiReturn(__WASI_ENOSYS);
#endif
        if (retlen < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            m3ApiReturn(errno_to_wasi(errno));
        } else if (retlen == buflen) {
            m3ApiReturn(__WASI_ESUCCESS);
        } else {
            buf    += retlen;
            buflen -= retlen;
        }
    }
}

m3ApiRawFunction(m3_wasi_unstable_clock_res_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_clockid_t     , wasi_clk_id)
    m3ApiGetArgMem   (__wasi_timestamp_t*  , resolution)

    if (runtime == NULL || resolution == NULL) { m3ApiReturn(__WASI_EINVAL); }

    int clk = convert_clockid(wasi_clk_id);
    if (clk < 0) m3ApiReturn(__WASI_EINVAL);

    struct timespec tp;
    if (clock_getres(clk, &tp) != 0)
        *resolution = 1000000;
    else
        *resolution = convert_timespec(&tp);

    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_clock_time_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_clockid_t     , wasi_clk_id)
    m3ApiGetArg      (__wasi_timestamp_t   , precision)
    m3ApiGetArgMem   (__wasi_timestamp_t*  , time)

    if (runtime == NULL || time == NULL) { m3ApiReturn(__WASI_EINVAL); }

    int clk = convert_clockid(wasi_clk_id);
    if (clk < 0) m3ApiReturn(__WASI_EINVAL);

    struct timespec tp;
    if (clock_gettime(clk, &tp) != 0) {
        m3ApiReturn(errno_to_wasi(errno));
    }

    *time = convert_timespec(&tp);
    m3ApiReturn(__WASI_ESUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_proc_exit)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t, code)

    // TODO: in repl mode, trap and bail out
    if (code) {
        fprintf(stderr, M3_ARCH "-wasi: exit(%d)\n", code);
    }
    exit(code);
}


static
M3Result SuppressLookupFailure(M3Result i_result)
{
    if (i_result == c_m3Err_functionLookupFailed)
        return c_m3Err_none;
    else
        return i_result;
}


M3Result  m3_LinkWASI  (IM3Module module)
{
    M3Result result = c_m3Err_none;

    const char* namespace  = "wasi_unstable";

#ifdef _WIN32
    // TODO
#else
    // Preopen dirs
    for (int i = 3; i < PREOPEN_CNT; i++) {
        preopen[i].fd = open(preopen[i].path, O_RDONLY);
    }
#endif
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "args_sizes_get",       &m3_wasi_unstable_args_sizes_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "environ_sizes_get",    &m3_wasi_unstable_environ_sizes_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "args_get",             &m3_wasi_unstable_args_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "environ_get",          &m3_wasi_unstable_environ_get)));

_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_prestat_dir_name",  &m3_wasi_unstable_fd_prestat_dir_name)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_prestat_get",       &m3_wasi_unstable_fd_prestat_get)));

_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "path_open",            &m3_wasi_unstable_path_open)));

_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_fdstat_get",        &m3_wasi_unstable_fd_fdstat_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_write",             &m3_wasi_unstable_fd_write)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_read",              &m3_wasi_unstable_fd_read)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_seek",              &m3_wasi_unstable_fd_seek)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_datasync",          &m3_wasi_unstable_fd_datasync)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "fd_close",             &m3_wasi_unstable_fd_close)));

//_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "sock_send",            &...)));
//_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "sock_recv",            &...)));

_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "random_get",           &m3_wasi_unstable_random_get)));

_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "clock_res_get",        &m3_wasi_unstable_clock_res_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "clock_time_get",       &m3_wasi_unstable_clock_time_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespace, "proc_exit",            &m3_wasi_unstable_proc_exit)));

_catch:
    return result;
}

#else  // d_m3HasWASI

M3Result  m3_LinkWASI  (IM3Module module)
{
    return c_m3Err_none;
}

#endif // d_m3HasWASI

