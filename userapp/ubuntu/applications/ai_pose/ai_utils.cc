#include <iostream>
#include "ai_utils.h"

using std::ofstream;
using std::vector;

/**
 * @brief Generate a color list for visualization based on class count
 *
 * Colors are selected cyclically using modulo operation when the number
 * of classes exceeds the available color palette size.
 *
 * @param num_classes Number of classes
 * @return Vector of colors corresponding to each class
 */
std::vector<cv::Scalar> getColorsForClasses(int num_classes) {
    std::vector<cv::Scalar> colors;
    int num_available_colors = color_four.size();
    for (int i = 0; i < num_classes; ++i) {
        colors.push_back(color_four[i % num_available_colors]);
    }
    return colors;
}

/**
 * @brief Fast block-wise transpose for detection output
 *
 * This function transposes a matrix of shape:
 *   [box_feature_len, box_num]
 * into:
 *   [box_num, box_feature_len]
 *
 * The implementation uses tiling and RISC-V Vector (RVV) instructions
 * to improve memory access locality and performance.
 *
 * @param input0           Pointer to input buffer (row-major)
 * @param output_det       Pointer to output buffer (row-major, transposed)
 * @param box_num          Number of boxes
 * @param box_feature_len  Feature length per box
 */
void transpose_block_fast(const float* input0, float* output_det,
                          int box_num, int box_feature_len)
{
    // Tile size for block-wise transpose
    const int TILE = 8;

    // Iterate over feature dimension in tiles
    for (int i = 0; i < box_feature_len; i += TILE)
    {
        int i_max = std::min(i + TILE, box_feature_len);

        // Iterate over box dimension in tiles
        for (int j = 0; j < box_num; j += TILE)
        {
            int j_max = std::min(j + TILE, box_num);

            int h = i_max - i; // Tile height
            int w = j_max - j; // Tile width

            // Small stack buffer to hold one tile in row-major order
            float buf[TILE][TILE] = {0};

            // Load each row of the tile using RVV
            for (int r = 0; r < h; ++r)
            {
                const float* src = input0 + (i + r) * box_num + j;

                // Set vector length for float32 elements
                size_t vl = vsetvl_e32m1(w);

                // Load vector data from source
                vfloat32m1_t vtmp = vle32_v_f32m1(src, vl);

                // Store vector data into temporary buffer
                vse32_v_f32m1(buf[r], vtmp, vl);
            }

            // Write transposed tile from temporary buffer to output
            // Output layout: [box_num, box_feature_len]
            for (int r = 0; r < h; ++r)
            {
                for (int c = 0; c < w; ++c)
                {
                    output_det[(j + c) * box_feature_len + (i + r)] = buf[r][c];
                }
            }
        }
    }
}

/**
 * @brief Configure ai2d pipeline for resize with one-side padding
 *
 * The input image is resized while preserving aspect ratio,
 * then padded on the bottom and right sides to match the output size.
 *
 * @param input_shape   Input tensor shape (CHW)
 * @param output_shape  Output tensor shape (CHW)
 * @param builder       ai2d builder instance (output)
 * @param padding       Padding color (Scalar)
 */
void Utils::padding_resize_one_side_set(
    FrameCHWSize input_shape,
    FrameCHWSize output_shape,
    std::unique_ptr<ai2d_builder> &builder,
    const cv::Scalar padding)
{
    // Compute resize ratios
    float ratiow = (float)output_shape.width / input_shape.width;
    float ratioh = (float)output_shape.height / input_shape.height;
    float ratio = ratiow < ratioh ? ratiow : ratioh;

    // Compute resized dimensions
    int new_w = (int)(ratio * input_shape.width);
    int new_h = (int)(ratio * input_shape.height);

    // Padding configuration (pad bottom and right)
    int top = 0;
    int bottom = output_shape.height - new_h;
    int left = 0;
    int right = output_shape.width - new_w;

    // ai2d data type configuration
    ai2d_datatype_t ai2d_dtype{
        ai2d_format::NCHW_FMT,
        ai2d_format::NCHW_FMT,
        typecode_t::dt_uint8,
        typecode_t::dt_uint8
    };

    // ai2d operation parameters
    ai2d_crop_param_t crop_param{false, 0, 0, 0, 0};
    ai2d_shift_param_t shift_param{false, 0};
    ai2d_pad_param_t pad_param{
        true,
        {{0, 0}, {0, 0}, {top, bottom}, {left, right}},
        ai2d_pad_mode::constant,
        {padding[0], padding[1], padding[2]}
    };
    ai2d_resize_param_t resize_param{
        true,
        ai2d_interp_method::tf_bilinear,
        ai2d_interp_mode::half_pixel
    };
    ai2d_affine_param_t affine_param{
        false,
        ai2d_interp_method::cv2_bilinear,
        0, 0, 127, 1,
        {0.5, 0.1, 0.0, 0.1, 0.5, 0.0}
    };

    // Define input and output tensor shapes
    dims_t in_shape  = {1, 3, input_shape.height, input_shape.width};
    dims_t out_shape = {1, 3, output_shape.height, output_shape.width};

    // Create and build ai2d pipeline
    builder.reset(new ai2d_builder(
        in_shape, out_shape,
        ai2d_dtype,
        crop_param,
        shift_param,
        pad_param,
        resize_param,
        affine_param
    ));
    builder->build_schedule();
}

/**
 * @brief Configure ai2d pipeline for crop followed by resize
 *
 * The input image is first cropped to the specified region,
 * then resized to the target output shape.
 *
 * @param input_shape   Input tensor shape (CHW)
 * @param output_shape  Output tensor shape (CHW)
 * @param x             Crop start x coordinate
 * @param y             Crop start y coordinate
 * @param w             Crop width
 * @param h             Crop height
 * @param builder       ai2d builder instance (output)
 */
void Utils::crop_resize_set(
    FrameCHWSize input_shape,
    FrameCHWSize output_shape,
    int x,
    int y,
    int w,
    int h,
    std::unique_ptr<ai2d_builder> &builder)
{
    // ai2d data type configuration
    ai2d_datatype_t ai2d_dtype{
        ai2d_format::NCHW_FMT,
        ai2d_format::NCHW_FMT,
        typecode_t::dt_uint8,
        typecode_t::dt_uint8
    };

    // Enable cropping
    ai2d_crop_param_t crop_param{true, x, y, w, h};
    ai2d_shift_param_t shift_param{false, 0};
    ai2d_pad_param_t pad_param{
        false,
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
        ai2d_pad_mode::constant,
        {0, 0, 0}
    };
    ai2d_resize_param_t resize_param{
        true,
        ai2d_interp_method::tf_bilinear,
        ai2d_interp_mode::half_pixel
    };
    ai2d_affine_param_t affine_param{
        false,
        ai2d_interp_method::cv2_bilinear,
        0, 0, 127, 1,
        {0.5, 0.1, 0.0, 0.1, 0.5, 0.0}
    };

    // Define input and output tensor shapes
    dims_t in_shape  = {1, 3, input_shape.height, input_shape.width};
    dims_t out_shape = {1, 3, output_shape.height, output_shape.width};

    // Create and build ai2d pipeline
    builder.reset(new ai2d_builder(
        in_shape, out_shape,
        ai2d_dtype,
        crop_param,
        shift_param,
        pad_param,
        resize_param,
        affine_param
    ));
    builder->build_schedule();
}
