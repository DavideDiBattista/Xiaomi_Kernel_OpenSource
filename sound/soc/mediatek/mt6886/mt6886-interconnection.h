/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mediatek MT6886 audio driver interconnection definition
 *
 *  Copyright (c) 2021 MediaTek Inc.
 *  Author: Tina Tsai <tina.tsai@mediatek.com>
 */

#ifndef _MT6886_INTERCONNECTION_H_
#define _MT6886_INTERCONNECTION_H_

/* in port define */
#define I_I2S0_CH1 0
#define I_I2S0_CH2 1
#define I_ADDA_UL_CH1 3
#define I_ADDA_UL_CH2 4
#define I_DL1_CH1 5
#define I_DL1_CH2 6
#define I_DL2_CH1 7
#define I_DL2_CH2 8
#define I_PCM_1_CAP_CH1 9
#define I_GAIN1_OUT_CH1 10
#define I_GAIN1_OUT_CH2 11
#define I_GAIN2_OUT_CH1 12
#define I_GAIN2_OUT_CH2 13
#define I_PCM_2_CAP_CH1 14
#define I_ADDA_UL_CH3 17
#define I_ADDA_UL_CH4 18
#define I_DL12_CH1 19
#define I_DL12_CH2 20
#define I_PCM_2_CAP_CH2 21
#define I_PCM_1_CAP_CH2 22
#define I_DL3_CH1 23
#define I_DL3_CH2 24
#define I_I2S2_CH1 25
#define I_I2S2_CH2 26
#define I_I2S2_CH3 27
#define I_I2S2_CH4 28

/* in port define >= 32 */
#define I_32_OFFSET 32
#define I_CONNSYS_I2S_CH1 (34 - I_32_OFFSET)
#define I_CONNSYS_I2S_CH2 (35 - I_32_OFFSET)
#define I_SRC_1_OUT_CH1 (36 - I_32_OFFSET)
#define I_SRC_1_OUT_CH2 (37 - I_32_OFFSET)
#define I_SRC_2_OUT_CH1 (38 - I_32_OFFSET)
#define I_SRC_2_OUT_CH2 (39 - I_32_OFFSET)
#define I_DL4_CH1 (40 - I_32_OFFSET)
#define I_DL4_CH2 (41 - I_32_OFFSET)
#define I_DL5_CH1 (42 - I_32_OFFSET)
#define I_DL5_CH2 (43 - I_32_OFFSET)
#define I_DL6_CH1 (44 - I_32_OFFSET)
#define I_DL6_CH2 (45 - I_32_OFFSET)
#define I_DL7_CH1 (46 - I_32_OFFSET)
#define I_DL7_CH2 (47 - I_32_OFFSET)
#define I_DL8_CH1 (48 - I_32_OFFSET)
#define I_DL8_CH2 (49 - I_32_OFFSET)
#define I_DL9_CH1 (50 - I_32_OFFSET)
#define I_DL9_CH2 (51 - I_32_OFFSET)

/* in port define >= 64 */
#define I_64_OFFSET 64
#define I_DL11_CH1 (66 - I_64_OFFSET)
#define I_DL11_CH2 (67 - I_64_OFFSET)
#define I_DL11_CH3 (68 - I_64_OFFSET)
#define I_DL11_CH4 (69 - I_64_OFFSET)
#define I_DL11_CH5 (70 - I_64_OFFSET)
#define I_DL11_CH6 (71 - I_64_OFFSET)
#define I_DL11_CH7 (72 - I_64_OFFSET)
#define I_DL11_CH8 (73 - I_64_OFFSET)
#define I_DL13_CH1 (83 - I_64_OFFSET)
#define I_DL13_CH2 (84 - I_64_OFFSET)

#endif
