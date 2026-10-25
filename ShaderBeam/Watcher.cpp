/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

#include "stdafx.h"

#include "Watcher.h"
#include "Helpers.h"

#define SNAPSHOT_DURATION TICKS_PER_SEC

namespace ShaderBeam
{

void Chart::Clear()
{
    m_index  = 0;
    m_marker = 0;
    m_time   = Helpers::GetTicks();
    ZeroMemory(m_values, sizeof(m_values));
}

void Chart::AddDelta()
{
    AddDelta(Helpers::GetTicks());
}

void Chart::AddDelta(float value)
{
    AddValue(value - m_time);
    SetStart(value);
}

void Chart::AddValue(float value)
{
    m_values[m_index] = value;
    m_index++;
    if(m_index >= CHARTS_LEN)
        m_index -= CHARTS_LEN;
}

void Chart::SetStart(float value)
{
    m_time = value;
}

bool Chart::Read(int& index, float& value)
{
    auto long_index = m_index;
    if(long_index < m_marker)
        long_index += CHARTS_LEN;
    if(m_marker < long_index)
    {
        index = m_marker;
        value = m_values[m_marker];
        m_marker++;
        if(m_marker >= CHARTS_LEN)
            m_marker -= CHARTS_LEN;
        return true;
    }
    return false;
}

float Chart::Min(int numSamples)
{
    if(numSamples == 0)
        return 0;

    int   samples = 0;
    float min     = FLT_MAX;
    auto  index   = m_index;
    while(samples++ < numSamples)
    {
        index--;
        if(index < 0)
            index += CHARTS_LEN;
        if(m_values[index] < min)
            min = m_values[index];
    }
    return min;
}

Watcher::Watcher(UI& ui) : m_ui(ui) { }

void Watcher::Start()
{
    m_receiveChart.Clear();
    m_submitChart.Clear();
    m_lastSnapshot = Helpers::GetTicks();
    m_inputFrames  = 0;
    m_outputFrames = 0;
    m_totalInput   = 0;
    m_gapLast      = -1.0f;
    m_gapMax = m_gapSum = 0.0;
    m_gapCount          = 0;
}

void Watcher::Stop() { }

void Watcher::FrameSubmitted()
{
    m_submitChart.AddDelta();
    m_outputFrames++;
    UpdateSnapshot();
}

void Watcher::FrameReceived(float value)
{
    auto now   = Helpers::GetTicks();
    auto delta = now - value;
    m_receiveChart.AddValue(delta);
    if(m_gapLast >= 0.0f)
    {
        const double gap = (double)value - (double)m_gapLast;
        if(gap > 0.0 && gap < 250.0)
        {
            m_gapMax = (std::max)(m_gapMax, gap);
            m_gapSum += gap;
            m_gapCount++;
        }
    }
    m_gapLast = value;
    m_inputFrames++;
    m_totalInput++;
    UpdateSnapshot();
}

void Watcher::UpdateSnapshot()
{
    auto now = Helpers::GetTicks();
    if(now - m_lastSnapshot > SNAPSHOT_DURATION)
    {
        auto secondsElapsed = (now - m_lastSnapshot) / TICKS_PER_SEC;
        m_ui.m_inputFPS     = m_inputFrames / secondsElapsed;
        m_ui.m_outputFPS    = m_outputFrames / secondsElapsed;
        m_ui.m_captureLag   = m_receiveChart.Min(m_inputFrames);
        m_inputFrames       = 0;
        m_outputFrames      = 0;
        m_lastSnapshot      = now;
    }
}

} // namespace ShaderBeam