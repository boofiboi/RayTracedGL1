// Copyright (c) 2020-2021 Sultim Tsyrendashiev
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <vector>
#include "Common.h"

namespace RTGL1
{

class Queues
{
public:
    explicit Queues( VkPhysicalDevice physDevice, VkSurfaceKHR surface, bool enableFrameGeneration = false );
    ~Queues() = default;

    Queues( const Queues& other )                  = delete;
    Queues( Queues&& other ) noexcept              = delete;
    Queues&                                operator=( const Queues& other ) = delete;
    Queues&                                operator=( Queues&& other ) noexcept = delete;

    void                                   SetDevice( VkDevice device );

    std::vector< VkDeviceQueueCreateInfo > GetDeviceQueueCreateInfos() const;

    uint32_t                               GetIndexGraphics() const;
    uint32_t                               GetIndexCompute() const;
    uint32_t                               GetIndexTransfer() const;
    uint32_t                               GetIndexPresent() const;
    uint32_t                               GetIndexImageAcquire() const;

    VkQueue                                GetGraphics() const;
    VkQueue                                GetCompute() const;
    VkQueue                                GetTransfer() const;
    VkQueue                                GetPresentQueue() const;
    VkQueue                                GetImageAcquireQueue() const;

private:
    std::vector< VkQueueFamilyProperties > queueFamilyProperties;
    std::vector< float >                   queuePriorities[ 16 ];

    uint32_t                               indexGraphics;
    uint32_t                               indexCompute;
    uint32_t                               indexTransfer;
    uint32_t                               indexPresent;
    uint32_t                               indexAcquire;

    uint32_t                               subIndexGraphics;
    uint32_t                               subIndexCompute;
    uint32_t                               subIndexTransfer;
    uint32_t                               subIndexPresent;
    uint32_t                               subIndexAcquire;

    uint32_t                               requestedQueueCounts[ 16 ];

    VkQueue                                graphics;
    VkQueue                                compute;
    VkQueue                                transfer;
    VkQueue                                present;
    VkQueue                                acquire;
};

}