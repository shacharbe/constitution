/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2014, 2016 Nicira, Inc.
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

//#include <config.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <rte_cycles.h>
#include <rte_flow.h>

#include "hw-pipeline-msgq.h"
#include "hw-pipeline-flow-tag-pool.h"
#include "hw-pipeline.h"
#include "hw-common-pipeline.h"


struct hw_pipeline dp;

struct set_rte_action {
    void (*set)(struct rte_flow_action *, uint32_t data, size_t *);
};

static int hw_pipeline_send_insert_flow(struct hw_pipeline *dp,
                                        uint32_t in_port,
                                        struct hw_pipeline_flow *flow,
                                        struct flow *masks,
                                        int rxqid);

static int hw_pipeline_remove_flow(struct hw_pipeline *dp,
                                   msg_hw_flow *ptr_rule);

static int hw_pipeline_create_external_rule(struct hw_pipeline *dp,
                                            msg_sw_flow *ptr_rule,
                                            struct set_rte_action *action,
                                            size_t action_num,
                                            struct rte_flow **hw_flow_h);

static void hw_pipeline_item_array_build(struct set_rte_item *item_any_flow,
                                         struct flow *mask,
                                         size_t *buf_size,
                                         size_t *item_num);

static inline int
hw_pipeline_rte_flow_create(struct hw_pipeline *dp,
			    struct flow *flow,
			    struct flow *mask,
			    uint32_t in_port,
			    struct set_rte_item item_op[],
			    struct set_rte_action action_op[],
			    uint32_t action_data[],
			    size_t item_op_size,
			    size_t action_op_size,
			    size_t buf_size,
			    size_t table_id,
			    struct rte_flow **hw_h);

static int hw_pipeline_send_remove_flow(struct hw_pipeline *dp,
                                        uint32_t flow_tag);

void *hw_pipeline_thread(void *);


flow_elem *hw_pipeline_ft_pool_read_elem(struct hw_pipeline *dp,
                                         uint32_t handle)
{
    uint32_t index;
    flow_elem *elem;

    if (unlikely(dp == NULL)) {
        printf("no dp pointer \n");
        return NULL;
    }

    index = APP_FLOW_TAG_INDEX_GET(handle);
    if (unlikely(index >= HW_MAX_FLOW_TAG)) {
        printf("index out of range\n");
        return NULL;
    }

    rte_spinlock_lock(&dp->ft_pool.lock);
    elem = &dp->ft_pool.ft_data[index];
    rte_spinlock_unlock(&dp->ft_pool.lock);

    return elem;
}



enum {
    ITEM_SET_MASK,
    ITEM_SET_SPEC
};

static inline void
rte_item_set_eth(struct flow *flow,
                 struct rte_flow_item *item,
                 size_t *offset,
                 int mode)
{
    struct rte_flow_item_eth *eth;

    switch (mode) {
       case ITEM_SET_MASK:
           eth = (struct rte_flow_item_eth *)item->mask;
           break;
       case ITEM_SET_SPEC:
           eth = (struct rte_flow_item_eth *)item->spec;
           break;
       default:
           return;
    }
    item->type = RTE_FLOW_ITEM_TYPE_ETH;
    *offset += sizeof(struct rte_flow_item_eth);

    memcpy(&eth->dst, &flow->dl_dst.ea[0], sizeof(eth->dst));
    memcpy(&eth->src, &flow->dl_src.ea[0], sizeof(eth->src));
}

static inline void
rte_item_set_eth_vlan(struct flow *flow,
                      struct rte_flow_item *item,
                      size_t *offset,
                      int mode)
{
    struct rte_flow_item_vlan *vlan ;

    switch (mode) {
       case ITEM_SET_MASK:
           vlan = (struct rte_flow_item_vlan *)item->mask;
           break;
       case ITEM_SET_SPEC:
          vlan = (struct rte_flow_item_vlan *)item->spec;
          break;
       default:
           return;
    }
    item->type = RTE_FLOW_ITEM_TYPE_VLAN;
    *offset += sizeof(*vlan);
    vlan->tci=0;// flow->vlans[0].tci;
    vlan->tpid=0;//flow->vlans[0].tpid;
}


static inline void
rte_item_set_ip(struct flow *flow,
                struct rte_flow_item *item,
                size_t *offset,
                int mode)
{
    struct rte_flow_item_ipv4 *ip;

    switch (mode) {
        case ITEM_SET_MASK:
            ip = (struct rte_flow_item_ipv4 *)item->mask;
            break;
        case ITEM_SET_SPEC:
          ip = (struct rte_flow_item_ipv4 *)item->spec;
          break;
        default:
          return;
    }
    item->type = RTE_FLOW_ITEM_TYPE_IPV4;
    *offset += sizeof(*ip);

    ip->hdr.src_addr = flow->nw_src;
    ip->hdr.dst_addr = flow->nw_dst;

    printf("%s - src ip: %d.%d.%d.%d dst ip: %d.%d.%d.%d\n",
          __func__,
          (ip->hdr.src_addr >> 0) & 0xff,
          (ip->hdr.src_addr >> 8) & 0xff,
          (ip->hdr.src_addr >> 16) & 0xff,
          (ip->hdr.src_addr >> 24) & 0xff,
          (ip->hdr.dst_addr >> 0) & 0xff,
          (ip->hdr.dst_addr >> 8) & 0xff,
          (ip->hdr.dst_addr >> 16) & 0xff,
          (ip->hdr.dst_addr >> 24) & 0xff);
}

static inline void
rte_item_set_udp(struct flow *flow,
                 struct rte_flow_item *item,
                 size_t *offset,
                 int mode)
{
    struct rte_flow_item_udp *udp;

    printf("rte_item_set_udp\n");
    switch (mode) {
       case ITEM_SET_MASK:
           udp = (struct rte_flow_item_udp *)item->mask;
           break;
       case ITEM_SET_SPEC:
          udp = (struct rte_flow_item_udp *)item->spec;
          break;
       default:
           return;
    }

    item->type = RTE_FLOW_ITEM_TYPE_UDP;
    *offset += sizeof(struct rte_flow_item_udp);

    udp->hdr.dst_port = flow->tp_dst;
    udp->hdr.src_port = flow->tp_src;
}


static inline void
rte_item_set_end(__attribute__ ((unused))struct flow *flow,
                 struct rte_flow_item *item,
                 __attribute__ ((unused))size_t *offset,
                 __attribute__ ((unused))int mode)
{
    item->type = RTE_FLOW_ITEM_TYPE_END;
}

static inline void
rte_action_set_mark(struct rte_flow_action *action,
                    uint32_t data,
                    size_t *offset)
{
    struct rte_flow_action_mark *mark =
                (struct rte_flow_action_mark *)action->conf;
    action->type = RTE_FLOW_ACTION_TYPE_MARK;
    *offset += sizeof(*mark);
    mark->id = data;
}

static inline void
rte_action_set_queue(struct rte_flow_action *action,
                     uint32_t data,
                     size_t *offset)
{
    struct rte_flow_action_queue *queue =
            (struct rte_flow_action_queue *)action->conf;
    action->type = RTE_FLOW_ACTION_TYPE_QUEUE;
    *offset += sizeof(*queue);

    queue->index = data;
}

static inline void
rte_action_set_end(struct rte_flow_action *action,
            __attribute__ ((unused))uint32_t data,
            __attribute__ ((unused))size_t *offset)
{
    action->type = RTE_FLOW_ACTION_TYPE_END;
}

struct set_rte_action action_mark_flow[] = {
    { .set = rte_action_set_mark },
    { .set = rte_action_set_queue },
    { .set = rte_action_set_end },
};

struct rte_flow *
dpdk_rte_flow_validate(uint32_t port_id,
                       struct rte_flow_attr *attr,
                       struct rte_flow_item *item,
                       struct rte_flow_action *action,
                       struct rte_flow_error *error)
{
    int ret;

    ret = rte_flow_validate(port_id, attr, item, action, error);

    if (!ret) {
        struct rte_flow *flow;

        /* I should really save the pointer somwhere and delete it :) */
        flow = rte_flow_create(port_id, attr, item, action, error);
        return flow;
    }
    return NULL;
}

static inline int
hw_pipeline_rte_flow_create(struct hw_pipeline *dp,
	 		    struct flow *flow,
			    struct flow *mask,
			    uint32_t in_port,
			    struct set_rte_item item_op[],
			    struct set_rte_action action_op[],
			    uint32_t action_data[],
			    size_t item_op_size,
			    size_t action_op_size,
			    size_t buf_size,
			    size_t table_id,
			    struct rte_flow **hw_h)
{
    struct rte_flow_attr attr = {.ingress = 1};
    struct rte_flow_item item[item_op_size];
    struct rte_flow_action action[action_op_size];
    struct rte_flow_error error = {0};
    uint8_t buf[buf_size];
    size_t offset = 0;
    size_t i;


    memset(item, 0, sizeof(item[0]) * item_op_size);
    memset(action, 0, sizeof(action[0]) * action_op_size);
    memset(buf, 0, sizeof(buf[0]) * buf_size);

    attr.priority = table_id;
    for (i = 0; i < item_op_size; i++) {
        item[i].spec = &buf[offset];
        item_op[i].set(flow, &item[i], &offset,ITEM_SET_SPEC);
        item[i].mask = &buf[offset];
        item_op[i].set(mask, &item[i], &offset,ITEM_SET_MASK);
    }

    for (i = 0; i < action_op_size; i++) {
        action[i].conf = buf + offset;
        action_op[i].set(&action[i], action_data[i], &offset);
    }

    *hw_h = dpdk_rte_flow_validate(in_port, &attr, item,
            action, &error);
    if (unlikely(*hw_h == NULL)) {
        printf("Can't insert (%s)\n", error.message);
        return NULL;
    }

    return 0;
}

void hw_pipeline_item_array_build(struct set_rte_item *item_any_flow,
				  struct flow *mask,
                                  size_t *buf_size,
                                  size_t *item_num)
{
    int ii=0;
    struct eth_addr eth_mac;

    *buf_size =0;
    memset(&eth_mac,0,sizeof(struct eth_addr));

    printf("rte_item_eth\n");
    item_any_flow[ii++].set = rte_item_set_eth;
    *buf_size += sizeof(struct rte_flow_item_eth);
    *buf_size += sizeof(struct rte_flow_item_eth);
    printf("rte_item_ip\n");
    item_any_flow[ii++].set = rte_item_set_ip;
    *buf_size += sizeof(struct rte_flow_item_ipv4);
    *buf_size += sizeof(struct rte_flow_item_ipv4);
    printf("rte_item_udp\n");
    item_any_flow[ii++].set = rte_item_set_udp;
    *buf_size += sizeof(struct rte_flow_item_udp);
    *buf_size += sizeof(struct rte_flow_item_udp);

    item_any_flow[ii].set = rte_item_set_end;
    *item_num=ii+1;
    return;
}


inline static void 
hw_pipeline_prepare_action(uint32_t flow_tag,
 		           int rxqid,
			   size_t *buf_size,
			   uint32_t *action_data)
{
    *buf_size += sizeof(struct rte_flow_action_mark) +
                 sizeof(struct rte_flow_action_queue);
    /* actions order:
     * Flow Tag mark
     * queue
     * end
     */
    action_data[0] = flow_tag;
    action_data[1] = rxqid;
    action_data[2] = 0;
    return;
}

/*
 This case intend to deal with all cases but VxLAN

  The flow attribute group is set to 0
  The flow item is flexible
  The flow action is flow tag marking plus destination queue
  flow tag is unique and taken from a pool of tags.
  It is saved for lookup later on in the processing phase.

*/

static int 
hw_pipeline_create_external_rule(struct hw_pipeline *dp,
                                 msg_sw_flow *ptr_rule,
                                 struct set_rte_action *action,
                                 size_t action_num,
                                 struct rte_flow **hw_flow_h)
{
    struct flow *sw_flow = (struct flow *)&ptr_rule->sw_flow.flow;
    struct flow *hw_flow = sw_flow;
    uint32_t flow_tag = ptr_rule->sw_flow.flow_tag;
    struct flow *wildcard_mask =  &ptr_rule->sw_flow_mask;
    size_t item_num = 0;
    size_t buf_size =   0;
    struct set_rte_item item_any_flow[] = {
                { .set = NULL },
                { .set = NULL },
                { .set = NULL },
                { .set = NULL },
                { .set = NULL },
    };
    uint32_t action_data[action_num];
    int ret =0;

    hw_pipeline_item_array_build(item_any_flow,wildcard_mask,&buf_size,
            &item_num);
    hw_pipeline_prepare_action(flow_tag,ptr_rule->rxqid,&buf_size,action_data);
    ret = hw_pipeline_rte_flow_create(dp,
                                      hw_flow, wildcard_mask,
				      ptr_rule->in_port,
                                      item_any_flow,
                                      action, action_data,
                                      item_num, action_num,
                                      buf_size, 0, hw_flow_h);
    if (unlikely(ret == -1)) {
        printf("Rule with flow_tag can not be inserted : %x  \n",flow_tag);
        return -1;
    }

    return 0;
}

static int hw_pipeline_send_remove_flow(struct hw_pipeline *dp,
					uint32_t flow_tag)
{
    msg_queue_elem rule;

    rule.data.rm_flow.in_port=
        dp->ft_pool.ft_data[flow_tag].sw_flow.flow.in_port;
    rule.data.rm_flow.flow_tag = flow_tag;
    //memcpy(&rule.data.rm_flow.ufid,ufidp,sizeof(uint32_t));
    rule.mode = HW_PIPELINE_REMOVE_RULE;

    if (unlikely(
            !hw_pipeline_msg_queue_enqueue(&dp->message_queue,&rule))) {
        printf("queue overflow");
        return -1;
    }

    return 0;
}


static int 
hw_pipeline_send_insert_flow(struct hw_pipeline *dp,
		             uint32_t in_port,
			     struct hw_pipeline_flow *flow,
			     struct flow *masks,
			     int rxqid)
{
    msg_queue_elem rule;

    rule.data.sw_flow.in_port = in_port;
    rule.data.sw_flow.rxqid   = rxqid;
    memcpy(&rule.data.sw_flow.sw_flow,flow,sizeof(struct hw_pipeline_flow));
    memcpy(&rule.data.sw_flow.sw_flow_mask,masks,sizeof(struct flow));
    rule.mode = HW_PIPELINE_INSERT_RULE;
    if (unlikely(
        !hw_pipeline_msg_queue_enqueue(&dp->message_queue,&rule))) {
        printf("queue overflow");
        return -1;
    }
    return 0;
}

static inline int
hw_pipeline_insert_flow(struct hw_pipeline *dp,
			msg_sw_flow *ptr_rule)
{
    int ret =-1;
    struct rte_flow *hw_flow_h;
    ret = hw_pipeline_create_external_rule(dp,ptr_rule,
        action_mark_flow,(sizeof(action_mark_flow) / sizeof((action_mark_flow)[0])),&hw_flow_h);
    dp->ft_pool.ft_data[ptr_rule->sw_flow.flow_tag].hw_flow_h = hw_flow_h;	
    //memcpy(dp->ft_pool.ft_data[ptr_rule->sw_flow.flow_tag].sw_flow,ptr_rule,sizeof(msg_sw_flow));

    dp->ft_pool.ft_data[ptr_rule->sw_flow.flow_tag].sw_flow.pmd_id = ptr_rule->sw_flow.pmd_id;
    dp->ft_pool.ft_data[ptr_rule->sw_flow.flow_tag].sw_flow.ufidp = ptr_rule->sw_flow.ufidp;
    dp->ft_pool.ft_data[ptr_rule->sw_flow.flow_tag].sw_flow.flow_tag = ptr_rule->sw_flow.flow_tag;

    if (unlikely(ret == -1)) {
        printf("create_rule failed to insert rule ");
    }
    return ret;
}

static int hw_pipeline_remove_flow(struct hw_pipeline *dp,
                                   msg_hw_flow *ptr_rule)
{
    struct rte_flow_error error;
    int ret=0;
    struct rte_flow *hw_flow_h;

    hw_flow_h = dp->ft_pool.ft_data[ptr_rule->flow_tag].hw_flow_h;
    ret =rte_flow_destroy( ptr_rule->in_port,hw_flow_h,&error);
    return ret;
}

void *hw_pipeline_thread(void *params)
{
    msg_queue_elem ptr_rule;
    int ret =0;
    msg_queue *msgq = &dp.message_queue;

    ptr_rule.mode = HW_PIPELINE_NO_RULE;
    if (dp.ppl_md.id == HW_OFFLOAD_PIPELINE) {
        printf(" HW_OFFLOAD_PIPELINE is set \n");
    }
    else {
        printf(" HW_OFFLOAD_PIPELINE is off \n");
    }


    while (1) {
    	sleep(1);
        // listen to read_socket :
        // call the rte_flow_create ( flow , wildcard mask)
        ret = hw_pipeline_msg_queue_dequeue(msgq,&ptr_rule);
        if (ret != 0) {
            continue;
        }
        if (ptr_rule.mode == HW_PIPELINE_REMOVE_RULE) {
            ret =hw_pipeline_remove_flow(&dp,&ptr_rule.data.rm_flow);
            if (unlikely(ret)) {
                printf(" hw_pipeline_remove_flow failed to remove flow  \n");
            }
        }
        if (ptr_rule.mode == HW_PIPELINE_INSERT_RULE) {
            ret =hw_pipeline_insert_flow(&dp,&ptr_rule.data.sw_flow);
            if (unlikely(ret)) {
                printf(" hw_pipeline_remove_flow failed to remove flow  \n");
            }
        }
        ptr_rule.mode = HW_PIPELINE_NO_RULE;
    }
    return NULL;
}
int hw_pipeline_init(void )
{
    int ret=0;
    static uint32_t id=0;
    pthread_t thread;

    ret = hw_pipeline_ft_pool_init(&dp.ft_pool,HW_MAX_FLOW_TAG);
    if (unlikely(ret != 0)) {
        printf(" hw_pipeline_ft_pool_init failed \n");
        return ret;
    }
    ret = hw_pipeline_msg_queue_init(&dp.message_queue,id++);
    if (unlikely(ret != 0)) {
        printf(" hw_pipeline_msg_queue_init failed \n");
        return ret;
    }
    dp.ppl_md.id = HW_OFFLOAD_PIPELINE;
    dp.thread_ofload = pthread_create(&thread,0,hw_pipeline_thread,NULL);
    return 0;
}

int hw_pipeline_uninit(void)
{
    int ret=0;
    ret = hw_pipeline_ft_pool_uninit(&dp.ft_pool);
    if (unlikely( ret != 0 )) {
        printf(" hw_pipeline_ft_pool_uninit failed \n");
        return ret;
    }
    ret = hw_pipeline_msg_queue_clear(&dp.message_queue);
    if (unlikely( ret != 0 )) {
        printf(" hw_pipeline_msg_queue_clear failed \n");
        return ret;
    }
    pthread_join(dp.thread_ofload, NULL);
    dp.ppl_md.id = DEFAULT_SW_PIPELINE;
    return 0;
}

/* Insert 'rule' into 'cls'.
 * Get a unique tag from pool
 * The function sends a message to the message queue
 * to insert a rule to HW, but
 * in the context of hw_pipeline_thread
 * */
void
hw_pipeline_dpcls_insert(struct hw_pipeline_flow *hwpipe_flow,
			 uint32_t *flow_tag,
                         uint32_t in_port,
                         struct flow *wc_masks,
                         int rxqid)
{
    *flow_tag=HW_NO_FREE_FLOW_TAG;

    *flow_tag = hw_pipeline_ft_pool_get(&dp.ft_pool,hwpipe_flow);
    if (unlikely(*flow_tag == HW_NO_FREE_FLOW_TAG)) {
        printf("No more free Tags \n");
        return;
    }
    hwpipe_flow->flow_tag = *flow_tag;
    if (unlikely(hw_pipeline_send_insert_flow(&dp,in_port,hwpipe_flow,
        wc_masks,rxqid)== -1)) {
        printf("The Message Queue is FULL \n");
        return;
    }
}

/* Removes 'rule' from 'cls', also distracting the 'rule'.
 * Free the unique tag back to pool.
 * The function sends a message to the message queue
 * to insert a rule to HW, but
 * in the context of hw_pipeline_thread
 * */
void
hw_pipeline_dpcls_remove(uint32_t flow_tag)
{
    if (hw_pipeline_send_remove_flow(&dp,flow_tag)==-1) {
        printf("The Message Queue is FULL \n");
        return;
    }
    if (likely(hw_pipeline_ft_pool_is_valid(&dp.ft_pool))) {
      if (unlikely(
          !hw_pipeline_ft_pool_free(&dp.ft_pool,flow_tag))) {
            printf("tag is out of range");
            return;
      }
    }
}

struct hw_pipeline_flow *
hw_pipeline_dpcls_lookup_flow(uint32_t flow_tag,
                              int *lookup_cnt)
{
    struct hw_pipeline_flow *hwpipe_flow=NULL;
    if (unlikely(flow_tag == HW_NO_FREE_FLOW_TAG)) {
        return NULL;
    }

    hwpipe_flow = hw_pipeline_read_flow(&dp.ft_pool,flow_tag);
    if (hwpipe_flow != NULL) {
        if (lookup_cnt != NULL) {
            *lookup_cnt=+1;
        }
    }
    else {
        printf("No flow found : hwpipe_flow %p for flow_tag %x",
                 hwpipe_flow,flow_tag);
    }
    return hwpipe_flow;
}
