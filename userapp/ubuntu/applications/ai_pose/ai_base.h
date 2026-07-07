#ifndef AI_BASE_H
#define AI_BASE_H

#include <vector>
#include <string>
#include <fstream>

#include <nncase/runtime/interpreter.h>
#include <nncase/runtime/runtime_op_utility.h>
#include "scoped_timing.h"

using std::string;
using std::vector;
using namespace nncase::runtime;

/**
 * @brief Base AI class that encapsulates nncase-related operations
 *
 * This class wraps nncase model loading, input initialization,
 * inference execution, and output retrieval.
 *
 * For demo or application development, users only need to focus on
 * model pre-processing and post-processing logic.
 */
class AIBase
{
public:
    /**
     * @brief AIBase constructor
     *
     * Loads the kmodel file and initializes model input and output tensors.
     *
     * @param kmodel_file Path to the kmodel file
     * @param model_name  Model name used for logging and timing
     * @param debug_mode  Debug level:
     *                    0 - no debug output
     *                    1 - print timing information only
     *                    2 - print all debug information
     */
    AIBase(const char *kmodel_file, const string model_name, const int debug_mode = 1);

    /**
     * @brief AIBase destructor
     */
    ~AIBase();

    /**
     * @brief Get kmodel input tensor by index
     *
     * @param idx Input tensor index
     * @return runtime_tensor corresponding to the input
     */
    runtime_tensor get_input_tensor(size_t idx);

    /**
     * @brief Run kmodel inference
     */
    void run();

    /**
     * @brief Retrieve kmodel output data
     *
     * The output results are stored in the corresponding class members.
     */
    void get_output();

    /**
     * @brief Get kmodel output tensor by index
     *
     * @param idx Output tensor index
     * @return runtime_tensor corresponding to the output
     */
    runtime_tensor get_output_tensor(int idx);

protected:
    string model_name_;                    // Model name
    int debug_mode_;                       // Debug mode:
                                          // 0 - no print
                                          // 1 - timing only
                                          // 2 - full debug output
    vector<float *> p_outputs_;            // List of pointers to kmodel output buffers
    vector<vector<int>> input_shapes_;     // Input shapes, e.g. {{N,C,H,W}, {N,C,H,W}, ...}
    vector<vector<int>> output_shapes_;    // Output shapes, e.g. {{N,C,H,W}, {N,C}, ...}
    vector<int> each_input_size_by_byte_;  // Cumulative input sizes in bytes:
                                          // {0, layer1_size, layer1+layer2_size, ...}
    vector<int> each_output_size_by_byte_; // Cumulative output sizes in bytes:
                                          // {0, layer1_size, layer1+layer2_size, ...}

private:
    /**
     * @brief Initialize kmodel inputs and retrieve input shapes
     *
     * This function is called once during construction.
     */
    void set_input_init();

    /**
     * @brief Initialize kmodel outputs and retrieve output shapes
     *
     * This function is called once during construction.
     */
    void set_output_init();

    interpreter kmodel_interp_;        // kmodel interpreter responsible for model loading,
                                       // input/output binding, and inference execution
    vector<unsigned char> kmodel_vec_; // Raw kmodel data read from file, used to load the model
};

#endif
