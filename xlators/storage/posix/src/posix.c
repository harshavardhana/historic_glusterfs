/*
  (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#include "glusterfs.h"
#include "dict.h"
#include "logging.h"
#include "posix.h"
#include "xlator.h"
#include "lock.h"

#include <sys/time.h>

/* TODO:
   do inode_unref() in appropriate places

   if a node doesnt have proper path resolution return ENOENT
*/

#define MAKE_REAL_PATH(var, this, ino, name) do {             \
  struct posix_private *priv = this->private;                 \
  int32_t base_len = priv->base_path_length;                  \
  int32_t len = base_len + 1 + 4096, newlen = 0;              \
  var = alloca (len);                                         \
  sprintf (var, "%s", priv->base_path);                       \
  newlen = inode_path (ino, name, var + base_len, len);       \
  if (newlen > len) {                                         \
    var = alloca (newlen);                                    \
    newlen = inode_path (ino, name, var + base_len, newlen);  \
  }                                                           \
} while (0)

static int32_t
posix_lookup (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *parent,
	      const char *name)
{
  inode_t *inode = NULL;
  struct stat buf;
  char *real_path;
  int32_t op_ret;
  int32_t op_errno;

  MAKE_REAL_PATH (real_path, this, parent, name);

  op_ret = lstat (real_path, &buf);
  op_errno = errno;

  if (op_ret == 0) {
    inode = inode_update (this->itable, parent, name, buf.st_ino);
    inode_lookup (inode);
    inode_unref (inode);
  }

  STACK_UNWIND (frame, op_ret, op_errno, inode, &buf);

  return 0;
}


static int32_t
posix_forget (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode,
	      uint64_t nlookup)
{
  inode_forget (inode, nlookup);

  STACK_UNWIND (frame, 0, 0);
  return 0;
}

static int32_t
posix_getattr (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *inode)
{
  struct stat buf;
  char *real_path;
  int32_t op_ret;
  int32_t op_errno;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = lstat (real_path, &buf);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}



static int32_t 
posix_opendir (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *inode)
{
  struct stat buf;
  char *real_path;
  int32_t op_ret;
  int32_t op_errno;
  fd_t *fd = NULL;
  int32_t _fd;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  _fd = open (real_path, O_DIRECTORY|O_RDONLY);
  op_errno = errno;
  op_ret = _fd;

  if (_fd != -1) {
    //    op_ret = fstat (_fd, &buf);
    //    op_errno = errno;

    fd = calloc (1, sizeof (*fd));
    fd->inode = inode_ref (inode);
    fd->ctx = get_new_dict ();
    dict_set (fd->ctx, this->name, data_from_int32 (_fd));
  }


  STACK_UNWIND (frame, op_ret, op_errno, fd, &buf);

  return 0;
}


static int32_t
posix_readdir (call_frame_t *frame,
	       xlator_t *this,
	       size_t size,
	       off_t off,
	       fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  dir_entry_t entries = {0, };
  dir_entry_t *tmp;
  DIR *dir;
  struct dirent *dirent;
  int real_path_len;
  int entry_path_len;
  char *entry_path;
  int count = 0;

  MAKE_REAL_PATH (real_path, this, fd->inode, NULL);
  real_path_len = strlen (real_path);
  entry_path_len = real_path_len + 256;
  entry_path = calloc (entry_path_len, 1);
  strcpy (entry_path, real_path);
  entry_path[real_path_len] = '/';

  dir = opendir (real_path);
  
  if (!dir){
    gf_log (this->name, GF_LOG_DEBUG, 
	    "failed to do opendir for %s", real_path);
    STACK_UNWIND (frame, -1, errno, &entries, 0);
    return 0;
  } else {
    op_ret = 0;
    op_errno = 0;
  }

  while ((dirent = readdir (dir))) {
    if (!dirent)
      break;
    tmp = malloc (sizeof (*tmp));
    tmp->name = strdup (dirent->d_name);
    if (entry_path_len < real_path_len + 1 + strlen (tmp->name) + 1) {
      entry_path_len = real_path_len + strlen (tmp->name) + 256;
      entry_path = realloc (entry_path, entry_path_len);
    }
    strcpy (&entry_path[real_path_len+1], tmp->name);
    lstat (entry_path, &tmp->buf);
    count++;

    tmp->next = entries.next;
    entries.next = tmp;
  }
  free (entry_path);
  closedir (dir);

  STACK_UNWIND (frame, op_ret, op_errno, &entries, count);
  while (entries.next) {
    tmp = entries.next;
    entries.next = entries.next->next;
    free (tmp->name);
    free (tmp);
  }
  return 0;
}


static int32_t 
posix_releasedir (call_frame_t *frame,
		  xlator_t *this,
		  fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;


  _fd = data_to_int32 (dict_get (fd->ctx, this->name));

  op_ret = close (_fd);
  op_errno = errno;

  dict_destroy (fd->ctx);
  inode_unref (fd->inode);
  free (fd);

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_readlink (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode,
		size_t size)
{
  char *dest = alloca (size + 1);
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = readlink (real_path, dest, size);
  if (op_ret > 0) 
    dest[op_ret] = 0;
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, dest);

  return 0;
}

static int32_t 
posix_mknod (call_frame_t *frame,
	     xlator_t *this,
	     inode_t *parent,
	     const char *name,
	     mode_t mode,
	     dev_t dev)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = { 0, };
  inode_t *inode = NULL;

  MAKE_REAL_PATH (real_path, this, parent, name);

  op_ret = mknod (real_path, mode, dev);
  op_errno = errno;

  if (op_ret == 0) {
    lchown (real_path, frame->root->uid, frame->root->gid);
    lstat (real_path, &stbuf);

    inode = inode_update (this->itable, parent, name, stbuf.st_ino);
    inode_lookup (inode);
    inode_unref (inode);
  }

  STACK_UNWIND (frame, op_ret, op_errno, inode, &stbuf);

  return 0;
}

static int32_t 
posix_mkdir (call_frame_t *frame,
	     xlator_t *this,
	     inode_t *parent,
	     const char *name,
	     mode_t mode)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = {0, };
  inode_t *inode;

  MAKE_REAL_PATH (real_path, this, parent, name);

  op_ret = mkdir (real_path, mode);
  op_errno = errno;

  if (op_ret == 0) {
    chown (real_path, frame->root->uid, frame->root->gid);
    lstat (real_path, &stbuf);

    inode = inode_update (this->itable, parent, name, stbuf.st_ino);
    inode_lookup (inode);
    inode_unref (inode);
  }

  STACK_UNWIND (frame, op_ret, op_errno, inode, &stbuf);

  return 0;
}


static int32_t 
posix_unlink (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *parent,
	      const char *name)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, parent, name);

  op_ret = unlink (real_path);
  op_errno = errno;

  inode_unlink (this->itable, parent, name);

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_rmdir (call_frame_t *frame,
	     xlator_t *this,
	     inode_t *parent,
	     const char *name)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, parent, name);
  op_ret = rmdir (real_path);
  op_errno = errno;

  inode_unlink (this->itable, parent, name);

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}

static int32_t 
posix_symlink (call_frame_t *frame,
	       xlator_t *this,
	       const char *linkname,
	       inode_t *parent,
	       const char *name)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = { 0, };
  inode_t *inode = NULL;

  MAKE_REAL_PATH (real_path, this, parent, name);

  op_ret = symlink (linkname, real_path);
  op_errno = errno;

  if (op_ret == 0) {
    lchown (real_path, frame->root->uid, frame->root->gid);
    lstat (real_path, &stbuf);
    inode = inode_update (this->itable, parent, name, stbuf.st_ino);
    inode_lookup (inode);
    inode_unref (inode);
  }

  STACK_UNWIND (frame, op_ret, op_errno, inode, &stbuf);

  return 0;
}

static int32_t 
posix_rename (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *olddir,
	      const char *oldname,
	      inode_t *newdir,
	      const char *newname)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_oldpath;
  char *real_newpath;
  struct stat stbuf = {0, };
  inode_t *inode = NULL;

  MAKE_REAL_PATH (real_oldpath, this, olddir, oldname);
  MAKE_REAL_PATH (real_newpath, this, newdir, newname);

  op_ret = rename (real_oldpath, real_newpath);
  op_errno = errno;

  if (op_ret == 0) {
    lstat (real_newpath, &stbuf);
    inode_rename (this->itable, olddir, oldname,
		  newdir, newname, stbuf.st_ino);
  }
  
  STACK_UNWIND (frame, op_ret, op_errno, inode, &stbuf);

  return 0;
}

static int32_t 
posix_link (call_frame_t *frame, 
	    xlator_t *this,
	    inode_t *oldinode,
	    inode_t *newdir,
	    const char *newname)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_oldpath;
  char *real_newpath;
  struct stat stbuf = {0, };
  inode_t *inode = NULL;


  MAKE_REAL_PATH (real_oldpath, this, oldinode, NULL);
  MAKE_REAL_PATH (real_newpath, this, newdir, newname);

  op_ret = link (real_oldpath, real_newpath);
  op_errno = errno;

  if (op_ret == 0) {
    lchown (real_newpath, frame->root->uid, frame->root->gid);
    lstat (real_newpath, &stbuf);
    inode = inode_update (this->itable, newdir, newname, stbuf.st_ino);
    inode_lookup (inode);
    inode_unref (inode);
  }

  STACK_UNWIND (frame, op_ret, op_errno, inode, &stbuf);

  return 0;
}


static int32_t 
posix_chmod (call_frame_t *frame,
	     xlator_t *this,
	     inode_t *inode,
	     mode_t mode)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf;
  
  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = chmod (real_path, mode);
  op_errno = errno;

  if (op_ret == 0)
    lstat (real_path, &stbuf);

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_chown (call_frame_t *frame,
	     xlator_t *this,
	     inode_t *inode,
	     uid_t uid,
	     gid_t gid)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = lchown (real_path, uid, gid);
  op_errno = errno;

  if (op_ret == 0)
    lstat (real_path, &stbuf);

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_truncate (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode,
		off_t offset)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = truncate (real_path, offset);
  op_errno = errno;

  if (op_ret == 0)
    lstat (real_path, &stbuf);

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_utimens (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *inode,
	       struct timespec ts[2])
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = {0, };
  struct timeval tv[2];
  
  MAKE_REAL_PATH (real_path, this, inode, NULL);

  /* TODO: fix timespec to timeval converstion */
  op_ret = utimes (real_path, (struct timeval *)ts);
  op_errno = errno;

  lstat (real_path, &stbuf);

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}

static int32_t 
posix_create (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *parent,
	      const char *name,
	      int32_t flags,
	      mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  int32_t _fd;
  char *real_path;
  struct stat stbuf = {0, };
  fd_t *fd = NULL;
  inode_t *inode = NULL;

  MAKE_REAL_PATH (real_path, this, parent, name);

  if (!flags) {
    _fd = open (real_path, 
		O_CREAT|O_RDWR|O_LARGEFILE|O_EXCL,
		mode);
  } else {
    _fd = open (real_path, 
		flags|O_CREAT,
		mode);
  }

  op_errno = errno;

  if (_fd >= 0) {
    /* trigger readahead in the kernel */
#if 0
    char buf[1024 * 64];
    read (_fd, buf, 1024 * 64);
    lseek (_fd, 0, SEEK_SET);
#endif

    fd = calloc (1, sizeof (*fd));
    fd->ctx = get_new_dict ();
    dict_set (fd->ctx, this->name, data_from_int32 (_fd));

    ((struct posix_private *)this->private)->stats.nr_files++;
    op_ret = 0;

    chown (real_path, frame->root->uid, frame->root->gid);
    lstat (real_path, &stbuf);

    inode = inode_update (this->itable, parent, name, stbuf.st_ino);
    inode_lookup (inode);
    fd->inode = (inode);
  }

  STACK_UNWIND (frame, op_ret, op_errno, fd, inode, &stbuf);

  return 0;
}

static int32_t 
posix_open (call_frame_t *frame,
	    xlator_t *this,
	    inode_t *inode,
	    int32_t flags)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  char *real_path;
  int32_t _fd;
  fd_t *fd;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  _fd = open (real_path, flags, 0);
  op_errno = errno;

  if (_fd >= 0) {
    fd = calloc (1, sizeof (*fd));
    fd->ctx = get_new_dict ();
    fd->inode = inode_ref (inode);
    dict_set (fd->ctx, this->name, data_from_int32 (_fd));

    ((struct posix_private *)this->private)->stats.nr_files++;
    op_ret = 0;
  }

  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}

static int32_t 
posix_readv (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     size_t size,
	     off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  char *buf = malloc (size);
  int32_t _fd;
  struct posix_private *priv = this->private;
  dict_t *reply_dict = NULL;
  struct iovec vec;
  data_t *fd_data;

  buf[0] = '\0';

  fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF, &vec, 0);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  priv->read_value += size;
  priv->interval_read += size;

  if (lseek (_fd, offset, SEEK_SET) == -1) {
    STACK_UNWIND (frame, -1, errno, &vec, 0);
    return 0;
  }

  op_ret = read (_fd, buf, size);
  op_errno = errno;
  vec.iov_base = buf;
  vec.iov_len = op_ret;

  if (op_ret >= 0) {
    data_t *buf_data = get_new_data ();
    reply_dict = get_new_dict ();

    buf_data->data = buf;
    buf_data->len = op_ret;
    dict_set (reply_dict,
	      NULL,
	      buf_data);
    frame->root->rsp_refs = dict_ref (reply_dict);
  }

  STACK_UNWIND (frame, op_ret, op_errno, &vec, 1);

  if (reply_dict)
    dict_unref (reply_dict);
  return 0;
}


static int32_t 
posix_writev (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      struct iovec *vector,
	      int32_t count,
	      off_t offset)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct posix_private *priv = this->private;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }
  _fd = data_to_int32 (fd_data);

  if (lseek (_fd, offset, SEEK_SET) == -1) {
    STACK_UNWIND (frame, -1, errno);
    return 0;
  }

  op_ret = writev (_fd, vector, count);
  op_errno = errno;

  priv->write_value += op_ret;
  priv->interval_write += op_ret;

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_statfs (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode)

{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct statvfs buf = {0, };

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = statvfs (real_path, &buf);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, &buf);
  return 0;
}


static int32_t 
posix_flush (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  int32_t _fd;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);
  /* do nothing */

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}

static int32_t 
posix_release (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct posix_private *priv = this->private;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  

  priv->stats.nr_files--;


  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  op_ret = close (_fd);
  op_errno = errno;
  
  STACK_UNWIND (frame, op_ret, op_errno);
  inode_unref (fd->inode);
  dict_destroy (fd->ctx);
  free (fd);
  return 0;
}

static int32_t 
posix_fsync (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     int32_t datasync)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }
  _fd = data_to_int32 (fd_data);
 
  if (datasync)
    op_ret = fdatasync (_fd);
  else
    op_ret = fsync (_fd);
  op_errno = errno;
  
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}

static int32_t 
posix_setxattr (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode,
		const char *name,
		const char *value,
		size_t size,
		int flags)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  
  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = lsetxattr (real_path, name, value, size, flags);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_getxattr (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode,
		const char *name,
		size_t size)
{
  int32_t op_ret;
  int32_t op_errno;
  char *value = alloca (size);
  char *real_path;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = lgetxattr (real_path, name, value, size);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, value);
  return 0;
}


static int32_t 
posix_listxattr (call_frame_t *frame,
		 xlator_t *this,
		 inode_t *inode,
		 size_t size)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  char *list = alloca (size);

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = llistxattr (real_path, list, size);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, list);

  return 0;
}
		     
static int32_t 
posix_removexattr (call_frame_t *frame,
		   xlator_t *this,
		   inode_t *inode,
		   const char *name)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, inode, NULL);
  
  op_ret = lremovexattr (real_path, name);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}


static int32_t 
posix_fsyncdir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int datasync)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }
  _fd = data_to_int32 (fd_data);
 
  if (datasync)
    op_ret = fdatasync (_fd);
  else
    op_ret = fsync (_fd);
  op_errno = errno;
  
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_access (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode,
	      int32_t mask)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, inode, NULL);

  op_ret = access (real_path, mask);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}


static int32_t 
posix_ftruncate (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 off_t offset)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  op_ret = ftruncate (_fd, offset);
  op_errno = errno;

  fstat (_fd, &buf);

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}



static int32_t 
posix_fchown (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      uid_t uid,
	      gid_t gid)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  op_ret = fchown (_fd, uid, gid);
  op_errno = errno;

  fstat (_fd, &buf);

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}


static int32_t 
posix_fchmod (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      mode_t mode)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  op_ret = fchmod (_fd, mode);
  op_errno = errno;

  fstat (_fd, &buf);

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}



static int32_t 
posix_fgetattr (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd)
{
  int32_t _fd;
  int32_t op_ret;
  int32_t op_errno;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }
  _fd = data_to_int32 (fd_data);

  op_ret = fstat (_fd, &buf);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, &buf);
  return 0;
}


static int32_t 
posix_lk (call_frame_t *frame,
	  xlator_t *this,
	  fd_t *fd,
	  int32_t cmd,
	  struct flock *lock)
{
  struct flock nullock = {0, };
  STACK_UNWIND (frame, -1, -ENOSYS, &nullock);
  return 0;
}

static int32_t 
posix_stats (call_frame_t *frame,
	     xlator_t *this,
	     int32_t flags)

{
  int32_t op_ret = 0;
  int32_t op_errno = 0;

  struct xlator_stats xlstats, *stats = &xlstats;
  struct statvfs buf;
  struct timeval tv;
  struct posix_private *priv = (struct posix_private *)this->private;
  int64_t avg_read = 0;
  int64_t avg_write = 0;
  int64_t _time_ms = 0; 

  op_ret = statvfs (priv->base_path, &buf);
  op_errno = errno;

  stats->nr_files = priv->stats.nr_files;
  stats->nr_clients = priv->stats.nr_clients; /* client info is maintained at FSd */
  stats->free_disk = buf.f_bfree * buf.f_bsize; // Number of Free block in the filesystem.
  stats->disk_usage = (buf.f_blocks - buf.f_bavail) * buf.f_bsize;

  /* Calculate read and write usage */
  gettimeofday (&tv, NULL);
  
  /* Read */
  _time_ms = (tv.tv_sec - priv->init_time.tv_sec) * 1000 +
             ((tv.tv_usec - priv->init_time.tv_usec) / 1000);

  avg_read = (_time_ms) ? (priv->read_value / _time_ms) : 0; /* KBps */
  avg_write = (_time_ms) ? (priv->write_value / _time_ms) : 0; /* KBps */
  
  _time_ms = (tv.tv_sec - priv->prev_fetch_time.tv_sec) * 1000 +
             ((tv.tv_usec - priv->prev_fetch_time.tv_usec) / 1000);
  if (_time_ms && ((priv->interval_read / _time_ms) > priv->max_read)) {
    priv->max_read = (priv->interval_read / _time_ms);
  }
  if (_time_ms && ((priv->interval_write / _time_ms) > priv->max_write)) {
    priv->max_write = priv->interval_write / _time_ms;
  }

  stats->read_usage = avg_read / priv->max_read;
  stats->write_usage = avg_write / priv->max_write;

  gettimeofday (&(priv->prev_fetch_time), NULL);
  priv->interval_read = 0;
  priv->interval_write = 0;

  STACK_UNWIND (frame, op_ret, op_errno, stats);
  return 0;
}


int32_t 
init (xlator_t *this)
{
  struct posix_private *_private = calloc (1, sizeof (*_private));

  data_t *directory = dict_get (this->options, "directory");

  if (this->children) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "FATAL: storage/posix cannot have subvolumes");
    return -1;
  }

  if (!directory) {
    gf_log (this->name, GF_LOG_ERROR,
	    "export directory not specified in spec file");
    exit (1);
  }
  umask (000); // umask `masking' is done at the client side
  if (mkdir (directory->data, 0777) == 0) {
    gf_log (this->name, GF_LOG_WARNING,
	    "directory specified not exists, created");
  }

  _private->base_path = strdup (directory->data);
  _private->base_path_length = strlen (_private->base_path);

  {
    /* Stats related variables */
    gettimeofday (&_private->init_time, NULL);
    gettimeofday (&_private->prev_fetch_time, NULL);
    _private->max_read = 1;
    _private->max_write = 1;
  }

  this->itable = inode_table_new (0, this->name);

  this->private = (void *)_private;
  return 0;
}

void
fini (xlator_t *this)
{
  struct posix_private *priv = this->private;
  free (priv);
  return;
}

struct xlator_mops mops = {
  .stats = posix_stats,
  .lock  = mop_lock_impl,
  .unlock = mop_unlock_impl
};

struct xlator_fops fops = {
  .lookup      = posix_lookup,
  .forget      = posix_forget,
  .getattr     = posix_getattr,
  .opendir     = posix_opendir,
  .readdir     = posix_readdir,
  .releasedir  = posix_releasedir,
  .readlink    = posix_readlink,
  .mknod       = posix_mknod,
  .mkdir       = posix_mkdir,
  .unlink      = posix_unlink,
  .rmdir       = posix_rmdir,
  .symlink     = posix_symlink,
  .rename      = posix_rename,
  .link        = posix_link,
  .chmod       = posix_chmod,
  .chown       = posix_chown,
  .truncate    = posix_truncate,
  .utimens     = posix_utimens,
  .create      = posix_create,
  .open        = posix_open,
  .readv       = posix_readv,
  .writev      = posix_writev,
  .statfs      = posix_statfs,
  .flush       = posix_flush,
  .release     = posix_release,
  .fsync       = posix_fsync,
  .setxattr    = posix_setxattr,
  .getxattr    = posix_getxattr,
  .listxattr   = posix_listxattr,
  .removexattr = posix_removexattr,
  .fsyncdir    = posix_fsyncdir,
  .access      = posix_access,
  .ftruncate   = posix_ftruncate,
  .fgetattr    = posix_fgetattr,
  .lk          = posix_lk,
  .fchown      = posix_fchown,
  .fchmod      = posix_fchmod
};
