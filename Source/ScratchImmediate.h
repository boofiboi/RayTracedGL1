// Copyright (c) 2022 Sultim Tsyrendashiev
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

#include <cassert>
#include <optional>
#include <span>
#include <vector>

#include "RTGL1/RTGL1.h"

namespace RTGL1
{

class ScratchImmediate
{
public:
    struct PrimitiveRange
    {
        uint32_t startVertex;
        uint32_t end;

        uint32_t Count() const
        {
            assert( startVertex <= end );
            return end - startVertex;
        }
    };

public:
    void Clear();
    void StartPrimitive( RgUtilImScratchTopology topology );
    void EndPrimitive();

    void Vertex( float x, float y, float z )
    {
        accumVertex.position[ 0 ] = x;
        accumVertex.position[ 1 ] = y;
        accumVertex.position[ 2 ] = z;

        if( accumTexLayer1 )
        {
            texLayer1.push_back( *accumTexLayer1 );
        }
        if( accumTexLayer2 )
        {
            texLayer2.push_back( *accumTexLayer2 );
        }
        if( accumTexLayer3 )
        {
            texLayer3.push_back( *accumTexLayer3 );
        }

        verts.push_back( accumVertex );
    }

    void Normal( float x, float y, float z )
    {
        accumVertex.normal[ 0 ] = x;
        accumVertex.normal[ 1 ] = y;
        accumVertex.normal[ 2 ] = z;
    }

    void TexCoord( float u, float v )
    {
        accumVertex.texCoord[ 0 ] = u;
        accumVertex.texCoord[ 1 ] = v;
    }
    void TexCoord_Layer1( float u, float v ) { accumTexLayer1 = { u, v }; }
    void TexCoord_Layer2( float u, float v ) { accumTexLayer2 = { u, v }; }
    void TexCoord_Layer3( float u, float v ) { accumTexLayer3 = { u, v }; }

    void Color( RgColor4DPacked32 color ) { accumVertex.color = color; }

    void PushVertices( uint32_t     vertexCount,
                       const float* pPositions,
                       uint32_t     strideVerts,
                       const float* pTexCoords,
                       uint32_t     strideTexCoords,
                       const float* pTexCoordsLayer1,
                       uint32_t     strideTexCoordsLayer1 )
    {
        const bool withLayer1 = pTexCoordsLayer1 != nullptr;
        if( withLayer1 )
        {
            texLayer1.reserve( texLayer1.size() + vertexCount );
        }
        verts.reserve( verts.size() + vertexCount );

        for( uint32_t i = 0; i < vertexCount; i++ )
        {
            accumVertex.position[ 0 ] = pPositions[ 0 ];
            accumVertex.position[ 1 ] = pPositions[ 1 ];
            accumVertex.position[ 2 ] = pPositions[ 2 ];
            accumVertex.texCoord[ 0 ] = pTexCoords[ 0 ];
            accumVertex.texCoord[ 1 ] = pTexCoords[ 1 ];

            if( withLayer1 )
            {
                texLayer1.push_back( RgFloat2D{ pTexCoordsLayer1[ 0 ], pTexCoordsLayer1[ 1 ] } );
            }

            verts.push_back( accumVertex );

            pPositions = reinterpret_cast< const float* >( reinterpret_cast< const uint8_t* >( pPositions ) + strideVerts );
            pTexCoords = reinterpret_cast< const float* >( reinterpret_cast< const uint8_t* >( pTexCoords ) + strideTexCoords );
            if( withLayer1 )
            {
                pTexCoordsLayer1 = reinterpret_cast< const float* >( reinterpret_cast< const uint8_t* >( pTexCoordsLayer1 ) + strideTexCoordsLayer1 );
            }
        }
    }

    void SetToPrimitive( RgMeshPrimitiveInfo* pTarget );

    std::span< const uint32_t > GetIndices( RgUtilImScratchTopology topology,
                                            uint32_t                vertexCount );

private:
    std::vector< RgPrimitiveVertex > verts;
    std::vector< RgFloat2D >         texLayer1;
    std::vector< RgFloat2D >         texLayer2;
    std::vector< RgFloat2D >         texLayer3;
    std::optional< PrimitiveRange >  lastbatch;

    std::vector< uint32_t > indicesTriangles;
    std::vector< uint32_t > indicesTriangleStrip;
    std::vector< uint32_t > indicesTriangleFan;
    std::vector< uint32_t > indicesQuad;

    std::vector< uint32_t > accumIndices;

    RgPrimitiveVertex accumVertex{
        .position = { 0.0f, 0.0f, 0.0f },
        .normal   = { 0.0f, 1.0f, 0.0f },
        .tangent  = { 0.0f, 0.0f, 1.0f },
        .texCoord = { 0.0f, 0.0f },
        .color    = rgUtilPackColorByte4D( 255, 255, 255, 255 ),
    };
    std::optional< RgUtilImScratchTopology > accumTopology;
    std::optional< RgFloat2D >               accumTexLayer1;
    std::optional< RgFloat2D >               accumTexLayer2;
    std::optional< RgFloat2D >               accumTexLayer3;
};

}
