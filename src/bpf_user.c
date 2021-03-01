#include "features.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/xsk.h>

#include "ishoal.h"
#include "bpf_kern.skel.h"

struct bpf_kern *obj;

macaddr_t switch_mac;
ipaddr_t switch_ip;

ipaddr_t fake_gateway_ip;

int xsk_broadcast_evt_broadcast_primary;
struct broadcast_event *xsk_broadcast_evt_broadcast;

int switch_change_broadcast_primary;
struct broadcast_event *switch_change_broadcast;

__attribute__((constructor))
static void switch_change_broadcast_init(void)
{
	switch_change_broadcast_primary = eventfd(0, EFD_CLOEXEC);
	if (switch_change_broadcast_primary < 0)
		perror_exit("eventfd");

	switch_change_broadcast = broadcast_new(switch_change_broadcast_primary);

	xsk_broadcast_evt_broadcast_primary = eventfd(0, EFD_CLOEXEC);
	if (xsk_broadcast_evt_broadcast_primary < 0)
		perror_exit("eventfd");

	xsk_broadcast_evt_broadcast = broadcast_new(xsk_broadcast_evt_broadcast_primary);
}

static void close_obj(void)
{
	bpf_kern__destroy(obj);
}

static void detach_obj(void)
{
	bpf_set_link_xdp_fd(ifindex, -1, 0);
}

static void clear_map(void)
{
	for (int i = 0; i < 64; i++) {
		int key = i;
		bpf_map_delete_elem(bpf_map__fd(obj->maps.xsks_map), &key);
	}
}

void bpf_add_connection(struct connection *conn)
{
	if (bpf_map_update_elem(bpf_map__fd(obj->maps.conn_by_ip), &conn->local_ip,
				conn, BPF_ANY))
		perror_exit("bpf_map_update_elem");
	if (bpf_map_update_elem(bpf_map__fd(obj->maps.conn_by_port), &conn->local_port,
				conn, BPF_ANY))
		perror_exit("bpf_map_update_elem");
}

void bpf_delete_connection(ipaddr_t local_ip, uint16_t local_port)
{
	bpf_map_delete_elem(bpf_map__fd(obj->maps.conn_by_ip), &local_ip);
	bpf_map_delete_elem(bpf_map__fd(obj->maps.conn_by_port), &local_port);
}

static void __on_switch_change(void)
{
	if (eventfd_write(switch_change_broadcast_primary, 1))
		perror_exit("eventfd_write");
}

void bpf_set_switch_ip(ipaddr_t addr)
{
	if (switch_ip == addr)
		return;

	switch_ip = addr;
	obj->bss->switch_ip = addr;
	__on_switch_change();
}

void bpf_set_switch_mac(macaddr_t addr)
{
	if (!memcmp(switch_mac, addr, sizeof(macaddr_t)))
		return;

	memcpy(switch_mac, addr, sizeof(macaddr_t));
	memcpy(obj->bss->switch_mac, addr, sizeof(macaddr_t));
	__on_switch_change();
}

static void update_subnet_mask(void)
{
	if (fake_gateway_ip)
		obj->bss->subnet_mask = htonl(0xFFFFFF00);
	else
		obj->bss->subnet_mask = real_subnet_mask;
}

void bpf_set_fake_gateway_ip(ipaddr_t addr)
{
	if (fake_gateway_ip == addr)
		return;

	fake_gateway_ip = addr;
	obj->bss->fake_gateway_ip = addr;

	update_subnet_mask();
}

static void on_xsk_pkt(void *ptr, size_t length)
{
	if (obj->bss->switch_ip != switch_ip ||
	    memcmp(obj->bss->switch_mac, switch_mac, sizeof(macaddr_t))) {
		switch_ip = obj->bss->switch_ip;
		memcpy(switch_mac, obj->bss->switch_mac, sizeof(macaddr_t));

		__on_switch_change();
	}

	if (eventfd_write(xsk_broadcast_evt_broadcast_primary, 1))
		perror_exit("eventfd_write");

	xdpemu(ptr, length);
}

void bpf_load_thread(void *arg)
{
	obj = bpf_kern__open_and_load();
	if (!obj)
		exit(1);

	atexit(close_obj);

	obj->bss->switch_ip = switch_ip;
	memcpy(obj->bss->switch_mac, switch_mac, sizeof(macaddr_t));

	obj->bss->public_host_ip = public_host_ip;
	memcpy(obj->bss->host_mac, host_mac, sizeof(macaddr_t));
	memcpy(obj->bss->gateway_mac, gateway_mac, sizeof(macaddr_t));

	obj->bss->fake_gateway_ip = fake_gateway_ip;
	update_subnet_mask();

	if (bpf_set_link_xdp_fd(ifindex, bpf_program__fd(obj->progs.xdp_prog), 0) < 0)
		perror_exit("bpf_set_link_xdp_fd");
	atexit(detach_obj);

	for (int i = 0; i < MAX_XSKS; i++) {
		struct xsk_socket *xsk = xsk_configure_socket(iface, i, on_xsk_pkt);
		if (!xsk) {
			if (i)
				break;
			else
				perror_exit("xsk_configure_socket");
		}

		int fd = xsk_socket__fd(xsk);
		int key = i;
		if (bpf_map_update_elem(bpf_map__fd(obj->maps.xsks_map), &key, &fd, 0))
			perror_exit("bpf_map_update_elem");
	}

	atexit(clear_map);
}
