/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RED_ABI_TRANSLATE_H
#define RED_ABI_TRANSLATE_H

#include <spice/qxl_dev.h>
#include "red_common.h"
#include "red_memslots.h"

typedef struct RedDrawable {
    int refs;
    QXLReleaseInfo *release_info;
    uint32_t surface_id; //绘制的目标surface
    uint8_t effect;
    uint8_t type;
    uint8_t self_bitmap; //self_bitmap_image是否有数据
	//如DRAW_COPY的SPICE_ROPD_OP_PUT直接使用下面字段图像进行绘制
    SpiceRect self_bitmap_area; //命令本身的绘图区域
    SpiceImage *self_bitmap_image; //绘图命令本身包含一副图像
    SpiceRect bbox; //绘图的目标矩形，也就是外围矩形，可能被裁剪出一部分
    SpiceClip clip; //裁剪区域，可能不裁剪，有裁剪区域，该区域表示剩余的区域
    uint32_t mm_time;
	// 最多依赖三个surface
    int32_t surfaces_dest[3]; //依赖的surface的id
    SpiceRect surfaces_rects[3]; //依赖surface的哪个矩形区域
    union {
        SpiceFill fill;
        SpiceOpaque opaque;
        SpiceCopy copy;
        SpiceTransparent transparent;
        SpiceAlphaBlend alpha_blend;
        struct {
            SpicePoint src_pos;
        } copy_bits;
        SpiceBlend blend;
        SpiceRop3 rop3;
        SpiceStroke stroke;
        SpiceText text;
        SpiceBlackness blackness;
        SpiceInvers invers;
        SpiceWhiteness whiteness;
        SpiceComposite composite;
    } u;
} RedDrawable;

typedef struct RedUpdateCmd {
    QXLReleaseInfo *release_info;
    SpiceRect area;
    uint32_t update_id;
    uint32_t surface_id;
} RedUpdateCmd;

typedef struct RedMessage {
    QXLReleaseInfo *release_info;
    uint8_t *data;
} RedMessage;

//数据块节点，以双向链表组织
typedef struct RedDataChunk RedDataChunk;
struct RedDataChunk {
    uint32_t data_size; //数据块大小
    RedDataChunk *prev_chunk;
    RedDataChunk *next_chunk;
    uint8_t *data; //数据块地址，
};

//QXL创建Surface时的命令参数
typedef struct RedSurfaceCreate {
    uint32_t format; //图像格式
    uint32_t width; //宽
    uint32_t height; //高
    int32_t stride; //行长
    uint8_t *data; //数据
} RedSurfaceCreate;

//QXL转换出来的RedSurface命令
typedef struct RedSurfaceCmd {
    QXLReleaseInfo *release_info; //QXL释放信息，用于Surface释放
    uint32_t surface_id; //SurfaceID
    uint8_t type; // 命令类型，现在只有创建和销毁
    uint32_t flags; //标记
    union {
        RedSurfaceCreate surface_create; //创建时的命令参数
    } u;
} RedSurfaceCmd;

/* 光标命令 */
typedef struct RedCursorCmd {
    QXLReleaseInfo *release_info;
    uint8_t type;
    union {
        struct {
            SpicePoint16 position;
            uint8_t visible;
            SpiceCursor shape;
        } set;
        struct {
            uint16_t length;
            uint16_t frequency;
        } trail;
        SpicePoint16 position;
    } u;
    uint8_t *device_data;
} RedCursorCmd;

void red_get_rect_ptr(SpiceRect *red, const QXLRect *qxl);

int red_get_drawable(RedMemSlotInfo *slots, int group_id,
                     RedDrawable *red, QXLPHYSICAL addr, uint32_t flags);
void red_put_drawable(RedDrawable *red);
void red_put_image(SpiceImage *red);

int red_get_update_cmd(RedMemSlotInfo *slots, int group_id,
                       RedUpdateCmd *red, QXLPHYSICAL addr);
void red_put_update_cmd(RedUpdateCmd *red);

int red_get_message(RedMemSlotInfo *slots, int group_id,
                    RedMessage *red, QXLPHYSICAL addr);
void red_put_message(RedMessage *red);

int red_get_surface_cmd(RedMemSlotInfo *slots, int group_id,
                        RedSurfaceCmd *red, QXLPHYSICAL addr);
void red_put_surface_cmd(RedSurfaceCmd *red);

int red_get_cursor_cmd(RedMemSlotInfo *slots, int group_id,
                       RedCursorCmd *red, QXLPHYSICAL addr);
void red_put_cursor_cmd(RedCursorCmd *red);

#endif
