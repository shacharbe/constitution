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
#include "hw-pipeline-flow-tag-pool.h"

/*****************************************************************************/
//    HW Flow Tags Pool
//        A pool of unique tags
//        The flow tag is used as an interface with the HW.
//        If there is a match between a packet & a rule then
//        the flow tag is received by the APP in the fdir.hash in the rte_mbuf
//        With this flow tag the Application find the associated flow.
//        The Pool is per pmd_thread.
//        Each flow points on a flow tag and vice versa.
/*****************************************************************************/
bool hw_pipeline_ft_pool_is_valid(flow_tag_pool *p)
{
    rte_spinlock_lock(&p->lock);
    if (p->ft_data != NULL && p->pool_size>0) {
        printf("The pool is allocated & its size is : %d\n", p->pool_size);
        rte_spinlock_unlock(&p->lock);
        return true;
    }
    printf("The pool is invalid its size is : %d\n", p->pool_size);
    rte_spinlock_unlock(&p->lock);
    return false;
}

inline struct hw_pipeline_flow *hw_pipeline_ft_pool_read_flow(flow_tag_pool *p,
                                                            uint32_t handle)
{
    uint32_t index;
    struct hw_pipeline_flow *flow=NULL;

    index = APP_FLOW_TAG_INDEX_GET(handle);
    if (unlikely(index >= HW_MAX_FLOW_TAG)) {
        printf("index out of range\n");
        return NULL;
    }

    p->ft_data[index].valid =true;
    flow = &p->ft_data[index].sw_flow;

    return flow;
}

uint32_t hw_pipeline_ft_pool_init(flow_tag_pool *p,
                                  uint32_t pool_size)
{
    uint32_t ii=0;

    if (unlikely( p == NULL )) {
        printf("pool size is too big or pool is NULL \n");
        return -1;
    }
    if (unlikely( pool_size > HW_MAX_FLOW_TAG )) {
        pool_size = HW_MAX_FLOW_TAG;
    }
    p->ft_data = (flow_elem *)malloc(pool_size * sizeof(flow_elem));
    if (unlikely( p->ft_data == NULL )) {
        printf("No free memory for the pool \n");
        return -1;
    }
    memset(p->ft_data,0,(pool_size * sizeof(flow_elem)));
    rte_spinlock_init(&p->lock);
    rte_spinlock_lock(&p->lock);
    p->head=0;
    p->tail=0;
    p->pool_size = pool_size;
    for (ii=0;ii<pool_size;ii++) {
        p->ft_data[ii].next = ii+1;
        rte_spinlock_init(&p->ft_data[ii].lock);
    }
    p->ft_data[pool_size-1].next = HW_NO_FREE_FLOW_TAG;
    rte_spinlock_unlock(&p->lock);
    return 0;
}

uint32_t hw_pipeline_ft_pool_uninit(flow_tag_pool *p)
{
    uint32_t ii=0;

    if (unlikely(p==NULL||p->ft_data==NULL)) {
        printf("No pool or no data allocated \n");
        return -1;
    }
    rte_spinlock_lock(&p->lock);
    p->head=0;
    p->tail=0;
    for (ii=0; ii < p->pool_size; ii++) {
        p->ft_data[ii].next = 0;
        p->ft_data[ii].valid=false;
    }
    free(p->ft_data);
    rte_spinlock_unlock(&p->lock);
    return 0;
}
/*
 *  hw_pipeline_ft_pool_get returns an index from the pool
 *  The index is returned from the head.
 *
 *  The function deals with 3 cases:
 *        1. no more indexes in the pool . returns HW_NO_FREE_FLOW_TAG
 *        2. There is an index:
 *        		a. This is the last index
 *        		b. This is the common index
 * */
uint32_t hw_pipeline_ft_pool_get(flow_tag_pool *p,struct hw_pipeline_flow *flow)
{
    uint32_t next;
    uint32_t index;

    rte_spinlock_lock(&p->lock);
    if (p->head != HW_NO_FREE_FLOW_TAG) {
        //(case 2b , see function header above)
        // returns the current head & update the head to head.next
        index = p->head;
        next = p->ft_data[index].next;
        p->head = next;
        if (next == HW_NO_FREE_FLOW_TAG) {
            //last index (case 2a , see function header above)
            p->tail = HW_NO_FREE_FLOW_TAG;
        }
        memcpy(&p->ft_data[index].sw_flow,flow,sizeof(struct hw_pipeline_flow));
        p->ft_data[index].valid = true;
        rte_spinlock_unlock(&p->lock);
        return index;
    }
    else {
    // no more free tags ( case 1, see function header above)
        rte_spinlock_unlock(&p->lock);
        printf("No more flow tags \n");
        return HW_NO_FREE_FLOW_TAG;
    }

    rte_spinlock_unlock(&p->lock);
    return index;
}
/*
 *  hw_pipeline_ft_pool_free returns an index to the pool.
 *  The index is returned to the tail.
 *  The function deals with 3 cases:
 *        1. index out of range in the pool . returns false
 *        2. There is an place in the pool :
 *        		a. This is the last place .
 *        		b. This is the common index .
 * */
bool hw_pipeline_ft_pool_free(flow_tag_pool *p,
                              uint32_t handle)
{
    uint32_t index ,tail;

    index = APP_FLOW_TAG_INDEX_GET(handle);
    if (unlikely(index >= HW_MAX_FLOW_TAG)) {
    // ( case 1, see function header above)
        printf("index out of range \n");
        return false;
    }
    rte_spinlock_lock(&p->lock);
    tail = p->tail;
    if (tail == HW_NO_FREE_FLOW_TAG) {
    // last place in the pool ( case 2a, see function header above)
        p->head = index;
    }
    else {
    // common case ( case 2b, see function header above)
        p->ft_data[tail].next = index;  // old tail next points on index
    }
    // current tail is updated to be index & its next HW_NO_FREE_FLOW_TAG
    p->tail = index;
    p->ft_data[index].next = HW_NO_FREE_FLOW_TAG;
    p->ft_data[index].valid = false;
    rte_spinlock_unlock(&p->lock);
    return true;
}

struct hw_pipeline_flow *hw_pipeline_read_flow(flow_tag_pool *p,
        uint32_t handle)
{
    struct hw_pipeline_flow *hwpipe_flow=NULL;

    hwpipe_flow = hw_pipeline_ft_pool_read_flow(p,handle);
    if (unlikely(hwpipe_flow == NULL)) {
        printf("No flow found");
        return NULL;
    }

    return hwpipe_flow;
}

