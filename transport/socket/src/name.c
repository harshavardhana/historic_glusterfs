#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>

#ifdef CLIENT_PORT_CIELING
#undef CLIENT_PORT_CIELING
#endif

#define CLIENT_PORT_CIELING 1024

#ifndef AF_INET_SDP
#define AF_INET_SDP 27
#endif

#include "transport.h"

int32_t
gf_resolve_ip6 (const char *hostname, 
		uint16_t port, 
		int family, 
		void **dnscache, 
		struct addrinfo **addr_info);

static int32_t
af_inet_bind_to_port_lt_cieling (int fd, struct sockaddr *sockaddr, 
				 socklen_t sockaddr_len, int cieling)
{
  int32_t ret = -1;
  /*  struct sockaddr_in sin = {0, }; */
  uint16_t port = cieling - 1;

  while (port)
    {
      switch (sockaddr->sa_family)
	{
	case AF_INET6:
	  ((struct sockaddr_in6 *)sockaddr)->sin6_port = htons (port);
	  break;

	case AF_INET_SDP:
	case AF_INET:
	  ((struct sockaddr_in *)sockaddr)->sin_port = htons (port);
	  break;
	}

      ret = bind (fd, sockaddr, sockaddr_len);

      if (ret == 0)
	break;

      if (ret == -1 && errno == EACCES)
	break;

      port--;
    }

  return ret;
}

static int32_t
af_unix_client_bind (transport_t *this, 
		     struct sockaddr *sockaddr, 
		     socklen_t sockaddr_len, 
		     int sock)
{
  data_t *path_data = NULL;
  struct sockaddr_un *addr = NULL;
  int32_t ret = 0;

  path_data = dict_get (this->xl->options, "bind-path");
  if (path_data) {
    char *path = data_to_str (path_data);
    if (!path || strlen (path) > UNIX_PATH_MAX) {
      gf_log (this->xl->name,
	      GF_LOG_DEBUG,
	      "bind-path not specfied for unix socket, letting connect to assign default value");
      goto err;
    }

    addr = (struct sockaddr_un *) sockaddr;
    strcpy (addr->sun_path, path);
    ret = bind (sock, (struct sockaddr *)addr, sockaddr_len);
    if (ret == -1) {
      gf_log (this->xl->name,
	      GF_LOG_ERROR,
	      "cannot bind to unix-domain socket %d (%s)", sock, strerror (errno));
      goto err;
    }
  }

 err:
  return ret;
}

static int32_t
client_fill_address_family (transport_t *this, struct sockaddr *sockaddr)
{
  data_t *address_family_data = NULL;

  address_family_data = dict_get (this->xl->options, "address-family");
  if (!address_family_data) {
    data_t *remote_host_data = NULL, *connect_path_data = NULL;
    remote_host_data = dict_get (this->xl->options, "remote-host");
    connect_path_data = dict_get (this->xl->options, "connect-path");

    if (!(remote_host_data || connect_path_data) || (remote_host_data && connect_path_data)) {
      gf_log (this->xl->name, GF_LOG_ERROR,
	      "address-family not specified and not able to determine the same from other options (remote-host:%s and connect-path:%s)", 
	      data_to_str (remote_host_data), data_to_str (connect_path_data));
      return -1;
    } 

    if (remote_host_data) {
      gf_log (this->xl->name,
	      GF_LOG_WARNING,
	      "address-family not specified, guessing it to be inet/inet6");
      sockaddr->sa_family = AF_UNSPEC;
    } else {
      gf_log (this->xl->name,
	      GF_LOG_WARNING,
	      "address-family not specified, guessing it to be unix");
      sockaddr->sa_family = AF_UNIX;
    }

  } else {
    char *address_family = data_to_str (address_family_data);
    if (!strcasecmp (address_family, "unix")) {
      sockaddr->sa_family = AF_UNIX;
    } else if (!strcasecmp (address_family, "inet")) {
      sockaddr->sa_family = AF_INET;
    } else if (!strcasecmp (address_family, "inet6")) {
      sockaddr->sa_family = AF_INET6;
    } else if (!strcasecmp (address_family, "inet-sdp")) {
      sockaddr->sa_family = AF_INET_SDP;
    } else if (!strcasecmp (address_family, "inet/inet6")
	       || !strcasecmp (address_family, "inet6/inet")) {
      sockaddr->sa_family = AF_UNSPEC;
    } else {
      gf_log (this->xl->name,
	      GF_LOG_ERROR,
	      "unknown address-family (%s) specified", address_family);
      return -1;
    }
  }

  return 0;
}

static int32_t
af_inet_client_get_remote_sockaddr (transport_t *this, 
				    struct sockaddr *sockaddr, 
				    socklen_t *sockaddr_len)
{
  dict_t *options = this->xl->options;
  data_t *remote_host_data = NULL;
  data_t *remote_port_data = NULL;
  char *remote_host = NULL;
  uint16_t remote_port = 0;
  struct addrinfo *addr_info = NULL;
  int32_t ret = 0;

  remote_host_data = dict_get (options, "remote-host");
  if (remote_host_data == NULL)
    {
      gf_log (this->xl->name, GF_LOG_ERROR,
	      "option remote-host missing in volume %s", this->xl->name);
      ret = -1;
      goto err;
    }

  remote_host = data_to_str (remote_host_data);
  if (remote_host == NULL)
    {
      gf_log (this->xl->name, GF_LOG_ERROR,
	      "option remote-host has data NULL in volume %s", this->xl->name);
      ret = -1;
      goto err;
    }

  remote_port_data = dict_get (options, "remote-port");
  if (remote_port_data == NULL)
    {
      gf_log (this->xl->name, GF_LOG_DEBUG,
	      "option remote-port missing in volume %s. Defaulting to 6996",
	      this->xl->name);

      remote_port = GF_DEFAULT_LISTEN_PORT;
    }
  else
    {
      remote_port = data_to_uint16 (remote_port_data);
    }

  if (remote_port == (uint16_t)-1)
    {
      gf_log (this->xl->name, GF_LOG_ERROR,
	      "option remote-port has invalid port in volume %s",
	      this->xl->name);
      ret = -1;
      goto err;
    }

  /* TODO: gf_resolve is a blocking call. kick in some
     non blocking dns techniques */
  ret = gf_resolve_ip6 (remote_host, remote_port,
			sockaddr->sa_family, &this->dnscache, &addr_info);
  if (ret == -1) {
    gf_log (this->xl->name, GF_LOG_ERROR,
	    "DNS resolution failed on host %s", remote_host);
    goto err;
  }

  memcpy (sockaddr, addr_info->ai_addr, addr_info->ai_addrlen);
  *sockaddr_len = addr_info->ai_addrlen;

 err:
  return ret;
}

static int32_t
af_unix_client_get_remote_sockaddr (transport_t *this, 
				    struct sockaddr *sockaddr, 
				    socklen_t *sockaddr_len)
{
  struct sockaddr_un *sockaddr_un = NULL;
  char *connect_path = NULL;
  data_t *connect_path_data = NULL;
  int32_t ret = 0;

  connect_path_data = dict_get (this->xl->options, "connect-path");
  if (!connect_path_data) {
    gf_log (this->xl->name,
	    GF_LOG_ERROR,
	    "option connect-path not specified for address-family unix");
    ret = -1;
    goto err;
  }

  connect_path = data_to_str (connect_path_data);
  if (!connect_path) {
    gf_log (this->xl->name,
	    GF_LOG_ERROR,
	    "connect-path is null-string");
    ret = -1;
    goto err;
  }

  if (strlen (connect_path) > UNIX_PATH_MAX) {
    gf_log (this->xl->name, GF_LOG_ERROR,
	    "connect-path value length %d > %d octets", 
	    strlen (connect_path), UNIX_PATH_MAX);
    ret = -1;
    goto err;
  }

  gf_log (this->xl->name,
	  GF_LOG_DEBUG,
	  "using connect-path %s", connect_path);
  sockaddr_un = (struct sockaddr_un *)sockaddr;
  strcpy (sockaddr_un->sun_path, connect_path);
  *sockaddr_len = sizeof (struct sockaddr_un);

 err:
  return ret;
}

static int32_t
af_unix_server_get_local_sockaddr (transport_t *this,
				   struct sockaddr *addr,
				   socklen_t *addr_len)
{
  data_t *listen_path_data = NULL;
  char *listen_path = NULL;
  int32_t ret = 0;
  struct sockaddr_un *sunaddr = (struct sockaddr_un *)addr;


  listen_path_data = dict_get (this->xl->options, "listen-path");
  if (!listen_path_data) {
    gf_log (this->xl->name, GF_LOG_ERROR,
	    "missing option listen-path");
    ret = -1;
    goto err;
  }

  listen_path = data_to_str (listen_path_data);

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

  if (strlen (listen_path) > UNIX_PATH_MAX) {
    gf_log (this->xl->name, GF_LOG_ERROR,
	    "option listen-path has value length %d > %d",
	    strlen (listen_path), UNIX_PATH_MAX);
    ret = -1;
    goto err;
  }

  sunaddr->sun_family = AF_UNIX;
  strcpy (sunaddr->sun_path, listen_path);
  *addr_len = sizeof (struct sockaddr_un);

 err:
  return ret;
}

static int32_t 
af_inet_server_get_local_sockaddr (transport_t *this, 
				   struct sockaddr *addr, 
				   socklen_t *addr_len)
{
  struct addrinfo hints, *res = 0;
  data_t *listen_port_data = NULL, *listen_host_data = NULL;
  uint16_t listen_port = -1;
  char service[NI_MAXSERV], *listen_host = NULL;
  dict_t *options = NULL;
  int32_t ret = 0;

  options = this->xl->options;

  listen_port_data = dict_get (options, "listen-port");
  listen_host_data = dict_get (options, "bind-address");

  if (listen_port_data)
    {
      listen_port = data_to_uint16 (listen_port_data);
    }

  if (listen_port == (uint16_t) -1)
    listen_port = GF_DEFAULT_LISTEN_PORT;


  if (listen_host_data)
    {
      listen_host = data_to_str (listen_host_data);
    }

  memset (service, 0, sizeof (service));
  sprintf (service, "%d", listen_port);

  memset (&hints, 0, sizeof (hints));
  hints.ai_family = addr->sa_family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags    = AI_ADDRCONFIG | AI_PASSIVE;

  ret = getaddrinfo(listen_host, service, &hints, &res);
  if (ret != 0) {
    gf_log (this->xl->name,
	    GF_LOG_ERROR,
	    "getaddrinfo failed (%s)", gai_strerror (ret));
    goto err;
  }

  memcpy (addr, res->ai_addr, res->ai_addrlen);
  *addr_len = res->ai_addrlen;

  freeaddrinfo (res);

 err:
  return ret;
}

int32_t 
client_bind (transport_t *this, 
	     struct sockaddr *sockaddr, 
	     socklen_t *sockaddr_len, 
	     int sock)
{
  int ret = 0;

  *sockaddr_len = sizeof (struct sockaddr_in6);
  switch (sockaddr->sa_family)
    {
    case AF_INET_SDP:
    case AF_INET:
      *sockaddr_len = sizeof (struct sockaddr_in);

    case AF_INET6:
      ret = af_inet_bind_to_port_lt_cieling (sock, sockaddr, 
					     *sockaddr_len, CLIENT_PORT_CIELING);
      if (ret == -1) {
	gf_log (this->xl->name, GF_LOG_ERROR,
		"cannot bind inet socket (%d) to port less than %d (%s)", 
		sock, CLIENT_PORT_CIELING, strerror (errno));
	ret = 0;
      }
      break;

    case AF_UNIX:
      *sockaddr_len = sizeof (struct sockaddr_un);
      ret = af_unix_client_bind (this, (struct sockaddr *)sockaddr, *sockaddr_len, sock);
      break;

    default:
      gf_log (this->xl->name,
	      GF_LOG_ERROR,
	      "unknown address family %d", sockaddr->sa_family);
      ret = -1;
      break;
    }

  return ret;
}

int32_t
client_get_remote_sockaddr (transport_t *this, 
			    struct sockaddr *sockaddr, 
			    socklen_t *sockaddr_len)
{
  int32_t ret = 0;
  char is_inet_sdp = 0;

  ret = client_fill_address_family (this, sockaddr);
  if (ret) {
    goto err;
  }
 
  switch (sockaddr->sa_family)
    {
    case AF_INET_SDP:
      sockaddr->sa_family = AF_INET;
      is_inet_sdp = 1;

    case AF_INET:
    case AF_INET6:
    case AF_UNSPEC:
      ret = af_inet_client_get_remote_sockaddr (this, sockaddr, sockaddr_len);

      if (is_inet_sdp) {
	sockaddr->sa_family = AF_INET_SDP;
      }

      break;

    case AF_UNIX:
      ret = af_unix_client_get_remote_sockaddr (this, sockaddr, sockaddr_len);
      break;

    default:
      gf_log (this->xl->name,
	      GF_LOG_ERROR,
	      "unknown address-family %d", sockaddr->sa_family);
      ret = -1;
    }
  
 err:
  return ret;
}

int32_t
server_get_local_sockaddr (transport_t *this, struct sockaddr *addr, socklen_t *addr_len)
{
  data_t *address_family_data = NULL;
  int32_t ret = 0;
  char is_inet_sdp = 0;

  address_family_data = dict_get (this->xl->options, "address-family");
  if (address_family_data) {
    char *address_family = NULL;
    address_family = data_to_str (address_family_data);

    if (!strcasecmp (address_family, "inet")) {
      addr->sa_family = AF_INET;
    } else if (!strcasecmp (address_family, "inet6")) {
      addr->sa_family = AF_INET6;
    } else if (!strcasecmp (address_family, "inet-sdp")) {
      addr->sa_family = AF_INET_SDP;
    } else if (!strcasecmp (address_family, "unix")) {
      addr->sa_family = AF_UNIX;
    } else if (!strcasecmp (address_family, "inet/inet6")
	       || !strcasecmp (address_family, "inet6/inet")) {
      addr->sa_family = AF_UNSPEC;
    } else {
      gf_log (this->xl->name,
	      GF_LOG_ERROR,
	      "unknown address family (%s) specified", address_family);
      ret = -1;
      goto err;
    }
  } else {
    gf_log (this->xl->name,
	    GF_LOG_WARNING,
	    "option address-family not specified, defaulting to inet/inet6");
    addr->sa_family = AF_UNSPEC;
  }

  switch (addr->sa_family)
    {
    case AF_INET_SDP:
      is_inet_sdp = 1;
      addr->sa_family = AF_INET;

    case AF_INET:
    case AF_INET6:
    case AF_UNSPEC:
      ret = af_inet_server_get_local_sockaddr (this, addr, addr_len);
      if (is_inet_sdp && !ret) {
	addr->sa_family = AF_INET_SDP;
      }
      break;

    case AF_UNIX:
      ret = af_unix_server_get_local_sockaddr (this, addr, addr_len);
      break;
    }

 err:
  return ret;
}

int32_t
get_transport_identifiers (transport_t *this)
{
  int32_t ret = 0;
  char is_inet_sdp = 0;
  switch (((struct sockaddr *) &this->myinfo.sockaddr)->sa_family)
    {
    case AF_INET_SDP:
      is_inet_sdp = 1;
      ((struct sockaddr *) &this->peerinfo.sockaddr)->sa_family = ((struct sockaddr *) &this->myinfo.sockaddr)->sa_family = AF_INET;

    case AF_INET:
    case AF_INET6:
      {
	char service[NI_MAXSERV], host[NI_MAXHOST];

        ret = getnameinfo((struct sockaddr *)&this->myinfo.sockaddr, 
			  this->myinfo.sockaddr_len,
			  host, sizeof (host),
			  service, sizeof (service),
			  NI_NUMERICHOST);
	if (ret != 0) {
	  gf_log (this->xl->name,
		  GF_LOG_ERROR,
		  "getnameinfo failed (%s)", gai_strerror (ret));
	  goto err;
	}
	sprintf (this->myinfo.identifier, "%s:%s", host, service);

	ret = getnameinfo ((struct sockaddr *)&this->peerinfo.sockaddr, 
			   this->peerinfo.sockaddr_len,
			   host, sizeof (host),
			   service, sizeof (service),
			   NI_NUMERICHOST);
	if (ret != 0) {
	  gf_log (this->xl->name,
		  GF_LOG_ERROR,
		  "getnameinfo failed (%s)", gai_strerror (ret));
	  goto err;
	}

	sprintf (this->peerinfo.identifier, "%s:%s", host, service);

	if (is_inet_sdp) {
	  ((struct sockaddr *) &this->peerinfo.sockaddr)->sa_family = ((struct sockaddr *) &this->myinfo.sockaddr)->sa_family = AF_INET_SDP;
	}
      }
      break;

    case AF_UNIX:
      {
	struct sockaddr_un *sunaddr = NULL;

	sunaddr = (struct sockaddr_un *) &this->myinfo.sockaddr;
	strcpy (this->myinfo.identifier, sunaddr->sun_path);

	sunaddr = (struct sockaddr_un *) &this->peerinfo.sockaddr;
	strcpy (this->peerinfo.identifier, sunaddr->sun_path);
      }
      break;

    default:
      gf_log (this->xl->name, GF_LOG_ERROR,
	      "unknown address family (%d)", 
	      ((struct sockaddr *) &this->myinfo.sockaddr)->sa_family);
      ret = -1;
      break;
    }

 err:
  return ret;
}