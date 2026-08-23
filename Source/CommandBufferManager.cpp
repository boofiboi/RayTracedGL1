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

#include "CommandBufferManager.h"
#include "Utils.h"

RTGL1::CommandBufferManager::CommandBufferManager( VkDevice                  _device,
                                                   std::shared_ptr< Queues > _queues )
    : device( _device )
    , currentFrameIndex( MAX_FRAMES_IN_FLIGHT - 1 )
    , queues( std::move( _queues ) )
{
    VkCommandPoolCreateInfo cmdPoolInfo = {};
    cmdPoolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.flags                   = 0;

    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
    {
        VkResult r;

        cmdPoolInfo.queueFamilyIndex = queues->GetIndexGraphics();
        r = vkCreateCommandPool( device, &cmdPoolInfo, nullptr, &graphicsCmds[ i ].pool );
        VK_CHECKERROR( r );
        graphicsCmds[ i ].queue = queues->GetGraphics();

        cmdPoolInfo.queueFamilyIndex = queues->GetIndexCompute();
        r = vkCreateCommandPool( device, &cmdPoolInfo, nullptr, &computeCmds[ i ].pool );
        VK_CHECKERROR( r );
        computeCmds[ i ].queue = queues->GetCompute();

        cmdPoolInfo.queueFamilyIndex = queues->GetIndexTransfer();
        r = vkCreateCommandPool( device, &cmdPoolInfo, nullptr, &transferCmds[ i ].pool );
        VK_CHECKERROR( r );
        transferCmds[ i ].queue = queues->GetTransfer();
    }
}

RTGL1::CommandBufferManager::~CommandBufferManager()
{
    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
    {
        vkDestroyCommandPool( device, graphicsCmds[ i ].pool, nullptr );
        vkDestroyCommandPool( device, computeCmds[ i ].pool, nullptr );
        vkDestroyCommandPool( device, transferCmds[ i ].pool, nullptr );
    }
}

void RTGL1::CommandBufferManager::PrepareForFrame( uint32_t frameIndex )
{
    vkResetCommandPool( device, graphicsCmds[ frameIndex ].pool, 0 );
    vkResetCommandPool( device, computeCmds[ frameIndex ].pool, 0 );
    vkResetCommandPool( device, transferCmds[ frameIndex ].pool, 0 );

    graphicsCmds[ frameIndex ].curCount = 0;
    computeCmds[ frameIndex ].curCount  = 0;
    transferCmds[ frameIndex ].curCount = 0;

    currentFrameIndex = frameIndex;
}

VkCommandBuffer RTGL1::CommandBufferManager::StartCmd( AllocatedCmds& allocated )
{
    VkResult r;

    size_t oldCount = allocated.cmds.size();

    // if not enough, allocate new buffers
    if( allocated.curCount + 1 > oldCount )
    {
        allocated.cmds.resize( oldCount + cmdAllocStep );

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool                 = allocated.pool;
        allocInfo.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount          = cmdAllocStep;

        r = vkAllocateCommandBuffers( device, &allocInfo, &allocated.cmds[ oldCount ] );
        VK_CHECKERROR( r );
    }

    VkCommandBuffer cmd = allocated.cmds[ allocated.curCount ];
    allocated.curCount++;

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    r = vkBeginCommandBuffer( cmd, &beginInfo );
    VK_CHECKERROR( r );

    return cmd;
}

VkCommandBuffer RTGL1::CommandBufferManager::StartGraphicsCmd()
{
    return StartCmd( graphicsCmds[ currentFrameIndex ] );
}

VkCommandBuffer RTGL1::CommandBufferManager::StartComputeCmd()
{
    return StartCmd( computeCmds[ currentFrameIndex ] );
}

VkCommandBuffer RTGL1::CommandBufferManager::StartTransferCmd()
{
    return StartCmd( transferCmds[ currentFrameIndex ] );
}

VkQueue RTGL1::CommandBufferManager::GetCmdOwnerQueue( VkCommandBuffer cmd ) const
{
    // submitted handles must never be removed from the active range,
    // otherwise they could be re-begun while still pending on the queue
    for( const AllocatedCmds* allocated : { &graphicsCmds[ currentFrameIndex ],
                                            &computeCmds[ currentFrameIndex ],
                                            &transferCmds[ currentFrameIndex ] } )
    {
        for( uint32_t i = 0; i < allocated->curCount; i++ )
        {
            if( allocated->cmds[ i ] == cmd )
            {
                return allocated->queue;
            }
        }
    }

    return VK_NULL_HANDLE;
}

void RTGL1::CommandBufferManager::Submit( VkCommandBuffer cmd, VkFence fence )
{
    VkResult r = vkEndCommandBuffer( cmd );
    VK_CHECKERROR( r );

    VkSubmitInfo submitInfo = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };

    VkQueue q = GetCmdOwnerQueue( cmd );
    assert( q != VK_NULL_HANDLE );

    r = vkQueueSubmit( q, 1, &submitInfo, fence );
    VK_CHECKERROR( r );
}


void RTGL1::CommandBufferManager::Submit( VkCommandBuffer             cmd,
                                          const VkSemaphore*          waitSemaphores,
                                          const VkPipelineStageFlags* waitStages,
                                          uint32_t                    waitCount,
                                          const VkSemaphore*          signalSemaphores,
                                          uint32_t                    signalCount,
                                          VkFence                     fence )
{
    VkResult r = vkEndCommandBuffer( cmd );
    VK_CHECKERROR( r );

    VkSubmitInfo submitInfo = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = waitCount,
        .pWaitSemaphores      = waitSemaphores,
        .pWaitDstStageMask    = waitStages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd,
        .signalSemaphoreCount = signalCount,
        .pSignalSemaphores    = signalSemaphores,
    };

    VkQueue q = GetCmdOwnerQueue( cmd );
    assert( q != VK_NULL_HANDLE );

    r = vkQueueSubmit( q, 1, &submitInfo, fence );
    VK_CHECKERROR( r );
}

void RTGL1::CommandBufferManager::Submit( VkCommandBuffer             cmd,
                                          const VkSemaphore*          waitSemaphores,
                                          const VkPipelineStageFlags* waitStages,
                                          uint32_t                    waitCount,
                                          VkSemaphore                 signalSemaphore,
                                          VkFence                     fence )
{
    Submit( cmd, waitSemaphores, waitStages, waitCount, &signalSemaphore, 1, fence );
}

void RTGL1::CommandBufferManager::Submit( VkCommandBuffer      cmd,
                                          VkSemaphore          waitSemaphore,
                                          VkPipelineStageFlags waitStages,
                                          VkSemaphore          signalSemaphore,
                                          VkFence              fence )
{
    Submit( cmd, &waitSemaphore, &waitStages, 1, signalSemaphore, fence );
}


void RTGL1::CommandBufferManager::WaitGraphicsIdle()
{
    VkResult r = vkQueueWaitIdle( queues->GetGraphics() );
    VK_CHECKERROR( r );
}

void RTGL1::CommandBufferManager::WaitComputeIdle()
{
    VkResult r = vkQueueWaitIdle( queues->GetCompute() );
    VK_CHECKERROR( r );
}

void RTGL1::CommandBufferManager::WaitTransferIdle()
{
    VkResult r = vkQueueWaitIdle( queues->GetTransfer() );
    VK_CHECKERROR( r );
}

void RTGL1::CommandBufferManager::WaitDeviceIdle()
{
    vkDeviceWaitIdle( device );
}
