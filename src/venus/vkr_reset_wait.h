/* SPDX-License-Identifier: MIT */
#ifndef VKR_RESET_WAIT_H
#define VKR_RESET_WAIT_H
#include <stdint.h>
enum vkr_reset_wait_status {
   VKR_RESET_WAIT_IDLE,
   VKR_RESET_WAIT_CONTEXT_MISSING,
   VKR_RESET_WAIT_INVALID_GENERATION,
   VKR_RESET_WAIT_GENERATION_MISMATCH,
   VKR_RESET_WAIT_RINGS_PRESENT,
   VKR_RESET_WAIT_RING_STOP_FAILED,
   VKR_RESET_WAIT_DEVICE_ERROR,
   VKR_RESET_WAIT_PROTOCOL_MISMATCH,
};
struct vkr_reset_wait_result {
   enum vkr_reset_wait_status status;
   uint32_t context_id;
   uint64_t generation;
   uint32_t devices_waited;
   int32_t vk_result; /* first non-success; zero if all returned success */
};
#endif
