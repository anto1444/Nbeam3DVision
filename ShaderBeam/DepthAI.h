/*
Nbeam 3D Vision: AI depth estimation for the "2D to 3D (AI)" 3D source
Runs a monocular depth model (Depth Anything style ONNX file) with ONNX Runtime, loaded at run time from onnxruntime.dll
(DirectML on the GPU when available, otherwise CPU). Everything happens on a worker thread; the renderer only hands
over small downscaled frames and picks up the latest depth map.
MIT License
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <vector>

namespace ShaderBeam
{

struct DepthParams
{
    float smoothing { 0.5f }; // 0..0.95, temporal smoothing of the depth map
    bool  invert { false };
};

class DepthEstimator
{
public:
    ~DepthEstimator() { Stop(); }

    // Starts the worker (loads onnxruntime.dll + the model in the background). `dir` = folder with the DLL and the model.
    void Start(const std::filesystem::path& dir, int width, int height, bool useGpu);
    void Stop();

    bool Ready() const { return m_state == 1; }
    bool Failed() const { return m_state == 2; }
    bool WantsFrame() const { return m_state == 1 && !m_busy; }

    // `bgra`: width x height pixels (the size given to Start), rows `pitch` bytes apart
    void Submit(const uint8_t* bgra, int pitch, const DepthParams& p);

    // latest depth map (16-bit, larger = closer) when a new one is available since the last call
    bool TryGetDepth(std::vector<uint16_t>& out);

    std::string Status() const;
    float       InferMs() const { return m_inferMs; }
    float       DepthFps() const { return m_depthFps; }

private:
    void Worker(std::filesystem::path dir, int width, int height, bool useGpu);

    std::thread             m_thread;
    std::atomic<bool>       m_quit { false };
    std::atomic<int>        m_state { 0 }; // 0 = loading / stopped, 1 = ready, 2 = failed
    std::atomic<bool>       m_busy { false };
    std::atomic<float>      m_inferMs { 0.0f };
    std::atomic<float>      m_depthFps { 0.0f };
    int                     m_width { 0 };
    int                     m_height { 0 };

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::vector<uint8_t>    m_input; // BGRA, tightly packed
    DepthParams             m_params;
    bool                    m_hasInput { false };
    std::vector<uint16_t>   m_output;
    bool                    m_hasOutput { false };
    std::string             m_status { "Not started" };
};

// Files needed next to the exe for 2D to 3D (onnxruntime.dll, onnxruntime_providers_shared.dll, a .onnx model): names of those missing
std::vector<std::string> MissingAiFiles(const std::filesystem::path& dir);
std::vector<std::string> MissingAiFilesNextToExe();

} // namespace ShaderBeam
