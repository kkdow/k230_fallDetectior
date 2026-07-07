/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _POSE_DETECT
#define _POSE_DETECT

#include <iostream>
#include <vector>
#include "ai_utils.h"
#include "ai_base.h"

/**
 * @brief 关键点信息
 */
struct KeyPoint
{
    cv::Point2f p;  // 关键点
    float prob;     // 关键点概率值
};

/** 
 * @brief action帮助信息
 */
struct action_helper {
  bool mark = false;  // 是否标记
  int action_count = 0;  // action计数
  int latency = 0;  // 延迟次数
};

/**
 * @brief Pose输出结果信息
 */
struct  OutputPose {
    cv::Rect box;  // 行人检测框 box
    int index;     // 检测框标签，默认为行人（0）
    float confidence; // 置信度
    std::vector<float> kps; // 关键点向量
};

const std::vector<std::vector<unsigned int>> KPS_COLORS =    // 关键点颜色
        {{0,   255, 0},
         {0,   255, 0},
         {0,   255, 0},
         {0,   255, 0},
         {0,   255, 0},
         {255, 128, 0},
         {255, 128, 0},
         {255, 128, 0},
         {255, 128, 0},
         {255, 128, 0},
         {255, 128, 0},
         {51,  153, 255},
         {51,  153, 255},
         {51,  153, 255},
         {51,  153, 255},
         {51,  153, 255},
         {51,  153, 255}};
 
const std::vector<std::vector<unsigned int>> SKELETON = {{16, 14},  // 骨骼连线
                                                         {14, 12},
                                                         {17, 15},
                                                         {15, 13},
                                                         {12, 13},
                                                         {6,  12},
                                                         {7,  13},
                                                         {6,  7},
                                                         {6,  8},
                                                         {7,  9},
                                                         {8,  10},
                                                         {9,  11},
                                                         {2,  3},
                                                         {1,  2},
                                                         {1,  3},
                                                         {2,  4},
                                                         {3,  5},
                                                         {4,  6},
                                                         {5,  7}};
 
const std::vector<std::vector<unsigned int>> LIMB_COLORS = {{51,  153, 255},  // 骨骼连线颜色
                                                            {51,  153, 255},
                                                            {51,  153, 255},
                                                            {51,  153, 255},
                                                            {255, 51,  255},
                                                            {255, 51,  255},
                                                            {255, 51,  255},
                                                            {255, 128, 0},
                                                            {255, 128, 0},
                                                            {255, 128, 0},
                                                            {255, 128, 0},
                                                            {255, 128, 0},
                                                            {0,   255, 0},
                                                            {0,   255, 0},
                                                            {0,   255, 0},
                                                            {0,   255, 0},
                                                            {0,   255, 0},
                                                            {0,   255, 0},
                                                            {0,   255, 0}};

/**
 * @brief 人体关键点检测任务
 * 主要封装了对于每一帧图片，从预处理、运行到后处理给出结果的过程
 */
class poseDetect: public AIBase
{
    public:
        /** 
        * for video
        * @brief poseDetect 构造函数，加载kmodel,并初始化kmodel输入、输出、类阈值和NMS阈值
        * @param kmodel_file kmodel文件路径
        * @param obj_thresh 检测框阈值
        * @param nms_thresh NMS阈值
        * @param image_size 输入分辨率
        * @param debug_mode 0（不调试）、 1（只显示时间）、2（显示所有打印信息）
        * @return None
        */
        poseDetect(char *kmodel_file, float obj_thresh,float nms_thresh, FrameCHWSize image_size, int debug_mode);

        /** 
        * @brief  poseDetect 析构函数
        * @return None
        */
        ~poseDetect();

        void pre_process(runtime_tensor &input_tensor);


        /**
         * @brief kmodel推理
         * @return None
         */
        void inference();

        void post_process( std::vector<OutputPose> &results);

        void draw_result(cv::Mat& img, FrameCHWSize frame_size, std::vector<OutputPose>& results);

    private:
        void pose_nms(std::vector<OutputPose> &bboxes, float confThreshold, float nmsThreshold, std::vector<int> &indices);

        float iou_calculate(cv::Rect &rect1, cv::Rect &rect2);

        float obj_thresh_;  // 检测框阈值
        float nms_thresh_;  // NMS阈值
        // 检测框的总数
        int box_num_;
        int max_box_num_;
        // 每个检测框的特征维度
        int box_feature_len_;
        // 标签列表
        std::vector<std::string> labels_;
        int label_num_;

        float *output_0;   // 输出

        std::unique_ptr<ai2d_builder> ai2d_builder_; // ai2d构建器
        runtime_tensor ai2d_in_tensor_;              // ai2d输入tensor
        runtime_tensor ai2d_out_tensor_;             // ai2d输出tensor
        FrameCHWSize image_size_;
        FrameCHWSize input_size_;

};
#endif
