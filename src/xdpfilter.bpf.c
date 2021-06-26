#include <assert.h>
#include <stdbool.h>

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>

#include "xdpfilter.h"
#include "pkt.h"

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__type(key, struct conntrack_key);
	__type(value, struct track_entry);
	__uint(max_entries, 1024);
} conntrack_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, MAX_XSKS);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(key_size, sizeof(ipaddr_t));
	__uint(max_entries, 1024);
	__uint(value_size, sizeof(int));
} ip_whitelist SEC(".maps");

macaddr_t host_mac;
macaddr_t gateway_mac;

ipaddr_t public_host_ip;
ipaddr_t fake_gateway_ip;

ipaddr_t subnet_mask;

char _license[] SEC("license") = "GPL";

#ifndef __BPF__
#define __BPF__ 1
#endif

#include "pkt.impl.h"
