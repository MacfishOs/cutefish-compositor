// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/main_loop.h"

#include "base/event_loop.h"

namespace cutefish::core {

namespace {

MainLoop *s_instance = nullptr;

} // namespace

MainLoop::MainLoop()
    : m_loop(std::make_unique<base::EventLoop>())
    , m_previous(s_instance)
{
    s_instance = this;
}

MainLoop::~MainLoop()
{
    if (s_instance == this)
        s_instance = m_previous;
    m_loop.reset();
}

base::EventLoop *mainLoop()
{
    return s_instance ? &s_instance->loop() : nullptr;
}

} // namespace cutefish::core
