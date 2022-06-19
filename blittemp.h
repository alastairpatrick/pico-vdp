static void STRIPED_SECTION FUNC_NAME(Opcode opcode) {
  //DisableXIPCache();

  int dest_daddr = g_blit_regs[BLIT_REG_DST_ADDR];
  int src_daddr = g_blit_regs[BLIT_REG_SRC_ADDR];
  int dest_baddr = g_blit_regs[BLIT_REG_DST_ADDR];
  int src_baddr_byte = g_blit_regs[BLIT_REG_SRC_ADDR] * 4;
  int pitch = (int16_t) g_blit_regs[BLIT_REG_PITCH];

  int flags = 0;
  if (opcode & BLIT_OP_FLAGS_EN) {
    flags = g_blit_regs[BLIT_REG_FLAGS];
  }

  uint32_t mask_disable8 = flags & BLIT_FLAG_MASKED ? 0x00000000 : 0xFFFFFFFF;

  uint32_t bg_color8 = 0x00000000;
  uint32_t fg_color8 = 0xFFFFFFFF;
  if (opcode & BLIT_OP_COLOR_EN) {
    bg_color8 = Parallel8(g_blit_regs[BLIT_REG_COLORS] & 0xF);
    fg_color8 = Parallel8((g_blit_regs[BLIT_REG_COLORS] >> 4) & 0xF);
  }

  int clip_left = 0;
  int clip_right = 0xFFFF;
  if (opcode & BLIT_OP_CLIP_EN) {
    int clip = g_blit_regs[BLIT_REG_CLIP];
    clip_left = clip & 0xFF;
    clip_right = clip >> 8;

    if (clip_right == 0) {
      clip_right = 0x100;
    }
  }

  int width, height;
  switch (opcode & BLIT_OP_TOPY) {
  case BLIT_OP_TOPY_LIN:
    width = g_blit_regs[BLIT_REG_COUNT];
    height = 1;
    break;
  case BLIT_OP_TOPY_PLAN:
    width = g_blit_regs[BLIT_REG_COUNT] & 0xFF;
    if (width == 0) {
      width = 0x100;
    }
    height = g_blit_regs[BLIT_REG_COUNT] >> 8;
    if (height == 0) {
      height = 0x100;
    }
    break;
  }

  bool src_planar = (opcode & BLIT_OP_SRC) == BLIT_OP_SRC_DISPLAY && (opcode & BLIT_OP_TOPY) == BLIT_OP_TOPY_PLAN;
  bool dest_planar = (opcode & BLIT_OP_DEST) == BLIT_OP_DEST_DISPLAY && (opcode & BLIT_OP_TOPY) == BLIT_OP_TOPY_PLAN;

  ReadSourceDataFn read_source_data = SRC_ZERO ? 0 : PrepareReadSourceData(opcode);

  Fifo64 fifo = {0};

  // x-coordinates are in bits beyond this point.
  clip_left *= 4;
  clip_right *= 4;
  width *= 4;

  src_daddr -= pitch;
  int src_daddr_word = 0;
  int src_x = width;
  int src_y = -1;
  dest_daddr -= pitch;
  int dest_daddr_word = 0;
  int dest_x = width;
  int dest_y = -1;

  int iteration_cycles = flags & BLIT_FLAG_BLEND ? BASE_ITERATION_CYLES + 1 : BASE_ITERATION_CYLES;

  for (;;) {
    if (src_x >= width) {
      //MCycle(1);

      src_daddr += pitch;
      src_daddr_word = src_daddr >> 3;

      ++src_y;

      if (src_planar) {
        src_x = -(src_daddr & 0x7) * 4;
      } else {
        src_x = 0;
      }
    }

    if (SRC_ZERO || (fifo.size <= 32 && src_y < height)) {
      if (!SRC_ZERO) {
        uint32_t in_data = read_source_data(src_daddr_word, &src_baddr_byte);
        int begin, end;
        Overlap(&begin, &end, src_x, width);
        Fifo64Push(&fifo, in_data >> begin, end - begin);
      }
      src_x += 32;
      ++src_daddr_word;
    }

    if (dest_x >= width) {
      dest_daddr += pitch;
      dest_daddr_word = dest_daddr >> 3;

      ++dest_y;
      if (dest_y == height) {
        //EnableXIPCache();
        return;
      }

      if (dest_planar) {
        dest_x = -(dest_daddr & 0x7) * 4;
      } else {
        dest_x = 0;
      }
    }

    {
      int begin, end;
      Overlap(&begin, &end, dest_x, width);
      int num = end - begin;
      if (SRC_ZERO || fifo.size >= num) {
        // SIMD 8 4-bit lanes

        const uint32_t one8 = 0x11111111;
        const uint32_t three8 = 0x33333333;  
        
        uint32_t color8 = SRC_ZERO ? 0 : (Fifo64Pop(&fifo, num) << begin);

        // Mask based on clip left and right.
        int clip_begin = Max(begin, clip_left - dest_x);
        int clip_end = Min(end, clip_right - dest_x);
        uint32_t mask8 = ((1 << (clip_end - clip_begin)) - 1) << clip_begin;
        
        // Mask based on src_color==0. Bit zero of each lane to mask value.
        uint32_t color_mask8 = color8;
        color_mask8 |= (color_mask8 & ~three8) >> 2;
        color_mask8 |= (color_mask8 & ~one8) >> 1;
        color_mask8 &= one8;

        // Mask value to all bits of each lane
        color_mask8 |= color_mask8 << 1;
        color_mask8 |= color_mask8 << 2;

        // Optionally, apply color mask.
        mask8 &= (color_mask8 | mask_disable8);

        // Optionally remap colors.
        color8 = (fg_color8 & color8) | (bg_color8 & ~color8);

        if (flags & BLIT_FLAG_BLEND) {
          uint32_t dest_color8 = ReadDestData(opcode, dest_daddr_word, dest_baddr);
          switch (flags & BLIT_FLAG_BLEND) {
          case BLIT_FLAG_BLEND_AND:
            color8 &= dest_color8;
            break;
          case BLIT_FLAG_BLEND_OR:
            color8 |= dest_color8;
            break;
          case BLIT_FLAG_BLEND_XOR:
            color8 ^= dest_color8;
            break;
          case BLIT_FLAG_BLEND_ADD:
            color8 = ParallelAdd(color8, dest_color8);
            break;          
          case BLIT_FLAG_BLEND_SUB:
            color8 = ParallelSub(color8, dest_color8);
            break;          
          case BLIT_FLAG_BLEND_ADDS:
            color8 = ParallelAddSat(color8, dest_color8);
            break;          
          case BLIT_FLAG_BLEND_SUBS:
            color8 = ParallelSubSat(color8, dest_color8);
            break;          
          }
        }

        MCycle(iteration_cycles);
        WriteDestData(opcode, dest_daddr_word, dest_baddr, color8, mask8);

        dest_x += 32;
        ++dest_daddr_word;
        ++dest_baddr;
      } else {
        MCycle(iteration_cycles);
      }
    }

  }
}
