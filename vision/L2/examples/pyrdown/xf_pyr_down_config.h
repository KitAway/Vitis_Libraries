/*
 * Copyright 2019 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _XF_PYR_DOWN_CONFIG_
#define _XF_PYR_DOWN_CONFIG_

#include "hls_stream.h"
#include "ap_int.h"

#include "common/xf_common.hpp"
#include "xf_config_params.h"
#include "common/xf_utility.hpp"
#include "imgproc/xf_pyr_down.hpp"
#define NPC_T XF_NPPC1
#if RGBA
#define TYPE XF_8UC3
#define CH_TYPE XF_RGB

#else
#define TYPE XF_8UC1
#define CH_TYPE XF_GRAY
#endif
// void pyr_down_accel(xf::cv::Mat<TYPE, HEIGHT, WIDTH, XF_NPPC1> &_src, xf::cv::Mat<TYPE, HEIGHT, WIDTH, XF_NPPC1>
// &_dst);
#endif
