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
#include "hw-pipeline-msgq.h"

/*************************************************************************/
// Msg Queue
//  A queue that contains pairs : (flow , key )
//  The queue is used a communication channel between pmd_thread_main &
//  hw_pipeline_thread .
//  The  hw_pipeline_thread dequeue (key,flow ) from the message queue
//  & calls emc_hw_insert that inserts classifier rules
//  to hardware flow tables.
//  The pmd_thread_main enqueue (key,flow) into the message queue and continues.
/*************************************************************************/
int hw_pipeline_msg_queue_init(msg_queue *message_queue,
        		       unsigned core_id)
{
    int ret;
    const char dir[] = "/tmp";
    const char fifo[] = "/tmp/msgq_pipe";
    char fifo_pmd[20];

    sprintf(fifo_pmd,"%s%d",fifo,core_id);
    message_queue->tv.tv_sec = 0;
    message_queue->tv.tv_usec = HW_PIPELINE_MSGQ_TO;
    strncpy(message_queue->pipeName,fifo_pmd,strlen(fifo_pmd));
    if (mkdir(dir, 0755) == -1 && errno != EEXIST) {
        printf("Failed to create directory: ");
        return -1;
    }
    ret = mkfifo(fifo_pmd,0666);
    if (unlikely(ret < 0)) {
        if (errno==EEXIST) {
            ret = unlink(fifo_pmd);
            if (unlikely(ret < 0)) {
                printf("Remove fifo failed .\n");
                return -1;
            }
        }
        else if (errno==EROFS) {
            printf("The name file resides on a read-only file-system\n");
            return -1;
        }
        else {
            printf("mkfifo failed %x \n",errno);
            return -1;
        }
    }
    message_queue->readFd  = open(message_queue->pipeName,
            O_RDONLY|O_NONBLOCK);
    if (unlikely(message_queue->readFd == -1)) {
        printf("Error creating read file descriptor");
        return -1;
    }
    message_queue->writeFd = open(message_queue->pipeName,
            O_WRONLY|O_NONBLOCK);
    if (unlikely(message_queue->writeFd == -1)) {
        printf("Error creating write file descriptor");
        return -1;
    }
    return 0;
}

int hw_pipeline_msg_queue_clear(msg_queue *message_queue)
{
    int ret =0;
    ret = close(message_queue->readFd);
    if (unlikely( ret == -1 )) {
        printf("Error while closing the read file descriptor.");
        return -1;
    }
    ret = close(message_queue->writeFd);
    if (unlikely( ret == -1 )) {
        printf("Error while closing the write file descriptor.");
        return -1;
    }

    ret = unlink(message_queue->pipeName);
    if (unlikely( ret < 0 )) {
        printf("Remove fifo failed .\n");
        return -1;
    }

    return 0;
}

bool hw_pipeline_msg_queue_enqueue(msg_queue *message_queue,
                                   msg_queue_elem *data)
{
    ssize_t ret =0;

    ret = write(message_queue->writeFd, data, sizeof(msg_queue_elem));
    if (unlikely( ret == -1)) {
        switch (errno) {
            case EBADF:
                printf("FD is non-valid , or is not open for writing.\n");
                break;
            case EFBIG:
                printf("File is too large.\n");
                break;
            case EINTR:
                printf("interrupted by a signal\n");
                break;
            case EIO:
                printf("hardware error\n");
                break;
            case ENOSPC:
                printf("The device's file is full\n");
                break;
            case EPIPE:
                printf("FIFO that isn't open for reading\n");
                break;
            case EINVAL:
                printf("Not aligned to the block size");
                break;
            default:
                break;
        }
        return false;
    }
    return true;
}

int hw_pipeline_msg_queue_dequeue(msg_queue *message_queue,
                                  msg_queue_elem *data)
{
    ssize_t ret=0;
    int err=0;
    fd_set readset;
    int readFd = (*(int *)&message_queue->readFd);
    FD_ZERO(&readset);
    FD_SET(readFd, &readset);
        // Now, check for readability
    err = select(readFd+1,&readset,NULL, NULL,&message_queue->tv);

    if (likely(err>0 && FD_ISSET(readFd, &readset))) {
        // Clear flags
        FD_CLR(readFd, &readset);
        ret = read(readFd, data, sizeof(msg_queue_elem));
        if (unlikely( ret == -1)) {
        	printf("Error reading from the  file descriptor.");
            return ret;
        }
    }
    else {
        return err;
    }

    return 0;
}
