/*
Nbeam 3D Vision: AI depth estimation for the "2D to 3D (AI)" 3D source
MIT License
*/

#include "stdafx.h"
#include "DepthAI.h"
#include "ort/onnxruntime_c_api.h"
#include <chrono>

namespace ShaderBeam
{

namespace
{
typedef OrtStatus*(ORT_API_CALL* AppendDmlFn)(OrtSessionOptions*, int);

std::string ErrorText(const OrtApi* api, OrtStatus* st)
{
    std::string text;
    if(st)
    {
        const char* m = api->GetErrorMessage(st);
        text          = m ? m : "unknown error";
        api->ReleaseStatus(st);
    }
    return text;
}

// first model found next to the exe: model_fp16.onnx, else any .onnx (also in a "models" subfolder)
std::filesystem::path FindModel(const std::filesystem::path& dir)
{
    std::error_code ec;
    for(const auto& d : { dir, dir / "models" })
    {
        auto preferred = d / "model_fp16.onnx";
        if(std::filesystem::exists(preferred, ec))
            return preferred;
    }
    for(const auto& d : { dir, dir / "models" })
    {
        if(!std::filesystem::is_directory(d, ec))
            continue;
        for(const auto& e : std::filesystem::directory_iterator(d, ec))
            if(e.is_regular_file(ec) && e.path().extension() == ".onnx")
                return e.path();
    }
    return {};
}
} // namespace

std::vector<std::string> MissingAiFiles(const std::filesystem::path& dir)
{
    std::vector<std::string> missing;
    std::error_code          ec;
    for(const char* dll : { "onnxruntime.dll", "onnxruntime_providers_shared.dll" })
        if(!std::filesystem::exists(dir / dll, ec))
            missing.push_back(dll);
    if(FindModel(dir).empty())
        missing.push_back("model_fp16.onnx (.onnx)");
    return missing;
}

std::vector<std::string> MissingAiFilesNextToExe()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return MissingAiFiles(std::filesystem::path(path).parent_path());
}

void DepthEstimator::Start(const std::filesystem::path& dir, int width, int height, bool useGpu)
{
    Stop();
    m_quit      = false;
    m_busy      = false;
    m_state     = 0;
    m_width     = width;
    m_height    = height;
    m_hasInput  = false;
    m_hasOutput = false;
    m_inferMs   = 0.0f;
    m_depthFps  = 0.0f;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_status = "Loading the AI model...";
    }
    m_thread = std::thread(&DepthEstimator::Worker, this, dir, width, height, useGpu);
}

void DepthEstimator::Stop()
{
    if(m_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    m_state = 0;
    m_busy  = false;
}

void DepthEstimator::Submit(const uint8_t* bgra, int pitch, const DepthParams& p)
{
    if(m_state != 1 || m_busy)
        return;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_input.resize((size_t)m_width * m_height * 4);
        for(int y = 0; y < m_height; y++)
            memcpy(&m_input[(size_t)y * m_width * 4], bgra + (size_t)y * pitch, (size_t)m_width * 4);
        m_params   = p;
        m_hasInput = true;
        m_busy     = true;
    }
    m_cv.notify_one();
}

bool DepthEstimator::TryGetDepth(std::vector<uint16_t>& out)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if(!m_hasOutput)
        return false;
    out         = m_output;
    m_hasOutput = false;
    return true;
}

std::string DepthEstimator::Status() const
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_status;
}

void DepthEstimator::Worker(std::filesystem::path dir, int width, int height, bool useGpu)
{
    auto setStatus = [&](const std::string& s) {
        std::lock_guard<std::mutex> l(m_mutex);
        m_status = s;
    };
    auto fail = [&](const std::string& s) {
        setStatus(s);
        m_state = 2;
    };

    const OrtApi*     api      = nullptr;
    OrtEnv*           env      = nullptr;
    OrtSession*       session  = nullptr;
    OrtMemoryInfo*    memInfo  = nullptr;
    HMODULE           lib      = nullptr;
    std::string       provider = "CPU";
    std::string       inName   = "pixel_values";
    std::string       outName  = "predicted_depth";

    try
    {
        // ---- onnxruntime.dll next to the exe (its dependencies, e.g. DirectML.dll, are searched there too) ----
        const auto dll = dir / "onnxruntime.dll";
        std::error_code ec;
        if(!std::filesystem::exists(dll, ec))
        {
            fail("onnxruntime.dll not found next to the exe");
            return;
        }
        lib = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!lib)
        {
            fail("Unable to load onnxruntime.dll");
            return;
        }
        auto getBase = (const OrtApiBase*(ORT_API_CALL*)())GetProcAddress(lib, "OrtGetApiBase");
        if(!getBase)
        {
            fail("onnxruntime.dll: OrtGetApiBase missing");
            return;
        }
        for(uint32_t v = ORT_API_VERSION; v >= 14 && !api; v--) // older runtimes expose a compatible prefix of the same table
            api = getBase()->GetApi(v);
        if(!api)
        {
            fail("onnxruntime.dll is too old");
            return;
        }

        const auto model = FindModel(dir);
        if(model.empty())
        {
            fail("AI model not found (put model_fp16.onnx next to the exe)");
            return;
        }

        std::string err = ErrorText(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "nbeam", &env));
        if(!err.empty())
        {
            fail("ONNX Runtime: " + err);
            return;
        }
        err = ErrorText(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo));
        if(!err.empty())
        {
            fail("ONNX Runtime: " + err);
            return;
        }

        // ---- session: GPU (DirectML) first, CPU as the fallback ----
        std::string lastError;
        for(int attempt = 0; attempt < 2 && !session; attempt++)
        {
            const bool gpu = attempt == 0 && useGpu;
            if(attempt == 0 && !useGpu)
                attempt = 1; // GPU not wanted: straight to the CPU
            OrtSessionOptions* so = nullptr;
            if(!ErrorText(api, api->CreateSessionOptions(&so)).empty() || !so)
                continue;
            api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
            api->AddFreeDimensionOverrideByName(so, "batch_size", 1);
            api->AddFreeDimensionOverrideByName(so, "height", height);
            api->AddFreeDimensionOverrideByName(so, "width", width);
            bool ok = true;
            if(gpu)
            {
                api->DisableMemPattern(so);
                api->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
                auto append = (AppendDmlFn)GetProcAddress(lib, "OrtSessionOptionsAppendExecutionProvider_DML");
                if(!append)
                {
                    lastError = "this onnxruntime.dll has no DirectML";
                    ok        = false;
                }
                else
                {
                    const std::string e = ErrorText(api, append(so, 0));
                    if(!e.empty())
                    {
                        lastError = e;
                        ok        = false;
                    }
                }
                provider = "GPU (DirectML)";
            }
            else
            {
                const unsigned hw = std::thread::hardware_concurrency();
                api->SetIntraOpNumThreads(so, (int)std::clamp<unsigned>(hw / 2, 2, 8));
                provider = useGpu ? "CPU (DirectML unavailable)" : "CPU";
            }
            if(ok)
            {
                OrtSession*       created = nullptr;
                const std::string e       = ErrorText(api, api->CreateSession(env, model.c_str(), so, &created));
                if(e.empty() && created)
                    session = created;
                else
                    lastError = e;
            }
            api->ReleaseSessionOptions(so);
        }
        if(!session)
        {
            fail("Unable to load the model: " + lastError);
            api->ReleaseMemoryInfo(memInfo);
            api->ReleaseEnv(env);
            return;
        }

        // tensor names from the model (fallback: the names of Depth Anything)
        {
            OrtAllocator* alloc = nullptr;
            if(ErrorText(api, api->GetAllocatorWithDefaultOptions(&alloc)).empty() && alloc)
            {
                char* n = nullptr;
                if(ErrorText(api, api->SessionGetInputName(session, 0, alloc, &n)).empty() && n)
                {
                    inName = n;
                    alloc->Free(alloc, n);
                }
                n = nullptr;
                if(ErrorText(api, api->SessionGetOutputName(session, 0, alloc, &n)).empty() && n)
                {
                    outName = n;
                    alloc->Free(alloc, n);
                }
            }
        }

        {
            char buf[160];
            snprintf(buf, sizeof(buf), "Ready - %s - %s - %dx%d", provider.c_str(), model.filename().string().c_str(), width, height);
            setStatus(buf);
        }
        m_state = 1;

        // ---- inference loop ----
        const size_t         plane = (size_t)width * height;
        std::vector<float>   input(plane * 3);
        std::vector<float>   prev; // smoothed depth (0..1)
        std::vector<uint8_t> bgra;
        float                loS = 0.0f, hiS = 1.0f;
        bool                 haveRange = false;
        auto                 lastDone  = std::chrono::steady_clock::now();
        const char*          inNames[]  = { inName.c_str() };
        const char*          outNames[] = { outName.c_str() };

        while(true)
        {
            DepthParams p;
            {
                std::unique_lock<std::mutex> l(m_mutex);
                m_cv.wait(l, [&] { return m_quit || m_hasInput; });
                if(m_quit)
                    break;
                bgra.swap(m_input);
                p          = m_params;
                m_hasInput = false;
            }
            const auto t0 = std::chrono::steady_clock::now();

            static const float mean[3] = { 0.485f, 0.456f, 0.406f };
            static const float istd[3] = { 1.0f / 0.229f, 1.0f / 0.224f, 1.0f / 0.225f };
            for(size_t i = 0; i < plane; i++)
            {
                const uint8_t* px = &bgra[i * 4];
                input[i]             = (px[2] * (1.0f / 255.0f) - mean[0]) * istd[0];
                input[plane + i]     = (px[1] * (1.0f / 255.0f) - mean[1]) * istd[1];
                input[plane * 2 + i] = (px[0] * (1.0f / 255.0f) - mean[2]) * istd[2];
            }

            int64_t        shape[4] = { 1, 3, height, width };
            OrtValue*      inVal    = nullptr;
            OrtValue*      outVal   = nullptr;
            std::string    e        = ErrorText(api, api->CreateTensorWithDataAsOrtValue(memInfo, input.data(), input.size() * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal));
            if(e.empty())
                e = ErrorText(api, api->Run(session, nullptr, inNames, &inVal, 1, outNames, 1, &outVal));
            std::vector<float> raw;
            if(e.empty() && outVal)
            {
                float* data = nullptr;
                if(ErrorText(api, api->GetTensorMutableData(outVal, (void**)&data)).empty() && data)
                {
                    OrtTensorTypeAndShapeInfo* info = nullptr;
                    size_t                     count = 0;
                    if(ErrorText(api, api->GetTensorTypeAndShape(outVal, &info)).empty() && info)
                    {
                        api->GetTensorShapeElementCount(info, &count);
                        api->ReleaseTensorTypeAndShapeInfo(info);
                    }
                    if(count == plane)
                        raw.assign(data, data + count);
                }
            }
            if(outVal)
                api->ReleaseValue(outVal);
            if(inVal)
                api->ReleaseValue(inVal);
            if(!e.empty())
            {
                fail("Inference error: " + e);
                break;
            }

            if(!raw.empty())
            {
                // robust range (1% .. 99%), itself smoothed in time so the depth does not pump
                float mn = raw[0], mx = raw[0];
                for(float v : raw)
                {
                    mn = (std::min)(mn, v);
                    mx = (std::max)(mx, v);
                }
                float lo = mn, hi = mx;
                if(mx > mn)
                {
                    const int       bins = 512;
                    std::vector<int> hist(bins, 0);
                    const float     k = (bins - 1) / (mx - mn);
                    for(float v : raw)
                        hist[(int)((v - mn) * k)]++;
                    const size_t cut = raw.size() / 100;
                    size_t       acc = 0;
                    for(int b = 0; b < bins; b++)
                    {
                        acc += hist[b];
                        if(acc > cut)
                        {
                            lo = mn + b / k;
                            break;
                        }
                    }
                    acc = 0;
                    for(int b = bins - 1; b >= 0; b--)
                    {
                        acc += hist[b];
                        if(acc > cut)
                        {
                            hi = mn + (b + 1) / k;
                            break;
                        }
                    }
                }
                if(hi - lo < 1e-6f)
                    hi = lo + 1e-6f;
                if(!haveRange)
                {
                    loS = lo;
                    hiS = hi;
                    haveRange = true;
                }
                else
                {
                    loS += (lo - loS) * 0.3f;
                    hiS += (hi - hiS) * 0.3f;
                }
                const float scale = 1.0f / (std::max)(hiS - loS, 1e-6f);
                const float keep  = std::clamp(p.smoothing, 0.0f, 0.95f);
                if(prev.size() != raw.size())
                    prev.assign(raw.size(), -1.0f);
                std::vector<uint16_t> out(raw.size());
                for(size_t i = 0; i < raw.size(); i++)
                {
                    float d = std::clamp((raw[i] - loS) * scale, 0.0f, 1.0f);
                    if(p.invert)
                        d = 1.0f - d;
                    prev[i] = prev[i] < 0.0f ? d : prev[i] * keep + d * (1.0f - keep);
                    out[i]  = (uint16_t)(prev[i] * 65535.0f + 0.5f);
                }
                {
                    std::lock_guard<std::mutex> l(m_mutex);
                    m_output    = std::move(out);
                    m_hasOutput = true;
                }
            }

            const auto  t1 = std::chrono::steady_clock::now();
            const float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            m_inferMs      = m_inferMs * 0.8f + ms * 0.2f;
            const float dt = std::chrono::duration<float>(t1 - lastDone).count();
            lastDone       = t1;
            if(dt > 0.0f)
                m_depthFps = m_depthFps * 0.8f + (1.0f / dt) * 0.2f;
            m_busy = false;
        }
    }
    catch(const std::exception& ex)
    {
        fail(std::string("AI depth error: ") + ex.what());
    }
    catch(...)
    {
        fail("AI depth: unexpected error");
    }

    if(api)
    {
        if(session)
            api->ReleaseSession(session);
        if(memInfo)
            api->ReleaseMemoryInfo(memInfo);
        if(env)
            api->ReleaseEnv(env);
    }
    // (onnxruntime.dll stays loaded: unloading it while DirectML threads wind down is not safe)
    m_busy = false;
}

} // namespace ShaderBeam
