/*
 * hw-pipeline.h
 *
 *  Created on: 13 Oct 2016
 *      Author: sugeshch
 */

#ifndef LIB_HW_PIPELINE_H_
#define LIB_HW_PIPELINE_H_
#include "unistd.h"
#include "stdio.h"
#include "sys/types.h"
#include "sys/stat.h"

#define HW_NO_FREE_FLOW_TAG 0xffffffff
#define HW_PIPELINE_MSGQ_TO 10000
#define HW_MAX_FLOW_TAG 65536
#define MSG_QUEUE_MAX_SIZE 65536
#define GRE_PROTOCOL 47
enum pipeline_id {
    DEFAULT_SW_PIPELINE = 0,
    HW_OFFLOAD_PIPELINE
};

typedef enum{
    HW_PIPELINE_NO_RULE,
    HW_PIPELINE_INSERT_RULE,
    HW_PIPELINE_REMOVE_RULE
}HW_pipeline_msgq_mode;

struct pipeline_md {
    uint16_t id; 
    uint32_t flow_tag;
};

#define APP_GET_HANDLE_VALUE(x,s,w)     ((x>>s)&((1<<(w+1))-1))

#define APP_FLOW_TAG_INDEX_W            16
#define APP_FLOW_TAG_INDEX_S            0
#define APP_FLOW_TAG_INDEX_M            ((1<<(APP_FLOW_TAG_INDEX_W+1))-1)
#define APP_FLOW_TAG_INDEX_GET(x) \
   APP_GET_HANDLE_VALUE(x,APP_FLOW_TAG_INDEX_S,APP_FLOW_TAG_INDEX_W)

#endif /* LIB_HW_PIPELINE_H_ */
