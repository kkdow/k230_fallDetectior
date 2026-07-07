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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.f
 */

#include "pose_detect.h"
#include "ai_utils.h"

action_helper action_helper_squat{false, 0, 0};

poseDetect::poseDetect(char *kmodel_file, float obj_thresh,float nms_thresh, FrameCHWSize image_size, int debug_mode)
: obj_thresh_(obj_thresh),nms_thresh_(nms_thresh), AIBase(kmodel_file,"poseDetect", debug_mode)
{
    model_name_ = "poseDetect";
    image_size_ = image_size;
    input_size_ = {input_shapes_[0][1], input_shapes_[0][2],input_shapes_[0][3]};
    obj_thresh_=obj_thresh;
    nms_thresh_=nms_thresh;
    labels_ = {"person"};
    label_num_=labels_.size();
    max_box_num_=50;
    box_num_=((input_size_.width/8)*(input_size_.height/8)+(input_size_.width/16)*(input_size_.height/16)+(input_size_.width/32)*(input_size_.height/32));
    box_feature_len_=label_num_+4+51;
    ai2d_out_tensor_ = get_input_tensor(0);
    Utils::padding_resize_one_side_set(image_size_,input_size_,ai2d_builder_, cv::Scalar(114, 114, 114));
}

poseDetect::~poseDetect()
{

}

void poseDetect::pre_process(runtime_tensor& input_tensor)
{
    ScopedTiming st(model_name_ + " pre_process image", debug_mode_);
    ai2d_builder_->invoke(input_tensor,ai2d_out_tensor_).expect("error occurred in ai2d running");   
}

void poseDetect::inference()
{
    this->run();
    this->get_output();
}

void poseDetect::post_process( std::vector<OutputPose> &results)
{
    ScopedTiming st(model_name_ + " post_process", debug_mode_);
    float ratiow = (float)input_size_.width / image_size_.width;
    float ratioh = (float)input_size_.height / image_size_.height;
    float ratio = ratiow < ratioh ? ratiow : ratioh;
    float *output_det = new float[box_num_ * box_feature_len_];
    // 模型推理结束后，进行后处理
    float* output0= p_outputs_[0];
    // 将输出数据排布从[label_num_+4+51,(w/8)*(h/8)+(w/16)*(h/16)+(w/32)*(h/32)]调整为[(w/8)*(h/8)+(w/16)*(h/16)+(w/32)*(h/32),label_num_+4+51],方便后续处理
    for(int r = 0; r < box_num_; r++)
    {
        for(int c = 0; c < box_feature_len_; c++)
        {
            output_det[r*box_feature_len_ + c] = output0[c*box_num_ + r];
        }
    }
    for(int i=0;i<box_num_;i++){
        float* vec=output_det+i*box_feature_len_;
        float box[4]={vec[0],vec[1],vec[2],vec[3]};
        float score=*(vec+4);
        float* kps_ptr=vec+5;
        if(score>obj_thresh_){
            OutputPose bbox;
            float x_=box[0]/ratio*1.0;
            float y_=box[1]/ratio*1.0;
            float w_=box[2]/ratio*1.0;
            float h_=box[3]/ratio*1.0;
            int x=int(MAX(x_-0.5*w_,0));
            int y=int(MAX(y_-0.5*h_,0));
            int w=int(w_);
            int h=int(h_);
            if (w <= 0 || h <= 0) { continue; }
            bbox.box=cv::Rect(x,y,w,h);
            bbox.confidence=score;
            bbox.index=0;
            for (int k=0; k< 17; k++){
                float kps_x = (*(kps_ptr + 3 * k))/ratio*1.0;
                float kps_y = (*(kps_ptr + 3 * k + 1))/ratio*1.0;
                float kps_s = *(kps_ptr + 3 * k + 2);
                bbox.kps.push_back(kps_x);
                bbox.kps.push_back(kps_y);
                bbox.kps.push_back(kps_s);
            }
            results.push_back(bbox);
        }

    }
    //执行非最大抑制以消除具有较低置信度的冗余重叠框（NMS）
    std::vector<int> nms_result;
    pose_nms(results, obj_thresh_, nms_thresh_, nms_result);
    delete[] output_det;
}

void poseDetect::pose_nms(std::vector<OutputPose> &bboxes, float confThreshold, float nmsThreshold, std::vector<int> &indices)
{
    std::sort(bboxes.begin(), bboxes.end(), [](OutputPose &a, OutputPose &b) { return a.confidence > b.confidence; });
    int updated_size = bboxes.size();
    for (int i = 0; i < updated_size; i++) {
        if (bboxes[i].confidence < confThreshold)
            continue;
        indices.push_back(i);
        // 这里使用移除冗余框，而不是 erase 操作，减少内存移动的开销
        for (int j = i + 1; j < updated_size;) {
            float iou = iou_calculate(bboxes[i].box, bboxes[j].box);
            if (iou > nmsThreshold) {
                bboxes[j].confidence = -1;  // 设置为负值，后续不会再计算其IOU
            }
            j++;
        }
    }
    // 移除那些置信度小于0的框
    bboxes.erase(std::remove_if(bboxes.begin(), bboxes.end(), [](OutputPose &b) { return b.confidence < 0; }), bboxes.end());
}

float poseDetect::iou_calculate(cv::Rect &rect1, cv::Rect &rect2)
{
    int xx1, yy1, xx2, yy2;
 
	xx1 = std::max(rect1.x, rect2.x);
	yy1 = std::max(rect1.y, rect2.y);
	xx2 = std::min(rect1.x + rect1.width - 1, rect2.x + rect2.width - 1);
	yy2 = std::min(rect1.y + rect1.height - 1, rect2.y + rect2.height - 1);
 
	int insection_width, insection_height;
	insection_width = std::max(0, xx2 - xx1 + 1);
	insection_height = std::max(0, yy2 - yy1 + 1);
 
	float insection_area, union_area, iou;
	insection_area = float(insection_width) * insection_height;
	union_area = float(rect1.width*rect1.height + rect2.width*rect2.height - insection_area);
	iou = insection_area / union_area;

	return iou;
}

void poseDetect::draw_result(cv::Mat& img, FrameCHWSize frame_size, std::vector<OutputPose>& results)
{
    int w_=img.cols;
    int h_=img.rows;
    int res_size=MIN(results.size(),max_box_num_);
    for(int i=0;i<results.size();i++){
        OutputPose box_=results[i];
        cv::Rect box=box_.box;
        int idx=box_.index;
        float score=box_.confidence;
        int x=int(box.x*float(w_)/image_size_.width);
        int y=int(box.y*float(h_)/image_size_.height);
        int w=int(box.width*float(w_)/image_size_.width);
        int h=int(box.height*float(h_)/image_size_.height);
        int x_right = x + w;
        int y_bottom = y + h;
        if (x_right > w_)
        {
            w = w_ - x;
        }
        if (y_bottom > h_)
        {
            h = h_ - y;
        }
        cv::Rect new_box(x,y,w,h);
        cv::rectangle(img, new_box, cv::Scalar(255, 0, 0,255), 2, 8);
        cv::putText(img, labels_[idx]+" "+std::to_string(score), cv::Point(MIN(new_box.x + 5,w_), MAX(new_box.y - 10,0)), cv::FONT_HERSHEY_DUPLEX, 1,cv::Scalar(255, 0, 0,255) , 2, 0);

        int num_point = 17;
        std::vector<KeyPoint> keypoints;
        for(int k=0;k<num_point;k++)
        {
            //关键点绘制
            int kps_x = box_.kps[k*3];
            int kps_y = box_.kps[k*3 + 1];
            float kps_s = box_.kps[k*3 + 2];
            int kps_x1 = int(kps_x * float(w_)/image_size_.width);
            int kps_y1 =  int(kps_y * float(h_)/image_size_.height);
            cv::circle(img, {kps_x1, kps_y1}, 5, cv::Scalar(KPS_COLORS[k][0],KPS_COLORS[k][1],KPS_COLORS[k][2],255), 3);
            cv::Point2f p(kps_x1,kps_y1);
            keypoints.push_back( { p,kps_s} );
        }

        for (int k=0; k<num_point+2; k++){
            // 骨骼连线绘制
            auto &ske = SKELETON[k];
            int pos1_x = keypoints[ske[0]-1].p.x;
            int pos1_y = keypoints[ske[0]-1].p.y;
            int pos2_x = keypoints[ske[1]-1].p.x;
            int pos2_y = keypoints[ske[1]-1].p.y;
            float pos1_s = keypoints[ske[0]-1].prob;
            float pos2_s = keypoints[ske[1]-1].prob;
            cv::line(img, {pos1_x, pos1_y}, {pos2_x, pos2_y}, cv::Scalar(LIMB_COLORS[k][0], LIMB_COLORS[k][1], LIMB_COLORS[k][2],255),3);
        }
    }
}

