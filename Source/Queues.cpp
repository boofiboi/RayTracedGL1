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

    for( uint32_t i = 0; i < queueFamilyProperties.size(); i++ )
    {
        auto flags = queueFamilyProperties[ i ].queueFlags;

        VkBool32 presentSupported = VK_FALSE;
        VkResult r =
            vkGetPhysicalDeviceSurfaceSupportKHR( physDevice, i, surface, &presentSupported );
        VK_CHECKERROR( r );

        if( ( flags & VK_QUEUE_GRAPHICS_BIT ) != 0 && ( flags & VK_QUEUE_COMPUTE_BIT ) != 0 &&
            ( flags & VK_QUEUE_TRANSFER_BIT ) != 0 && presentSupported )
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
        uint32_t allocated[ 16 ] = {};

        subIndexGraphics = 0;
        allocated[ indexGraphics ] = std::max( allocated[ indexGraphics ], 1U );

        if( dedicatedCompute != UINT32_MAX )
        {
            indexCompute = dedicatedCompute;
            subIndexCompute = allocated[ dedicatedCompute ];
            if( allocated[ dedicatedCompute ] < queueFamilyProperties[ dedicatedCompute ].queueCount )
            {
                allocated[ dedicatedCompute ]++;
            }
        }
        else
        {
            indexCompute = indexGraphics;
            if( allocated[ indexGraphics ] < queueFamilyProperties[ indexGraphics ].queueCount )
            {
                subIndexCompute = allocated[ indexGraphics ]++;
            }
            else
            {
                subIndexCompute = 0;
            }
        }

        indexPresent = indexGraphics;
        if( allocated[ indexGraphics ] < queueFamilyProperties[ indexGraphics ].queueCount )
        {
            subIndexPresent = allocated[ indexGraphics ]++;
        }
        else
        {
            subIndexPresent = 0;
        }

        if( dedicatedTransfer != UINT32_MAX )
        {
            indexAcquire = dedicatedTransfer;
            subIndexAcquire = 0;
            allocated[ dedicatedTransfer ] = std::max( allocated[ dedicatedTransfer ], 1U );
        }
        else if( dedicatedCompute != UINT32_MAX && allocated[ dedicatedCompute ] < queueFamilyProperties[ dedicatedCompute ].queueCount )
        {
            indexAcquire = dedicatedCompute;
            subIndexAcquire = allocated[ dedicatedCompute ]++;
        }
        else
        {
            indexAcquire = indexGraphics;
            subIndexAcquire = 0;
        }

        indexTransfer = ( dedicatedTransfer != UINT32_MAX ) ? dedicatedTransfer : indexGraphics;
        subIndexTransfer = ( indexTransfer == dedicatedTransfer ) ? 0 : subIndexGraphics;

        for( uint32_t i = 0; i < queueFamilyProperties.size(); i++ )
        {
            requestedQueueCounts[ i ] = std::min( allocated[ i ], queueFamilyProperties[ i ].queueCount );
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
