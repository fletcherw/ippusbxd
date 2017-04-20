/* Copyright (C) 2014 Daniel Dressler and contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include "options.h"
#include "logging.h"
#include "http.h"
#include "tcp.h"
#include "usb.h"
#include "dnssd.h"

struct service_thread_param {
  struct tcp_conn_t *tcp;
  struct usb_sock_t *usb_sock;
  pthread_t thread_handle;
  int thread_num;
};

static void
sigterm_handler(int sig) {
  /* Flag that we should stop and return... */
  g_options.terminate = 1;
  NOTE("Caught signal %d, shutting down ...", sig);
}

static void *service_connection(void *arg_void)
{
  struct service_thread_param *arg =
    (struct service_thread_param *)arg_void;

  NOTE("Thread #%d: Starting", arg->thread_num);
  // classify priority
  struct usb_conn_t *usb = NULL;
  int usb_failed = 0;
  while (!arg->tcp->is_closed && usb_failed == 0 && !g_options.terminate) {
    struct http_message_t *server_msg = NULL;
    struct http_message_t *client_msg = NULL;

    // Client's request
    client_msg = http_message_new();
    if (client_msg == NULL) {
      ERR("Thread #%d: Failed to create client message", arg->thread_num);
      break;
    }
    NOTE("Thread #%d: M %p: Client msg starting",
	 arg->thread_num, client_msg);

    while (!client_msg->is_completed) {
      struct http_packet_t *pkt;
      pkt = tcp_packet_get(arg->tcp, client_msg);
      if (pkt == NULL) {
	if (arg->tcp->is_closed) {
	  NOTE("Thread #%d: M %p: Client closed connection\n",
	       arg->thread_num, client_msg);
	  goto cleanup_subconn;
	}
	ERR("Thread #%d: M %p: Got null packet from tcp",
	    arg->thread_num, client_msg);
	goto cleanup_subconn;
      }
      if (usb == NULL && arg->usb_sock != NULL) {
	usb = usb_conn_acquire(arg->usb_sock);
	if (usb == NULL) {
	  ERR("Thread #%d: M %p: Failed to acquire usb interface",
	      arg->thread_num, client_msg);
	  packet_free(pkt);
	  usb_failed = 1;
	  goto cleanup_subconn;
	}
	usb_failed = 0;
	NOTE("Thread #%d: M %p: Interface #%d: acquired usb conn",
	     arg->thread_num, client_msg,
	     usb->interface_index);
      }

      NOTE("Thread #%d: M %p P %p: Pkt from tcp (buffer size: %d)\n===\n%s===",
	   arg->thread_num, client_msg, pkt,
	   pkt->filled_size,
	   hexdump(pkt->buffer, (int)pkt->filled_size));
      // In no-printer mode we simply ignore passing the
      // client message on to the printer
      if (arg->usb_sock != NULL) {
	if (usb_conn_packet_send(usb, pkt) != 0) {
	  ERR("Thread #%d: M %p P %p: Interface #%d: Unable to send client package via USB",
	      arg->thread_num,
	      client_msg, pkt, usb->interface_index);
	  packet_free(pkt);
	  goto cleanup_subconn;
	}
	NOTE("Thread #%d: M %p P %p: Interface #%d: Client pkt done",
	     arg->thread_num,
	     client_msg, pkt, usb->interface_index);
      }
      packet_free(pkt);
    }
    if (usb != NULL)
      NOTE("Thread #%d: M %p: Interface #%d: Client msg completed\n",
	   arg->thread_num, client_msg,
	   usb->interface_index);
    else
      NOTE("Thread #%d: M %p: Client msg completed\n",
	   arg->thread_num, client_msg);
    message_free(client_msg);
    client_msg = NULL;

    // Server's response
    server_msg = http_message_new();
    if (server_msg == NULL) {
      ERR("Thread #%d: Failed to create server message",
	  arg->thread_num);
      goto cleanup_subconn;
    }
    if (usb != NULL)
      NOTE("Thread #%d: M %p: Interface #%d: Server msg starting",
	   arg->thread_num, server_msg,
	   usb->interface_index);
    else
      NOTE("Thread #%d: M %p: Server msg starting",
	   arg->thread_num, server_msg);
    while (!server_msg->is_completed) {
      struct http_packet_t *pkt;
      if (arg->usb_sock != NULL) {
	pkt = usb_conn_packet_get(usb, server_msg);
	if (pkt == NULL) {
	  usb_failed = 1;
	  goto cleanup_subconn;
	}
      } else {
	// In no-printer mode we "invent" the answer
	// of the printer, a simple HTML message as
	// a pseudo web interface
	pkt = packet_new(server_msg);
	snprintf((char*)(pkt->buffer),
		 pkt->buffer_capacity - 1,
		 "HTTP/1.1 200 OK\r\nContent-Type: text/html; name=ippusbxd.html; charset=UTF-8\r\n\r\n<html><h2>ippusbxd</h2><p>Debug/development mode without connection to IPP-over-USB printer</p></html>\r\n");
	pkt->filled_size = 183;
	// End the TCP connection, so that a
	// web browser does not wait for more data
	server_msg->is_completed = 1;
	arg->tcp->is_closed = 1;
      }

      NOTE("Thread #%d: M %p P %p: Pkt from usb (buffer size: %d)\n===\n%s===",
	   arg->thread_num, server_msg, pkt, pkt->filled_size,
	   hexdump(pkt->buffer, (int)pkt->filled_size));
      if (tcp_packet_send(arg->tcp, pkt) != 0) {
	ERR("Thread #%d: M %p P %p: Unable to send client package via TCP",
	    arg->thread_num,
	    client_msg, pkt);
	packet_free(pkt);
	goto cleanup_subconn;
      }
      if (usb != NULL)
	NOTE("Thread #%d: M %p P %p: Interface #%d: Server pkt done",
	     arg->thread_num, server_msg, pkt,
	     usb->interface_index);
      else
	NOTE("Thread #%d: M %p P %p: Server pkt done",
	     arg->thread_num, server_msg, pkt);
      packet_free(pkt);
    }
    if (usb != NULL)
      NOTE("Thread #%d: M %p: Interface #%d: Server msg completed\n",
	   arg->thread_num, server_msg,
	   usb->interface_index);
    else
      NOTE("Thread #%d: M %p: Server msg completed\n",
	   arg->thread_num, server_msg);

  cleanup_subconn:
    if (usb != NULL && (arg->tcp->is_closed || usb_failed == 1)) {
      NOTE("Thread #%d: M %p: Interface #%d: releasing usb conn",
	   arg->thread_num, server_msg, usb->interface_index);
      usb_conn_release(usb);
      usb = NULL;
    }
    if (client_msg != NULL)
      message_free(client_msg);
    if (server_msg != NULL)
      message_free(server_msg);
  }

  NOTE("Thread #%d: Closing", arg->thread_num);
  tcp_conn_close(arg->tcp);
  free(arg);
  return NULL;
}

static void start_daemon()
{
  // Capture USB device if not in no-printer mode
  struct usb_sock_t *usb_sock;

  // Termination flag
  g_options.terminate = 0;

  if (g_options.noprinter_mode == 0) {
    usb_sock = usb_open();
    if (usb_sock == NULL)
      goto cleanup_usb;
  } else
    usb_sock = NULL;

  // Capture a socket
  uint16_t desired_port = g_options.desired_port;
  struct tcp_sock_t *tcp_socket = NULL, *tcp6_socket = NULL;
  for (;;) {
    tcp_socket = tcp_open(desired_port, g_options.interface);
    tcp6_socket = tcp6_open(desired_port, g_options.interface);
    if (tcp_socket || tcp6_socket || g_options.only_desired_port)
      break;
    // Search for a free port
    desired_port ++;
    // We failed with 0 as port number or we reached the max
    // port number
    if (desired_port == 1 || desired_port == 0)
      // IANA recommendation of 49152 to 65535 for ephemeral
      // ports
      // https://en.wikipedia.org/wiki/Ephemeral_port
      desired_port = 49152;
    NOTE("Access to desired port failed, trying alternative port %d", desired_port);
  }
  if (tcp_socket == NULL && tcp6_socket == NULL)
    goto cleanup_tcp;

  if (tcp_socket)
    g_options.real_port = tcp_port_number_get(tcp_socket);
  else
    g_options.real_port = tcp_port_number_get(tcp6_socket);
  if (desired_port != 0 && g_options.only_desired_port == 1 &&
      desired_port != g_options.real_port) {
    ERR("Received port number did not match requested port number."
	" The requested port number may be too high.");
    goto cleanup_tcp;
  }
  printf("%u|", g_options.real_port);
  fflush(stdout);

  NOTE("Port: %d, IPv4 %savailable, IPv6 %savailable",
       g_options.real_port, tcp_socket ? "" : "not ", tcp6_socket ? "" : "not ");

  // Lose connection to caller
  uint16_t pid;
  if (!g_options.nofork_mode && (pid = fork()) > 0) {
    printf("%u|", pid);
    exit(0);
  }

  // Redirect SIGINT and SIGTERM so that we do a proper shutdown, unregistering
  // the printer from DNS-SD
#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGTERM, sigterm_handler);
  sigset(SIGINT, sigterm_handler);
  NOTE("Using signal handler SIGSET");
#elif defined(HAVE_SIGACTION)
  struct sigaction action; /* Actions for POSIX signals */
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  action.sa_handler = sigterm_handler;
  sigaction(SIGTERM, &action, NULL);
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGINT);
  action.sa_handler = sigterm_handler;
  sigaction(SIGINT, &action, NULL);
  NOTE("Using signal handler SIGACTION");
#else
  signal(SIGTERM, sigterm_handler);
  signal(SIGINT, sigterm_handler);
  NOTE("Using signal handler SIGNAL");
#endif /* HAVE_SIGSET */

  // Register for unplug event
  if (usb_can_callback(usb_sock))
    usb_register_callback(usb_sock);

  // DNS-SD-broadcast the printer on the local machine so
  // that cups-browsed and ippfind will discover it
  if (usb_sock && g_options.nobroadcast == 0) {
    if (dnssd_init() == -1)
      goto cleanup_tcp;
  }

  // Main loop
  int i = 0;
  while (!g_options.terminate) {
    i ++;
    struct service_thread_param *args = calloc(1, sizeof(*args));
    if (args == NULL) {
      ERR("Preparing thread #%d: Failed to alloc space for thread args",
	  i);
      goto cleanup_thread;
    }

    args->thread_num = i;
    args->usb_sock = usb_sock;

    // For each request/response round we use the socket (IPv4 or
    // IPv6) which receives data first
    args->tcp = tcp_conn_select(tcp_socket, tcp6_socket);
    if (g_options.terminate)
      goto cleanup_thread;
    if (args->tcp == NULL) {
      ERR("Preparing thread #%d: Failed to open tcp connection", i);
      goto cleanup_thread;
    }

    int status = pthread_create(&args->thread_handle, NULL,
				&service_connection, args);
    if (status) {
      ERR("Creating thread #%d: Failed to spawn thread, error %d",
	  i, status);
      goto cleanup_thread;
    }

    continue;

  cleanup_thread:
    if (args != NULL) {
      if (args->tcp != NULL)
	tcp_conn_close(args->tcp);
      free(args);
    }
    break;
  }

 cleanup_tcp:
  if (g_options.dnssd_data != NULL)
    dnssd_shutdown();

  if (tcp_socket!= NULL)
    tcp_close(tcp_socket);
  if (tcp6_socket!= NULL)
    tcp_close(tcp6_socket);
 cleanup_usb:
  if (usb_sock != NULL)
    usb_close(usb_sock);
  return;
}

static uint16_t strto16hex(const char *str)
{
  unsigned long val = strtoul(str, NULL, 16);
  if (val > UINT16_MAX)
    exit(1);
  return (uint16_t)val;
}

static uint16_t strto16dec(const char *str)
{
  unsigned long val = strtoul(str, NULL, 10);
  if (val > UINT16_MAX)
    exit(1);
  return (uint16_t)val;
}

int main(int argc, char *argv[])
{
  int c;
  int option_index = 0;
  static struct option long_options[] = {
    {"vid",          required_argument, 0,  'v' },
    {"pid",          required_argument, 0,  'm' },
    {"serial",       required_argument, 0,  's' },
    {"bus",          required_argument, 0,  'b' },
    {"device",       required_argument, 0,  'D' },
    {"bus-device",   required_argument, 0,  'X' },
    {"from-port",    required_argument, 0,  'P' },
    {"only-port",    required_argument, 0,  'p' },
    {"interface",    required_argument, 0,  'i' },
    {"logging",      no_argument,       0,  'l' },
    {"debug",        no_argument,       0,  'd' },
    {"verbose",      no_argument,       0,  'q' },
    {"no-fork",      no_argument,       0,  'n' },
    {"no-broadcast", no_argument,       0,  'B' },
    {"no-printer",   no_argument,       0,  'N' },
    {"help",         no_argument,       0,  'h' },
    {NULL,           0,                 0,  0   }
  };
  g_options.log_destination = LOGGING_STDERR;
  g_options.only_desired_port = 1;
  g_options.interface = "lo";
  g_options.serial_num = NULL;
  g_options.vendor_id = 0;
  g_options.product_id = 0;
  g_options.bus = 0;
  g_options.device = 0;

  while ((c = getopt_long(argc, argv, "qnhdp:P:i:s:lv:m:NB",
			  long_options, &option_index)) != -1) {
    switch (c) {
    case '?':
    case 'h':
      g_options.help_mode = 1;
      break;
    case 'p':
    case 'P':
      {
	long long port = 0;
	// Request specific port
	port = atoi(optarg);
	if (port < 0) {
	  ERR("Port number must be non-negative");
	  return 1;
	}
	if (port > UINT16_MAX) {
	  ERR("Port number must be %u or less, "
	      "but not negative", UINT16_MAX);
	  return 2;
	}
	g_options.desired_port = (uint16_t)port;
	if (c == 'p')
	  g_options.only_desired_port = 1;
	else
			  g_options.only_desired_port = 0;
	break;
      }
    case 'i':
      // Request a specific network interface
      g_options.interface = strdup(optarg);
      break;
    case 'l':
      g_options.log_destination = LOGGING_SYSLOG;
      break;
    case 'd':
      g_options.nofork_mode = 1;
      g_options.verbose_mode = 1;
      break;
    case 'q':
      g_options.verbose_mode = 1;
      break;
    case 'n':
      g_options.nofork_mode = 1;
      break;
    case 'v':
      g_options.vendor_id = strto16hex(optarg);
      break;
    case 'm':
      g_options.product_id = strto16hex(optarg);
      break;
    case 'b':
      g_options.bus = strto16dec(optarg);
      break;
    case 'D':
      g_options.device = strto16dec(optarg);
      break;
    case 'X':
      {
	char *p = strchr(optarg, ':');
	if (p == NULL) {
	  ERR("Bus and device must be given in the format <bus>:<device>");
	  return 3;
	}
	p ++;
	g_options.bus = strto16dec(optarg);
	g_options.device = strto16dec(p);
	break;
      }
    case 's':
      g_options.serial_num = (unsigned char *)optarg;
      break;
    case 'N':
      g_options.noprinter_mode = 1;
      break;
    case 'B':
      g_options.nobroadcast = 1;
      break;
    }
  }

  if (g_options.help_mode) {
    printf("Usage: %s -v <vendorid> -m <productid> -s <serial> -P <port>\n"
	   "       %s --bus <bus> --device <device> -P <port>\n"
	   "       %s -h\n"
	   "Options:\n"
	   "  --help\n"
	   "  -h           Show this help message\n"
	   "  --vid <vid>\n"
	   "  -v <vid>     Vendor ID of desired printer (as hexadecimal number)\n"
	   "  --pid <pid>\n"
	   "  -m <pid>     Product ID of desired printer (as hexadecimal number)\n"
	   "  --serial <serial>\n"
	   "  -s <serial>  Serial number of desired printer\n"
	   "  --bus <bus>\n"
	   "  --device <device>\n"
	   "  --bus-device <bus>:<device>\n"
	   "               USB bus and device numbers where the device is currently\n"
	   "               connected (see output of lsusb). Note that these numbers change\n"
	   "               when the device is disconnected and reconnected. This method of\n"
	   "               calling ippusbxd is only for calling via UDEV. <bus> and\n"
	   "               <device> have to be given in decimal numbers.\n"
	   "  --only-port <portnum>\n"
	   "  -p <portnum> Port number to bind against, error out if port already taken\n"
	   "  --from-port <portnum>\n"
	   "  -P <portnum> Port number to bind against, use another port if port already\n"
	   "               taken\n"
	   "  --interface <interface>\n"
	   "  -i <interface> Network interface to use. Default is the loopback interface\n"
	   "               (lo, localhost). As the loopback interface does not allow\n"
	   "               DNS-SD broadcasting with Avahi, set up the dummy interface\n"
	   "               (dummy0) for DNS-SD broadcasting.\n"
	   "  --logging\n"
	   "  -l           Redirect logging to syslog\n"
	   "  --verbose\n"
	   "  -q           Enable verbose tracing\n"
	   "  --debug\n"
	   "  -d           Debug mode for verbose output and no fork\n"
	   "  --no-fork\n"
	   "  -n           No-fork mode\n"
	   "  --no-broadcast\n"
	   "  -B           No-broadcast mode, do not DNS-SD-broadcast\n"
	   "  --no-printer\n"
	   "  -N           No-printer mode, debug/developer mode which makes ippusbxd\n"
	   "               run without IPP-over-USB printer\n"
	   , argv[0], argv[0], argv[0]);
    return 0;
  }

  start_daemon();
  return 0;
}
