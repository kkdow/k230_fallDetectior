#ifndef _SCOPEDTIMING_H
#define _SCOPEDTIMING_H

#include <chrono>
#include <string>
#include <iostream>

/**
 * @brief Scoped timing utility class
 *
 * This class measures and reports the elapsed time during the lifetime
 * of a ScopedTiming object using RAII semantics.
 */
class ScopedTiming
{
public:
    /**
     * @brief Constructor of ScopedTiming
     *
     * Initializes the timing object with a name and optionally starts timing.
     *
     * @param info           Name/description of the timed scope
     * @param enable_profile Enable timing and profiling (1: enable, 0: disable)
     * @return None
     */
    ScopedTiming(std::string info = "ScopedTiming", int enable_profile = 1)
        : m_info(info), enable_profile(enable_profile)
    {
        if (enable_profile)
        {
            m_start = std::chrono::steady_clock::now();
        }
    }

    /**
     * @brief Destructor of ScopedTiming
     *
     * Stops timing and prints the elapsed time when profiling is enabled.
     *
     * @return None
     */
    ~ScopedTiming()
    {
        if (enable_profile)
        {
            m_stop = std::chrono::steady_clock::now();
            double elapsed_ms =
                std::chrono::duration<double, std::milli>(m_stop - m_start).count();
            std::cout << m_info << " took " << elapsed_ms << " ms" << std::endl;
        }
    }

private:
    int enable_profile;                             // Whether profiling is enabled
    std::string m_info;                             // Name/description of the timed scope
    std::chrono::steady_clock::time_point m_start;  // Start time point
    std::chrono::steady_clock::time_point m_stop;   // End time point
};
#endif
