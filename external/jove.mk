# Copyright (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
#  
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#  
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#  
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#  
jove.%: PACKAGE_NAME=jove4.16.0.61
jove.%: LIVE_CONFIGURE_CMD=
jove.%: LIVE_BUILD_CMD=make -j 8 $(LIVE_BUILD_ENV) $(LIVE_CONFIGURE_ENV) LOCALCC=gcc
jove.%: LIVE_INSTALL_CMD=make install JOVEHOME=$1/usr; ln -sf jove $1/usr/bin/emacs