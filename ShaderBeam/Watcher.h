/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#pragma once

#include "UI.h"
#include <atomic>

namespace ShaderBeam
{

#define CHARTS_LEN 1024

struct Chart
{
    int   m_index;
    int   m_marker;
    float m_values[CHARTS_LEN];
    float m_time;

    void Clear();
    void AddDelta();
    void AddDelta(float value);
    void SetStart(float value);
    void AddValue(float value);

    bool Read(int& index, float& value);
    float Min(int numSamples);
};

class Watcher
{
public:
    Watcher(UI& ui);

    void Start();

    void FrameSubmitted();

    void FrameReceived(float value);

    void Stop();

    // total number of game frames received from the capture API since Start() (monotonic; used to measure the game frame rate)
    unsigned long long TotalInputFrames() const { return m_totalInput; }

    // gaps (ms) between the production times of consecutive game frames since the last call: worst, average and count (gaps over 250 ms = static picture, ignored)
    void TakeGapStats(double& maxMs, double& meanMs, int& count)
    {
        maxMs   = m_gapMax;
        count   = m_gapCount;
        meanMs  = m_gapCount > 0 ? m_gapSum / m_gapCount : 0.0;
        m_gapMax = m_gapSum = 0.0;
        m_gapCount = 0;
    }


    Chart m_submitChart;
    Chart m_receiveChart;

private:
    UI& m_ui;

    float m_lastSnapshot { 0 };

    int m_inputFrames { 0 };
    int m_outputFrames { 0 };

    unsigned long long m_totalInput { 0 };

    float  m_gapLast { -1.0f };
    double m_gapMax { 0.0 };
    double m_gapSum { 0.0 };
    int    m_gapCount { 0 };

    void UpdateSnapshot();
};
} // namespace ShaderBeam
