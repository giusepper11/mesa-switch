/* SPDX-License-Identifier: MIT */
#ifndef NVK_DESCRIPTOR_SET_UBO_H
#define NVK_DESCRIPTOR_SET_UBO_H

#include "nvk_descriptor_set_layout.h"
#include "nvk_descriptor_types.h"

/* Only CPU-written, fixed-count UBO bindings on GM20B are admitted.  The
 * caller must hold a live ordinary descriptor set during command recording.
 * Vulkan permits early consumption of ordinary descriptors, but bindings
 * permitting updates or invalid unused elements must retain GPU resolution.
 */
static inline bool
nvk_descriptor_set_try_cpu_ubo(const struct nvk_physical_device *pdev,
                              const struct nvk_descriptor_set_layout *layout,
                              const void *map, uint32_t size_B,
                              uint32_t offset_B,
                              struct nvk_buffer_address *addr_out)
{
   if (pdev->info.sm != 53 || nvk_use_bindless_cbuf(&pdev->info) ||
       layout == NULL || map == NULL ||
       (layout->flags &
        (VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT |
         VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
         VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT |
         VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT)))
      return false;

   /* Subtraction avoids wrapping offset_B + sizeof(desc).  Allocation
    * padding is not evidence that a logical descriptor exists.
    */
   if (offset_B > size_B ||
       size_B - offset_B < sizeof(union nvk_buffer_descriptor))
      return false;

   bool is_ubo = false;
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      const struct nvk_descriptor_set_binding_layout *binding =
         &layout->binding[b];

      /* Reject the entire set before reading any descriptor.  Actual
       * variable counts are not retained separately from aligned size_B.
       */
      if (binding->flags &
          (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))
         return false;

      if (binding->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
          binding->stride != sizeof(union nvk_buffer_descriptor) ||
          offset_B < binding->offset)
         continue;

      const uint32_t delta = offset_B - binding->offset;
      if (delta % binding->stride == 0 &&
          delta / binding->stride < binding->array_size)
         is_ubo = true;
   }
   if (!is_ubo)
      return false;

   union nvk_buffer_descriptor descriptor;
   memcpy(&descriptor, (const char *)map + offset_B, sizeof(descriptor));
   const struct nvk_buffer_address addr =
      nvk_ubo_descriptor_addr(pdev, descriptor);

   const uint32_t alignment = nvk_min_cbuf_alignment(&pdev->info);
   if (addr.zero != 0 || addr.base_addr % alignment != 0 ||
       addr.size % alignment != 0 || addr.size > NVK_MAX_CBUF_SIZE ||
       (addr.base_addr == 0 && addr.size != 0))
      return false;

   *addr_out = addr;
   return true;
}

#endif /* NVK_DESCRIPTOR_SET_UBO_H */
