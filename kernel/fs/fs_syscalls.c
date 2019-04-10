/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/process.h>
#include <tilck/kernel/hal.h>
#include <tilck/kernel/fs/vfs.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/user.h>
#include <tilck/kernel/fault_resumable.h>
#include <tilck/kernel/syscalls.h>

#include <fcntl.h>      // system header

static inline bool is_fd_in_valid_range(u32 fd)
{
   return fd < MAX_HANDLES;
}

static u32 get_free_handle_num_ge(process_info *pi, u32 ge)
{
   ASSERT(kmutex_is_curr_task_holding_lock(&pi->fslock));

   for (u32 free_fd = ge; free_fd < MAX_HANDLES; free_fd++)
      if (!pi->handles[free_fd])
         return free_fd;

   return (u32) -1;
}

static u32 get_free_handle_num(process_info *pi)
{
   return get_free_handle_num_ge(pi, 0);
}

/*
 * Even if getting the fs_handle this way is safe now, it won't be anymore
 * after thread-support is added to the kernel. For example, a thread might
 * work with given handle while another closes it.
 *
 * TODO: introduce a ref-count in the fs_base_handle struct and function like
 * put_fs_handle() or rename both to something like acquire/release_fs_handle.
 */
fs_handle get_fs_handle(u32 fd)
{
   task_info *curr = get_curr_task();
   fs_handle handle = NULL;

   kmutex_lock(&curr->pi->fslock);

   if (is_fd_in_valid_range(fd) && curr->pi->handles[fd])
      handle = curr->pi->handles[fd];

   kmutex_unlock(&curr->pi->fslock);
   return handle;
}


int sys_open(const char *user_path, int flags, mode_t mode)
{
   int ret;
   task_info *curr = get_curr_task();
   char *orig_path = curr->args_copybuf;
   char *path = curr->args_copybuf + ARGS_COPYBUF_SIZE / 2;
   size_t written = 0;
   fs_handle h = NULL;

   STATIC_ASSERT((ARGS_COPYBUF_SIZE / 2) >= MAX_PATH);

   if ((ret = duplicate_user_path(orig_path, user_path, MAX_PATH, &written)))
      return ret;

   kmutex_lock(&curr->pi->fslock);

   if ((ret = compute_abs_path(orig_path, curr->pi->cwd, path, MAX_PATH)))
      goto end;

   u32 free_fd = get_free_handle_num(curr->pi);

   if (!is_fd_in_valid_range(free_fd))
      goto no_fds;

   if ((ret = vfs_open(path, &h, flags, mode)) < 0)
      goto end;

   ASSERT(h != NULL);

   curr->pi->handles[free_fd] = h;
   ret = (int) free_fd;

end:
   kmutex_unlock(&curr->pi->fslock);
   return ret;

no_fds:
   ret = -EMFILE;
   goto end;
}

int sys_close(int user_fd)
{
   task_info *curr = get_curr_task();
   fs_handle handle;
   int ret = 0;
   u32 fd = (u32) user_fd;

   if (!(handle = get_fs_handle(fd)))
      return -EBADF;

   vfs_close(handle);

   kmutex_lock(&curr->pi->fslock);
   {
      curr->pi->handles[fd] = NULL;
   }
   kmutex_unlock(&curr->pi->fslock);
   return ret;
}

int sys_read(int user_fd, void *user_buf, size_t count)
{
   int ret;
   task_info *curr = get_curr_task();
   fs_handle handle;
   const u32 fd = (u32) user_fd;

   handle = get_fs_handle(fd);

   if (!handle)
      return -EBADF;

   /*
    * NOTE:
    *
    * From `man 2 read`:
    *
    *    On  Linux,  read()  (and similar system calls) will transfer at most
    *    0x7ffff000 (2,147,479,552) bytes, returning the number of bytes
    *    actually transferred. (This is true on both 32-bit and 64-bit systems.)
    *
    * This means that it's perfectly fine to use `int` instead of ssize_t as
    * return type of sys_read().
    */

   count = MIN(count, IO_COPYBUF_SIZE);
   ret = (int) vfs_read(handle, curr->io_copybuf, count);

   if (ret > 0) {
      if (copy_to_user(user_buf, curr->io_copybuf, (size_t)ret) < 0) {
         // Do we have to rewind the stream in this case? It don't think so.
         ret = -EFAULT;
         goto end;
      }
   }

end:
   return ret;
}

int sys_write(int user_fd, const void *user_buf, size_t count)
{
   task_info *curr = get_curr_task();
   fs_handle handle;
   sptr ret;
   const u32 fd = (u32) user_fd;

   if (!(handle = get_fs_handle(fd)))
      return -EBADF;

   count = MIN(count, IO_COPYBUF_SIZE);
   ret = copy_from_user(curr->io_copybuf, user_buf, count);

   if (ret < 0)
      return -EFAULT;

   return (int)vfs_write(handle, (char *)curr->io_copybuf, count);
}

int sys_ioctl(int user_fd, uptr request, void *argp)
{
   const u32 fd = (u32) user_fd;
   fs_handle handle = get_fs_handle(fd);

   if (!handle)
      return -EBADF;

   return vfs_ioctl(handle, request, argp);
}

int sys_writev(int user_fd, const struct iovec *user_iov, int user_iovcnt)
{
   task_info *curr = get_curr_task();
   const u32 fd = (u32) user_fd;
   const u32 iovcnt = (u32) user_iovcnt;
   fs_handle handle;
   int rc, ret = 0;

   if (user_iovcnt <= 0)
      return -EINVAL;

   if (sizeof(struct iovec) * iovcnt > ARGS_COPYBUF_SIZE)
      return -EINVAL;

   rc = copy_from_user(curr->args_copybuf,
                       user_iov,
                       sizeof(struct iovec) * iovcnt);

   if (rc != 0)
      return -EFAULT;

   if (!(handle = get_fs_handle(fd)))
      return -EBADF;

   vfs_exlock(handle);

   const struct iovec *iov = (const struct iovec *)curr->args_copybuf;

   // TODO: avoid grabbing a lock here. In order to achieve that, it might be
   // necessary to entirely implement writev() in vfs.

   for (u32 i = 0; i < iovcnt; i++) {

      rc = sys_write(user_fd, iov[i].iov_base, iov[i].iov_len);

      if (rc < 0) {
         ret = rc;
         break;
      }

      ret += rc;

      if (rc < (sptr)iov[i].iov_len) {
         // For some reason (perfectly legit) we couldn't write the whole
         // user data (i.e. network card's buffers are full).
         break;
      }
   }

   vfs_exunlock(handle);
   return ret;
}

int sys_readv(int user_fd, const struct iovec *user_iov, int user_iovcnt)
{
   task_info *curr = get_curr_task();
   const u32 fd = (u32) user_fd;
   const u32 iovcnt = (u32) user_iovcnt;
   fs_handle handle;
   int rc, ret = 0;

   if (user_iovcnt <= 0)
      return -EINVAL;

   if (sizeof(struct iovec) * iovcnt > ARGS_COPYBUF_SIZE)
      return -EINVAL;

   rc = copy_from_user(curr->args_copybuf,
                       user_iov,
                       sizeof(struct iovec) * iovcnt);

   if (rc != 0)
      return -EFAULT;

   if (!(handle = get_fs_handle(fd)))
      return -EBADF;

   vfs_shlock(handle);

   // TODO: avoid grabbing a lock here. In order to achieve that, it might be
   // necessary to entirely implement readv() in vfs.

   const struct iovec *iov = (const struct iovec *)curr->args_copybuf;

   for (u32 i = 0; i < iovcnt; i++) {

      rc = sys_read(user_fd, iov[i].iov_base, iov[i].iov_len);

      if (rc < 0) {
         ret = rc;
         break;
      }

      ret += rc;

      if (rc < (sptr)iov[i].iov_len)
         break; // Not enough data to fill all the user buffers.
   }

   vfs_shunlock(handle);
   return ret;
}

int sys_stat64(const char *user_path, struct stat64 *user_statbuf)
{
   task_info *curr = get_curr_task();
   char *orig_path = curr->args_copybuf;
   char *path = curr->args_copybuf + ARGS_COPYBUF_SIZE / 2;
   struct stat64 statbuf;
   int rc = 0;
   fs_handle h = NULL;

   rc = copy_str_from_user(orig_path, user_path, MAX_PATH, NULL);

   if (rc < 0)
      return -EFAULT;

   if (rc > 0)
      return -ENAMETOOLONG;

   kmutex_lock(&curr->pi->fslock);
   {
      rc = compute_abs_path(orig_path, curr->pi->cwd, path, MAX_PATH);
   }
   kmutex_unlock(&curr->pi->fslock);

   if (rc < 0)
      return rc;

   if ((rc = vfs_open(path, &h, O_RDONLY, 0)) < 0)
      return rc;

   ASSERT(h != NULL);

   if ((rc = vfs_stat64(h, &statbuf)) < 0)
      goto out;

   if (copy_to_user(user_statbuf, &statbuf, sizeof(struct stat64)))
      rc = -EFAULT;

out:
   vfs_close(h);
   return rc;
}

int sys_lstat64(const char *user_path, struct stat64 *user_statbuf)
{
   /*
    * For moment, symlinks are not supported in Tilck. Therefore, make lstat()
    * behave exactly as stat().
    */

   return sys_stat64(user_path, user_statbuf);
}

int sys_readlink(const char *u_pathname, char *u_buf, size_t u_bufsize)
{
   /*
    * For moment, symlinks are not supported in Tilck. Therefore, just always
    * -EINVAL, the correct error for the case the named file is NOT a symbolic
    * link.
    */

   return -EINVAL;
}

int sys_llseek(u32 fd, size_t off_hi, size_t off_low, u64 *result, u32 whence)
{
   const s64 off64 = (s64)(((u64)off_hi << 32) | off_low);
   fs_handle handle;
   s64 new_off;

   STATIC_ASSERT(sizeof(new_off) >= sizeof(off_t));

   if (!(handle = get_fs_handle(fd)))
      return -EBADF;

   new_off = vfs_seek(handle, off64, (int)whence);

   if (new_off < 0)
      return (int) new_off; /* return back vfs_seek's error */

   if (copy_to_user(result, &new_off, sizeof(*result)))
      return -EBADF;

   return 0;
}

int sys_getdents64(int user_fd, struct linux_dirent64 *user_dirp, u32 buf_size)
{
   const u32 fd = (u32) user_fd;
   fs_handle handle;

   if (!(handle = get_fs_handle(fd)))
      return -EBADF;

   return vfs_getdents64(handle, user_dirp, buf_size);
}

int sys_access(const char *pathname, int mode)
{
   // TODO: check mode and file r/w flags.
   return 0;
}

int sys_dup2(int oldfd, int newfd)
{
   int rc;
   fs_handle old_h, new_h;
   task_info *curr = get_curr_task();

   if (!is_fd_in_valid_range((u32) oldfd))
      return -EBADF;

   if (!is_fd_in_valid_range((u32) newfd))
      return -EBADF;

   if (newfd == oldfd)
      return -EINVAL;

   kmutex_lock(&curr->pi->fslock);

   old_h = get_fs_handle((u32) oldfd);

   if (!old_h) {
      rc = -EBADF;
      goto out;
   }

   new_h = get_fs_handle((u32) newfd);

   if (new_h) {
      vfs_close(new_h);
      new_h = NULL;
   }

   rc = vfs_dup(old_h, &new_h);

   if (rc != 0)
      goto out;

   curr->pi->handles[newfd] = new_h;
   rc = newfd;

out:
   kmutex_unlock(&curr->pi->fslock);
   return rc;
}

int sys_dup(int oldfd)
{
   int rc;
   u32 free_fd;
   process_info *pi = get_curr_task()->pi;

   kmutex_lock(&pi->fslock);

   free_fd = get_free_handle_num(pi);

   if (!is_fd_in_valid_range(free_fd)) {
      rc = -EMFILE;
      goto out;
   }

   rc = sys_dup2(oldfd, (int) free_fd);

out:
   kmutex_unlock(&pi->fslock);
   return rc;
}

static void debug_print_fcntl_command(int cmd)
{
   switch (cmd) {

      case F_DUPFD:
         printk("fcntl: F_DUPFD\n");
         break;
      case F_DUPFD_CLOEXEC:
         printk("fcntl: F_DUPFD_CLOEXEC\n");
         break;
      case F_GETFD:
         printk("fcntl: F_GETFD\n");
         break;
      case F_SETFD:
         printk("fcntl: F_SETFD\n");
         break;
      case F_GETFL:
         printk("fcntl: F_GETFL\n");
         break;
      case F_SETFL:
         printk("fcntl: F_SETFL\n");
         break;
      case F_SETLK:
         printk("fcntl: F_SETLK\n");
         break;
      case F_SETLKW:
         printk("fcntl: F_SETLKW\n");
         break;
      case F_GETLK:
         printk("fcntl: F_GETLK\n");
         break;

      /* Skipping several other commands */

      default:
         printk("fcntl: unknown command\n");
   }
}

void close_cloexec_handles(process_info *pi)
{
   kmutex_lock(&pi->fslock);

   for (u32 i = 0; i < MAX_HANDLES; i++) {

      fs_handle_base *h = pi->handles[i];

      if (h && (h->fd_flags & FD_CLOEXEC)) {
         vfs_close(h);
         pi->handles[i] = NULL;
      }
   }

   kmutex_unlock(&pi->fslock);
}

int sys_fcntl64(int user_fd, int cmd, int arg)
{
   int rc = 0;
   const u32 fd = (u32) user_fd;
   task_info *curr = get_curr_task();
   fs_handle_base *hb;

   hb = get_fs_handle(fd);

   if (!hb)
      return -EBADF;

   switch (cmd) {

      case F_DUPFD:
         {
            kmutex_lock(&curr->pi->fslock);
            u32 new_fd = get_free_handle_num_ge(curr->pi, (u32)arg);
            rc = sys_dup2(user_fd, (int)new_fd);
            kmutex_unlock(&curr->pi->fslock);
            return rc;
         }

      case F_DUPFD_CLOEXEC:
         {
            kmutex_lock(&curr->pi->fslock);
            u32 new_fd = get_free_handle_num_ge(curr->pi, (u32)arg);
            rc = sys_dup2(user_fd, (int)new_fd);
            if (!rc) {
               fs_handle_base *h2 = get_fs_handle(new_fd);
               ASSERT(h2 != NULL);
               h2->fd_flags |= O_CLOEXEC;
            }
            kmutex_unlock(&curr->pi->fslock);
            return rc;
         }

      case F_SETFD:
         hb->fd_flags = arg;
         break;

      case F_GETFD:
         return hb->fd_flags;

      case F_SETFL:
         hb->fl_flags = arg;
         break;

      case F_GETFL:
         return hb->fl_flags;

      default:
         rc = vfs_fcntl(hb, cmd, arg);
   }

   if (rc == -EINVAL) {
      printk("[fcntl64] Ignored unknown cmd %d\n", cmd);
   }

   return rc;
}
