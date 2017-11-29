#ifndef SR_FLOW_TAG_POOL_H
#define SR_FLOW_TAG_POOL_H
#include "hw-common-pipeline.h"

struct hw_pipeline_flow *hw_pipeline_read_flow(flow_tag_pool *p,
                                             uint32_t flow_tag);

uint32_t hw_pipeline_ft_pool_get(flow_tag_pool *p,
                                                         struct hw_pipeline_flow *flow);

bool hw_pipeline_ft_pool_free(flow_tag_pool *p,uint32_t flow_tag);

bool hw_pipeline_ft_pool_is_valid(flow_tag_pool *p);

// Internal functions Flow Tags Pool
uint32_t hw_pipeline_ft_pool_init(flow_tag_pool *p,uint32_t pool_size);

uint32_t hw_pipeline_ft_pool_uninit(flow_tag_pool *p);

struct hw_pipeline_flow *hw_pipeline_ft_pool_read_flow(flow_tag_pool *p,
                                                     uint32_t handle);
#endif
