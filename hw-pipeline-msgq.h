#ifndef SR_MSGQ_H
#define SR_MSGQ_H

#include "hw-common-pipeline.h"

int hw_pipeline_msg_queue_init(msg_queue *message_queue,
                                      unsigned core_id);
int hw_pipeline_msg_queue_clear(msg_queue *message_queue);

bool hw_pipeline_msg_queue_enqueue(msg_queue *message_queue,
                                   msg_queue_elem *data);

int hw_pipeline_msg_queue_dequeue(msg_queue *message_queue,
                                  msg_queue_elem *data);
#endif
