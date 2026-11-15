#include "network.h"
#include "kernel.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "ne2000.h"
#include "netif/ethernet.h"

unsigned char *network_heap;
static struct netif interface;
static bool available, servicing;
static uint8_t frame[NE2000_FRAME_MAX];
enum stream_state { UNUSED, CREATED, CONNECTING, CONNECTED, FAILED };
struct net_stream {
  enum stream_state state;
  struct tcp_pcb *pcb;
  uint8_t *rx;
  size_t head, length;
  bool eof, shut_read, shut_write;
  enum net_error terminal, pending;
  uint64_t deadline;
  struct net_address local;
};
static struct net_stream streams[NET_STREAM_LIMIT];

u32_t sys_now(void) { return (u32_t)(clock_monotonic_ns() / 1000000UL); }
static enum net_error translate(err_t e) {
  switch (e) {
  case ERR_OK:
    return NET_OK;
  case ERR_MEM:
  case ERR_BUF:
    return NET_NO_MEMORY;
  case ERR_TIMEOUT:
    return NET_TIMEOUT;
  case ERR_RTE:
    return NET_UNREACHABLE;
  case ERR_INPROGRESS:
    return NET_IN_PROGRESS;
  case ERR_VAL:
  case ERR_ARG:
    return NET_INVALID;
  case ERR_USE:
    return NET_ADDRESS_IN_USE;
  case ERR_ALREADY:
    return NET_ALREADY;
  case ERR_ISCONN:
    return NET_IS_CONNECTED;
  case ERR_CONN:
    return NET_NOT_CONNECTED;
  case ERR_ABRT:
  case ERR_RST:
    return NET_RESET;
  case ERR_CLSD:
    return NET_BROKEN_PIPE;
  case ERR_WOULDBLOCK:
    return NET_WOULD_BLOCK;
  default:
    return NET_IO;
  }
}
static bool valid(struct net_stream *s) {
  uintptr_t p = (uintptr_t)s, b = (uintptr_t)streams;
  return p >= b && p < b + sizeof streams && (p - b) % sizeof(*s) == 0 &&
         s->state != UNUSED;
}
static void detach(struct tcp_pcb *pcb) {
  tcp_arg(pcb, NULL);
  tcp_recv(pcb, NULL);
  tcp_sent(pcb, NULL);
  tcp_err(pcb, NULL);
  tcp_poll(pcb, NULL, 0);
}
static void fail(struct net_stream *s, enum net_error error) {
  if (s->terminal == NET_OK)
    s->terminal = s->pending = error;
  s->state = FAILED;
  if (s->pcb) {
    detach(s->pcb);
    tcp_abort(s->pcb);
    s->pcb = NULL;
  }
}
static err_t receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                     err_t error) {
  struct net_stream *s = arg;
  if (!p) {
    s->eof = true;
    return ERR_OK;
  }
  if (error != ERR_OK)
    return error;
  if (s->shut_read) {
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
  }
  if (p->tot_len > NET_BUFFER_SIZE - s->length)
    return ERR_MEM;
  size_t tail = (s->head + s->length) % NET_BUFFER_SIZE, n = p->tot_len;
  size_t first = NET_BUFFER_SIZE - tail;
  if (first > n)
    first = n;
  pbuf_copy_partial(p, s->rx + tail, (u16_t)first, 0);
  if (first < n)
    pbuf_copy_partial(p, s->rx, (u16_t)(n - first), (u16_t)first);
  s->length += n;
  pbuf_free(p);
  /* Advertise receive capacity only when the application actually reads. */
  return ERR_OK;
}
static void failed(void *arg, err_t error) {
  struct net_stream *s = arg;
  s->pcb = NULL;
  if (error == ERR_CLSD && s->shut_write && (s->eof || s->shut_read)) {
    s->eof = true;
    return;
  }
  enum net_error e = (s->state == CONNECTING && error == ERR_RST)
                         ? NET_REFUSED
                         : translate(error);
  if (s->terminal == NET_OK)
    s->terminal = s->pending = e;
  s->state = FAILED;
}
static err_t connected(void *arg, struct tcp_pcb *pcb, err_t error) {
  (void)pcb;
  struct net_stream *s = arg;
  if (error != ERR_OK) {
    fail(s, translate(error));
    return ERR_ABRT;
  }
  s->state = CONNECTED;
  return ERR_OK;
}
static err_t output(struct netif *n, struct pbuf *p) {
  (void)n;
  if (p->tot_len > sizeof frame)
    return ERR_BUF;
  pbuf_copy_partial(p, frame, p->tot_len, 0);
  enum net_error e = ne2000_transmit(frame, p->tot_len);
  return e == NET_OK ? ERR_OK : e == NET_WOULD_BLOCK ? ERR_WOULDBLOCK : ERR_IF;
}
static err_t interface_init(struct netif *n) {
  n->name[0] = 'e';
  n->name[1] = 'n';
  n->hwaddr_len = 6;
  n->mtu = 1500;
  n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  n->output = etharp_output;
  n->linkoutput = output;
  return ne2000_init(n->hwaddr) ? ERR_OK : ERR_IF;
}
void network_init(void) {
  if (available)
    return;
  network_heap = kmalloc(MEM_SIZE + 64);
  if (!network_heap)
    return;
  lwip_init();
  ip4_addr_t ip, mask, gateway;
  IP4_ADDR(&ip, 10, 0, 2, 15);
  IP4_ADDR(&mask, 255, 255, 255, 0);
  IP4_ADDR(&gateway, 10, 0, 2, 2);
  if (!netif_add(&interface, &ip, &mask, &gateway, NULL, interface_init,
                 ethernet_input)) {
    kfree(network_heap);
    network_heap = NULL;
    return;
  }
  netif_set_default(&interface);
  netif_set_up(&interface);
  netif_set_link_up(&interface);
  available = true;
}
bool network_available(void) { return available; }
void network_service(void) {
  if (!available || servicing)
    return;
  servicing = true;
  for (unsigned i = 0; i < 32; i++) {
    size_t n;
    enum net_error e = ne2000_receive(frame, sizeof frame, &n);
    if (e != NET_OK) {
      available = false;
      netif_set_link_down(&interface);
      for (unsigned j = 0; j < NET_STREAM_LIMIT; j++)
        if (streams[j].state != UNUSED && streams[j].state != FAILED)
          fail(&streams[j], NET_IO);
      break;
    }
    if (!n)
      break;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_RAM);
    if (!p)
      break;
    pbuf_take(p, frame, (u16_t)n);
    if (interface.input(p, &interface) != ERR_OK)
      pbuf_free(p);
  }
  /* TIME_WAIT PCBs belong to lwIP, whose timer frees them without a callback.
   */
  for (unsigned i = 0; i < NET_STREAM_LIMIT; i++)
    if (streams[i].pcb && streams[i].pcb->state == TIME_WAIT) {
      detach(streams[i].pcb);
      streams[i].pcb = NULL;
      streams[i].eof = true;
    }
  sys_check_timeouts();
  uint64_t now = clock_monotonic_ns();
  for (unsigned i = 0; i < NET_STREAM_LIMIT; i++)
    if (streams[i].state == CONNECTING && now >= streams[i].deadline)
      fail(&streams[i], NET_TIMEOUT);
  servicing = false;
}
enum net_error stream_create(struct net_stream **result) {
  if (!result)
    return NET_INVALID;
  *result = NULL;
  if (!available)
    return NET_NO_DEVICE;
  for (unsigned i = 0; i < NET_STREAM_LIMIT; i++)
    if (streams[i].state == UNUSED) {
      struct net_stream *s = &streams[i];
      memset(s, 0, sizeof *s);
      s->rx = kmalloc(NET_BUFFER_SIZE);
      if (!s->rx)
        return NET_NO_MEMORY;
      s->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
      if (!s->pcb) {
        kfree(s->rx);
        s->rx = NULL;
        return NET_NO_MEMORY;
      }
      s->state = CREATED;
      tcp_arg(s->pcb, s);
      tcp_recv(s->pcb, receive);
      tcp_err(s->pcb, failed);
      *result = s;
      return NET_OK;
    }
  return NET_NO_MEMORY;
}
enum net_error stream_connect(struct net_stream *s,
                              const struct net_address *address) {
  if (!valid(s) || !address || !address->port)
    return NET_INVALID;
  if (s->state == CONNECTING)
    return NET_ALREADY;
  if (s->state == CONNECTED)
    return NET_IS_CONNECTED;
  if (s->state == FAILED)
    return s->terminal;
  ip_addr_t remote;
  IP_ADDR4(&remote, address->ip[0], address->ip[1], address->ip[2],
           address->ip[3]);
  err_t e = tcp_bind(s->pcb, netif_ip_addr4(&interface), 0);
  if (e != ERR_OK)
    return translate(e);
  s->local = (struct net_address){{10, 0, 2, 15}, s->pcb->local_port};
  e = tcp_connect(s->pcb, &remote, address->port, connected);
  if (e != ERR_OK) {
    fail(s, translate(e));
    return s->terminal;
  }
  s->state = CONNECTING;
  s->deadline = clock_monotonic_ns() + NET_CONNECT_TIMEOUT_MS * 1000000UL;
  return NET_IN_PROGRESS;
}
enum net_error stream_local(struct net_stream *s, struct net_address *address) {
  if (!valid(s) || !address)
    return NET_INVALID;
  *address = s->local;
  return NET_OK;
}
enum net_error stream_read(struct net_stream *s, void *data, size_t n,
                           size_t *got) {
  if (!valid(s) || !got || (!data && n))
    return NET_INVALID;
  *got = 0;
  if (!n)
    return NET_OK;
  if (s->length) {
    if (n > s->length)
      n = s->length;
    size_t first = NET_BUFFER_SIZE - s->head;
    if (first > n)
      first = n;
    memcpy(data, s->rx + s->head, first);
    if (first < n)
      memcpy((uint8_t *)data + first, s->rx, n - first);
    s->head = (s->head + n) % NET_BUFFER_SIZE;
    s->length -= n;
    *got = n;
    if (s->pcb)
      tcp_recved(s->pcb, (u16_t)n);
    return NET_OK;
  }
  if (s->terminal != NET_OK)
    return s->terminal;
  if (s->eof || s->shut_read)
    return NET_OK;
  return s->state == CONNECTED || s->state == CONNECTING ? NET_WOULD_BLOCK
                                                         : NET_NOT_CONNECTED;
}
enum net_error stream_write(struct net_stream *s, const void *data, size_t n,
                            size_t *sent) {
  if (!valid(s) || !sent || (!data && n))
    return NET_INVALID;
  *sent = 0;
  if (!n)
    return NET_OK;
  if (s->terminal != NET_OK)
    return s->terminal;
  if (s->shut_write)
    return NET_BROKEN_PIPE;
  if (s->state != CONNECTED)
    return s->state == CONNECTING ? NET_WOULD_BLOCK : NET_NOT_CONNECTED;
  if (n > tcp_sndbuf(s->pcb))
    n = tcp_sndbuf(s->pcb);
  if (!n)
    return NET_WOULD_BLOCK;
  err_t e = tcp_write(s->pcb, data, (u16_t)n, TCP_WRITE_FLAG_COPY);
  if (e == ERR_MEM)
    return NET_WOULD_BLOCK;
  if (e != ERR_OK)
    return translate(e);
  *sent = n;
  tcp_output(s->pcb);
  return NET_OK;
}
enum net_error stream_shutdown(struct net_stream *s, bool read_side,
                               bool write_side) {
  if (!valid(s))
    return NET_INVALID;
  if (s->terminal != NET_OK)
    return s->terminal;
  if (s->state != CONNECTED)
    return NET_NOT_CONNECTED;
  if (read_side && !s->shut_read) {
    if (s->length)
      tcp_recved(s->pcb, (u16_t)s->length);
    s->head = s->length = 0;
    /* Retain the raw receive callback to drain ignored data and observe final
     * PCB destruction; TF_RXCLOSED would suppress that notification. */
    s->shut_read = true;
  }
  if (write_side && !s->shut_write) {
    err_t e = tcp_shutdown(s->pcb, 0, 1);
    if (e != ERR_OK)
      return translate(e);
    s->shut_write = true;
    tcp_output(s->pcb);
  }
  return NET_OK;
}
enum net_error stream_close(struct net_stream *s) {
  if (!valid(s))
    return NET_INVALID;
  enum net_error result = NET_OK;
  if (s->pcb) {
    detach(s->pcb);
    err_t e = tcp_close(s->pcb);
    if (e != ERR_OK) {
      tcp_abort(s->pcb);
      result = translate(e);
    }
  }
  kfree(s->rx);
  memset(s, 0, sizeof *s);
  return result;
}
enum net_error stream_take_error(struct net_stream *s) {
  if (!valid(s))
    return NET_INVALID;
  enum net_error e = s->pending;
  s->pending = NET_OK;
  return e;
}
bool stream_read_ready(struct net_stream *s) {
  return valid(s) && (s->length || s->eof || s->shut_read ||
                      s->terminal != NET_OK || s->state == CREATED);
}
bool stream_write_ready(struct net_stream *s) {
  return valid(s) && s->state != CONNECTING &&
         (s->state != CONNECTED || s->shut_write || tcp_sndbuf(s->pcb) > 0);
}
