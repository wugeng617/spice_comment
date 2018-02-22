/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2013 Jeremy White <jwhite@codeweavers.com>

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

#ifndef _H_SND_CODEC
#define _H_SND_CODEC


#if HAVE_CELT051
#include <celt051/celt.h>
#endif

#if HAVE_OPUS
#include  <opus.h>
#endif

/* Spice uses a very fixed protocol when transmitting CELT audio;
   audio must be transmitted in frames of 256, and we must compress
   data down to a fairly specific size (47, computation below).
   While the protocol doesn't inherently specify this, the expectation
   of older clients and server mandates it.
*/
										
/*一帧PCM音频数据包含多少个声音样本，（注意不是字节数）
  一个典型的立体声S16样本包括左声道、右声道两个S16的采样值，共4个字节。*/
#define SND_CODEC_CELT_FRAME_SIZE 	  256
										
										
#define SND_CODEC_CELT_BIT_RATE         (64 * 1024) //64kbitps，也就是说celt音频压缩后的数据量为64kbitps
#define SND_CODEC_CELT_PLAYBACK_FREQ    44100 //celt压缩处理的声音频率
										
										/**
										 * 假设一个样本的大小为S（可假设=4），帧的样本数量为FS，目标压缩比特率为BT（单位为bitps）, 频率为F。
										 * 那么压缩比R = BT/(F * S * 8)。由此，可以计算出一帧数据经过压缩后的大小为 (FS * S * 8) * R （单位bit）
										 * (FS * S * 8) * R = FS * S * 8 * BT /(F * S * 8) = FS * BT / F，转换成字节也就是 FS * BT / F /8字节，
										 * 所以，有如下定义公式。
										**/
#define SND_CODEC_CELT_COMPRESSED_FRAME_BYTES (SND_CODEC_CELT_FRAME_SIZE * SND_CODEC_CELT_BIT_RATE / \
																				SND_CODEC_CELT_PLAYBACK_FREQ / 8)//一帧PCM数据经过CELT压缩后的字节数
															 
/* OPUS 只支持以48000为基数的采样率 */										
#define SND_CODEC_OPUS_FRAME_SIZE       480 //OPUS一帧PCM音频数据包含的样本数量
#define SND_CODEC_OPUS_PLAYBACK_FREQ    48000 //OPUS处理的频率
#define SND_CODEC_OPUS_COMPRESSED_FRAME_BYTES 480 //一帧PCM数据经过OPUS压缩后的字节数
										
#define SND_CODEC_PLAYBACK_CHAN         2 //回放通道支持的通道数量
										
#define SND_CODEC_MAX_FRAME_SIZE        (MAX(SND_CODEC_CELT_FRAME_SIZE, SND_CODEC_OPUS_FRAME_SIZE)) //一帧数据最大的样本数量
#define SND_CODEC_MAX_FRAME_BYTES       (SND_CODEC_MAX_FRAME_SIZE * SND_CODEC_PLAYBACK_CHAN * 2 /* FMT_S16 */) //一帧数据的最大原始字节数，默认S16音频数据
#define SND_CODEC_MAX_COMPRESSED_BYTES  MAX(SND_CODEC_CELT_COMPRESSED_FRAME_BYTES, SND_CODEC_OPUS_COMPRESSED_FRAME_BYTES) //压缩后数据字节数的最大值
										
#define SND_CODEC_ANY_FREQUENCY        -1 //用一个特殊值表示任意频率
										
#define SND_CODEC_OK                    0 //编解码成功
#define SND_CODEC_UNAVAILABLE           1 //编解码器不可用
#define SND_CODEC_ENCODER_UNAVAILABLE   2 //编码器不可用
#define SND_CODEC_DECODER_UNAVAILABLE   3 //解码器不可用
#define SND_CODEC_ENCODE_FAILED         4 //编码失败
#define SND_CODEC_DECODE_FAILED         5 //解码失败
#define SND_CODEC_INVALID_ENCODE_SIZE   6 //非法的编码大小
										
#define SND_CODEC_ENCODE                0x0001 //编码标志
#define SND_CODEC_DECODE                0x0002 //解码标志
										
SPICE_BEGIN_DECLS
										
typedef struct SndCodecInternal * SndCodec;
										
int  snd_codec_is_capable(int mode, int frequency); //查询指定的模式是否支持制定的模式
										
/**
 * 使用制定的模式、频率创建编解码器
 * mode: celt或者opus
 * frequency: 频率
 * purpose: 编码还是解码，还是两者都创建
 */

int  snd_codec_create(SndCodec *codec, int mode, int frequency, int purpose);
void snd_codec_destroy(SndCodec *codec);

int  snd_codec_frame_size(SndCodec codec);

int  snd_codec_encode(SndCodec codec, uint8_t *in_ptr, int in_size, uint8_t *out_ptr, int *out_size);
int  snd_codec_decode(SndCodec codec, uint8_t *in_ptr, int in_size, uint8_t *out_ptr, int *out_size);

SPICE_END_DECLS

#endif
