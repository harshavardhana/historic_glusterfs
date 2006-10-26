/*
  (C) 2006 Gluster core team <http://www.gluster.org/>
  
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

#ifndef __TRANSPORT_H__
#define __TRANSPORT_H__

#include <inttypes.h>

struct transport_ops;
typedef struct transport transport_t;

#include "xlator.h"


typedef int32_t (*transport_event_notify_t) (int32_t fd,
					     int32_t event,
					     void *data);

typedef int32_t (*transport_register_ckb_t) (int32_t fd,
					     transport_event_notify_t fn,
					     void *data);

struct transport {
  struct transport_ops *ops;
  void *private;

  xlator_t *xl;
  int32_t (*init) (transport_t *this, dict_t *options,
		   int32_t (*notify) (xlator_t *xl, transport_t *trans));
  void (*fini) (transport_t *this);
  int32_t (*notify) (xlator_t *xl, transport_t *trans);
};

struct transport_ops {
  int32_t (*send) (transport_t *this);

  int32_t (*recieve) (transport_t *this, int8_t *buf, int32_t len);
  int32_t (*submit) (transport_t *this, int8_t *buf, int32_t len);

  int32_t (*except) (transport_t *this);
};

transport_t *transport_load (dict_t *options, xlator_t *xl,
			     int32_t (*notify) (xlator_t *xl, transport_t *trans));

int32_t transport_notify (transport_t *this);
int32_t transport_submit (transport_t *this, int8_t *buf, int32_t len);
int32_t transport_except (transport_t *this);
int32_t transport_destroy (struct transport *this);

int32_t register_transport (transport_t *new_trans, int32_t fd);

void
set_transport_register_cbk (int32_t (*fn)(int32_t fd,
					  int32_t (*hnd) (int32_t fd,
							  int32_t event,
							  void *data),
					  void *data));

#endif /* __TRANSPORT_H__ */