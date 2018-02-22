/*
   Copyright (C) 2009 Red Hat, Inc.

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

#ifndef _H_RED_TYPES
#define _H_RED_TYPES

struct PixmapHeader {
    uint8_t* data; //数据
    int width; //宽
    int height; //高
    int stride; //行长，由每像素的字节数、宽、对齐要求决定
};

struct IconHeader {
    int width;
    int height;
    uint8_t* pixmap;
    uint8_t* mask;
};

class RedDrawable;

#endif
