/*!
 * @file window.hpp
 *
 * @brief Window class header file
 */

#pragma once

#include <stdlib.h>
#include <vector>
#include <memory>
#include <string>
#include <utility>

namespace spoa {
    class AlignmentEngine;
}

namespace racon {

enum class WindowType {
    kNGS, // Next Generation Sequencing
    kTGS // Third Generation Sequencing
};

class Window;
std::shared_ptr<Window> createWindow(uint64_t id, uint32_t rank, WindowType type, bool overlap,
    const char* backbone, uint32_t backbone_length, const char* quality,
    uint32_t quality_length);

class Window {

public:
    ~Window();

    uint64_t id() const {
        return id_;
    }
    uint32_t rank() const {
        return rank_;
    }

    const std::string& consensus() const {
        return consensus_;
    }

    const std::vector<uint32_t>& summary() const {
        return summary_;
    }

    const std::vector<int32_t>& coder() const {
        return coder_;
    }

    const std::vector<uint32_t>& q_ids() const {
        return q_ids_;
    }

    const std::vector<std::pair<uint32_t, uint32_t>>& positions() const {
        return positions_;
    }

    bool generate_consensus(std::shared_ptr<spoa::AlignmentEngine> alignment_engine,
        bool trim);

    void add_layer(const char* sequence, uint32_t sequence_length,
        const char* quality, uint32_t quality_length, uint32_t begin,
        uint32_t end, uint32_t q_id);

    friend std::shared_ptr<Window> createWindow(uint64_t id, uint32_t rank,
        WindowType type, bool overlap, const char* backbone, uint32_t backbone_length,
        const char* quality, uint32_t quality_length);

#ifdef CUDA_ENABLED
    friend class CUDABatchProcessor;
#endif
private:
    Window(uint64_t id, uint32_t rank, WindowType type, bool overlap, const char* backbone,
        uint32_t backbone_length, const char* quality, uint32_t quality_length);
    Window(const Window&) = delete;
    const Window& operator=(const Window&) = delete;

    uint64_t id_;
    uint32_t rank_;
    WindowType type_;
    bool overlap_;
    std::string consensus_;
    std::vector<uint32_t> summary_;
    std::vector<int32_t> coder_;
    std::vector<std::pair<const char*, uint32_t>> sequences_;
    std::vector<std::pair<const char*, uint32_t>> qualities_;
    std::vector<std::pair<uint32_t, uint32_t>> positions_;
    std::vector<uint32_t> q_ids_;
};

}
