#ifndef UTILS_H
#define UTILS_H

#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include "riscv_vector.h"
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <nncase/functional/ai2d/ai2d_builder.h>

// nncase namespaces
using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::k230;
using namespace nncase::F::k230;

// Commonly used types and streams
using cv::Mat;
using std::cout;
using std::endl;
using std::ifstream;
using std::vector;

/**
 * @brief Color palette for visualization
 *
 * Contains 80 predefined colors.
 * When the number of categories exceeds 80, the color index is taken modulo 80.
 */
const std::vector<cv::Scalar> color_four = {
       cv::Scalar(127, 220, 20, 60),
       cv::Scalar(127, 119, 11, 32),
       cv::Scalar(127, 0, 0, 142),
       cv::Scalar(127, 0, 0, 230),
       cv::Scalar(127, 106, 0, 228),
       cv::Scalar(127, 0, 60, 100),
       cv::Scalar(127, 0, 80, 100),
       cv::Scalar(127, 0, 0, 70),
       cv::Scalar(127, 0, 0, 192),
       cv::Scalar(127, 250, 170, 30),
       cv::Scalar(127, 100, 170, 30),
       cv::Scalar(127, 220, 220, 0),
       cv::Scalar(127, 175, 116, 175),
       cv::Scalar(127, 250, 0, 30),
       cv::Scalar(127, 165, 42, 42),
       cv::Scalar(127, 255, 77, 255),
       cv::Scalar(127, 0, 226, 252),
       cv::Scalar(127, 182, 182, 255),
       cv::Scalar(127, 0, 82, 0),
       cv::Scalar(127, 120, 166, 157),
       cv::Scalar(127, 110, 76, 0),
       cv::Scalar(127, 174, 57, 255),
       cv::Scalar(127, 199, 100, 0),
       cv::Scalar(127, 72, 0, 118),
       cv::Scalar(127, 255, 179, 240),
       cv::Scalar(127, 0, 125, 92),
       cv::Scalar(127, 209, 0, 151),
       cv::Scalar(127, 188, 208, 182),
       cv::Scalar(127, 0, 220, 176),
       cv::Scalar(127, 255, 99, 164),
       cv::Scalar(127, 92, 0, 73),
       cv::Scalar(127, 133, 129, 255),
       cv::Scalar(127, 78, 180, 255),
       cv::Scalar(127, 0, 228, 0),
       cv::Scalar(127, 174, 255, 243),
       cv::Scalar(127, 45, 89, 255),
       cv::Scalar(127, 134, 134, 103),
       cv::Scalar(127, 145, 148, 174),
       cv::Scalar(127, 255, 208, 186),
       cv::Scalar(127, 197, 226, 255),
       cv::Scalar(127, 171, 134, 1),
       cv::Scalar(127, 109, 63, 54),
       cv::Scalar(127, 207, 138, 255),
       cv::Scalar(127, 151, 0, 95),
       cv::Scalar(127, 9, 80, 61),
       cv::Scalar(127, 84, 105, 51),
       cv::Scalar(127, 74, 65, 105),
       cv::Scalar(127, 166, 196, 102),
       cv::Scalar(127, 208, 195, 210),
       cv::Scalar(127, 255, 109, 65),
       cv::Scalar(127, 0, 143, 149),
       cv::Scalar(127, 179, 0, 194),
       cv::Scalar(127, 209, 99, 106),
       cv::Scalar(127, 5, 121, 0),
       cv::Scalar(127, 227, 255, 205),
       cv::Scalar(127, 147, 186, 208),
       cv::Scalar(127, 153, 69, 1),
       cv::Scalar(127, 3, 95, 161),
       cv::Scalar(127, 163, 255, 0),
       cv::Scalar(127, 119, 0, 170),
       cv::Scalar(127, 0, 182, 199),
       cv::Scalar(127, 0, 165, 120),
       cv::Scalar(127, 183, 130, 88),
       cv::Scalar(127, 95, 32, 0),
       cv::Scalar(127, 130, 114, 135),
       cv::Scalar(127, 110, 129, 133),
       cv::Scalar(127, 166, 74, 118),
       cv::Scalar(127, 219, 142, 185),
       cv::Scalar(127, 79, 210, 114),
       cv::Scalar(127, 178, 90, 62),
       cv::Scalar(127, 65, 70, 15),
       cv::Scalar(127, 127, 167, 115),
       cv::Scalar(127, 59, 105, 106),
       cv::Scalar(127, 142, 108, 45),
       cv::Scalar(127, 196, 172, 0),
       cv::Scalar(127, 95, 54, 80),
       cv::Scalar(127, 128, 76, 255),
       cv::Scalar(127, 201, 57, 1),
       cv::Scalar(127, 246, 0, 122),
       cv::Scalar(127, 191, 162, 208)
};

/**
 * @brief Generate color palette for a specified number of classes
 *
 * @param num_classes Number of object categories
 * @return std::vector<cv::Scalar> Vector of colors for each class
 */
std::vector<cv::Scalar> getColorsForClasses(int num_classes);

/**
 * @brief Frame size definition in CHW layout
 *
 * Represents the size of a single image or frame
 * using Channel-Height-Width order.
 */
typedef struct FrameCHWSize
{
    size_t channel; // Number of channels
    size_t height;  // Image height
    size_t width;   // Image width
} FrameCHWSize;

/**
 * @brief Tracking information structure
 *
 * Used to store basic lifecycle information for a tracked object.
 */
struct TrackInfo {
    int id;           // Unique track ID
    int first_frame;  // Index of the first frame where the object appears
    int last_frame;   // Index of the last frame where the object appears
    bool active;      // Whether the track is currently active
};

/**
 * @brief Fast block-wise transpose for detection output
 *
 * @param input0            Pointer to input buffer
 * @param output_det        Pointer to output buffer
 * @param box_num           Number of boxes
 * @param box_feature_len   Feature length per box
 */
void transpose_block_fast(const float* input0,
                          float* output_det,
                          int box_num,
                          int box_feature_len);

/**
 * @brief Utility class
 *
 * Encapsulates commonly used AI-related helper functions,
 * mainly for image pre-processing and ai2d configuration.
 */
class Utils
{
public:
    // ------------------------------ ai2d processing configuration -------------------------------------------

    /**
     * @brief Configure ai2d for one-side padding followed by resize
     *
     * @param input_shape   Input tensor shape (CHW)
     * @param output_shape  Output tensor shape (CHW)
     * @param builder       ai2d builder instance
     * @param padding       Padding value (Scalar)
     */
    static void padding_resize_one_side_set(
        FrameCHWSize input_shape,
        FrameCHWSize output_shape,
        std::unique_ptr<ai2d_builder> &builder,
        const cv::Scalar padding
    );

    /**
     * @brief Configure ai2d for crop followed by resize
     *
     * @param input_shape   Input tensor shape (CHW)
     * @param output_shape  Output tensor shape (CHW)
     * @param x             Crop start x coordinate
     * @param y             Crop start y coordinate
     * @param w             Crop width
     * @param h             Crop height
     * @param builder       ai2d builder instance
     */
    static void crop_resize_set(
        FrameCHWSize input_shape,
        FrameCHWSize output_shape,
        int x,
        int y,
        int w,
        int h,
        std::unique_ptr<ai2d_builder> &builder
    );
};

#endif
