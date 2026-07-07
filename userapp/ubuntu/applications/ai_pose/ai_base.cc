#include "ai_base.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <string>
#include <nncase/runtime/debug.h>
#include "ai_utils.h"

// Use standard output streams
using std::cout;
using std::endl;

// nncase namespaces for runtime and K230 backend
using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::k230;
using namespace nncase::F::k230;
using namespace nncase::runtime::detail;

/**
 * @brief AIBase constructor
 *
 * Load kmodel from file, initialize input and output tensors.
 *
 * @param kmodel_file Path to kmodel binary file
 * @param model_name  Model name used for debug/timing information
 * @param debug_mode  Debug verbosity level
 */
AIBase::AIBase(const char *kmodel_file,
               const string model_name,
               const int debug_mode)
    : debug_mode_(debug_mode),
      model_name_(model_name)
{
    // Print kmodel path when debug level is high
    if (debug_mode > 1)
        cout << "kmodel_file:" << kmodel_file << endl;

    // Open kmodel file in binary mode
    std::ifstream ifs(kmodel_file, std::ios::binary);

    // Load kmodel into nncase interpreter
    kmodel_interp_.load_model(ifs).expect("Invalid kmodel");

    // Initialize input and output tensors
    set_input_init();
    set_output_init();
}

/**
 * @brief AIBase destructor
 *
 * No explicit resource release is required here,
 * nncase runtime handles tensor lifetime automatically.
 */
AIBase::~AIBase()
{
}

/**
 * @brief Initialize model input tensors
 *
 * Create host-side runtime tensors according to model input
 * descriptors and shapes, and bind them to the interpreter.
 */
void AIBase::set_input_init()
{
    // Measure input initialization time
    ScopedTiming st(model_name_ + " set_input init", debug_mode_);

    int input_total_size = 0;

    // Insert a dummy 0 for later offset-based usage
    each_input_size_by_byte_.push_back(0);

    // Iterate over all model inputs
    for (int i = 0; i < kmodel_interp_.inputs_size(); ++i)
    {
        // Get input tensor descriptor and shape
        auto desc  = kmodel_interp_.input_desc(i);
        auto shape = kmodel_interp_.input_shape(i);

        // Create host runtime tensor in shared memory pool
        auto tensor = host_runtime_tensor::create(
            desc.datatype,
            shape,
            hrt::pool_shared
        ).expect("cannot create input tensor");

        // Bind tensor to model input
        kmodel_interp_.input_tensor(i, tensor)
            .expect("cannot set input tensor");

        vector<int> in_shape;

        // Debug print input information
        if (debug_mode_ > 1)
            cout << "input " << std::to_string(i)
                 << " : " << to_string(desc.datatype) << ",";

        int dsize = 1;

        // Parse input tensor shape
        for (int j = 0; j < shape.size(); ++j)
        {
            in_shape.push_back(shape[j]);
            dsize *= shape[j];

            if (debug_mode_ > 1)
                cout << shape[j] << ",";
        }

        if (debug_mode_ > 1)
            cout << endl;

        // Store input shape for later use
        input_shapes_.push_back(in_shape);
    }
}

/**
 * @brief Get input runtime tensor by index
 *
 * @param idx Input tensor index
 * @return runtime_tensor corresponding to the input
 */
runtime_tensor AIBase::get_input_tensor(size_t idx)
{
    return kmodel_interp_.input_tensor(idx)
        .expect("cannot get input tensor");
}

/**
 * @brief Initialize model output information
 *
 * Only output shapes and descriptors are parsed here;
 * actual output tensor access happens after inference.
 */
void AIBase::set_output_init()
{
    // Measure output initialization time
    ScopedTiming st(model_name_ + " set_output_init", debug_mode_);

    each_output_size_by_byte_.clear();
    int output_total_size = 0;

    // Insert a dummy 0 for offset alignment
    each_output_size_by_byte_.push_back(0);

    // Iterate over all model outputs
    for (size_t i = 0; i < kmodel_interp_.outputs_size(); i++)
    {
        // Get output tensor descriptor and shape
        auto desc  = kmodel_interp_.output_desc(i);
        auto shape = kmodel_interp_.output_shape(i);

        vector<int> out_shape;

        // Debug print output information
        if (debug_mode_ > 1)
            cout << "output " << std::to_string(i)
                 << " : " << to_string(desc.datatype) << ",";

        int dsize = 1;

        // Parse output tensor shape
        for (int j = 0; j < shape.size(); ++j)
        {
            out_shape.push_back(shape[j]);
            dsize *= shape[j];

            if (debug_mode_ > 1)
                cout << shape[j] << ",";
        }

        if (debug_mode_ > 1)
            cout << endl;

        // Store output shape for post-processing
        output_shapes_.push_back(out_shape);
    }
}

/**
 * @brief Run model inference
 *
 * Executes the loaded kmodel using nncase runtime.
 */
void AIBase::run()
{
    ScopedTiming st(model_name_ + " run", debug_mode_);

    kmodel_interp_.run()
        .expect("error occurred in running model");
}

/**
 * @brief Fetch model output data
 *
 * Maps output tensors to host memory and stores raw float pointers
 * for subsequent post-processing.
 */
void AIBase::get_output()
{
    ScopedTiming st(model_name_ + " get_output", debug_mode_);

    // Clear previous output pointers
    p_outputs_.clear();

    // Iterate over all outputs
    for (int i = 0; i < kmodel_interp_.outputs_size(); i++)
    {
        // Get output runtime tensor
        auto out = kmodel_interp_.output_tensor(i)
            .expect("cannot get output tensor");

        // Map tensor buffer to host-readable memory
        auto buf = out.impl()
            ->to_host().unwrap()
            ->buffer().as_host().unwrap()
            .map(map_access_::map_read)
            .unwrap()
            .buffer();

        // Cast raw buffer to float pointer
        float *p_out = reinterpret_cast<float *>(buf.data());

        // Store output pointer
        p_outputs_.push_back(p_out);
    }
}

/**
 * @brief Get output runtime tensor by index
 *
 * @param idx Output tensor index
 * @return runtime_tensor corresponding to the output
 */
runtime_tensor AIBase::get_output_tensor(int idx)
{
    return kmodel_interp_.output_tensor(idx)
        .expect("cannot get current output tensor");
}
