/*
Nbeam 3D Vision - motion test
MIT License
*/

#include "stdafx.h"
#include "CarTest.h"
#include "Helpers.h"
#include "resource.h"
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace ShaderBeam
{
namespace
{

// ---- the pictures: the car is drawn once with GDI+ in the accent colour; the GPU draws the grid of cells (background colour, wide black
// lines) and scrolls it with the cars ----------------------------------------------------------------------------------------------------
static void DrawCar(Gdiplus::Graphics& g, float x0, float top, float cw, float ch, Gdiplus::Color body)
{
    using namespace Gdiplus;
    auto shade = [](Color c, float k) {
        return Color(255, (BYTE)std::clamp(c.GetR() * k, 0.0f, 255.0f), (BYTE)std::clamp(c.GetG() * k, 0.0f, 255.0f), (BYTE)std::clamp(c.GetB() * k, 0.0f, 255.0f));
    };
    const float belt = top + ch * 0.42f, sill = top + ch * 0.80f, wr = ch * 0.19f;
    const float wx1 = x0 + cw * 0.20f, wx2 = x0 + cw * 0.79f, wy = sill + ch * 0.02f;
    // shadow on the road
    SolidBrush shadow(Color(110, 0, 0, 0));
    g.FillEllipse(&shadow, x0 + cw * 0.02f, wy + wr * 0.75f, cw * 0.96f, ch * 0.14f);
    // body: rounded lower part, lighter on top
    GraphicsPath lower;
    lower.AddBezier(PointF(x0 + cw * 0.02f, sill), PointF(x0, belt + ch * 0.12f), PointF(x0 + cw * 0.02f, belt), PointF(x0 + cw * 0.10f, belt - ch * 0.02f));
    lower.AddLine(PointF(x0 + cw * 0.10f, belt - ch * 0.02f), PointF(x0 + cw * 0.80f, belt - ch * 0.01f));
    lower.AddBezier(PointF(x0 + cw * 0.80f, belt - ch * 0.01f), PointF(x0 + cw * 0.95f, belt + ch * 0.02f), PointF(x0 + cw * 1.0f, belt + ch * 0.12f), PointF(x0 + cw * 0.99f, sill - ch * 0.04f));
    lower.AddLine(PointF(x0 + cw * 0.99f, sill - ch * 0.04f), PointF(x0 + cw * 0.97f, sill));
    lower.CloseFigure();
    LinearGradientBrush bodyBrush(PointF(0, belt - ch * 0.05f), PointF(0, sill), shade(body, 1.35f), shade(body, 0.55f));
    g.FillPath(&bodyBrush, &lower);
    // cabin
    GraphicsPath cabin;
    cabin.AddBezier(PointF(x0 + cw * 0.14f, belt), PointF(x0 + cw * 0.20f, top + ch * 0.12f), PointF(x0 + cw * 0.26f, top + ch * 0.02f), PointF(x0 + cw * 0.36f, top));
    cabin.AddLine(PointF(x0 + cw * 0.36f, top), PointF(x0 + cw * 0.56f, top));
    cabin.AddBezier(PointF(x0 + cw * 0.56f, top), PointF(x0 + cw * 0.62f, top + ch * 0.02f), PointF(x0 + cw * 0.70f, top + ch * 0.24f), PointF(x0 + cw * 0.76f, belt));
    cabin.CloseFigure();
    LinearGradientBrush cabinBrush(PointF(0, top), PointF(0, belt), shade(body, 1.2f), shade(body, 0.85f));
    g.FillPath(&cabinBrush, &cabin);
    // windows (glass with a reflection), split by the pillar
    GraphicsPath glassR, glassF;
    glassR.AddBezier(PointF(x0 + cw * 0.19f, belt - ch * 0.02f), PointF(x0 + cw * 0.23f, top + ch * 0.14f), PointF(x0 + cw * 0.28f, top + ch * 0.07f), PointF(x0 + cw * 0.37f, top + ch * 0.06f));
    glassR.AddLine(PointF(x0 + cw * 0.37f, top + ch * 0.06f), PointF(x0 + cw * 0.45f, top + ch * 0.06f));
    glassR.AddLine(PointF(x0 + cw * 0.45f, top + ch * 0.06f), PointF(x0 + cw * 0.45f, belt - ch * 0.02f));
    glassR.CloseFigure();
    glassF.AddLine(PointF(x0 + cw * 0.47f, top + ch * 0.06f), PointF(x0 + cw * 0.55f, top + ch * 0.06f));
    glassF.AddBezier(PointF(x0 + cw * 0.55f, top + ch * 0.06f), PointF(x0 + cw * 0.60f, top + ch * 0.08f), PointF(x0 + cw * 0.66f, top + ch * 0.22f), PointF(x0 + cw * 0.71f, belt - ch * 0.02f));
    glassF.AddLine(PointF(x0 + cw * 0.71f, belt - ch * 0.02f), PointF(x0 + cw * 0.47f, belt - ch * 0.02f));
    glassF.CloseFigure();
    LinearGradientBrush glass(PointF(0, top), PointF(0, belt), Color(255, 150, 200, 235), Color(255, 20, 45, 80));
    g.FillPath(&glass, &glassR);
    g.FillPath(&glass, &glassF);
    Pen reflection(Color(120, 255, 255, 255), ch * 0.025f);
    g.DrawLine(&reflection, x0 + cw * 0.50f, belt - ch * 0.06f, x0 + cw * 0.56f, top + ch * 0.10f);
    g.DrawLine(&reflection, x0 + cw * 0.27f, belt - ch * 0.06f, x0 + cw * 0.33f, top + ch * 0.10f);
    // door lines, handles, mirror
    Pen line(shade(body, 0.45f), ch * 0.012f);
    g.DrawLine(&line, x0 + cw * 0.46f, belt - ch * 0.02f, x0 + cw * 0.46f, sill - ch * 0.04f);
    g.DrawLine(&line, x0 + cw * 0.72f, belt, x0 + cw * 0.71f, sill - ch * 0.06f);
    SolidBrush handle(shade(body, 0.4f));
    g.FillRectangle(&handle, x0 + cw * 0.39f, belt + ch * 0.07f, cw * 0.04f, ch * 0.025f);
    g.FillRectangle(&handle, x0 + cw * 0.64f, belt + ch * 0.07f, cw * 0.04f, ch * 0.025f);
    g.FillEllipse(&handle, x0 + cw * 0.70f, belt - ch * 0.09f, cw * 0.04f, ch * 0.07f);
    // highlight along the body
    Pen shine(Color(90, 255, 255, 255), ch * 0.02f);
    g.DrawLine(&shine, x0 + cw * 0.08f, belt + ch * 0.05f, x0 + cw * 0.90f, belt + ch * 0.06f);
    // bumpers, lights
    SolidBrush bumper(Color(255, 40, 40, 44));
    g.FillRectangle(&bumper, x0 + cw * 0.005f, sill - ch * 0.12f, cw * 0.06f, ch * 0.10f);
    g.FillRectangle(&bumper, x0 + cw * 0.935f, sill - ch * 0.12f, cw * 0.06f, ch * 0.10f);
    SolidBrush headlight(Color(255, 255, 245, 200));
    g.FillEllipse(&headlight, x0 + cw * 0.92f, belt + ch * 0.04f, cw * 0.06f, ch * 0.09f);
    SolidBrush indicator(Color(255, 255, 160, 20));
    g.FillRectangle(&indicator, x0 + cw * 0.95f, belt + ch * 0.15f, cw * 0.03f, ch * 0.04f);
    SolidBrush tail(Color(255, 220, 20, 25));
    g.FillRectangle(&tail, x0 + cw * 0.008f, belt + ch * 0.03f, cw * 0.035f, ch * 0.11f);
    // wheels: arch, tyre, rim with spokes, hub
    SolidBrush arch(Color(255, 18, 18, 20)), tyre(Color(255, 25, 25, 27));
    for(const float wx : { wx1, wx2 })
    {
        g.FillEllipse(&arch, wx - wr * 1.15f, wy - wr * 1.15f, wr * 2.3f, wr * 2.3f);
        g.FillEllipse(&tyre, wx - wr, wy - wr, wr * 2.0f, wr * 2.0f);
        LinearGradientBrush rim(PointF(wx - wr, wy - wr), PointF(wx + wr, wy + wr), Color(255, 235, 238, 242), Color(255, 105, 110, 118));
        g.FillEllipse(&rim, wx - wr * 0.62f, wy - wr * 0.62f, wr * 1.24f, wr * 1.24f);
        Pen spoke(Color(255, 70, 72, 78), wr * 0.12f);
        for(int k = 0; k < 5; k++)
        {
            const float a = 3.14159265f * 2.0f * k / 5.0f;
            g.DrawLine(&spoke, wx, wy, wx + std::cos(a) * wr * 0.58f, wy + std::sin(a) * wr * 0.58f);
        }
        SolidBrush hub(Color(255, 190, 30, 30));
        g.FillEllipse(&hub, wx - wr * 0.14f, wy - wr * 0.14f, wr * 0.28f, wr * 0.28f);
    }
}

// the car tile: car in the accent colour and "NBEAM" (accent colour on a black band) under it, transparent around (the cell, its
// background colour and the black grid are drawn by the shader, at screen depth)
static std::vector<uint32_t> MakeCarTile(int tw, int th, Gdiplus::Color accent, const wchar_t* eyeMark)
{
    using namespace Gdiplus;
    static ULONG_PTR s_gdipToken = 0;
    if(!s_gdipToken)
    {
        GdiplusStartupInput in;
        if(GdiplusStartup(&s_gdipToken, &in, nullptr) != Ok)
            return {};
    }
    std::vector<uint32_t> px((size_t)tw * th, 0u);
    {
        Bitmap   bmp(tw, th, tw * 4, PixelFormat32bppARGB, (BYTE*)px.data());
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        DrawCar(g, tw * 0.10f, th * 0.10f, tw * 0.80f, th * 0.50f, accent);
        FontFamily    family(L"Segoe UI");
        Gdiplus::Font font(&family, th * 0.11f, FontStyleBold, UnitPixel); // (qualified: another Font exists)
        StringFormat  center;
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);
        SolidBrush  black(Color(255, 0, 0, 0)), text(accent);
        const RectF band(tw * 0.32f, th * 0.71f, tw * 0.36f, th * 0.17f);
        g.FillRectangle(&black, band);
        g.DrawString(L"NBEAM", -1, &font, band, &center, &text);
        if(eyeMark)
        {
            // the eye of this picture on both sides of NBEAM (G / D, L / R): the eyes are swapped if the other letter is seen
            const float sq = th * 0.17f;
            const RectF left(band.X - sq * 1.25f, band.Y, sq, sq), right(band.X + band.Width + sq * 0.25f, band.Y, sq, sq);
            g.FillRectangle(&black, left);
            g.FillRectangle(&black, right);
            g.DrawString(eyeMark, -1, &font, left, &center, &text);
            g.DrawString(eyeMark, -1, &font, right, &center, &text);
        }
    }
    return px;
}

// the Nbeam logo (the exe icon) on black, for the bottom left corner of the test
static std::vector<uint32_t> MakeLogoTile(int size)
{
    std::vector<uint32_t> px;
    BITMAPINFO bi {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void*   bits = nullptr;
    HDC     dc   = CreateCompatibleDC(nullptr);
    HBITMAP bmp  = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if(bmp && bits)
    {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)size * size * 4);
        if(HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SHADERBEAM), IMAGE_ICON, size, size, LR_DEFAULTCOLOR))
        {
            DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
        GdiFlush();
        px.assign((const uint32_t*)bits, (const uint32_t*)bits + (size_t)size * size);
        for(auto& p : px)
            p |= 0xFF000000u;
        SelectObject(dc, old);
    }
    if(bmp)
        DeleteObject(bmp);
    if(dc)
        DeleteDC(dc);
    return px;
}

static winrt::com_ptr<ID3D11ShaderResourceView> MakeTestTexture(ID3D11Device* dev, const std::vector<uint32_t>& px, int w, int h)
{
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    if(px.size() != (size_t)w * h)
        return srv;
    D3D11_TEXTURE2D_DESC td {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd {};
    sd.pSysMem     = px.data();
    sd.SysMemPitch = w * 4;
    winrt::com_ptr<ID3D11Texture2D> tex;
    if(SUCCEEDED(dev->CreateTexture2D(&td, &sd, tex.put())))
        dev->CreateShaderResourceView(tex.get(), nullptr, srv.put());
    return srv;
}


// ---- the window --------------------------------------------------------------------------------------------------------------------------
struct Test
{
    HWND                                     wnd { NULL };
    int                                      w { 0 }, h { 0 };
    double                                   hz { 60.0 };
    CarTestParams                            p;
    bool                                     haveParams { false }; // black until the settings of Nbeam arrive
    bool                                     tilesDirty { true };
    double                                   scroll { 0.0 };
    ULONGLONG                                hzCheck { 0 };
    HANDLE                                   nbeam { NULL }; // the Nbeam process: the test ends with it
    winrt::com_ptr<ID3D11Device>             dev;
    winrt::com_ptr<ID3D11DeviceContext>      ctx;
    winrt::com_ptr<IDXGISwapChain2>          chain;
    HANDLE                                   waitable { NULL };
    winrt::com_ptr<ID3D11RenderTargetView>   rtv;
    winrt::com_ptr<ID3D11VertexShader>       vs;
    winrt::com_ptr<ID3D11PixelShader>        ps;
    winrt::com_ptr<ID3D11Buffer>             cb;
    winrt::com_ptr<ID3D11ShaderResourceView> atlas, logo;
    int                                      tileW { 0 }, tileH { 0 }, logoSize { 0 };
};
Test s_test;

LRESULT CALLBACK TestProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
    case WM_SETCURSOR:
        if(LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(NULL); // no mouse pointer over the cars
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if(wParam == VK_ESCAPE)
        {
            DestroyWindow(w); // Escape ends the test (Nbeam sees the process end and goes back to the desktop)
            return 0;
        }
        break;
    case WM_COPYDATA: {
        const auto* cd = (const COPYDATASTRUCT*)lParam;
        if(cd && cd->dwData == CARTEST_COPYDATA && cd->cbData == sizeof(CarTestParams) && cd->lpData)
        {
            CarTestParams np;
            memcpy(&np, cd->lpData, sizeof(np));
            if(np.carSize != s_test.p.carSize || np.french != s_test.p.french || memcmp(np.accent, s_test.p.accent, sizeof(np.accent)) != 0)
                s_test.tilesDirty = true;
            s_test.p          = np;
            s_test.haveParams = true;
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, message, wParam, lParam);
}

bool CreateGraphics()
{
    auto& t = s_test;
    D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, t.dev.put(), &fl, t.ctx.put())))
        return false;
    winrt::com_ptr<IDXGIFactory2> factory;
    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), factory.put_void())))
        return false;
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width            = t.w;
    sd.Height           = t.h;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM; // a plain sRGB picture, like a game
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    winrt::com_ptr<IDXGISwapChain1> chain1;
    if(FAILED(factory->CreateSwapChainForHwnd(t.dev.get(), t.wnd, &sd, nullptr, nullptr, chain1.put())))
        return false;
    factory->MakeWindowAssociation(t.wnd, DXGI_MWA_NO_ALT_ENTER);
    t.chain = chain1.try_as<IDXGISwapChain2>();
    if(!t.chain)
        return false;
    t.chain->SetMaximumFrameLatency(1);
    t.waitable = t.chain->GetFrameLatencyWaitableObject();
    winrt::com_ptr<ID3D11Texture2D> back;
    if(FAILED(t.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())) || FAILED(t.dev->CreateRenderTargetView(back.get(), nullptr, t.rtv.put())))
        return false;

    static const char hlsl[] = R"HLSL(
cbuffer Params : register(b0) { float4 a; float4 b; float4 c; float4 d; };
// a: width, height, scroll (px), border (px)   b: layout (0 = 2D, 1 = side by side, 2 = top / bottom, 3 = checkerboard), -, tile w, tile h
// c: background colour   d: logo x, y, size, parallax (px)
Texture2D<float4> car  : register(t0);
Texture2D<float4> logo : register(t1);
float4 VSmain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PScars(float4 pos : SV_Position) : SV_Target
{
    // the picture of each eye, in the layout of the 3D source format (2D: one picture, no eye)
    float2 q = pos.xy;
    float eye = -1.0;
    int layout = (int)b.x;
    if(layout == 1)
    {
        eye = q.x >= a.x * 0.5 ? 1.0 : 0.0;
        q.x = (q.x - eye * a.x * 0.5) * 2.0;
    }
    else if(layout == 2)
    {
        eye = q.y >= a.y * 0.5 ? 1.0 : 0.0;
        q.y = (q.y - eye * a.y * 0.5) * 2.0;
    }
    else if(layout == 3)
        eye = (float)(((int)q.x + (int)q.y) & 1);
    float tw = b.z, th = b.w;
    int   row = (int)(q.y / th);
    float ly  = q.y - row * th;
    // in 3D the cars and their grid come out of the screen (crossed: the left eye sees them moved right, the right eye left); the eye
    // mark G / D next to NBEAM tells which eye sees this picture; the logo stays on the screen
    float p    = eye >= 0.0 ? (eye < 0.5 ? 0.5 : -0.5) * d.w : 0.0;
    float lx   = fmod(q.x - a.z - p + tw * 1000.0, tw);
    float3 col = (lx > tw - a.w || ly > th - a.w) ? float3(0.0, 0.0, 0.0) : c.rgb;
    int   tile = eye < 0.0 ? 0 : (eye < 0.5 ? 1 : 2);
    float4 v   = car.Load(int3((int)lx, (int)ly + tile * (int)th, 0));
    col        = lerp(col, v.rgb, v.a);
    if(q.x >= d.x && q.x < d.x + d.z && q.y >= d.y && q.y < d.y + d.z)
        col = logo.Load(int3((int)(q.x - d.x), (int)(q.y - d.y), 0)).rgb;
    return float4(col, 1.0);
}
)HLSL";
    winrt::com_ptr<ID3DBlob> vsb, psb;
    std::string              errors;
    if(FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "CarTestWindow1", nullptr, "VSmain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, vsb, errors))
       || FAILED(Helpers::CompileCached(hlsl, sizeof(hlsl) - 1, "CarTestWindow1", nullptr, "PScars", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, psb, errors)))
        return false;
    if(FAILED(t.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, t.vs.put()))
       || FAILED(t.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, t.ps.put())))
        return false;
    D3D11_BUFFER_DESC cbd {};
    cbd.ByteWidth      = 64;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(t.dev->CreateBuffer(&cbd, nullptr, t.cb.put()));
}

// refresh rate of the screen of the window (the speed of the cars is in pixels per second)
double ScreenHz()
{
    MONITORINFOEXW mi {};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm {};
    dm.dmSize = sizeof(dm);
    if(GetMonitorInfoW(MonitorFromWindow(s_test.wnd, MONITOR_DEFAULTTOPRIMARY), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (double)dm.dmDisplayFrequency;
    return s_test.hz;
}

void RenderFrame()
{
    auto&     t = s_test;
    const int w = t.w, h = t.h;
    ID3D11RenderTargetView* target = t.rtv.get();
    if(!t.haveParams)
    {
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        t.ctx->ClearRenderTargetView(target, black);
        return;
    }
    // the car tile: size from "Car size" (100 % = 3 rows), in the accent colour; made again when one of them changes
    const int size = std::clamp(t.p.carSize, 25, 200);
    const int th   = (std::max)(32, h * 100 / (3 * size));
    const int tw   = th * 2;
    if(!t.atlas || t.tilesDirty || t.tileW != tw || t.tileH != th)
    {
        // three tiles stacked: 2D (no eye mark), left eye, right eye
        const auto           to8 = [](float v) { return (BYTE)std::clamp((int)(v * 255.0f + 0.5f), 0, 255); };
        const Gdiplus::Color c((BYTE)255, to8(t.p.accent[0]), to8(t.p.accent[1]), to8(t.p.accent[2]));
        std::vector<uint32_t> px;
        for(const wchar_t* mark : { (const wchar_t*)nullptr, t.p.french ? L"G" : L"L", t.p.french ? L"D" : L"R" })
        {
            const auto tile = MakeCarTile(tw, th, c, mark);
            if(tile.empty())
                return;
            px.insert(px.end(), tile.begin(), tile.end());
        }
        t.atlas = MakeTestTexture(t.dev.get(), px, tw, th * 3);
        if(!t.atlas)
            return;
        t.tileW      = tw;
        t.tileH      = th;
        t.tilesDirty = false;
    }
    const int logoSize = (std::max)(48, h / 9);
    if(!t.logo || t.logoSize != logoSize)
    {
        t.logo     = MakeTestTexture(t.dev.get(), MakeLogoTile(logoSize), logoSize, logoSize);
        t.logoSize = logoSize;
    }
    // one new position every refresh of the screen, in perfectly regular steps (the refresh rate is read again every 2 seconds)
    if(GetTickCount64() - t.hzCheck > 2000)
    {
        t.hz      = ScreenHz();
        t.hzCheck = GetTickCount64();
    }
    t.scroll            = std::fmod(t.scroll + std::clamp(t.p.speed, 0, 7680) / t.hz, (double)tw);
    const double scroll = t.scroll;
    // cars in front of the screen: 1.2 % of the width between the eyes (the Convergence of Nbeam moves the whole picture from there)
    const float disparity = w * 0.012f;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if(SUCCEEDED(t.ctx->Map(t.cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        const float margin = h * 0.03f;
        const float v[16]  = { (float)w, (float)h, (float)scroll, (std::max)(4.0f, th * 0.035f),
                               (float)std::clamp(t.p.layout, 0, 3), 0.0f, (float)tw, (float)th,
                               t.p.bg[0], t.p.bg[1], t.p.bg[2], 0.0f,
                               margin, h - margin - logoSize, (float)logoSize, disparity };
        memcpy(mapped.pData, v, sizeof(v));
        t.ctx->Unmap(t.cb.get(), 0);
    }
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    t.ctx->RSSetViewports(1, &vp);
    t.ctx->OMSetRenderTargets(1, &target, nullptr);
    t.ctx->IASetInputLayout(nullptr);
    t.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.ctx->VSSetShader(t.vs.get(), nullptr, 0);
    t.ctx->PSSetShader(t.ps.get(), nullptr, 0);
    ID3D11Buffer* cb = t.cb.get();
    t.ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srvs[2] = { t.atlas.get(), t.logo.get() };
    t.ctx->PSSetShaderResources(0, 2, srvs);
    t.ctx->Draw(3, 0);
}

} // namespace

int RunCarTest(HINSTANCE instance, const wchar_t* cmdLine)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // the screen rectangle from Nbeam is in real pixels
    auto& t = s_test;
    // the screen and Nbeam: "/cartest x y w h nbeamPid" (the main screen otherwise)
    int x = 0, y = 0, w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if(const wchar_t* arg = cmdLine ? wcsstr(cmdLine, L"/cartest") : nullptr)
    {
        int      ax, ay, aw, ah;
        unsigned pid = 0;
        const int n  = swscanf_s(arg + 8, L"%d %d %d %d %u", &ax, &ay, &aw, &ah, &pid);
        if(n >= 4 && aw >= 64 && ah >= 64)
            x = ax, y = ay, w = aw, h = ah;
        if(n == 5 && pid)
            t.nbeam = OpenProcess(SYNCHRONIZE, FALSE, pid);
    }
    t.w = w;
    t.h = h;
    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TestProc;
    wc.hInstance     = instance;
    wc.hIcon         = LoadIcon(instance, MAKEINTRESOURCE(IDI_SHADERBEAM));
    wc.hIconSm       = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CARTEST_CLASS;
    RegisterClassExW(&wc);
    t.wnd = CreateWindowExW(WS_EX_APPWINDOW, CARTEST_CLASS, CARTEST_TITLE, WS_POPUP | WS_VISIBLE, x, y, w, h, nullptr, nullptr, instance, nullptr);
    if(!t.wnd)
        return 1;
    SetForegroundWindow(t.wnd);
    t.hz      = ScreenHz();
    t.hzCheck = GetTickCount64();
    if(!CreateGraphics())
    {
        DestroyWindow(t.wnd);
        return 2;
    }
    MSG msg {};
    for(;;)
    {
        while(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if(msg.message == WM_QUIT)
                return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if(t.nbeam && WaitForSingleObject(t.nbeam, 0) == WAIT_OBJECT_0)
            return 0; // Nbeam is closed: the test ends with it
        if(t.waitable)
            WaitForSingleObjectEx(t.waitable, 100, TRUE);
        RenderFrame();
        const HRESULT hr = t.chain->Present(1, 0); // one picture per refresh of the screen
        if(hr == DXGI_STATUS_OCCLUDED)
            Sleep(16);
        else if(FAILED(hr))
            return 3; // graphics card lost: the test ends (Nbeam goes back to the desktop)
    }
}

} // namespace ShaderBeam
