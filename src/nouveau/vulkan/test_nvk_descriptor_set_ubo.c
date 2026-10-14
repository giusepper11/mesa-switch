/* SPDX-License-Identifier: MIT */
#include "nvk_descriptor_set_ubo.h"
#include "nvk_cmd_buffer.h"

#include <stdio.h>

struct test_case {
   const char *name;
   VkDescriptorSetLayoutCreateFlags layout_flags;
   VkDescriptorBindingFlags binding_flags;
   VkDescriptorBindingFlags other_flags;
   VkDescriptorType type;
   uint32_t offset, size, count, stride;
   struct nvk_buffer_address addr;
   bool null_map, null_layout, empty_binding, expect;
};

#define ADDRESS .addr = { .base_addr = 0x1234567800ull, .size = 256 }

int
main(void)
{
   struct nvk_physical_device pdev = {
      .info = { .sm = 53, .cls_eng3d = 0xb197 },
   };
   struct nvk_descriptor_set_layout *layout =
      calloc(1, sizeof(*layout) + 3 * sizeof(layout->binding[0]));
   if (layout == NULL)
      return 1;

   /* Zero-size rows model the existing representation, not a Vulkan write
    * with range=0.  Invalid offsets and representations are rejection tests.
    */
   const struct test_case cases[] = {
      { "static UBO", ADDRESS, .expect = true },
      { "last array element", ADDRESS, .offset = 48, .expect = true },
      { "null descriptor", .expect = true },
      { "zero size", .addr = { .base_addr = 0x1000 }, .expect = true },
      { "max cbuf", .addr = { .base_addr = 0x1000, .size = NVK_MAX_CBUF_SIZE }, .expect = true },
      { "exact byte end", ADDRESS, .size = 48, .expect = true },
      { "short byte range", ADDRESS, .size = 47 },
      { "offset at end", ADDRESS, .offset = 256 },
      { "wrapping offset", ADDRESS, .offset = UINT32_MAX },
      { "before binding", ADDRESS, .offset = 16 },
      { "inside descriptor", ADDRESS, .offset = 33 },
      { "array padding", ADDRESS, .offset = 64 },
      { "empty binding", ADDRESS, .empty_binding = true },
      { "unknown stride", ADDRESS, .stride = 32 },
      { "dynamic binding", ADDRESS, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC },
      { "mutable binding", ADDRESS, .type = VK_DESCRIPTOR_TYPE_MUTABLE_EXT },
      { "storage binding", ADDRESS, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
      { "null map", ADDRESS, .null_map = true },
      { "null layout", ADDRESS, .null_layout = true },
      { "update-after-bind layout", ADDRESS,
        .layout_flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT },
      { "descriptor buffer", ADDRESS,
        .layout_flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT },
      { "push layout", ADDRESS,
        .layout_flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT },
      { "host-only layout", ADDRESS,
        .layout_flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT },
      { "update-after-bind binding", ADDRESS,
        .binding_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT },
      { "update unused", ADDRESS,
        .binding_flags = VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT },
      { "partially bound", ADDRESS, .binding_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT },
      { "unused partially bound", ADDRESS,
        .binding_flags = VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT },
      { "unsafe later binding", ADDRESS,
        .other_flags = VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT },
      { "partially bound later binding", ADDRESS,
        .other_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT },
      { "variable binding", ADDRESS,
        .binding_flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT },
      { "other variable binding", ADDRESS,
        .other_flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT },
      { "variable zero count padding", ADDRESS,
        .empty_binding = true, .binding_flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT },
      { "misaligned address", .addr = { .base_addr = 0x1001, .size = 256 } },
      { "misaligned size", .addr = { .base_addr = 0x1000, .size = 255 } },
      { "too large", .addr = { .base_addr = 0x1000, .size = NVK_MAX_CBUF_SIZE + 256 } },
      { "reserved word", .addr = { .base_addr = 0x1000, .size = 256, .zero = 1 } },
      { "null nonzero size", .addr = { .size = 256 } },
   };

   uint32_t failures = 0;
   struct nvk_device dev = { .vk.physical = &pdev.vk };
   struct nvk_cmd_buffer cmd = { .vk.base.device = &dev.vk };
   struct nvk_shader shader = { .info.stage = MESA_SHADER_FRAGMENT };
   struct nvk_descriptor_set set = { .layout = layout };
   struct nvk_descriptor_state state = { 0 };
   state.sets[0].type = NVK_DESCRIPTOR_SET_TYPE_SET;
   state.sets[0].set = &set;
   struct nvk_cbuf cbuf = { .type = NVK_CBUF_TYPE_UBO_DESC };
   unsetenv("NVK_SWITCH_CPU_RESOLVE_UBO");

   for (uint32_t i = 0; i < ARRAY_SIZE(cases); i++) {
      struct test_case row = cases[i];
      if (row.type == 0)
         row.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      if (row.offset == 0)
         row.offset = 32;
      if (row.size == 0)
         row.size = 256;
      if (row.count == 0 && !row.empty_binding)
         row.count = 2;
      if (row.stride == 0)
         row.stride = 16;
      const struct test_case *t = &row;
      layout->flags = t->layout_flags;
      layout->binding_count = 3;
      layout->binding[1] = (struct nvk_descriptor_set_binding_layout) {
         .type = t->type, .flags = t->binding_flags,
         .offset = 32, .stride = t->stride, .array_size = t->count,
      };
      layout->binding[2] = (struct nvk_descriptor_set_binding_layout) {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .flags = t->other_flags, .offset = 96, .stride = 16, .array_size = 1,
      };

      /* An unaligned CPU pointer exercises memcpy rather than a typed load. */
      uint8_t bytes[257] = { 0 };
      void *map = bytes + 1;
      if (t->offset <= 240)
         memcpy((char *)map + t->offset, &t->addr, sizeof(t->addr));

      const struct nvk_buffer_address sentinel = {
         .base_addr = UINT64_MAX, .size = UINT32_MAX, .zero = UINT32_MAX,
      };
      struct nvk_buffer_address out = sentinel;
      const bool direct = nvk_descriptor_set_try_cpu_ubo(
         &pdev, t->null_layout ? NULL : layout,
         t->null_map ? NULL : map, t->size, t->offset, &out);

      union nvk_buffer_descriptor descriptor = { .addr = t->addr };
      const struct nvk_buffer_address expected = direct ? t->addr : sentinel;
      bool pass = direct == t->expect &&
                  memcmp(&out, &expected, sizeof(out)) == 0 &&
                  (!direct || memcmp(&out, &descriptor.addr,
                                     sizeof(out)) == 0);

      set.layout = t->null_layout ? NULL : layout;
      set.map = t->null_map ? NULL : map;
      set.size_B = t->size;
      cbuf.desc_offset = t->offset;
      unsetenv("NVK_SWITCH_CPU_RESOLVE_UBO");
      pass &= !nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &out);
      setenv("NVK_SWITCH_CPU_RESOLVE_UBO", "1", 1);
      out = sentinel;
      const bool routed =
         nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &out);
#ifdef HAVE_SWITCH_PLATFORM
      pass &= routed == t->expect &&
              memcmp(&out, &expected, sizeof(out)) == 0;
#else
      pass &= !routed && memcmp(&out, &sentinel, sizeof(out)) == 0;
#endif
      printf("%s: %s\n", pass ? "PASS" : "FAIL", t->name);
      failures += !pass;
   }

   /* Legal ordering: write/copy before bind/record; retain the recorded
    * value; update after completed uses, then record a fresh command buffer.
    * This exercises snapshot semantics, not Vulkan runtime validation.
    */
   layout->flags = 0;
   layout->binding_count = 1;
   layout->binding[0] = (struct nvk_descriptor_set_binding_layout) {
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .array_size = 1, .stride = 16,
   };
   union nvk_buffer_descriptor descriptor = {
      .addr = { .base_addr = 0x1000, .size = 256 },
   };
   union nvk_buffer_descriptor copy = descriptor;
   struct nvk_buffer_address first, second;
   bool pass = nvk_descriptor_set_try_cpu_ubo(&pdev, layout, &copy,
                                             sizeof(copy), 0, &first);
   copy.addr.base_addr = 0x2000;
   pass &= nvk_descriptor_set_try_cpu_ubo(&pdev, layout, &copy,
                                         sizeof(copy), 0, &second);
   pass &= first.base_addr == 0x1000 && second.base_addr == 0x2000;
   printf("%s: write/copy/record and valid reuse snapshot\n", pass ? "PASS" : "FAIL");
   failures += !pass;

   /* No stale or inaccessible mapping may be read on a rejection path.
    * A non-null freed set remains invalid usage; this gate cannot detect it.
    */
   layout->binding[0].flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
   pass = !nvk_descriptor_set_try_cpu_ubo(&pdev, layout,
                                         (void *)(uintptr_t)1, 16, 0, &first);
   layout->binding[0].flags = 0;
   pdev.info.sm = 75;
   pass &= !nvk_descriptor_set_try_cpu_ubo(&pdev, layout,
                                          (void *)(uintptr_t)1, 16, 0, &first);
   pdev.info.sm = 53;
   pdev.info.cls_eng3d = 0xc597;
   pass &= !nvk_descriptor_set_try_cpu_ubo(&pdev, layout,
                                          (void *)(uintptr_t)1, 16, 0, &first);
   printf("%s: reject before descriptor read\n", pass ? "PASS" : "FAIL");
   failures += !pass;

   /* Exercise the actual dispatcher without touching descriptor-buffer
    * memory, and preserve push/dynamic UBO behavior with opt-in off and on.
    */
   state.sets[0].type = NVK_DESCRIPTOR_SET_TYPE_BUFFER;
   state.sets[0].set = (void *)(uintptr_t)1;
   cbuf.desc_offset = 0;
   pdev.info.sm = 53;
   pdev.info.cls_eng3d = 0xb197;
   pass = !nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &first);

   struct nvk_push_descriptor_set push = { 0 };
   memcpy(push.data, &descriptor, sizeof(descriptor));
   state.sets[0].type = NVK_DESCRIPTOR_SET_TYPE_PUSH;
   state.sets[0].push = &push;
   struct nvk_root_descriptor_table *root = (void *)state.root;
   for (uint32_t i = 0; i < 4; i++)
      root->dynamic_buffers[i][0] = descriptor.values[i];
   for (uint32_t enabled = 0; enabled < 2; enabled++) {
      setenv("NVK_SWITCH_CPU_RESOLVE_UBO", enabled ? "1" : "0", 1);
      cbuf.type = NVK_CBUF_TYPE_UBO_DESC;
      pass &= nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &first);
      pass &= memcmp(&first, &descriptor.addr, sizeof(first)) == 0;
      cbuf.type = NVK_CBUF_TYPE_DYNAMIC_UBO;
      pass &= nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &first);
      pass &= memcmp(&first, &descriptor.addr, sizeof(first)) == 0;
   }
   printf("%s: descriptor buffer fallback and push/dynamic routing\n",
          pass ? "PASS" : "FAIL");
   failures += !pass;

   cbuf.type = NVK_CBUF_TYPE_UBO_DESC;
   state.sets[0].type = NVK_DESCRIPTOR_SET_TYPE_SET;
   state.sets[0].set = &set;
   set.layout = layout;
   set.map = (void *)(uintptr_t)1;
   set.size_B = 16;
   shader.info.stage = MESA_SHADER_VERTEX;
   setenv("NVK_SWITCH_CPU_RESOLVE_UBO", "1", 1);
   pass = !nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &first);
   shader.info.stage = MESA_SHADER_FRAGMENT;
   state.sets[0].set = NULL;
   pass &= !nvk_cmd_buffer_get_cbuf_addr(&cmd, &state, &shader, &cbuf, &first);
   printf("%s: non-fragment and null set fallback\n", pass ? "PASS" : "FAIL");
   failures += !pass;
   unsetenv("NVK_SWITCH_CPU_RESOLVE_UBO");

   free(layout);
   printf("%u failures\n", failures);
   return failures != 0;
}
