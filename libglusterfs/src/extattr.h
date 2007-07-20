/*
  (C) 2007 Z RESEARCH Inc. <http://www.zresearch.com>

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

#ifndef __EXTATTR_H__
#define __EXTATTR_H__

/* This header file exists to map Linux names for the FS extended attribute
   system calls to their FreeBSD equivalents */

#ifndef HAVE_LLISTXATTR

#define llistxattr(path,key,size)  extattr_list_link(path, EXTATTR_NAMESPACE_USER, key, size)

#define lgetxattr(path, key, value, size) extattr_get_link(path, EXTATTR_NAMESPACE_USER, key, value, size)

#define lsetxattr(path,key,value,size,flags) extattr_set_link(path, EXTATTR_NAMESPACE_USER, key, value, size)

#define lremovexattr(path,key) extattr_delete_link(path, EXTATTR_NAMESPACE_USER, key)
#endif

#endif /* __EXTATTR_H__ */