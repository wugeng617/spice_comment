/*
   Copyright (C) 2009 Red Hat, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in
         the documentation and/or other materials provided with the
         distribution.
       * Neither the name of the copyright holder nor the names of its
         contributors may be used to endorse or promote products derived
         from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _H_SPICE_DRAW
#define _H_SPICE_DRAW

#include <spice/macros.h>
#include <spice/types.h>
#include <spice/enums.h>
#include "mem.h"

SPICE_BEGIN_DECLS

#define SPICE_GET_ADDRESS(addr) ((void *)(uintptr_t)(addr))
#define SPICE_SET_ADDRESS(addr, val) ((addr) = (uintptr_t)(val))

typedef int32_t SPICE_FIXED28_4;

typedef struct SpicePointFix {
    SPICE_FIXED28_4 x;
    SPICE_FIXED28_4 y;
} SpicePointFix;

typedef struct SpicePoint {
    int32_t x;
    int32_t y;
} SpicePoint;

typedef struct SpicePoint16 {
    int16_t x;
    int16_t y;
} SpicePoint16;

typedef struct SpiceRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} SpiceRect;

typedef struct SpicePathSeg {
    uint32_t flags;
    uint32_t count;
    SpicePointFix points[0];
} SpicePathSeg;

typedef struct SpicePath {
  uint32_t num_segments;
  SpicePathSeg *segments[0];
} SpicePath;

typedef struct SpiceClipRects {
  uint32_t num_rects;
  SpiceRect rects[0];
} SpiceClipRects;

typedef struct SpiceClip {
    uint8_t type;
    SpiceClipRects *rects;
} SpiceClip;

typedef struct SpicePalette { //跟QXLPalette一样
    uint64_t unique;
    uint16_t num_ents;
    uint32_t ents[0];
} SpicePalette;

#define SPICE_SURFACE_FMT_DEPTH(_d) ((_d) & 0x3f)

typedef struct SpiceImageDescriptor {//跟QXLImageDescriptor一样
    uint64_t id; //图像id，包括组字段和唯一id，高32位unique, 低32位位组，用于缓存匹配
    uint8_t type; //图像类型
    uint8_t flags; //标记
    uint32_t width; //宽
    uint32_t height; //高
} SpiceImageDescriptor;

typedef struct SpiceBitmap {//跟QXLBitmap类似
    uint8_t format; //像素格式
    uint8_t flags; //图像布局标记，比如TOP_DOWN标记
    uint32_t x; //宽
    uint32_t y; //高
    uint32_t stride; //行长，因为是U32，所以行长应该是>0，根topdown flag来实现<0 stride
    SpicePalette *palette; //同QXLPalette一样
    uint64_t palette_id; //比QXLBitmap多的部分，应该是用于调色板缓存
    SpiceChunks *data; //内存块集合，因为内存没有线性化，所以是多块
} SpiceBitmap;

typedef struct SpiceSurface {//跟QXLSurface一样
    uint32_t surface_id;
} SpiceSurface;

typedef struct SpiceQUICData {//跟QXLQUICData一样
    uint32_t data_size;
    SpiceChunks *data;
} SpiceQUICData, SpiceLZRGBData, SpiceJPEGData;

typedef struct SpiceLZPLTData {
    uint8_t flags;
    uint32_t data_size;
    SpicePalette *palette;
    uint64_t palette_id;
    SpiceChunks *data;
} SpiceLZPLTData;

typedef struct SpiceZlibGlzRGBData {
    uint32_t glz_data_size;
    uint32_t data_size;
    SpiceChunks *data;
} SpiceZlibGlzRGBData;

typedef struct SpiceJPEGAlphaData {
    uint8_t flags;
    uint32_t jpeg_size;
    uint32_t data_size;
    SpiceChunks *data;
} SpiceJPEGAlphaData;


typedef struct SpiceImage {
    SpiceImageDescriptor descriptor;
    union {
        SpiceBitmap         bitmap;//从QXL来
        SpiceQUICData       quic; //从QXL来
        SpiceSurface        surface; //从QXL来
        SpiceLZRGBData      lz_rgb; //spice生成
        SpiceLZPLTData      lz_plt; //spice生成
        SpiceJPEGData       jpeg; //spice生成
        SpiceZlibGlzRGBData zlib_glz; //spice生成
        SpiceJPEGAlphaData  jpeg_alpha; //spice生成
    } u;
} SpiceImage;

typedef struct SpicePattern {
    SpiceImage *pat;
    SpicePoint pos;
} SpicePattern;

typedef struct SpiceBrush {
    uint32_t type;
    union {
        uint32_t color;
        SpicePattern pattern;
    } u;
} SpiceBrush;

typedef struct SpiceQMask {//蒙版
    uint8_t flags; //标记
    SpicePoint pos; //坐标
    SpiceImage *bitmap; //蒙版图像
} SpiceQMask;

typedef struct SpiceFill {
    SpiceBrush brush;
    uint16_t rop_descriptor;
    SpiceQMask mask;
} SpiceFill;

typedef struct SpiceOpaque {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    SpiceBrush brush;
    uint16_t rop_descriptor;
    uint8_t scale_mode;
    SpiceQMask mask;
} SpiceOpaque;

//DrawCopy，有三种对原图像的copy变形：
// 1.将原图像区域进行ROP光栅处理
// 2.将原图像区域进行缩放 
// 3.对原图像区域进行蒙皮
typedef struct SpiceCopy {
    SpiceImage *src_bitmap; //copy的源图像
    SpiceRect src_area; //copy的源图像区域
    uint16_t rop_descriptor; //ROP描述符， 去SpiceRopd的值
    uint8_t scale_mode; //缩放模式,控制copy时，缩放源图像
    SpiceQMask mask; //蒙版
} SpiceCopy, SpiceBlend;

typedef struct SpiceTransparent {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    uint32_t src_color;
    uint32_t true_color;
} SpiceTransparent;

typedef struct SpiceAlphaBlend {
    uint16_t alpha_flags;
    uint8_t alpha;
    SpiceImage *src_bitmap;
    SpiceRect src_area;
} SpiceAlphaBlend;

typedef struct SpiceRop3 {
    SpiceImage *src_bitmap;
    SpiceRect src_area;
    SpiceBrush brush;
    uint8_t rop3;
    uint8_t scale_mode;
    SpiceQMask mask;
} SpiceRop3;

/* Given in 16.16 fixed point */
typedef struct SpiceTransform {
    uint32_t t00;
    uint32_t t01;
    uint32_t t02;
    uint32_t t10;
    uint32_t t11;
    uint32_t t12;
} SpiceTransform;

typedef struct SpiceComposite {
    uint32_t flags;
    SpiceImage *src_bitmap;
    SpiceImage *mask_bitmap;
    SpiceTransform src_transform;
    SpiceTransform mask_transform;
    SpicePoint16 src_origin;
    SpicePoint16 mask_origin;
} SpiceComposite;

typedef struct SpiceBlackness {
    SpiceQMask mask;
} SpiceBlackness, SpiceInvers, SpiceWhiteness;

typedef struct SpiceLineAttr {
    uint8_t flags;
    uint8_t style_nseg;
    SPICE_FIXED28_4 *style;
} SpiceLineAttr;

typedef struct SpiceStroke {
    SpicePath *path;
    SpiceLineAttr attr;
    SpiceBrush brush;
    uint16_t fore_mode;
    uint16_t back_mode;
} SpiceStroke;

typedef struct SpiceRasterGlyph {
    SpicePoint render_pos;
    SpicePoint glyph_origin;
    uint16_t width;
    uint16_t height;
    uint8_t data[0];
} SpiceRasterGlyph;

typedef struct SpiceString {
    uint16_t length;
    uint16_t flags;
    SpiceRasterGlyph *glyphs[0];
} SpiceString;

typedef struct SpiceText {
    SpiceString *str;
    SpiceRect back_area;
    SpiceBrush fore_brush;
    SpiceBrush back_brush;
    uint16_t fore_mode;
    uint16_t back_mode;
} SpiceText;

typedef struct SpiceCursorHeader {
    uint64_t unique;
    uint16_t type;
    uint16_t width;
    uint16_t height;
    uint16_t hot_spot_x;
    uint16_t hot_spot_y;
} SpiceCursorHeader;

SPICE_END_DECLS

#endif /* _H_SPICE_DRAW */
