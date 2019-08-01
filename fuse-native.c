#define FUSE_USE_VERSION 29

#include "semaphore.h"

#include <uv.h>
#include <node_api.h>
#include <napi-macros.h>

#include <stdio.h>
#include <stdlib.h>

#include <fuse.h>
#include <fuse_opt.h>
#include <fuse_lowlevel.h>

#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#define FUSE_NATIVE_CALLBACK(fn, blk)           \
  napi_env env = ft->env;                       \
  napi_handle_scope scope;                      \
  napi_open_handle_scope(env, &scope);          \
  napi_value ctx;                               \
  napi_get_reference_value(env, ft->ctx, &ctx); \
  napi_value callback;                          \
  napi_get_reference_value(env, fn, &callback); \
  blk                                           \
  napi_close_handle_scope(env, scope);

#define FUSE_NATIVE_HANDLER(name, blk)                      \
  struct fuse_context *ctx = fuse_get_context();            \
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;  \
  fuse_thread_locals_t *l = get_thread_locals();            \
  l->fuse = ft;                                             \
  l->op = op_##name;                                        \
  blk                                                       \
  uv_async_send(&(l->async));                               \
  fuse_native_semaphore_wait(&(l->sem));                    \
  return l->res;

#define FUSE_METHOD(name, callbackArgs, signalArgs, signature, callBlk, callbackBlk, signalBlk)                          \
  static void fuse_native_dispatch_##name (uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) { \
    uint32_t op = op_##name;                                                                                             \
    FUSE_NATIVE_CALLBACK(ft->handlers[op], {                                                                             \
      napi_value argv[callbackArgs + 2];                                                                                         \
      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));                         \
      napi_create_uint32(env, l->op, &(argv[1]));                                                                        \
      callbackBlk                                                                                                        \
      NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, callbackArgs + 2, argv, NULL)                                                 \
    })                                                                                                                   \
  }                                                                                                                      \
  NAPI_METHOD(fuse_native_signal_##name) {                                                                            \
    NAPI_ARGV(signalArgs + 2)                                                                                            \
    NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);                                                                 \
    NAPI_ARGV_INT32(res, 1);                                                                                             \
    signalBlk                                                                                                            \
    l->res = res;                                                                                                        \
    fuse_native_semaphore_signal(&(l->sem));                                                                             \
    return NULL;                                                                                                         \
  }                                                                                                                      \
  static int fuse_native_##name signature {                                                                           \
    FUSE_NATIVE_HANDLER(name, callBlk)                                   \
  }                                                                                                                      \

// Opcodes

static const uint32_t op_init = 0;
static const uint32_t op_error = 1;
static const uint32_t op_access = 2;
static const uint32_t op_statfs = 3;
static const uint32_t op_fgetattr = 4;
static const uint32_t op_getattr = 5;
static const uint32_t op_flush = 6;
static const uint32_t op_fsync = 7;
static const uint32_t op_fsyncdir = 8;
static const uint32_t op_readdir = 9;
static const uint32_t op_truncate = 10;
static const uint32_t op_ftruncate = 11;
static const uint32_t op_utimens = 12;
static const uint32_t op_readlink = 13;
static const uint32_t op_chown = 14;
static const uint32_t op_chmod = 15;
static const uint32_t op_mknod = 16;
static const uint32_t op_setxattr = 17;
static const uint32_t op_getxattr = 18;
static const uint32_t op_listxattr = 19;
static const uint32_t op_removexattr = 20;
static const uint32_t op_open = 21;
static const uint32_t op_opendir = 22;
static const uint32_t op_read = 23;
static const uint32_t op_write = 24;
static const uint32_t op_release = 25;
static const uint32_t op_releasedir = 26;
static const uint32_t op_create = 27;
static const uint32_t op_unlink = 28;
static const uint32_t op_rename = 29;
static const uint32_t op_link = 30;
static const uint32_t op_symlink = 31;
static const uint32_t op_mkdir = 32;
static const uint32_t op_rmdir = 33;
static const uint32_t op_destroy = 34;

// Data structures

typedef struct {
  napi_env env;
  pthread_t thread;
  pthread_attr_t attr;
  napi_ref ctx;

  // Operation handlers
  napi_ref handlers[34];

  struct fuse *fuse;
  struct fuse_chan *ch;
  bool mounted;
  uv_async_t async;
} fuse_thread_t;

typedef struct {
  // Opcode
  uint32_t op;

  // Payloads
  const char *path;
  const char *dest;
  char *linkname;
  struct fuse_file_info *info;
  void *buf;
  off_t offset;
  size_t len;
  mode_t mode;
  dev_t dev;
  uid_t uid;
  gid_t gid;
  uint32_t atim[2];
  uint32_t mtim[2];
  int32_t res;

  // Extended attributes
  const char *name;
  const char *value;
  char *list;
  size_t size;
  uint32_t position;
  int flags;

  // Stat + Statfs
  struct stat *stat;
  struct statvfs *statvfs;

  // Readdir
  fuse_fill_dir_t readdir_filler;

  // Internal bookkeeping
  fuse_thread_t *fuse;
  fuse_native_semaphore_t sem;
  uv_async_t async;

} fuse_thread_locals_t;

static pthread_key_t thread_locals_key;
static fuse_thread_locals_t* get_thread_locals ();

// Helpers
// TODO: Extract into a separate file.

static void fin (napi_env env, void *fin_data, void* fin_hint) {
  printf("finaliser is run\n");
  // exit(0);
}

static void to_timespec (struct timespec* ts, uint32_t* int_ptr) {
  long unsigned int ms = *int_ptr + (*(int_ptr + 1) * 4294967296);
  ts->tv_sec = ms / 1000;
  ts->tv_nsec = (ms % 1000) * 1000000;
}

static void from_timespec(const struct timespec* ts, uint32_t* int_ptr) {
  long unsigned int ms = (ts->tv_sec * 1000) + (ts->tv_nsec / 1000000);
  *int_ptr = ms % 4294967296;
  *(int_ptr + 1) = (ms - *int_ptr) / 4294967296;
}

static void populate_stat (uint32_t *ints, struct stat* stat) {
  stat->st_mode = *ints++;
  stat->st_uid = *ints++;
  stat->st_gid = *ints++;
  stat->st_size = *ints++;
  stat->st_dev = *ints++;
  stat->st_nlink = *ints++;
  stat->st_ino = *ints++;
  stat->st_rdev = *ints++;
  stat->st_blksize = *ints++;
  stat->st_blocks = *ints++;
#ifdef __APPLE__
  to_timespec(&stat->st_atimespec, ints);
  to_timespec(&stat->st_mtimespec, ints + 2);
  to_timespec(&stat->st_ctimespec, ints + 4);
#else
  to_timespec(&stat->st_atim, ints);
  to_timespec(&stat->st_mtim, ints + 2);
  to_timespec(&stat->st_ctim, ints + 4);
#endif
}

static void populate_statvfs (uint32_t *ints, struct statvfs* statvfs) {
  statvfs->f_bsize =  *ints++;
  statvfs->f_frsize =  *ints++;
  statvfs->f_blocks =  *ints++;
  statvfs->f_bfree =  *ints++;
  statvfs->f_bavail =  *ints++;
  statvfs->f_files =  *ints++;
  statvfs->f_ffree =  *ints++;
  statvfs->f_favail =  *ints++;
  statvfs->f_fsid =  *ints++;
  statvfs->f_flag =  *ints++;
  statvfs->f_namemax =  *ints++;
}

// Methods

FUSE_METHOD(statfs, 0, 1, (struct statvfs *statvfs), {
    l->statvfs = statvfs;
  },
  {},
  {
    NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
    populate_statvfs(ints, l->statvfs);
  })

FUSE_METHOD(getattr, 1, 1, (const char *path, struct stat *stat, struct fuse_file_info *info), {
    l->path = path;
    l->stat = stat;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {
    NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
    populate_stat(ints, l->stat);
  })

FUSE_METHOD(fgetattr, 2, 1, (const char *path, struct stat *stat, struct fuse_file_info *info), {
    l->path = path;
    l->stat = stat;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {
    NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
    populate_stat(ints, l->stat);
  })

FUSE_METHOD(access, 2, 0, (const char *path, int mode), {
    l->path = path;
    l->mode = mode;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {})

FUSE_METHOD(open, 1, 1, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {
    NAPI_ARGV_INT32(fd, 2)
    if (fd != 0) {
      l->info->fh = fd;
    }
  })

FUSE_METHOD(opendir, 0, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[2]));
    } else {
      napi_create_uint32(env, 0, &(argv[2]));
    }
  },
  {
    NAPI_ARGV_INT32(fd, 2)
    if (fd != 0) {
      l->info->fh = fd;
    }
  })

FUSE_METHOD(create, 2, 1, (const char *path, mode_t mode, struct fuse_file_info *info), {
    l->path = path;
    l->mode = mode;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {
    NAPI_ARGV_INT32(fd, 2)
    if (fd != 0) {
      l->info->fh = fd;
    }
  })

FUSE_METHOD(utimens, 2, 0, (const char *path, const struct timespec tv[2]), {
    l->path = path;
    from_timespec(&tv[0], l->atim);
    from_timespec(&tv[1], l->mtim);
  },
  {
    napi_create_external_arraybuffer(env, l->atim, 2 * sizeof(uint32_t), &fin, NULL, &argv[2]);
    napi_create_external_arraybuffer(env, l->mtim, 2 * sizeof(uint32_t), &fin, NULL, &argv[3]);
  },
  {})

FUSE_METHOD(release, 2, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {})

FUSE_METHOD(releasedir, 2, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {})

FUSE_METHOD(read, 5, 1, (const char *path, const char *buf, size_t len, off_t offset, struct fuse_file_info *info), {
    l->path = path;
    l->buf = buf;
    l->len = len;
    l->offset = offset;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->info->fh, &(argv[3]));
    napi_create_external_buffer(env, l->len, l->buf, &fin, NULL, &(argv[4]));
    napi_create_uint32(env, l->len, &(argv[5]));
    napi_create_uint32(env, l->offset, &(argv[6]));
  },
  {
    // TODO: handle bytes processed?
  })

FUSE_METHOD(write, 5, 1, (const char *path, const char *buf, size_t len, off_t offset, struct fuse_file_info *info), {
    l->path = path;
    l->buf = buf;
    l->len = len;
    l->offset = offset;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->info->fh, &(argv[3]));
    napi_create_external_buffer(env, l->len, l->buf, &fin, NULL, &(argv[4]));
    napi_create_uint32(env, l->len, &(argv[5]));
    napi_create_uint32(env, l->offset, &(argv[6]));
  },
  {
    // TODO: handle bytes processed?
  })

FUSE_METHOD(readdir, 1, 2, (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *info), {
    l->buf = buf;
    l->path = path;
    l->offset = offset;
    l->info = info;
    l->readdir_filler = filler;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {
    uint32_t stats_length;
    uint32_t names_length;
    napi_get_array_length(env, argv[3], &stats_length);
    napi_get_array_length(env, argv[2], &names_length);

    napi_value raw_names = argv[2];
    napi_value raw_stats = argv[3];

    if (names_length != stats_length) {
      NAPI_FOR_EACH(raw_names, raw_name) {
        NAPI_UTF8(name, 1024, raw_name)
          int err = l->readdir_filler(l->buf, name, NULL, 0);
        if (err == 1) {
          break;
        }
      }
    } else {
      NAPI_FOR_EACH(raw_names, raw_name) {
        NAPI_UTF8(name, 1024, raw_name)
        napi_value raw_stat;
        napi_get_element(env, raw_stats, i, &raw_stat);

        NAPI_BUFFER_CAST(uint32_t*, stats_array, raw_stat);
        struct stat st;
        populate_stat(stats_array, &st);

        int err = l->readdir_filler(l->buf, name, stat, 0);
        if (err == 1) {
          break;
        }
      }
    }
  })

#ifdef __APPLE__

FUSE_METHOD(setxattr, 6, 0, (const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->flags = flags;
    l->position = position;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
    napi_create_uint32(env, l->flags, &(argv[6]));
    napi_create_uint32(env, l->position, &(argv[7]));
  },
  {})

FUSE_METHOD(getxattr, 5, 0, (const char *path, const char *name, const char *value, size_t size, uint32_t position), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->position = position;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
    napi_create_uint32(env, l->position, &(argv[6]));
  },
  {})

#else

FUSE_METHOD(setxattr, 5, 0, (const char *path, const char *name, const char *value, size_t size, int flags), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->flags = flags;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
    napi_create_uint32(env, l->flags, &(argv[6]));
  },
  {})

FUSE_METHOD(getxattr, 4, 0, (const char *path, const char *name, const char *value, size_t size), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
  },
  {})

#endif

FUSE_METHOD(listxattr, 3,  0, (const char *path, char *list, size_t size), {
    l->path = path;
    l->list = list;
    l->size = size;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_external_buffer(env, l->size, l->list, &fin, NULL, &(argv[3]));
    napi_create_uint32(env, l->size, &(argv[4]));
  },
  {})

FUSE_METHOD(removexattr, 2, 0, (const char *path, const char *name), {
    l->path = path;
    l->name = name;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(init, 0, 0, (struct fuse_conn_info *conn, struct fuse_config *cfg), {}, {}, {})

FUSE_METHOD(error, 0, 0, (), {}, {}, {})

FUSE_METHOD(flush, 2, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {})

FUSE_METHOD(fsync, 3, 0, (const char *path, int datasync, struct fuse_file_info *info), {
    l->path = path;
    l->mode = datasync;
    l->info = info;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[4]));
    }
  },
  {})

FUSE_METHOD(fsyncdir, 3, 0, (const char *path, int datasync, struct fuse_file_info *info), {
    l->path = path;
    l->mode = datasync;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[4]));
    }
  },
  {})


FUSE_METHOD(truncate, 2, 0, (const char *path, off_t size), {
    l->path = path;
    l->len = size;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->len, &(argv[3]));
  },
  {})

FUSE_METHOD(ftruncate, 2, 0, (const char *path, off_t size, struct fuse_file_info *info), {
    l->path = path;
    l->len = size;
    l->info = info;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->len, &(argv[3]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[4]));
    }
  },
  {})

FUSE_METHOD(readlink, 1, 1, (const char *path, char *linkname, size_t len), {
    l->path = path;
    l->linkname = linkname;
    l->len = len;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  }, {
    NAPI_ARGV_UTF8(linkname, 1024, 2)
    strncpy(linkname, l->linkname, l->len);
  })

FUSE_METHOD(chown, 3, 0, (const char *path, uid_t uid, gid_t gid), {
    l->path = path;
    l->uid = uid;
    l->gid = gid;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->uid, &(argv[3]));
    napi_create_uint32(env, l->gid, &(argv[4]));
  },
  {})

FUSE_METHOD(chmod, 2, 0, (const char *path, mode_t mode), {
    l->path = path;
    l->mode = mode;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {})

FUSE_METHOD(mknod, 3, 0, (const char *path, mode_t mode, dev_t dev), {
    l->path = path;
    l->mode = mode;
    l->dev = dev;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
    napi_create_uint32(env, l->dev, &(argv[4]));
  },
  {})

FUSE_METHOD(unlink, 1, 0, (const char *path), {
    l->path = path;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {})

FUSE_METHOD(rename, 3, 0, (const char *path, const char *dest, unsigned int flags), {
    l->path = path;
    l->dest = dest;
    l->flags = flags;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_uint32(env, l->flags, &(argv[4]));
  },
  {})

FUSE_METHOD(link, 2, 0, (const char *path, const char *dest), {
    l->path = path;
    l->dest = dest;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(symlink, 2, 0, (const char *path, const char *dest), {
    l->path = path;
    l->dest = dest;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(mkdir, 2, 0, (const char *path, mode_t mode), {
    l->path = path;
    l->mode = mode;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {})

FUSE_METHOD(rmdir, 1, 0, (const char *path), {
    l->path = path;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {})

FUSE_METHOD(destroy, 0, 0, (void *private_data), {}, {}, {})

// Top-level dispatcher
// TODO: Generate this with a macro

static void fuse_native_dispatch (uv_async_t* handle, int status) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;

  // TODO: Either use a function pointer (like ft->handlers[op]) or generate with a macro.
  switch (l->op) {
    case (op_init): return fuse_native_dispatch_init(handle, status, l, ft);
    case (op_statfs): return fuse_native_dispatch_statfs(handle, status, l, ft);
    case (op_fgetattr): return fuse_native_dispatch_fgetattr(handle, status, l, ft);
    case (op_getattr): return fuse_native_dispatch_getattr(handle, status, l, ft);
    case (op_readdir): return fuse_native_dispatch_readdir(handle, status, l, ft);
    case (op_open): return fuse_native_dispatch_open(handle, status, l, ft);
    case (op_create): return fuse_native_dispatch_create(handle, status, l, ft);
    case (op_access): return fuse_native_dispatch_access(handle, status, l, ft);
    case (op_utimens): return fuse_native_dispatch_utimens(handle, status, l, ft);
    case (op_release): return fuse_native_dispatch_release(handle, status, l, ft);
    case (op_releasedir): return fuse_native_dispatch_releasedir(handle, status, l, ft);
    case (op_read): return fuse_native_dispatch_read(handle, status, l, ft);
    case (op_write): return fuse_native_dispatch_write(handle, status, l, ft);
    case (op_getxattr): return fuse_native_dispatch_getxattr(handle, status, l, ft);
    case (op_setxattr): return fuse_native_dispatch_setxattr(handle, status, l, ft);
    case (op_listxattr): return fuse_native_dispatch_listxattr(handle, status, l, ft);
    case (op_removexattr): return fuse_native_dispatch_removexattr(handle, status, l, ft);
    case (op_error): return fuse_native_dispatch_error(handle, status, l, ft);
    case (op_flush): return fuse_native_dispatch_flush(handle, status, l, ft);
    case (op_fsync): return fuse_native_dispatch_fsync(handle, status, l, ft);
    case (op_fsyncdir): return fuse_native_dispatch_fsyncdir(handle, status, l, ft);
    case (op_truncate): return fuse_native_dispatch_truncate(handle, status, l, ft);
    case (op_ftruncate): return fuse_native_dispatch_ftruncate(handle, status, l, ft);
    case (op_readlink): return fuse_native_dispatch_readlink(handle, status, l, ft);
    case (op_chown): return fuse_native_dispatch_chown(handle, status, l, ft);
    case (op_chmod): return fuse_native_dispatch_chmod(handle, status, l, ft);
    case (op_mknod): return fuse_native_dispatch_mknod(handle, status, l, ft);
    case (op_opendir): return fuse_native_dispatch_opendir(handle, status, l, ft);
    case (op_unlink): return fuse_native_dispatch_unlink(handle, status, l, ft);
    case (op_rename): return fuse_native_dispatch_rename(handle, status, l, ft);
    case (op_link): return fuse_native_dispatch_link(handle, status, l, ft);
    case (op_symlink): return fuse_native_dispatch_symlink(handle, status, l, ft);
    case (op_mkdir): return fuse_native_dispatch_mkdir(handle, status, l, ft);
    case (op_rmdir): return fuse_native_dispatch_rmdir(handle, status, l, ft);
    case (op_destroy): return fuse_native_dispatch_destroy(handle, status, l, ft);
    default: return;
  }
}

static fuse_thread_locals_t* get_thread_locals () {
  void *data = pthread_getspecific(thread_locals_key);

  if (data != NULL) {
    return (fuse_thread_locals_t *) data;
  }

  fuse_thread_locals_t* l = (fuse_thread_locals_t *) malloc(sizeof(fuse_thread_locals_t));

  // TODO: mutex me??
  int err = uv_async_init(uv_default_loop(), &(l->async), (uv_async_cb) fuse_native_dispatch);

  l->async.data = l;

  if (err < 0) {
    printf("uv_async failed: %i\n", err);
    return NULL;
  }

  fuse_native_semaphore_init(&(l->sem));
  pthread_setspecific(thread_locals_key, (void *) l);

  return l;
}

static void* start_fuse_thread (void *data) {
  fuse_thread_t *ft = (fuse_thread_t *) data;
  fuse_loop_mt(ft->fuse);

  // printf("her nu\n");
  // fuse_unmount(mnt, ch);
  // fuse_session_remove_chan(ch);
  // fuse_destroy(fuse);

  return NULL;
}

NAPI_METHOD(fuse_native_mount) {
  NAPI_ARGV(11)

  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_UTF8(mntopts, 1024, 1);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 2);
  napi_create_reference(env, argv[3], 1, &(ft->ctx));

  napi_ref handlers;
  napi_create_reference(env, argv[4], 1, &handlers);

  NAPI_FOR_EACH(handlers, handler) {
    napi_create_reference(env, handler, 1, &ft->handlers[i]);
  }

  ft->env = env;

  struct fuse_operations ops = {
    .getattr = fuse_native_getattr,
    .fgetattr = fuse_native_fgetattr,
    .statfs = fuse_native_statfs,
    .readdir = fuse_native_readdir,
    .open = fuse_native_open,
    .create = fuse_native_create,
    .read = fuse_native_read,
    .write = fuse_native_write,
    .release = fuse_native_release,
    .releasedir = fuse_native_releasedir,
    .access = fuse_native_access,
    .setxattr = fuse_native_setxattr,
    .getxattr = fuse_native_getxattr,
    .listxattr = fuse_native_listxattr,
    .removexattr = fuse_native_removexattr,
    .utimens = fuse_native_utimens,
    .init = fuse_native_init,
    .flush = fuse_native_flush,
    .fsync = fuse_native_fsync,
    .fsyncdir = fuse_native_fsyncdir,
    .truncate = fuse_native_truncate,
    .ftruncate = fuse_native_ftruncate,
    .readlink = fuse_native_readlink,
    .chown = fuse_native_chown,
    .chmod = fuse_native_chmod,
    .mknod = fuse_native_mknod,
    .opendir = fuse_native_opendir,
    .unlink = fuse_native_unlink,
    .rename = fuse_native_rename,
    .link = fuse_native_link,
    .symlink = fuse_native_symlink,
    .mkdir = fuse_native_mkdir,
    .rmdir = fuse_native_rmdir,
    .destroy = fuse_native_destroy
  };

  int _argc = 2;
  char *_argv[] = {
    (char *) "fuse_bindings_dummy",
    (char *) mntopts
  };

  struct fuse_args args = FUSE_ARGS_INIT(_argc, _argv);
  struct fuse_chan *ch = fuse_mount(mnt, &args);

  if (ch == NULL) {
    napi_throw_error(env, "fuse failed", "fuse failed");
    return NULL;
  }

  struct fuse *fuse = fuse_new(ch, &args, &ops, sizeof(struct fuse_operations), ft);

  ft->fuse = fuse;
  ft->ch = ch;
  ft->mounted = true;

  if (fuse == NULL) {
    napi_throw_error(env, "fuse failed", "fuse failed");
    return NULL;
  }

  pthread_attr_init(&(ft->attr));
  pthread_create(&(ft->thread), &(ft->attr), start_fuse_thread, ft);

  return NULL;
}

NAPI_METHOD(fuse_native_unmount) {
  NAPI_ARGV(2)
  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 1);

  if (ft != NULL && ft->mounted) {
    fuse_unmount(mnt, ft->ch);
    printf("joining\n");
    pthread_join(ft->thread, NULL);
    printf("joined\n");
  }
  ft->mounted = false;

  return NULL;
}

NAPI_INIT() {
  pthread_key_create(&(thread_locals_key), NULL); // TODO: add destructor

  NAPI_EXPORT_SIZEOF(fuse_thread_t)

  NAPI_EXPORT_FUNCTION(fuse_native_mount)
  NAPI_EXPORT_FUNCTION(fuse_native_unmount)

  NAPI_EXPORT_FUNCTION(fuse_native_signal_getattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_init)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_error)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_access)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_statfs)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_fgetattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_getattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_flush)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_fsync)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_fsyncdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_readdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_truncate)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_ftruncate)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_utimens)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_readlink)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_chown)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_chmod)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_mknod)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_setxattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_getxattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_listxattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_removexattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_open)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_opendir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_read)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_write)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_release)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_releasedir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_create)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_unlink)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_rename)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_link)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_symlink)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_mkdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_rmdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_destroy)

  NAPI_EXPORT_UINT32(op_getattr)
  NAPI_EXPORT_UINT32(op_init)
  NAPI_EXPORT_UINT32(op_error)
  NAPI_EXPORT_UINT32(op_access)
  NAPI_EXPORT_UINT32(op_statfs)
  NAPI_EXPORT_UINT32(op_fgetattr)
  NAPI_EXPORT_UINT32(op_getattr)
  NAPI_EXPORT_UINT32(op_flush)
  NAPI_EXPORT_UINT32(op_fsync)
  NAPI_EXPORT_UINT32(op_fsyncdir)
  NAPI_EXPORT_UINT32(op_readdir)
  NAPI_EXPORT_UINT32(op_truncate)
  NAPI_EXPORT_UINT32(op_ftruncate)
  NAPI_EXPORT_UINT32(op_utimens)
  NAPI_EXPORT_UINT32(op_readlink)
  NAPI_EXPORT_UINT32(op_chown)
  NAPI_EXPORT_UINT32(op_chmod)
  NAPI_EXPORT_UINT32(op_mknod)
  NAPI_EXPORT_UINT32(op_setxattr)
  NAPI_EXPORT_UINT32(op_getxattr)
  NAPI_EXPORT_UINT32(op_listxattr)
  NAPI_EXPORT_UINT32(op_removexattr)
  NAPI_EXPORT_UINT32(op_open)
  NAPI_EXPORT_UINT32(op_opendir)
  NAPI_EXPORT_UINT32(op_read)
  NAPI_EXPORT_UINT32(op_write)
  NAPI_EXPORT_UINT32(op_release)
  NAPI_EXPORT_UINT32(op_releasedir)
  NAPI_EXPORT_UINT32(op_create)
  NAPI_EXPORT_UINT32(op_unlink)
  NAPI_EXPORT_UINT32(op_rename)
  NAPI_EXPORT_UINT32(op_link)
  NAPI_EXPORT_UINT32(op_symlink)
  NAPI_EXPORT_UINT32(op_mkdir)
  NAPI_EXPORT_UINT32(op_rmdir)
  NAPI_EXPORT_UINT32(op_destroy)
}


