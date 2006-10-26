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

#include "dict.h"
#include "glusterfs.h"
#include "transport.h"
#include "protocol.h"
#include "logging.h"
#include "xlator.h"
#include "tcp.h"

static int32_t
tcp_server_except (transport_t *this)
{
  GF_ERROR_IF_NULL (this);

  tcp_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);

  priv->connected = 0;

  int fini (struct transport *this);  
  fini (this);

  return 0;
}

int32_t
tcp_server_notify (xlator_t *xl, 
		   transport_t *trans)
{
  transport_t *this = calloc (1, sizeof (transport_t));
  this->private = calloc (1, sizeof (tcp_private_t));

  pthread_mutex_init (&((tcp_private_t *)this->private)->read_mutex, NULL);
  pthread_mutex_init (&((tcp_private_t *)this->private)->write_mutex, NULL);
  pthread_mutex_init (&((tcp_private_t *)this->private)->queue_mutex, NULL);

  GF_ERROR_IF_NULL (xl);

  trans->xl = xl;
  this->xl = xl;

  tcp_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);

  struct sockaddr_in sin;
  socklen_t addrlen = sizeof (sin);

  priv->sock = accept (priv->sock, &sin, &addrlen);
  if (priv->sock == -1) {
    gf_log ("transport: tcp: server: ", GF_LOG_ERROR, "accept() failed: %s", strerror (errno));
    return -1;
  }

  this->notify = ((tcp_private_t *)trans->private)->notify;
  priv->connected = 1;
  priv->addr = sin.sin_addr.s_addr;
  priv->port = sin.sin_port;

  priv->options = get_new_dict ();
  dict_set (priv->options, "address", 
	    str_to_data (inet_ntoa (sin.sin_addr)));

  register_transport (this, priv->sock);
  return 0;
}

struct transport_ops transport_ops = {
  .send = tcp_send,
  .recieve = tcp_recieve,

  .submit = tcp_submit,
  .except = tcp_server_except
};

int 
init (struct transport *this, 
      dict_t *options,
      int32_t (*notify) (xlator_t *xl, transport_t *trans))
{
  this->private = calloc (1, sizeof (tcp_private_t));
  ((tcp_private_t *)this->private)->notify = notify;

  this->notify = tcp_server_notify;
  struct tcp_private *priv = this->private;

  struct sockaddr_in sin;
  priv->sock = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (priv->sock == -1) {
    gf_log ("transport: tcp: server: ", GF_LOG_CRITICAL, "init: failed to create socket, error: %s", strerror (errno));
    return -1;
  }

  sin.sin_family = AF_INET;
  sin.sin_port = htons (data_to_int (dict_get (options, "listen-port")));
  char *bind_addr = dict_get (options, "bind-address");
  
  sin.sin_addr.s_addr = bind_addr ? inet_addr (bind_addr) : htonl (INADDR_ANY);

  int opt = 1;
  setsockopt (priv->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
  if (bind (priv->sock, (struct sockaddr *)&sin, sizeof (sin)) != 0) {
    gf_log ("transport: tcp: server:", GF_LOG_CRITICAL, "init: failed to bind to socket on port %d, error: %s", sin.sin_port, strerror (errno));
    return -1;
  }

  if (listen (priv->sock, 10) != 0) {
    gf_log ("transport: tcp: server: ", GF_LOG_CRITICAL, "init: listen () failed on socket, error: %s", strerror (errno));
    return -1;
  }

  pthread_mutex_init (&((tcp_private_t *)this->private)->read_mutex, NULL);
  pthread_mutex_init (&((tcp_private_t *)this->private)->write_mutex, NULL);
  pthread_mutex_init (&((tcp_private_t *)this->private)->queue_mutex, NULL);

  return 0;
}

int 
fini (struct transport *this)
{
  tcp_private_t *priv = this->private;
  this->ops->send (this);

  dict_destroy (priv->options);
  close (priv->sock);
  free (priv);
  return 0;
}