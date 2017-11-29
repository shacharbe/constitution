/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HW_COMMON_PIPELINE_H
#define HW_COMMON_PIPELINE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <rte_flow.h>
#include <rte_spinlock.h>
#include "hw-pipeline.h"
#ifdef  __cplusplus
extern "C" {
#endif

/* Enough headroom to add a vlan tag, plus an extra 2 bytes to allow IP
 * headers to be aligned on a 4-byte boundary.  */

#define NR_QUEUE   1
#define NR_PMD_THREADS 1

struct eth_addr {
   uint8_t ea[6];
};

struct flow {
    uint32_t in_port; /* Input port.*/ 
    /* L2, Order the same as in the Ethernet header! (64-bit aligned) */
    struct eth_addr dl_dst;     /* Ethernet destination address. */
    struct eth_addr dl_src;     /* Ethernet source address. */
    uint16_t dl_type;           /* Ethernet frame type.
                                   Note: This also holds the Ethertype for L3
                                   packets of type PACKET_TYPE(1, Ethertype) */
    uint8_t pad1[2];            /* Pad to 64 bits. */
    /* L3 (64-bit aligned) */
    uint32_t nw_src;            /* IPv4 source address or ARP SPA. */
    uint32_t nw_dst;            /* IPv4 destination address or ARP TPA. */
    uint32_t ipv6_label;        /* IPv6 flow label. */
    uint8_t nw_frag;            /* FLOW_FRAG_* flags. */
    uint8_t nw_tos;             /* IP ToS (including DSCP and ECN). */
    uint8_t nw_ttl;             /* IP TTL/Hop Limit. */
    uint8_t nw_proto;           /* IP protocol or low 8 bits of ARP opcode. */
    uint16_t tcp_flags;         /* TCP flags. With L3 to avoid matching L4. */
    uint8_t pad2[6];            /* Pad to 64 bits. */ 

    uint16_t tp_src;            /* TCP/UDP/SCTP source port/ICMP type. */
    uint16_t tp_dst;            /* TCP/UDP/SCTP destination port/ICMP code. */
};

enum dp_packet_source {
    DPBUF_MALLOC,              /* Obtained via malloc(). */
    DPBUF_STACK,               /* Un-movable stack space or static buffer. */
    DPBUF_STUB,                /* Starts on stack, may expand into heap. */
    DPBUF_DPDK,                /* buffer data is from DPDK allocated memory.
                                * ref to dp_packet_init_dpdk() in dp-packet.c.
                                */
};
struct dp_packet {
    struct rte_mbuf mbuf;       /* DPDK mbuf */
    enum dp_packet_source source;  /* Source of memory allocated as 'base'. */

    /* All the following elements of this struct are copied in a single call
     * of memcpy in dp_packet_clone_with_headroom. */
    uint8_t l2_pad_size;           /* Detected l2 padding size.
                                    * Padding is non-pullable. */
    uint16_t l2_5_ofs;             /* MPLS label stack offset, or UINT16_MAX */
    uint16_t l3_ofs;               /* Network-level header offset,
                                    * or UINT16_MAX. */
    uint16_t l4_ofs;               /* Transport-level header offset,
                                      or UINT16_MAX. */
    uint32_t cutlen;               /* length in bytes to cut from the end. */
    //ovs_be32 packet_type;          /* Packet type as defined in OpenFlow */
    union {
        //struct pkt_metadata md;
        uint64_t data[8];
    };
};

struct nlattr {
    uint16_t   nla_len;
    uint16_t   nla_type;
};

struct hw_pipeline_actions {
    /* These members are immutable: they do not change during the struct's
     * lifetime.  */
    unsigned int size;          /* Size of 'actions', in bytes. */
    struct nlattr *actions;     /* Sequence of ACTION_ATTR_* attributes. */
};


struct hw_pipeline_flow {
    struct flow flow;      /* Unmasked flow that created this entry. */
    /* Hash table index by unmasked flow. */
    //const struct cmap_node node; /* In owning hw_pipeline_pmd_thread's */
                                 /* 'flow_table'. */
    uint32_t ufidp;    /* Unique flow identifier. */
    unsigned pmd_id;       /* The 'core_id' of pmd thread owning this */
                                 /* flow. */

    /* Number of references.
     * The classifier owns one reference.
     * Any thread trying to keep a rule from being freed should hold its own
     * reference. */
    //struct ovs_refcount ref_cnt;

    bool dead;

    /* Statistics. */
    //struct hw_pipeline_flow_stats stats;

    /* Actions. */
    struct hw_pipeline_actions * actions;

    /* While processing a group of input packets, the datapath uses the next
     * member to store a pointer to the output batch for the flow.  It is
     * reset after the batch has been sent out (See hw_pipeline_queue_batches(),
     * packet_batch_per_flow_init() and packet_batch_per_flow_execute()). */
    //struct packet_batch_per_flow *batch;

    /* Packet classification. */
    uint32_t flow_tag;        /* In owning hw_pipeline's 'cls'. */
    /* 'cr' must be the last member. */
    uint32_t in_port;

};

struct save_hw_flows{
    //struct ovs_list node;
    struct rte_flow *hw_flow;
}save_hw_flows;

typedef struct flow_elem {
    struct hw_pipeline_flow sw_flow;
    struct rte_flow       *hw_flow_h;
    bool                  is_tunnel;
    bool                  valid;
    uint32_t              next;
    rte_spinlock_t        lock;
} flow_elem;

typedef struct msg_hw_flow{
    uint32_t               in_port;
    uint32_t               flow_tag;
    uint32_t               ufid;
}msg_hw_flow;

typedef struct msg_sw_flow{
    struct hw_pipeline_flow  sw_flow;
    struct flow            sw_flow_mask;
    uint32_t               in_port;
    int                    rxqid;
}msg_sw_flow;

typedef struct msg_queue_elem {
    union{
        msg_hw_flow    rm_flow;
        msg_sw_flow    sw_flow;
    }data;
    int    mode;
} msg_queue_elem;

typedef struct flow_tag_pool {
     uint32_t head;
     uint32_t tail;
     uint32_t pool_size;
     rte_spinlock_t lock;
     flow_elem *ft_data; // flow_elem;
}flow_tag_pool;

typedef struct msg_queue {
     int writeFd;
     int readFd;
     struct timeval tv;
     char pipeName[20];
}msg_queue;

/* Should be moved to vlib/main.h vlib_main_t */
struct hw_pipeline {    
    flow_tag_pool ft_pool;
    msg_queue 	  message_queue;
    struct pipeline_md ppl_md;
    pthread_t thread_ofload;
};

flow_elem *hw_pipeline_ft_pool_read_elem(struct hw_pipeline *dp,uint32_t handle);
void *hw_pipeline_thread(void *);
int hw_pipeline_init(void);
int hw_pipeline_uninit(void);
void hw_pipeline_dpcls_insert(struct hw_pipeline_flow *hwpipe_flow,
                              uint32_t *flow_tag,
                              uint32_t in_port,
                              struct flow *wc_masks,
                              int rxqid);

void hw_pipeline_dpcls_remove(uint32_t flow_tag);

bool hw_pipeline_dpcls_lookup(struct pipeline_md *md_tags,
                              const size_t cnt,
                              int *lookup_cnt);

struct hw_pipeline_flow *hw_pipeline_dpcls_lookup_flow(uint32_t flow_tag,
                              int *lookup_cnt);

struct set_rte_item {
    void (*set)(struct flow *, struct rte_flow_item *, size_t *,int);
};

#ifdef  __cplusplus
}
#endif

#endif /* hw-common-pipeline.h */
