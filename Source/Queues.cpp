#include "Queues.h"
#include <algorithm>

using namespace RTGL1;

uint32_t Queues::GetIndexGraphics() const
{
    return indexGraphics;
}

uint32_t Queues::GetIndexCompute() const
{
    return indexCompute;
}

uint32_t Queues::GetIndexTransfer() const
{
    return indexTransfer;
}

uint32_t Queues::GetIndexPresent() const
{
    return indexPresent;
}

uint32_t Queues::GetIndexImageAcquire() const
{
    return indexAcquire;
}

VkQueue Queues::GetGraphics() const
{
    return graphics;
}

VkQueue Queues::GetCompute() const
{
    return compute;
}

VkQueue Queues::GetTransfer() const
{
    return transfer;
}

VkQueue Queues::GetPresentQueue() const
{
    return present;
}

VkQueue Queues::GetImageAcquireQueue() const
{
    return acquire;
}

Queues::Queues( VkPhysicalDevice physDevice, VkSurfaceKHR surface, bool enableFrameGeneration )
    : indexGraphics( UINT32_MAX )
    , indexCompute( UINT32_MAX )
    , indexTransfer( UINT32_MAX )
    , indexPresent( UINT32_MAX )
    , indexAcquire( UINT32_MAX )
    , subIndexGraphics( 0 )
    , subIndexCompute( 0 )
    , subIndexTransfer( 0 )
    , subIndexPresent( 0 )
    , subIndexAcquire( 0 )
    , requestedQueueCounts{}
    , graphics( VK_NULL_HANDLE )
    , compute( VK_NULL_HANDLE )
    , transfer( VK_NULL_HANDLE )
    , present( VK_NULL_HANDLE )
    , acquire( VK_NULL_HANDLE )
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties( physDevice, &queueFamilyCount, nullptr );
    assert( queueFamilyCount > 0 );

    queueFamilyProperties.resize( queueFamilyCount );
    vkGetPhysicalDeviceQueueFamilyProperties(
        physDevice, &queueFamilyCount, queueFamilyProperties.data() );

    uint32_t dedicatedCompute = UINT32_MAX;
    uint32_t dedicatedTransfer = UINT32_MAX;

    std::vector< VkBool32 > presentSupported( queueFamilyCount, VK_FALSE );

    for( uint32_t i = 0; i < queueFamilyProperties.size(); i++ )
    {
        auto flags = queueFamilyProperties[ i ].queueFlags;

        VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(
            physDevice, i, surface, &presentSupported[ i ] );
        VK_CHECKERROR( r );

        if( ( flags & VK_QUEUE_GRAPHICS_BIT ) != 0 && ( flags & VK_QUEUE_COMPUTE_BIT ) != 0 &&
            ( flags & VK_QUEUE_TRANSFER_BIT ) != 0 && presentSupported[ i ] )
        {
            indexGraphics = i;
        }

        if( ( flags & VK_QUEUE_GRAPHICS_BIT ) == 0 && ( flags & VK_QUEUE_COMPUTE_BIT ) != 0 )
        {
            dedicatedCompute = i;
        }

        if( ( flags & VK_QUEUE_GRAPHICS_BIT ) == 0 && ( flags & VK_QUEUE_COMPUTE_BIT ) == 0 &&
            ( flags & VK_QUEUE_TRANSFER_BIT ) != 0 )
        {
            dedicatedTransfer = i;
        }
    }

    assert( indexGraphics != UINT32_MAX );

    if( !enableFrameGeneration )
    {
        indexCompute = ( dedicatedCompute != UINT32_MAX ) ? dedicatedCompute : indexGraphics;
        indexTransfer = ( dedicatedTransfer != UINT32_MAX ) ? dedicatedTransfer : indexGraphics;
        indexPresent = indexGraphics;
        indexAcquire = indexGraphics;

        requestedQueueCounts[ indexGraphics ] = 1;
        if( indexCompute != indexGraphics )
        {
            requestedQueueCounts[ indexCompute ] = 1;
        }
        if( indexTransfer != indexGraphics && indexTransfer != indexCompute )
        {
            requestedQueueCounts[ indexTransfer ] = 1;
        }

        subIndexGraphics = 0;
        subIndexCompute = 0;
        subIndexTransfer = 0;
        subIndexPresent = 0;
        subIndexAcquire = 0;
    }
    else
    {
        // Frame generation: the FFX swapchain replacement takes over acquire/present and runs
        // interpolation workloads. Its queues must be separate from the engine's queues:
        // a present queue shared with rendering serializes (or deadlocks) frame pacing,
        // and the async compute queue should be a dedicated compute queue.
        // See ffxCreateContextDescFrameGenerationSwapChainVK in ffx_api_vk.h.
        uint32_t allocated[ 16 ] = {};

        auto takeQueue = [ & ]( uint32_t family ) -> uint32_t
        {
            if( allocated[ family ] < queueFamilyProperties[ family ].queueCount )
            {
                return allocated[ family ]++;
            }
            return 0;
        };

        subIndexGraphics = takeQueue( indexGraphics );

        if( dedicatedTransfer != UINT32_MAX )
        {
            indexTransfer    = dedicatedTransfer;
            subIndexTransfer = takeQueue( dedicatedTransfer );
        }
        else
        {
            indexTransfer    = indexGraphics;
            subIndexTransfer = takeQueue( indexGraphics );
        }

        if( dedicatedCompute != UINT32_MAX )
        {
            indexCompute    = dedicatedCompute;
            subIndexCompute = takeQueue( dedicatedCompute );
        }
        else
        {
            indexCompute    = indexGraphics;
            subIndexCompute = takeQueue( indexGraphics );
        }

        // present queue: needs transfer + present support; prefer a family
        // that the engine doesn't use (engine: graphics + transfer)
        indexPresent    = UINT32_MAX;
        subIndexPresent = 0;
        for( uint32_t i = 0; i < queueFamilyCount; i++ )
        {
            if( i == indexGraphics || i == indexTransfer )
            {
                continue;
            }
            if( !presentSupported[ i ] )
            {
                continue;
            }
            if( ( queueFamilyProperties[ i ].queueFlags & VK_QUEUE_TRANSFER_BIT ) == 0 )
            {
                continue;
            }
            if( allocated[ i ] >= queueFamilyProperties[ i ].queueCount )
            {
                continue;
            }

            indexPresent    = i;
            subIndexPresent = takeQueue( i );
            break;
        }
        if( indexPresent == UINT32_MAX )
        {
            // last resort: a separate queue in the graphics family
            indexPresent    = indexGraphics;
            subIndexPresent = takeQueue( indexGraphics );
        }

        // acquire queue: no capability requirements; prefer a family used by no one
        indexAcquire    = UINT32_MAX;
        subIndexAcquire = 0;
        for( uint32_t i = 0; i < queueFamilyCount; i++ )
        {
            if( i == indexGraphics || i == indexTransfer || i == indexCompute ||
                i == indexPresent )
            {
                continue;
            }
            if( allocated[ i ] >= queueFamilyProperties[ i ].queueCount )
            {
                continue;
            }

            indexAcquire    = i;
            subIndexAcquire = takeQueue( i );
            break;
        }
        if( indexAcquire == UINT32_MAX )
        {
            if( allocated[ indexCompute ] < queueFamilyProperties[ indexCompute ].queueCount )
            {
                indexAcquire    = indexCompute;
                subIndexAcquire = takeQueue( indexCompute );
            }
            else
            {
                indexAcquire    = indexGraphics;
                subIndexAcquire = takeQueue( indexGraphics );
            }
        }

        for( uint32_t i = 0; i < queueFamilyProperties.size(); i++ )
        {
            requestedQueueCounts[ i ] =
                std::min( allocated[ i ], queueFamilyProperties[ i ].queueCount );
        }
    }

    for( uint32_t i = 0; i < queueFamilyProperties.size(); i++ )
    {
        if( requestedQueueCounts[ i ] > 0 )
        {
            queuePriorities[ i ].assign( requestedQueueCounts[ i ], 1.0f );
        }
    }
}

void Queues::SetDevice( VkDevice device )
{
    vkGetDeviceQueue( device, indexGraphics, subIndexGraphics, &graphics );
    vkGetDeviceQueue( device, indexCompute, subIndexCompute, &compute );
    vkGetDeviceQueue( device, indexTransfer, subIndexTransfer, &transfer );
    vkGetDeviceQueue( device, indexPresent, subIndexPresent, &present );
    vkGetDeviceQueue( device, indexAcquire, subIndexAcquire, &acquire );
}

std::vector< VkDeviceQueueCreateInfo > Queues::GetDeviceQueueCreateInfos() const
{
    std::vector< VkDeviceQueueCreateInfo > infos;

    for( uint32_t i = 0; i < queueFamilyProperties.size(); i++ )
    {
        if( requestedQueueCounts[ i ] > 0 )
        {
            VkDeviceQueueCreateInfo info = {
                .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = i,
                .queueCount       = requestedQueueCounts[ i ],
                .pQueuePriorities = queuePriorities[ i ].data(),
            };
            infos.push_back( info );
        }
    }

    return infos;
}
