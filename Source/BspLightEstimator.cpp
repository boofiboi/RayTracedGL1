#include "BspLightEstimator.h"

#include "Utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace RTGL1
{
namespace
{

constexpr int32_t BSP_VERSION_Q1   = 29;
constexpr int32_t BSP_VERSION_HL1  = 30;
constexpr int32_t BSP_VERSION_BSP2 = ( 'B' << 0 ) | ( 'S' << 8 ) | ( 'P' << 16 ) | ( '2' << 24 );

constexpr int LUMP_ENTITIES  = 0;
constexpr int LUMP_PLANES    = 1;
constexpr int LUMP_TEXTURES  = 2;
constexpr int LUMP_VERTEXES  = 3;
constexpr int LUMP_NODES     = 5;
constexpr int LUMP_TEXINFO   = 6;
constexpr int LUMP_FACES     = 7;
constexpr int LUMP_LIGHTING  = 8;
constexpr int LUMP_LEAFS     = 10;
constexpr int LUMP_EDGES     = 12;
constexpr int LUMP_SURFEDGES = 13;
constexpr int LUMP_MODELS    = 14;
constexpr int NUM_LUMPS      = 15;

constexpr int CONTENTS_EMPTY = -1;
constexpr int CONTENTS_SOLID = -2;
constexpr int CONTENTS_SKY   = -6;

constexpr float DIST_EPSILON = 0.03125f;

#pragma pack( push, 1 )
struct BspLump
{
    int32_t fileofs;
    int32_t filelen;
};

struct BspHeader
{
    int32_t version;
    BspLump lumps[ NUM_LUMPS ];
};

struct BspPlane
{
    float   normal[ 3 ];
    float   dist;
    int32_t type;
};

struct BspVertex
{
    float point[ 3 ];
};

struct BspTexInfo
{
    float   vecs[ 2 ][ 4 ];
    int32_t miptex;
    int32_t flags;
};

struct BspEdge16
{
    uint16_t v[ 2 ];
};

struct BspEdge32
{
    uint32_t v[ 2 ];
};

struct BspFace16
{
    int16_t planenum;
    int16_t side;
    int32_t firstedge;
    int16_t numedges;
    int16_t texinfo;
    uint8_t styles[ 4 ];
    int32_t lightofs;
};

struct BspFace32
{
    int32_t planenum;
    int32_t side;
    int32_t firstedge;
    int32_t numedges;
    int32_t texinfo;
    uint8_t styles[ 4 ];
    int32_t lightofs;
};

struct BspNode16
{
    int32_t  planenum;
    int16_t  children[ 2 ];
    int16_t  mins[ 3 ];
    int16_t  maxs[ 3 ];
    uint16_t firstface;
    uint16_t numfaces;
};

struct BspNode32
{
    int32_t planenum;
    int32_t children[ 2 ];
    float   mins[ 3 ];
    float   maxs[ 3 ];
    int32_t firstface;
    int32_t numfaces;
};

struct BspLeaf16
{
    int32_t  contents;
    int32_t  visofs;
    int16_t  mins[ 3 ];
    int16_t  maxs[ 3 ];
    uint16_t firstmarksurface;
    uint16_t nummarksurfaces;
    uint8_t  ambient[ 4 ];
};

struct BspLeaf32
{
    int32_t contents;
    int32_t visofs;
    float   mins[ 3 ];
    float   maxs[ 3 ];
    int32_t firstmarksurface;
    int32_t nummarksurfaces;
    uint8_t ambient[ 4 ];
};

struct BspModel
{
    float   mins[ 3 ];
    float   maxs[ 3 ];
    float   origin[ 3 ];
    int32_t headnode[ 4 ];
    int32_t visleafs;
    int32_t firstface;
    int32_t numfaces;
};

struct BspMiptex
{
    char     name[ 16 ];
    uint32_t width;
    uint32_t height;
    uint32_t offsets[ 4 ];
};
#pragma pack( pop )

struct Vec3
{
    float x, y, z;

    Vec3 operator+( const Vec3& o ) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-( const Vec3& o ) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*( float s ) const { return { x * s, y * s, z * s }; }
    Vec3 operator/( float s ) const { return { x / s, y / s, z / s }; }
    Vec3 operator-() const { return { -x, -y, -z }; }
};

float Dot( const Vec3& a, const Vec3& b )
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross( const Vec3& a, const Vec3& b )
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float LengthSq( const Vec3& v )
{
    return Dot( v, v );
}

float Length( const Vec3& v )
{
    return std::sqrt( LengthSq( v ) );
}

Vec3 Normalize( const Vec3& v )
{
    float len = Length( v );
    if( len > 1e-8f )
    {
        return v / len;
    }
    return { 0.0f, 0.0f, 1.0f };
}

RgFloat3D ToRg( const Vec3& v )
{
    return RgFloat3D{ v.x, v.y, v.z };
}

Vec3 FromRg( const RgFloat3D& v )
{
    return Vec3{ v.data[ 0 ], v.data[ 1 ], v.data[ 2 ] };
}

std::string ToLowerAscii( std::string_view s )
{
    std::string out( s );
    for( char& c : out )
    {
        c = char( std::tolower( static_cast< unsigned char >( c ) ) );
    }
    return out;
}

bool SolveLinear3x3( const Vec3& r0,
                      const Vec3& r1,
                      const Vec3& r2,
                      const Vec3& rhs,
                      Vec3&       out )
{
    float m[ 3 ][ 4 ] = {
        { r0.x, r0.y, r0.z, rhs.x },
        { r1.x, r1.y, r1.z, rhs.y },
        { r2.x, r2.y, r2.z, rhs.z },
    };

    for( int col = 0; col < 3; col++ )
    {
        int   pivot = col;
        float best  = std::abs( m[ col ][ col ] );
        for( int row = col + 1; row < 3; row++ )
        {
            float v = std::abs( m[ row ][ col ] );
            if( v > best )
            {
                best  = v;
                pivot = row;
            }
        }
        if( best < 1e-8f )
        {
            return false;
        }
        if( pivot != col )
        {
            for( int k = col; k < 4; k++ )
            {
                std::swap( m[ col ][ k ], m[ pivot ][ k ] );
            }
        }
        float invDiag = 1.0f / m[ col ][ col ];
        for( int k = col; k < 4; k++ )
        {
            m[ col ][ k ] *= invDiag;
        }
        for( int row = 0; row < 3; row++ )
        {
            if( row == col )
            {
                continue;
            }
            float f = m[ row ][ col ];
            for( int k = col; k < 4; k++ )
            {
                m[ row ][ k ] -= f * m[ col ][ k ];
            }
        }
    }

    out = { m[ 0 ][ 3 ], m[ 1 ][ 3 ], m[ 2 ][ 3 ] };
    return std::isfinite( out.x ) && std::isfinite( out.y ) && std::isfinite( out.z );
}

bool PointInConvexPolygon( const Vec3&                p,
                           const Vec3&                faceNormal,
                           const std::vector< Vec3 >& poly,
                           float                      tolerance )
{
    if( poly.size() < 3 )
    {
        return false;
    }

    for( size_t i = 0; i < poly.size(); i++ )
    {
        const Vec3& a    = poly[ i ];
        const Vec3& b    = poly[ ( i + 1 ) % poly.size() ];
        Vec3        edge = b - a;
        float       elen = Length( edge );
        if( elen < 1e-5f )
        {
            continue;
        }
        Vec3 inward = Cross( faceNormal, edge / elen );
        if( Dot( p - a, inward ) < -tolerance )
        {
            return false;
        }
    }
    return true;
}

struct BspNodeUnified
{
    int32_t planenum;
    int32_t children[ 2 ];
};

struct BspLeafUnified
{
    int32_t contents;
};

struct BspFaceUnified
{
    int32_t planenum;
    int32_t side;
    int32_t firstedge;
    int32_t numedges;
    int32_t texinfo;
    uint8_t styles[ 4 ];
    int32_t lightofs;
};

struct BspData
{
    int32_t                      version{ 0 };
    std::vector< BspPlane >      planes;
    std::vector< Vec3 >          vertices;
    std::vector< BspEdge32 >     edges;
    std::vector< int32_t >       surfedges;
    std::vector< BspTexInfo >    texinfos;
    std::vector< std::string >   textureNames;
    std::vector< BspFaceUnified > faces;
    std::vector< BspNodeUnified > nodes;
    std::vector< BspLeafUnified > leafs;
    std::vector< BspModel >      models;
    std::vector< uint8_t >       lightdata;
    int                          lightmapSamples{ 3 };
    int32_t                      worldRootNode{ 0 };
    bool                         hasAuthoredBspLights{ false };
    std::optional< RgDirectionalLightUploadInfo > bspSunLight;

    template< typename T >
    static bool ReadLump( const std::vector< uint8_t >& buf,
                          const BspLump&                lump,
                          std::vector< T >&             out )
    {
        if( lump.fileofs < 0 || lump.filelen < 0 ||
            size_t( lump.fileofs ) + size_t( lump.filelen ) > buf.size() )
        {
            return false;
        }
        if( lump.filelen % int32_t( sizeof( T ) ) != 0 )
        {
            return false;
        }
        size_t count = size_t( lump.filelen ) / sizeof( T );
        out.resize( count );
        if( count > 0 )
        {
            std::memcpy( out.data(), buf.data() + lump.fileofs, size_t( lump.filelen ) );
        }
        return true;
    }

    int PointContents( const Vec3& p ) const
    {
        if( nodes.empty() || leafs.empty() )
        {
            return CONTENTS_EMPTY;
        }

        int32_t num = worldRootNode;
        while( num >= 0 )
        {
            if( size_t( num ) >= nodes.size() )
            {
                return CONTENTS_SOLID;
            }
            const BspNodeUnified& node = nodes[ num ];
            if( node.planenum < 0 || size_t( node.planenum ) >= planes.size() )
            {
                return CONTENTS_SOLID;
            }
            const BspPlane& pl = planes[ node.planenum ];
            float d = pl.normal[ 0 ] * p.x + pl.normal[ 1 ] * p.y + pl.normal[ 2 ] * p.z - pl.dist;
            num     = node.children[ d >= 0.0f ? 0 : 1 ];
        }

        int32_t leafIdx = ~num;
        if( leafIdx < 0 || size_t( leafIdx ) >= leafs.size() )
        {
            return CONTENTS_SOLID;
        }
        return leafs[ leafIdx ].contents;
    }

    bool TraceRecursive( int32_t     num,
                         float       p1f,
                         float       p2f,
                         const Vec3& p1,
                         const Vec3& p2,
                         int&        hitContents,
                         float&      hitFraction ) const
    {
        while( num >= 0 )
        {
            if( size_t( num ) >= nodes.size() )
            {
                hitContents = CONTENTS_SOLID;
                hitFraction = p1f;
                return false;
            }

            const BspNodeUnified& node = nodes[ num ];
            if( node.planenum < 0 || size_t( node.planenum ) >= planes.size() )
            {
                hitContents = CONTENTS_SOLID;
                hitFraction = p1f;
                return false;
            }

            const BspPlane& pl = planes[ node.planenum ];
            float t1 = pl.normal[ 0 ] * p1.x + pl.normal[ 1 ] * p1.y + pl.normal[ 2 ] * p1.z - pl.dist;
            float t2 = pl.normal[ 0 ] * p2.x + pl.normal[ 1 ] * p2.y + pl.normal[ 2 ] * p2.z - pl.dist;

            if( t1 >= 0.0f && t2 >= 0.0f )
            {
                num = node.children[ 0 ];
                continue;
            }
            if( t1 < 0.0f && t2 < 0.0f )
            {
                num = node.children[ 1 ];
                continue;
            }

            int   side = ( t1 < 0.0f ) ? 1 : 0;
            float frac = side ? ( t1 + DIST_EPSILON ) / ( t1 - t2 ) : ( t1 - DIST_EPSILON ) / ( t1 - t2 );
            frac       = std::clamp( frac, 0.0f, 1.0f );
            float midf = p1f + ( p2f - p1f ) * frac;
            Vec3  mid  = p1 + ( p2 - p1 ) * frac;

            if( !TraceRecursive( node.children[ side ], p1f, midf, p1, mid, hitContents, hitFraction ) )
            {
                return false;
            }

            return TraceRecursive(
                node.children[ side ^ 1 ], midf, p2f, mid, p2, hitContents, hitFraction );
        }

        int32_t leafIdx = ~num;
        if( leafIdx < 0 || size_t( leafIdx ) >= leafs.size() )
        {
            hitContents = CONTENTS_SOLID;
            hitFraction = p1f;
            return false;
        }

        int contents = leafs[ leafIdx ].contents;
        if( contents == CONTENTS_SOLID || contents == CONTENTS_SKY )
        {
            hitContents = contents;
            hitFraction = p1f;
            return false;
        }

        return true;
    }

    bool HasLineOfSight( const Vec3& a, const Vec3& b ) const
    {
        if( nodes.empty() || leafs.empty() )
        {
            return true;
        }
        int   hitContents = CONTENTS_EMPTY;
        float hitFrac     = 1.0f;
        return TraceRecursive( worldRootNode, 0.0f, 1.0f, a, b, hitContents, hitFrac );
    }

    bool TraceHitsSky( const Vec3& a, const Vec3& dir, float maxDist ) const
    {
        if( nodes.empty() || leafs.empty() )
        {
            return false;
        }
        int   hitContents = CONTENTS_EMPTY;
        float hitFrac     = 1.0f;
        bool  clear       = TraceRecursive(
            worldRootNode, 0.0f, 1.0f, a, a + dir * maxDist, hitContents, hitFrac );
        return !clear && hitContents == CONTENTS_SKY;
    }
};

std::optional< std::filesystem::path > FindBspFile( const std::filesystem::path& explicitPath,
                                                    std::string_view             mapName,
                                                    const std::filesystem::path& scenesFolder )
{
    std::error_code ec;
    if( !explicitPath.empty() && std::filesystem::is_regular_file( explicitPath, ec ) )
    {
        return explicitPath;
    }

    if( mapName.empty() || scenesFolder.empty() )
    {
        return std::nullopt;
    }

    std::string relBsp = std::string( mapName ) + ".bsp";
    auto        rtRoot = scenesFolder.parent_path();
    auto        gameRoot = rtRoot.parent_path();
    if( gameRoot.empty() || !std::filesystem::is_directory( gameRoot, ec ) )
    {
        return std::nullopt;
    }

    for( const auto& entry : std::filesystem::directory_iterator( gameRoot, ec ) )
    {
        if( !entry.is_directory( ec ) )
        {
            continue;
        }
        auto candidate = entry.path() / "maps" / relBsp;
        if( std::filesystem::is_regular_file( candidate, ec ) )
        {
            return candidate;
        }
    }

    return std::nullopt;
}

bool LoadBspFromFile( const std::filesystem::path& bspPath, BspData& out )
{
    std::ifstream f( bspPath, std::ios::binary | std::ios::ate );
    if( !f.is_open() )
    {
        return false;
    }
    auto sz = f.tellg();
    if( sz < std::streamoff( sizeof( BspHeader ) ) )
    {
        return false;
    }
    std::vector< uint8_t > buf( static_cast< size_t >( sz ) );
    f.seekg( 0 );
    f.read( reinterpret_cast< char* >( buf.data() ), sz );
    if( !f )
    {
        return false;
    }

    BspHeader hdr{};
    std::memcpy( &hdr, buf.data(), sizeof( hdr ) );
    out.version = hdr.version;
    if( hdr.version != BSP_VERSION_Q1 && hdr.version != BSP_VERSION_HL1 &&
        hdr.version != BSP_VERSION_BSP2 )
    {
        return false;
    }

    if( !BspData::ReadLump( buf, hdr.lumps[ LUMP_PLANES ], out.planes ) ||
        !BspData::ReadLump( buf, hdr.lumps[ LUMP_TEXINFO ], out.texinfos ) ||
        !BspData::ReadLump( buf, hdr.lumps[ LUMP_SURFEDGES ], out.surfedges ) ||
        !BspData::ReadLump( buf, hdr.lumps[ LUMP_MODELS ], out.models ) )
    {
        return false;
    }

    std::vector< BspVertex > rawVerts;
    if( !BspData::ReadLump( buf, hdr.lumps[ LUMP_VERTEXES ], rawVerts ) )
    {
        return false;
    }
    out.vertices.resize( rawVerts.size() );
    for( size_t i = 0; i < rawVerts.size(); i++ )
    {
        out.vertices[ i ] = { rawVerts[ i ].point[ 0 ], rawVerts[ i ].point[ 1 ], rawVerts[ i ].point[ 2 ] };
    }

    if( hdr.version == BSP_VERSION_BSP2 )
    {
        if( !BspData::ReadLump( buf, hdr.lumps[ LUMP_EDGES ], out.edges ) )
        {
            return false;
        }
        std::vector< BspFace32 > rawFaces;
        std::vector< BspNode32 > rawNodes;
        std::vector< BspLeaf32 > rawLeafs;
        if( !BspData::ReadLump( buf, hdr.lumps[ LUMP_FACES ], rawFaces ) ||
            !BspData::ReadLump( buf, hdr.lumps[ LUMP_NODES ], rawNodes ) ||
            !BspData::ReadLump( buf, hdr.lumps[ LUMP_LEAFS ], rawLeafs ) )
        {
            return false;
        }
        out.faces.resize( rawFaces.size() );
        for( size_t i = 0; i < rawFaces.size(); i++ )
        {
            out.faces[ i ] = {
                rawFaces[ i ].planenum,
                rawFaces[ i ].side,
                rawFaces[ i ].firstedge,
                rawFaces[ i ].numedges,
                rawFaces[ i ].texinfo,
                { rawFaces[ i ].styles[ 0 ], rawFaces[ i ].styles[ 1 ], rawFaces[ i ].styles[ 2 ], rawFaces[ i ].styles[ 3 ] },
                rawFaces[ i ].lightofs,
            };
        }
        out.nodes.resize( rawNodes.size() );
        for( size_t i = 0; i < rawNodes.size(); i++ )
        {
            out.nodes[ i ] = { rawNodes[ i ].planenum, { rawNodes[ i ].children[ 0 ], rawNodes[ i ].children[ 1 ] } };
        }
        out.leafs.resize( rawLeafs.size() );
        for( size_t i = 0; i < rawLeafs.size(); i++ )
        {
            out.leafs[ i ] = { rawLeafs[ i ].contents };
        }
    }
    else
    {
        std::vector< BspEdge16 > rawEdges;
        std::vector< BspFace16 > rawFaces;
        std::vector< BspNode16 > rawNodes;
        std::vector< BspLeaf16 > rawLeafs;
        if( !BspData::ReadLump( buf, hdr.lumps[ LUMP_EDGES ], rawEdges ) ||
            !BspData::ReadLump( buf, hdr.lumps[ LUMP_FACES ], rawFaces ) ||
            !BspData::ReadLump( buf, hdr.lumps[ LUMP_NODES ], rawNodes ) ||
            !BspData::ReadLump( buf, hdr.lumps[ LUMP_LEAFS ], rawLeafs ) )
        {
            return false;
        }
        out.edges.resize( rawEdges.size() );
        for( size_t i = 0; i < rawEdges.size(); i++ )
        {
            out.edges[ i ] = { rawEdges[ i ].v[ 0 ], rawEdges[ i ].v[ 1 ] };
        }
        out.faces.resize( rawFaces.size() );
        for( size_t i = 0; i < rawFaces.size(); i++ )
        {
            out.faces[ i ] = {
                rawFaces[ i ].planenum,
                rawFaces[ i ].side,
                rawFaces[ i ].firstedge,
                rawFaces[ i ].numedges,
                rawFaces[ i ].texinfo,
                { rawFaces[ i ].styles[ 0 ], rawFaces[ i ].styles[ 1 ], rawFaces[ i ].styles[ 2 ], rawFaces[ i ].styles[ 3 ] },
                rawFaces[ i ].lightofs,
            };
        }
        out.nodes.resize( rawNodes.size() );
        for( size_t i = 0; i < rawNodes.size(); i++ )
        {
            out.nodes[ i ] = { rawNodes[ i ].planenum, { rawNodes[ i ].children[ 0 ], rawNodes[ i ].children[ 1 ] } };
        }
        out.leafs.resize( rawLeafs.size() );
        for( size_t i = 0; i < rawLeafs.size(); i++ )
        {
            out.leafs[ i ] = { rawLeafs[ i ].contents };
        }
    }

    out.worldRootNode = 0;
    if( !out.models.empty() )
    {
        out.worldRootNode = out.models[ 0 ].headnode[ 0 ];
    }

    out.hasAuthoredBspLights = false;
    out.bspSunLight.reset();
    const BspLump& entLump   = hdr.lumps[ LUMP_ENTITIES ];
    if( entLump.fileofs >= 0 && entLump.filelen > 0 &&
        size_t( entLump.fileofs ) + size_t( entLump.filelen ) <= buf.size() )
    {
        std::string_view entText(
            reinterpret_cast< const char* >( buf.data() + entLump.fileofs ),
            size_t( entLump.filelen ) );
        if( entText.find( "\"light\"" ) != std::string_view::npos ||
            entText.find( "\"light_spot\"" ) != std::string_view::npos )
        {
            out.hasAuthoredBspLights = true;
        }

        float bestScore = -1.0f;
        bool  bestIsEnv = false;
        size_t p        = 0;
        while( p < entText.size() )
        {
            size_t open = entText.find( '{', p );
            if( open == std::string_view::npos )
            {
                break;
            }
            size_t close = entText.find( '}', open + 1 );
            if( close == std::string_view::npos )
            {
                break;
            }
            std::string_view block = entText.substr( open + 1, close - open - 1 );
            p                      = close + 1;

            bool  isEnv      = false;
            bool  hasSky     = false;
            bool  hasColor   = false;
            bool  hasPitch   = false;
            bool  hasAngle   = false;
            bool  hasAngles  = false;
            float rgb[ 3 ]   = { 255.0f, 255.0f, 255.0f };
            float scale      = 255.0f;
            float pitchVal   = -45.0f;
            float angleVal   = 0.0f;
            float angles[ 3 ] = { 0.0f, 0.0f, 0.0f };

            size_t q = 0;
            while( q < block.size() )
            {
                size_t k0 = block.find( '"', q );
                if( k0 == std::string_view::npos )
                {
                    break;
                }
                size_t k1 = block.find( '"', k0 + 1 );
                if( k1 == std::string_view::npos )
                {
                    break;
                }
                size_t v0 = block.find( '"', k1 + 1 );
                if( v0 == std::string_view::npos )
                {
                    break;
                }
                size_t v1 = block.find( '"', v0 + 1 );
                if( v1 == std::string_view::npos )
                {
                    break;
                }
                std::string_view key = block.substr( k0 + 1, k1 - k0 - 1 );
                std::string      val( block.substr( v0 + 1, v1 - v0 - 1 ) );
                q = v1 + 1;

                if( key == "classname" )
                {
                    isEnv = ( val == "light_environment" );
                }
                else if( key == "_sky" )
                {
                    int tmp = 0;
                    if( std::sscanf( val.c_str(), "%i", &tmp ) == 1 && tmp != 0 )
                    {
                        hasSky = true;
                    }
                }
                else if( key == "_light" )
                {
                    float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f, s = 255.0f;
                    int   n = std::sscanf( val.c_str(), "%f %f %f %f", &c0, &c1, &c2, &s );
                    if( n == 1 )
                    {
                        rgb[ 0 ] = rgb[ 1 ] = rgb[ 2 ] = c0;
                        scale                          = 255.0f;
                        hasColor                       = true;
                    }
                    else if( n == 3 )
                    {
                        rgb[ 0 ] = c0;
                        rgb[ 1 ] = c1;
                        rgb[ 2 ] = c2;
                        scale    = 255.0f;
                        hasColor = true;
                    }
                    else if( n == 4 )
                    {
                        rgb[ 0 ] = c0;
                        rgb[ 1 ] = c1;
                        rgb[ 2 ] = c2;
                        scale    = s;
                        hasColor = true;
                    }
                }
                else if( key == "pitch" )
                {
                    if( std::sscanf( val.c_str(), "%f", &pitchVal ) == 1 )
                    {
                        hasPitch = true;
                    }
                }
                else if( key == "angle" )
                {
                    if( std::sscanf( val.c_str(), "%f", &angleVal ) == 1 )
                    {
                        hasAngle = true;
                    }
                }
                else if( key == "angles" )
                {
                    if( std::sscanf( val.c_str(), "%f %f %f", &angles[ 0 ], &angles[ 1 ], &angles[ 2 ] ) == 3 )
                    {
                        hasAngles = true;
                    }
                }
            }

            if( !( isEnv || hasSky ) || !hasColor )
            {
                continue;
            }

            float pitch = -45.0f;
            float yaw   = 0.0f;
            if( hasAngles )
            {
                pitch = -angles[ 0 ];
                yaw   = angles[ 1 ];
            }
            if( hasAngle )
            {
                if( angleVal == -2.0f )
                {
                    pitch = -90.0f;
                }
                else if( angleVal != -1.0f )
                {
                    yaw = angleVal;
                }
            }
            if( hasPitch )
            {
                pitch = pitchVal;
            }
            if( pitch > 0.0f )
            {
                pitch = -pitch;
            }
            if( pitch > -5.0f )
            {
                pitch = -5.0f;
            }

            float r = std::pow( std::clamp( rgb[ 0 ] / 255.0f, 0.0f, 1.0f ), 2.2f );
            float g = std::pow( std::clamp( rgb[ 1 ] / 255.0f, 0.0f, 1.0f ), 2.2f );
            float b = std::pow( std::clamp( rgb[ 2 ] / 255.0f, 0.0f, 1.0f ), 2.2f );
            float sunIntensity =
                scale <= 255.0f ? ( scale / 255.0f ) : ( 1.0f + std::log2( scale / 255.0f ) * 0.25f );
            sunIntensity = std::clamp( sunIntensity, 0.05f, 2.0f );
            float score  = sunIntensity * std::max( { r, g, b } );

            if( !out.bspSunLight.has_value() || ( isEnv && !bestIsEnv ) ||
                ( isEnv == bestIsEnv && score > bestScore ) )
            {
                bestIsEnv = isEnv;
                bestScore = score;

                constexpr float DEG2RAD = 3.14159265358979323846f / 180.0f;
                float           pr      = -pitch * DEG2RAD;
                float           yr      = yaw * DEG2RAD;
                float           cp      = std::cos( pr );
                Vec3            dir     = Normalize( { cp * std::cos( yr ), cp * std::sin( yr ), -std::sin( pr ) } );

                out.bspSunLight = RgDirectionalLightUploadInfo{
                    .uniqueID               = 0,
                    .isExportable           = true,
                    .extra                  = {},
                    .color                  = Utils::PackColorFromFloat( r, g, b, 1.0f ),
                    .intensity              = sunIntensity * 7000.0f,
                    .direction              = ToRg( dir ),
                    .angularDiameterDegrees = 0.5f,
                };
            }
        }
    }

    const BspLump& texLump = hdr.lumps[ LUMP_TEXTURES ];
    if( texLump.fileofs >= 0 && texLump.filelen >= int32_t( sizeof( int32_t ) ) &&
        size_t( texLump.fileofs ) + size_t( texLump.filelen ) <= buf.size() )
    {
        int32_t numMiptex = 0;
        std::memcpy( &numMiptex, buf.data() + texLump.fileofs, sizeof( numMiptex ) );
        if( numMiptex > 0 && numMiptex < 16384 &&
            size_t( 4 + numMiptex * 4 ) <= size_t( texLump.filelen ) )
        {
            out.textureNames.resize( size_t( numMiptex ) );
            for( int32_t i = 0; i < numMiptex; i++ )
            {
                int32_t ofs = -1;
                std::memcpy( &ofs, buf.data() + texLump.fileofs + 4 + i * 4, sizeof( ofs ) );
                if( ofs >= 0 && size_t( ofs ) + sizeof( BspMiptex ) <= size_t( texLump.filelen ) )
                {
                    BspMiptex mt{};
                    std::memcpy( &mt, buf.data() + texLump.fileofs + ofs, sizeof( mt ) );
                    mt.name[ 15 ]              = '\0';
                    out.textureNames[ i ] = mt.name;
                }
            }
        }
    }

    const BspLump& lightLump = hdr.lumps[ LUMP_LIGHTING ];
    if( lightLump.fileofs < 0 || lightLump.filelen <= 0 ||
        size_t( lightLump.fileofs ) + size_t( lightLump.filelen ) > buf.size() )
    {
        return false;
    }

    out.lightmapSamples = ( hdr.version == BSP_VERSION_HL1 ) ? 3 : 1;
    out.lightdata.resize( size_t( lightLump.filelen ) );
    std::memcpy( out.lightdata.data(), buf.data() + lightLump.fileofs, size_t( lightLump.filelen ) );

    if( out.lightmapSamples == 1 )
    {
        std::filesystem::path litPath = bspPath;
        litPath.replace_extension( ".lit" );
        std::ifstream litFile( litPath, std::ios::binary | std::ios::ate );
        if( litFile.is_open() )
        {
            auto litSz = litFile.tellg();
            if( litSz == std::streamoff( 8 + out.lightdata.size() * 3 ) )
            {
                char hdrLit[ 8 ]{};
                litFile.seekg( 0 );
                litFile.read( hdrLit, 8 );
                int32_t litVer = 0;
                std::memcpy( &litVer, hdrLit + 4, 4 );
                if( std::memcmp( hdrLit, "QLIT", 4 ) == 0 && litVer == 1 )
                {
                    out.lightdata.resize( out.lightdata.size() * 3 );
                    litFile.read( reinterpret_cast< char* >( out.lightdata.data() ),
                                  std::streamsize( out.lightdata.size() ) );
                    out.lightmapSamples = 3;
                }
            }
        }
    }

    return !out.faces.empty() && !out.lightdata.empty();
}

bool IsTextureEmissiveOrSky( const std::string&        texName,
                             const TextureMetaManager& textureMeta,
                             bool&                     outIsSky )
{
    std::string lower = ToLowerAscii( texName );
    outIsSky          = lower.starts_with( "sky" );
    if( outIsSky )
    {
        return true;
    }

    if( lower.empty() || lower == "clip" || lower == "aaatrigger" || lower == "hint" ||
        lower == "skip" || lower == "null" || lower.starts_with( "!" ) ||
        lower.starts_with( "*" ) )
    {
        return true;
    }

    if( lower.find( '~' ) != std::string::npos )
    {
        return true;
    }

    const std::array< std::string, 4 > candidates = {
        std::string( "#maps/" ) + lower + ".mip",
        std::string( "#maps/" ) + texName + ".mip",
        lower,
        texName,
    };

    for( const auto& cand : candidates )
    {
        if( auto meta = textureMeta.Access( cand.c_str() ) )
        {
            if( meta->attachedLightIntensity > 0.0f || meta->emissiveMult > 0.0f ||
                meta->forceIgnore )
            {
                return true;
            }
        }
    }

    return false;
}

struct LuxelSample
{
    Vec3    pos;
    Vec3    normal;
    Vec3    origRgb;
    Vec3    resRgb;
    float   areaWeight;
    uint8_t style;
    int     faceIndex;
    bool    skyVisible{ false };
};

float MaxComp( const Vec3& v )
{
    return std::max( { v.x, v.y, v.z } );
}

float Luma( const Vec3& v )
{
    return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
}

float AttenuationKernel( const Vec3& lightPos, const Vec3& surfPos, const Vec3& surfNormal )
{
    Vec3  toLight = lightPos - surfPos;
    float distSq  = LengthSq( toLight );
    if( distSq < 1e-4f )
    {
        return 0.0f;
    }
    float dist  = std::sqrt( distSq );
    float nDotL = Dot( surfNormal, toLight / dist );
    if( nDotL < 0.15f )
    {
        return 0.0f;
    }
    return nDotL / ( distSq + 64.0f );
}

float SpotFalloff( const Vec3& lightDir, const Vec3& lightToSurfDir, float cosInner, float cosOuter )
{
    float cosA = Dot( lightDir, lightToSurfDir );
    if( cosA <= cosOuter )
    {
        return 0.0f;
    }
    if( cosA >= cosInner )
    {
        return 1.0f;
    }
    float t = ( cosA - cosOuter ) / std::max( 1e-5f, cosInner - cosOuter );
    return t * t;
}

struct FitEval
{
    float loss{ std::numeric_limits< float >::max() };
    Vec3  flux{ 0.0f, 0.0f, 0.0f };
    int   supportCount{ 0 };
};

FitEval EvaluatePointCandidate( const BspData&                    bsp,
                                const Vec3&                       candidatePos,
                                const std::vector< LuxelSample >& samples,
                                const std::vector< int >&         nearbyIndices )
{
    FitEval res{};
    if( bsp.PointContents( candidatePos ) == CONTENTS_SOLID )
    {
        return res;
    }

    struct VisItem
    {
        int   idx;
        float g;
    };
    std::vector< VisItem > visible;
    visible.reserve( nearbyIndices.size() );

    double sumWg2 = 0.0;
    double sumWgR = 0.0;
    double sumWgG = 0.0;
    double sumWgB = 0.0;

    for( int idx : nearbyIndices )
    {
        const LuxelSample& s = samples[ idx ];
        float              g = AttenuationKernel( candidatePos, s.pos, s.normal );
        if( g <= 1e-8f )
        {
            continue;
        }
        Vec3 pOffset = s.pos + s.normal * 0.5f;
        if( !bsp.HasLineOfSight( candidatePos, pOffset ) )
        {
            continue;
        }
        visible.push_back( { idx, g } );

        double w   = double( s.areaWeight );
        double wg  = w * double( g );
        double wg2 = wg * double( g );
        sumWg2 += wg2;
        sumWgR += wg * double( s.resRgb.x );
        sumWgG += wg * double( s.resRgb.y );
        sumWgB += wg * double( s.resRgb.z );
    }

    if( visible.size() < 3 || sumWg2 <= 1e-12 )
    {
        return res;
    }

    Vec3 flux = {
        float( std::max( 0.0, sumWgR / sumWg2 ) ),
        float( std::max( 0.0, sumWgG / sumWg2 ) ),
        float( std::max( 0.0, sumWgB / sumWg2 ) ),
    };
    if( MaxComp( flux ) <= 1e-4f )
    {
        return res;
    }

    double loss = 0.0;
    for( const VisItem& item : visible )
    {
        const LuxelSample& s    = samples[ item.idx ];
        Vec3               pred = flux * item.g;
        Vec3               diff = s.resRgb - pred;
        Vec3               over = {
            std::max( 0.0f, pred.x - s.resRgb.x ),
            std::max( 0.0f, pred.y - s.resRgb.y ),
            std::max( 0.0f, pred.z - s.resRgb.z ),
        };
        loss += double( s.areaWeight ) *
                ( double( LengthSq( diff ) ) + 3.0 * double( LengthSq( over ) ) );
    }

    for( int idx : nearbyIndices )
    {
        const LuxelSample& s = samples[ idx ];
        loss += double( s.areaWeight ) * double( LengthSq( s.resRgb ) ) * 0.05;
    }
    for( const VisItem& item : visible )
    {
        const LuxelSample& s = samples[ item.idx ];
        loss -= double( s.areaWeight ) * double( LengthSq( s.resRgb ) ) * 0.05;
    }

    res.loss         = float( loss );
    res.flux         = flux;
    res.supportCount = int( visible.size() );
    return res;
}

struct SpotEval
{
    bool  useSpot{ false };
    Vec3  direction{ 0.0f, 0.0f, -1.0f };
    float angleInnerRad{ 0.6f };
    float angleOuterRad{ 1.1f };
    Vec3  flux{ 0.0f, 0.0f, 0.0f };
    float loss{ std::numeric_limits< float >::max() };
};

SpotEval EvaluateSpotUpgrade( const BspData&                    bsp,
                              const Vec3&                       lightPos,
                              const FitEval&                    pointFit,
                              const std::vector< LuxelSample >& samples,
                              const std::vector< int >&         nearbyIndices )
{
    SpotEval spot{};
    Vec3     weightedDir{ 0.0f, 0.0f, 0.0f };
    double   totalW = 0.0;

    struct RaySample
    {
        int   idx;
        float g;
        Vec3  dirFromLight;
        float wLuma;
    };
    std::vector< RaySample > rays;
    rays.reserve( nearbyIndices.size() );

    for( int idx : nearbyIndices )
    {
        const LuxelSample& s = samples[ idx ];
        float              g = AttenuationKernel( lightPos, s.pos, s.normal );
        if( g <= 1e-8f )
        {
            continue;
        }
        if( !bsp.HasLineOfSight( lightPos, s.pos + s.normal * 0.5f ) )
        {
            continue;
        }
        Vec3  dir   = Normalize( s.pos - lightPos );
        float luma  = Luma( s.resRgb );
        float wLuma = luma * s.areaWeight;
        weightedDir = weightedDir + dir * wLuma;
        totalW += double( wLuma );
        rays.push_back( { idx, g, dir, wLuma } );
    }

    if( rays.size() < 6 || LengthSq( weightedDir ) < 1e-6f || totalW <= 1e-6 )
    {
        return spot;
    }

    Vec3 meanDir = Normalize( weightedDir );

    std::vector< std::pair< float, float > > angleWeights;
    angleWeights.reserve( rays.size() );
    for( const RaySample& r : rays )
    {
        float cosA  = std::clamp( Dot( meanDir, r.dirFromLight ), -1.0f, 1.0f );
        float angle = std::acos( cosA );
        angleWeights.emplace_back( angle, r.wLuma );
    }
    std::sort( angleWeights.begin(), angleWeights.end(), []( const auto& a, const auto& b ) {
        return a.first < b.first;
    } );

    float innerAngle = 0.45f;
    float outerAngle = 0.95f;
    double accum     = 0.0;
    for( const auto& [ ang, w ] : angleWeights )
    {
        accum += double( w );
        if( accum >= totalW * 0.55 && innerAngle == 0.45f )
        {
            innerAngle = ang;
        }
        if( accum >= totalW * 0.92 )
        {
            outerAngle = ang;
            break;
        }
    }

    outerAngle = std::clamp( outerAngle, Utils::DegToRad( 15.0f ), Utils::DegToRad( 82.0f ) );
    innerAngle = std::clamp( innerAngle, Utils::DegToRad( 5.0f ), outerAngle - Utils::DegToRad( 5.0f ) );
    if( outerAngle > Utils::DegToRad( 78.0f ) )
    {
        return spot;
    }

    float cosInner = std::cos( innerAngle );
    float cosOuter = std::cos( outerAngle );

    double sumWg2 = 0.0;
    double sumWgR = 0.0;
    double sumWgG = 0.0;
    double sumWgB = 0.0;
    for( const RaySample& r : rays )
    {
        const LuxelSample& s  = samples[ r.idx ];
        float              sf = SpotFalloff( meanDir, r.dirFromLight, cosInner, cosOuter );
        float              gs = r.g * sf;
        double             wg = double( s.areaWeight ) * double( gs );
        sumWg2 += wg * double( gs );
        sumWgR += wg * double( s.resRgb.x );
        sumWgG += wg * double( s.resRgb.y );
        sumWgB += wg * double( s.resRgb.z );
    }

    if( sumWg2 <= 1e-12 )
    {
        return spot;
    }

    Vec3 flux = {
        float( std::max( 0.0, sumWgR / sumWg2 ) ),
        float( std::max( 0.0, sumWgG / sumWg2 ) ),
        float( std::max( 0.0, sumWgB / sumWg2 ) ),
    };
    if( MaxComp( flux ) <= 1e-4f )
    {
        return spot;
    }

    double spotLoss  = 0.0;
    double pointLoss = 0.0;
    for( const RaySample& r : rays )
    {
        const LuxelSample& s = samples[ r.idx ];
        {
            float sf   = SpotFalloff( meanDir, r.dirFromLight, cosInner, cosOuter );
            Vec3  pred = flux * ( r.g * sf );
            Vec3  diff = s.resRgb - pred;
            Vec3  over = {
                std::max( 0.0f, pred.x - s.resRgb.x ),
                std::max( 0.0f, pred.y - s.resRgb.y ),
                std::max( 0.0f, pred.z - s.resRgb.z ),
            };
            spotLoss += double( s.areaWeight ) *
                        ( double( LengthSq( diff ) ) + 3.0 * double( LengthSq( over ) ) );
        }
        {
            Vec3 pred = pointFit.flux * r.g;
            Vec3 diff = s.resRgb - pred;
            Vec3 over = {
                std::max( 0.0f, pred.x - s.resRgb.x ),
                std::max( 0.0f, pred.y - s.resRgb.y ),
                std::max( 0.0f, pred.z - s.resRgb.z ),
            };
            pointLoss += double( s.areaWeight ) *
                         ( double( LengthSq( diff ) ) + 3.0 * double( LengthSq( over ) ) );
        }
    }

    if( spotLoss < pointLoss * 0.70 )
    {
        spot.useSpot       = true;
        spot.direction     = meanDir;
        spot.angleInnerRad = innerAngle;
        spot.angleOuterRad = outerAngle;
        spot.flux          = flux;
        spot.loss          = float( spotLoss );
    }

    return spot;
}

float MetricLumensFromBspFlux( float maxFluxChannel, float oneGameUnitInMeters )
{
    float nonMetric = ( maxFluxChannel / 9500.0f ) * 1000.0f;
    return Utils::IntensityFromNonMetric( nonMetric, oneGameUnitInMeters );
}

float BspFluxFromMetricLumens( float metricLumens, float oneGameUnitInMeters )
{
    float nonMetric = Utils::IntensityToNonMetric( metricLumens, oneGameUnitInMeters );
    return ( nonMetric / 1000.0f ) * 9500.0f;
}

}
}

std::vector< RTGL1::GenericLight > RTGL1::BspLightEstimator::EstimateLights(
    const std::filesystem::path&       bspPath,
    std::string_view                   mapName,
    const std::filesystem::path&       scenesFolder,
    const TextureMetaManager&          textureMeta,
    const std::vector< GenericLight >& existingLights,
    float                              oneGameUnitInMeters )
{
    std::vector< GenericLight > estimated;

    auto foundPath = FindBspFile( bspPath, mapName, scenesFolder );
    if( !foundPath )
    {
        return estimated;
    }

    BspData bsp;
    if( !LoadBspFromFile( *foundPath, bsp ) )
    {
        debug::Warning( "BspLightEstimator: failed to load {}", foundPath->string() );
        return estimated;
    }

    if( bsp.hasAuthoredBspLights )
    {
        return estimated;
    }

    int firstWorldFace = 0;
    int numWorldFaces  = int( bsp.faces.size() );
    if( !bsp.models.empty() )
    {
        firstWorldFace = std::max( 0, bsp.models[ 0 ].firstface );
        numWorldFaces  = std::min( int( bsp.faces.size() ) - firstWorldFace, bsp.models[ 0 ].numfaces );
    }

    std::vector< LuxelSample > samples;
    samples.reserve( 32768 );

    bool hasSkyFaces = false;

    for( int fIdx = firstWorldFace; fIdx < firstWorldFace + numWorldFaces; fIdx++ )
    {
        const BspFaceUnified& face = bsp.faces[ fIdx ];
        if( face.numedges < 3 || face.texinfo < 0 || size_t( face.texinfo ) >= bsp.texinfos.size() ||
            face.planenum < 0 || size_t( face.planenum ) >= bsp.planes.size() )
        {
            continue;
        }

        const BspTexInfo& ti = bsp.texinfos[ face.texinfo ];
        std::string       texName;
        if( ti.miptex >= 0 && size_t( ti.miptex ) < bsp.textureNames.size() )
        {
            texName = bsp.textureNames[ ti.miptex ];
        }

        bool isSky = false;
        if( IsTextureEmissiveOrSky( texName, textureMeta, isSky ) )
        {
            if( isSky )
            {
                hasSkyFaces = true;
            }
            continue;
        }

        if( face.lightofs < 0 || ( ti.flags & 1 ) != 0 )
        {
            continue;
        }

        const BspPlane& pl = bsp.planes[ face.planenum ];
        Vec3            planeN{ pl.normal[ 0 ], pl.normal[ 1 ], pl.normal[ 2 ] };
        Vec3            faceN = face.side ? -planeN : planeN;

        std::vector< Vec3 > poly;
        poly.reserve( size_t( face.numedges ) );
        float minS = std::numeric_limits< float >::max();
        float minT = std::numeric_limits< float >::max();
        float maxS = -std::numeric_limits< float >::max();
        float maxT = -std::numeric_limits< float >::max();

        for( int e = 0; e < face.numedges; e++ )
        {
            int seIdx = face.firstedge + e;
            if( seIdx < 0 || size_t( seIdx ) >= bsp.surfedges.size() )
            {
                poly.clear();
                break;
            }
            int32_t edgeRef = bsp.surfedges[ seIdx ];
            uint32_t vIdx   = 0;
            if( edgeRef >= 0 )
            {
                if( size_t( edgeRef ) >= bsp.edges.size() )
                {
                    poly.clear();
                    break;
                }
                vIdx = bsp.edges[ edgeRef ].v[ 0 ];
            }
            else
            {
                if( size_t( -edgeRef ) >= bsp.edges.size() )
                {
                    poly.clear();
                    break;
                }
                vIdx = bsp.edges[ -edgeRef ].v[ 1 ];
            }
            if( size_t( vIdx ) >= bsp.vertices.size() )
            {
                poly.clear();
                break;
            }
            const Vec3& p = bsp.vertices[ vIdx ];
            poly.push_back( p );

            float s = p.x * ti.vecs[ 0 ][ 0 ] + p.y * ti.vecs[ 0 ][ 1 ] + p.z * ti.vecs[ 0 ][ 2 ] +
                      ti.vecs[ 0 ][ 3 ];
            float t = p.x * ti.vecs[ 1 ][ 0 ] + p.y * ti.vecs[ 1 ][ 1 ] + p.z * ti.vecs[ 1 ][ 2 ] +
                      ti.vecs[ 1 ][ 3 ];
            minS = std::min( minS, s );
            minT = std::min( minT, t );
            maxS = std::max( maxS, s );
            maxT = std::max( maxT, t );
        }

        if( poly.size() < 3 )
        {
            continue;
        }

        int bMinS  = int( std::floor( minS / 16.0f ) );
        int bMinT  = int( std::floor( minT / 16.0f ) );
        int bMaxS  = int( std::ceil( maxS / 16.0f ) );
        int bMaxT  = int( std::ceil( maxT / 16.0f ) );
        int smax   = bMaxS - bMinS + 1;
        int tmax   = bMaxT - bMinT + 1;
        if( smax <= 0 || tmax <= 0 || smax > 256 || tmax > 256 )
        {
            continue;
        }

        Vec3 vecS{ ti.vecs[ 0 ][ 0 ], ti.vecs[ 0 ][ 1 ], ti.vecs[ 0 ][ 2 ] };
        Vec3 vecT{ ti.vecs[ 1 ][ 0 ], ti.vecs[ 1 ][ 1 ], ti.vecs[ 1 ][ 2 ] };

        Vec3 dPds{};
        Vec3 dPdt{};
        if( !SolveLinear3x3( vecS, vecT, planeN, { 1.0f, 0.0f, 0.0f }, dPds ) ||
            !SolveLinear3x3( vecS, vecT, planeN, { 0.0f, 1.0f, 0.0f }, dPdt ) )
        {
            continue;
        }
        float luxelArea = Length( Cross( dPds * 16.0f, dPdt * 16.0f ) );
        float areaW     = std::clamp( luxelArea / 256.0f, 0.1f, 8.0f );

        int step = ( smax * tmax > 1024 ) ? 2 : 1;

        for( int m = 0; m < 4; m++ )
        {
            if( face.styles[ m ] == 0xFF )
            {
                break;
            }
            size_t styleOffset =
                size_t( face.lightofs ) + size_t( m ) * size_t( smax * tmax * bsp.lightmapSamples );
            size_t styleBytes = size_t( smax * tmax * bsp.lightmapSamples );
            if( styleOffset + styleBytes > bsp.lightdata.size() )
            {
                break;
            }

            for( int t = 0; t < tmax; t += step )
            {
                for( int s = 0; s < smax; s += step )
                {
                    float uTarget = float( bMinS + s ) * 16.0f;
                    float vTarget = float( bMinT + t ) * 16.0f;
                    Vec3  worldP{};
                    if( !SolveLinear3x3(
                            vecS,
                            vecT,
                            planeN,
                            { uTarget - ti.vecs[ 0 ][ 3 ], vTarget - ti.vecs[ 1 ][ 3 ], pl.dist },
                            worldP ) )
                    {
                        continue;
                    }

                    if( !PointInConvexPolygon( worldP, faceN, poly, 12.0f ) )
                    {
                        continue;
                    }

                    size_t pxIdx = size_t( t * smax + s ) * size_t( bsp.lightmapSamples );
                    float  r = 0.0f, g = 0.0f, b = 0.0f;
                    if( bsp.lightmapSamples == 3 )
                    {
                        r = float( bsp.lightdata[ styleOffset + pxIdx + 0 ] ) / 255.0f;
                        g = float( bsp.lightdata[ styleOffset + pxIdx + 1 ] ) / 255.0f;
                        b = float( bsp.lightdata[ styleOffset + pxIdx + 2 ] ) / 255.0f;
                    }
                    else
                    {
                        r = g = b = float( bsp.lightdata[ styleOffset + pxIdx ] ) / 255.0f;
                    }

                    Vec3 linRgb = {
                        std::pow( std::clamp( r, 0.0f, 1.0f ), 2.2f ),
                        std::pow( std::clamp( g, 0.0f, 1.0f ), 2.2f ),
                        std::pow( std::clamp( b, 0.0f, 1.0f ), 2.2f ),
                    };

                    samples.push_back( LuxelSample{
                        .pos        = worldP,
                        .normal     = faceN,
                        .origRgb    = linRgb,
                        .resRgb     = linRgb,
                        .areaWeight = areaW * float( step * step ),
                        .style      = face.styles[ m ],
                        .faceIndex  = fIdx,
                    } );
                }
            }
        }
    }

    if( samples.empty() )
    {
        return estimated;
    }

    bool hasExistingDirectional = false;
    for( const GenericLight& gl : existingLights )
    {
        if( std::holds_alternative< RgDirectionalLightUploadInfo >( gl ) )
        {
            hasExistingDirectional = true;
            continue;
        }

        Vec3  lPos{};
        Vec3  lDir{};
        Vec3  lColor{};
        float lFluxQu  = 0.0f;
        bool  isSpot   = false;
        float cosInner = 1.0f;
        float cosOuter = -1.0f;
        int   lStyle   = 0;

        if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
        {
            lPos    = FromRg( sph->position );
            auto c  = Utils::UnpackColor4DPacked32< RgFloat3D >( sph->color );
            lColor  = FromRg( c );
            lFluxQu = BspFluxFromMetricLumens( sph->intensity, oneGameUnitInMeters );
            lStyle  = sph->extra.exists ? sph->extra.lightstyle : 0;
        }
        else if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &gl ) )
        {
            lPos     = FromRg( sp->position );
            lDir     = Normalize( FromRg( sp->direction ) );
            auto c   = Utils::UnpackColor4DPacked32< RgFloat3D >( sp->color );
            lColor   = FromRg( c );
            lFluxQu  = BspFluxFromMetricLumens( sp->intensity, oneGameUnitInMeters );
            isSpot   = true;
            cosInner = std::cos( sp->angleInner );
            cosOuter = std::cos( sp->angleOuter );
            lStyle   = sp->extra.exists ? sp->extra.lightstyle : 0;
        }
        else
        {
            continue;
        }

        if( lFluxQu <= 0.0f )
        {
            continue;
        }

        for( LuxelSample& s : samples )
        {
            if( s.style != uint8_t( lStyle ) )
            {
                continue;
            }
            if( LengthSq( s.pos - lPos ) > 512.0f * 512.0f )
            {
                continue;
            }
            float g = AttenuationKernel( lPos, s.pos, s.normal );
            if( g <= 0.0f )
            {
                continue;
            }
            if( isSpot )
            {
                g *= SpotFalloff( lDir, Normalize( s.pos - lPos ), cosInner, cosOuter );
                if( g <= 0.0f )
                {
                    continue;
                }
            }
            if( !bsp.HasLineOfSight( lPos, s.pos + s.normal * 0.5f ) )
            {
                continue;
            }
            Vec3 sub = lColor * ( lFluxQu * g );
            s.resRgb = {
                std::max( 0.0f, s.resRgb.x - sub.x ),
                std::max( 0.0f, s.resRgb.y - sub.y ),
                std::max( 0.0f, s.resRgb.z - sub.z ),
            };
        }
    }

    if( hasSkyFaces && !hasExistingDirectional && bsp.bspSunLight.has_value() )
    {
        estimated.push_back( *bsp.bspSunLight );
        hasExistingDirectional = true;
    }

    if( hasSkyFaces && !hasExistingDirectional )
    {
        std::vector< int > skySampleIndices;
        size_t             stride = std::max< size_t >( 1, samples.size() / 2048 );
        for( size_t i = 0; i < samples.size(); i += stride )
        {
            if( samples[ i ].style != 0 )
            {
                continue;
            }
            if( bsp.TraceHitsSky( samples[ i ].pos + samples[ i ].normal * 2.0f,
                                  samples[ i ].normal,
                                  16384.0f ) )
            {
                samples[ i ].skyVisible = true;
                skySampleIndices.push_back( int( i ) );
            }
        }

        if( skySampleIndices.size() >= 12 )
        {
            constexpr int   NUM_SUN_DIRS = 48;
            constexpr float GOLDEN_ANGLE = 2.39996323f;

            Vec3  bestSunDir{ 0.3f, 0.3f, -0.9f };
            Vec3  bestSunRgb{ 0.0f, 0.0f, 0.0f };
            float bestScore = std::numeric_limits< float >::max();
            float baseLoss  = 0.0f;

            Vec3 meanRgb{ 0.0f, 0.0f, 0.0f };
            for( int idx : skySampleIndices )
            {
                meanRgb = meanRgb + samples[ idx ].resRgb;
            }
            meanRgb = meanRgb / float( skySampleIndices.size() );
            for( int idx : skySampleIndices )
            {
                baseLoss += LengthSq( samples[ idx ].resRgb - meanRgb );
            }

            for( int k = 0; k < NUM_SUN_DIRS; k++ )
            {
                float z     = ( float( k ) + 0.5f ) / float( NUM_SUN_DIRS );
                float r     = std::sqrt( std::max( 0.0f, 1.0f - z * z ) );
                float phi   = GOLDEN_ANGLE * float( k );
                Vec3  toSun = { r * std::cos( phi ), r * std::sin( phi ), z };
                if( toSun.z < 0.15f )
                {
                    continue;
                }

                double sumG2 = 0.0, sumGR = 0.0, sumGG = 0.0, sumGB = 0.0;
                double sumR = 0.0, sumG = 0.0, sumB = 0.0;
                double cnt = 0.0;
                std::vector< float > gs( skySampleIndices.size(), 0.0f );

                for( size_t j = 0; j < skySampleIndices.size(); j++ )
                {
                    const LuxelSample& s   = samples[ skySampleIndices[ j ] ];
                    float              ndl = Dot( s.normal, toSun );
                    float              vis = ( ndl > 0.05f &&
                                  bsp.TraceHitsSky( s.pos + s.normal * 2.0f, toSun, 16384.0f ) )
                                                 ? ndl
                                                 : 0.0f;
                    gs[ j ] = vis;
                    sumG2 += double( vis * vis );
                    sumGR += double( vis * s.resRgb.x );
                    sumGG += double( vis * s.resRgb.y );
                    sumGB += double( vis * s.resRgb.z );
                    sumR += double( s.resRgb.x );
                    sumG += double( s.resRgb.y );
                    sumB += double( s.resRgb.z );
                    cnt += 1.0;
                }

                if( sumG2 < 0.25 )
                {
                    continue;
                }

                Vec3  amb{ float( sumR / cnt ) * 0.25f, float( sumG / cnt ) * 0.25f, float( sumB / cnt ) * 0.25f };
                Vec3  sun{
                    float( std::max( 0.0, ( sumGR - double( amb.x ) * std::sqrt( sumG2 ) ) / sumG2 ) ),
                    float( std::max( 0.0, ( sumGG - double( amb.y ) * std::sqrt( sumG2 ) ) / sumG2 ) ),
                    float( std::max( 0.0, ( sumGB - double( amb.z ) * std::sqrt( sumG2 ) ) / sumG2 ) ),
                };
                float loss = 0.0f;
                for( size_t j = 0; j < skySampleIndices.size(); j++ )
                {
                    Vec3 pred = amb + sun * gs[ j ];
                    loss += LengthSq( samples[ skySampleIndices[ j ] ].resRgb - pred );
                }
                if( loss < bestScore )
                {
                    bestScore  = loss;
                    bestSunDir = -toSun;
                    bestSunRgb = sun;
                }
            }

            if( baseLoss > 1e-4f && bestScore < baseLoss * 0.72f && MaxComp( bestSunRgb ) > 0.08f )
            {
                float maxSunC = MaxComp( bestSunRgb );
                Vec3  normC   = bestSunRgb / maxSunC;
                estimated.push_back( RgDirectionalLightUploadInfo{
                    .uniqueID               = 0,
                    .isExportable           = true,
                    .extra                  = {},
                    .color                  = Utils::PackColorFromFloat( normC.x, normC.y, normC.z, 1.0f ),
                    .intensity              = std::clamp( maxSunC * 7000.0f, 1500.0f, 12000.0f ),
                    .direction              = ToRg( bestSunDir ),
                    .angularDiameterDegrees = 0.5f,
                } );

                Vec3 toSun = -bestSunDir;
                for( LuxelSample& s : samples )
                {
                    if( s.style != 0 )
                    {
                        continue;
                    }
                    float ndl = Dot( s.normal, toSun );
                    if( ndl > 0.05f &&
                        bsp.TraceHitsSky( s.pos + s.normal * 2.0f, toSun, 16384.0f ) )
                    {
                        Vec3 sub = bestSunRgb * ndl;
                        s.resRgb = {
                            std::max( 0.0f, s.resRgb.x - sub.x ),
                            std::max( 0.0f, s.resRgb.y - sub.y ),
                            std::max( 0.0f, s.resRgb.z - sub.z ),
                        };
                    }
                }
            }
        }
    }

    std::vector< uint8_t > activeStyles;
    for( const LuxelSample& s : samples )
    {
        if( std::find( activeStyles.begin(), activeStyles.end(), s.style ) == activeStyles.end() )
        {
            activeStyles.push_back( s.style );
        }
    }

    constexpr int   MAX_ESTIMATED_LIGHTS = 768;
    constexpr float SEARCH_RADIUS        = 320.0f;

    std::vector< uint8_t > visited( samples.size(), 0 );
    float existingClearSq = ( 1.5f / oneGameUnitInMeters ) * ( 1.5f / oneGameUnitInMeters );
    for( const GenericLight& gl : existingLights )
    {
        Vec3 ePos{};
        if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
        {
            ePos = FromRg( sph->position );
        }
        else if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &gl ) )
        {
            ePos = FromRg( sp->position );
        }
        else
        {
            continue;
        }
        for( size_t i = 0; i < samples.size(); i++ )
        {
            if( LengthSq( samples[ i ].pos - ePos ) < existingClearSq )
            {
                visited[ i ] = 1;
            }
        }
    }

    for( uint8_t style : activeStyles )
    {
        float ambientFloor = 0.05f;
        if( style == 0 )
        {
            std::vector< float > lumas;
            lumas.reserve( samples.size() / 4 + 1 );
            for( size_t i = 0; i < samples.size(); i += 4 )
            {
                if( samples[ i ].style == 0 )
                {
                    lumas.push_back( Luma( samples[ i ].resRgb ) );
                }
            }
            if( !lumas.empty() )
            {
                std::nth_element(
                    lumas.begin(), lumas.begin() + lumas.size() / 4, lumas.end() );
                ambientFloor = std::clamp( lumas[ lumas.size() / 4 ] * 1.4f, 0.04f, 0.25f );
            }
        }
        else
        {
            ambientFloor = 0.025f;
        }

        while( int( estimated.size() ) < MAX_ESTIMATED_LIGHTS )
        {
            int   peakIdx  = -1;
            float peakLuma = ambientFloor;
            for( size_t i = 0; i < samples.size(); i++ )
            {
                if( visited[ i ] || samples[ i ].style != style )
                {
                    continue;
                }
                float l = MaxComp( samples[ i ].resRgb );
                if( l > peakLuma )
                {
                    peakLuma = l;
                    peakIdx  = int( i );
                }
            }

            if( peakIdx < 0 )
            {
                break;
            }

            visited[ peakIdx ] = 1;
            const LuxelSample& peak = samples[ peakIdx ];

            std::vector< int > nearbyIndices;
            nearbyIndices.reserve( 256 );
            float rHalfSum = 0.0f;
            int   rHalfCnt = 0;

            for( size_t i = 0; i < samples.size(); i++ )
            {
                if( samples[ i ].style != style )
                {
                    continue;
                }
                Vec3  delta  = samples[ i ].pos - peak.pos;
                float distSq = LengthSq( delta );
                if( distSq <= SEARCH_RADIUS * SEARCH_RADIUS )
                {
                    nearbyIndices.push_back( int( i ) );
                    if( samples[ i ].faceIndex == peak.faceIndex )
                    {
                        float l = MaxComp( samples[ i ].resRgb );
                        if( l >= peakLuma * 0.35f && l <= peakLuma * 0.65f && distSq > 64.0f )
                        {
                            rHalfSum += std::sqrt( distSq );
                            rHalfCnt++;
                        }
                    }
                }
            }

            if( nearbyIndices.size() < 3 )
            {
                continue;
            }

            float estDist = ( rHalfCnt > 0 ) ? std::clamp( ( rHalfSum / float( rHalfCnt ) ) * 1.305f, 12.0f, 144.0f )
                                             : 32.0f;

            std::vector< Vec3 > candidates;
            const std::array< float, 5 > scales = { estDist, 16.0f, 28.0f, 48.0f, 80.0f };
            for( float d : scales )
            {
                candidates.push_back( peak.pos + peak.normal * d );
            }

            Vec3  convergingNormal{ 0.0f, 0.0f, 0.0f };
            int   otherFaces = 0;
            for( int idx : nearbyIndices )
            {
                const LuxelSample& s = samples[ idx ];
                if( s.faceIndex != peak.faceIndex && Dot( s.normal, peak.normal ) < 0.5f &&
                    MaxComp( s.resRgb ) > peakLuma * 0.4f )
                {
                    Vec3 c = s.pos + s.normal * 24.0f;
                    if( bsp.PointContents( c ) != CONTENTS_SOLID )
                    {
                        candidates.push_back( ( peak.pos + peak.normal * 24.0f + c ) * 0.5f );
                        convergingNormal = convergingNormal + s.normal;
                        otherFaces++;
                    }
                }
                if( otherFaces >= 4 )
                {
                    break;
                }
            }

            Vec3    bestPos = peak.pos + peak.normal * 16.0f;
            FitEval bestFit{};
            for( const Vec3& cand : candidates )
            {
                FitEval f = EvaluatePointCandidate( bsp, cand, samples, nearbyIndices );
                if( f.loss < bestFit.loss )
                {
                    bestFit = f;
                    bestPos = cand;
                }
            }

            if( bestFit.supportCount < 3 )
            {
                continue;
            }

            const std::array< float, 3 > stepSizes = { 16.0f, 8.0f, 4.0f };
            const std::array< Vec3, 6 >  dirs      = { {
                { 1, 0, 0 },
                { -1, 0, 0 },
                { 0, 1, 0 },
                { 0, -1, 0 },
                { 0, 0, 1 },
                { 0, 0, -1 },
            } };

            for( float stepLen : stepSizes )
            {
                for( int iter = 0; iter < 3; iter++ )
                {
                    bool improved = false;
                    for( const Vec3& d : dirs )
                    {
                        Vec3    trial = bestPos + d * stepLen;
                        FitEval f     = EvaluatePointCandidate( bsp, trial, samples, nearbyIndices );
                        if( f.loss < bestFit.loss )
                        {
                            bestFit  = f;
                            bestPos  = trial;
                            improved = true;
                        }
                    }
                    if( !improved )
                    {
                        break;
                    }
                }
            }

            SpotEval spotFit =
                EvaluateSpotUpgrade( bsp, bestPos, bestFit, samples, nearbyIndices );

            Vec3  chosenFlux = spotFit.useSpot ? spotFit.flux : bestFit.flux;
            float maxC       = MaxComp( chosenFlux );
            if( maxC <= 1.0f )
            {
                continue;
            }

            float metricLumens = MetricLumensFromBspFlux( maxC, oneGameUnitInMeters );
            float maxMetricLumens = Utils::IntensityFromNonMetric( 350.0f, oneGameUnitInMeters );
            metricLumens = std::min( metricLumens, maxMetricLumens );
            if( metricLumens < 2500.0f )
            {
                continue;
            }

            float minSep = 2.0f / oneGameUnitInMeters;
            bool  duplicate = false;
            auto isNear    = [ &bestPos, minSep ]( const GenericLight& gl ) {
                if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
                {
                    return LengthSq( FromRg( sph->position ) - bestPos ) < minSep * minSep;
                }
                if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &gl ) )
                {
                    return LengthSq( FromRg( sp->position ) - bestPos ) < minSep * minSep;
                }
                return false;
            };
            for( const GenericLight& gl : existingLights )
            {
                if( isNear( gl ) )
                {
                    duplicate = true;
                    break;
                }
            }
            for( const GenericLight& gl : estimated )
            {
                if( isNear( gl ) )
                {
                    duplicate = true;
                    break;
                }
            }

            if( !duplicate )
            {
                Vec3              normC       = chosenFlux / maxC;
                RgColor4DPacked32 packedColor = Utils::PackColorFromFloat( normC.x, normC.y, normC.z, 1.0f );
                RgLightExtraInfo  extra{ .exists = ( style != 0 ) ? 1u : 0u, .lightstyle = int( style ) };

                float exportLumens = std::clamp(
                    metricLumens * 0.15f,
                    500.0f,
                    Utils::IntensityFromNonMetric( 60.0f, oneGameUnitInMeters ) );

                if( spotFit.useSpot )
                {
                    estimated.push_back( RgSpotLightUploadInfo{
                        .uniqueID     = 0,
                        .isExportable = true,
                        .extra        = extra,
                        .color        = packedColor,
                        .intensity    = exportLumens,
                        .position     = ToRg( bestPos ),
                        .direction    = ToRg( spotFit.direction ),
                        .radius       = 0.05f / oneGameUnitInMeters,
                        .angleOuter   = spotFit.angleOuterRad,
                        .angleInner   = spotFit.angleInnerRad,
                    } );
                }
                else
                {
                    estimated.push_back( RgSphericalLightUploadInfo{
                        .uniqueID     = 0,
                        .isExportable = true,
                        .extra        = extra,
                        .color        = packedColor,
                        .intensity    = exportLumens,
                        .position     = ToRg( bestPos ),
                        .radius       = 0.05f / oneGameUnitInMeters,
                    } );
                }
            }

            float cosInner = std::cos( spotFit.angleInnerRad );
            float cosOuter = std::cos( spotFit.angleOuterRad );
            for( int idx : nearbyIndices )
            {
                LuxelSample& s = samples[ idx ];
                float        g = AttenuationKernel( bestPos, s.pos, s.normal );
                if( g <= 0.0f )
                {
                    continue;
                }
                if( spotFit.useSpot )
                {
                    g *= SpotFalloff(
                        spotFit.direction, Normalize( s.pos - bestPos ), cosInner, cosOuter );
                    if( g <= 0.0f )
                    {
                        continue;
                    }
                }
                if( !bsp.HasLineOfSight( bestPos, s.pos + s.normal * 0.5f ) )
                {
                    continue;
                }
                Vec3 sub = chosenFlux * g;
                s.resRgb = {
                    std::max( 0.0f, s.resRgb.x - sub.x ),
                    std::max( 0.0f, s.resRgb.y - sub.y ),
                    std::max( 0.0f, s.resRgb.z - sub.z ),
                };
                if( MaxComp( s.resRgb ) <= ambientFloor ||
                    LengthSq( s.pos - bestPos ) < minSep * minSep )
                {
                    visited[ idx ] = 1;
                }
            }
        }
    }

    debug::Info( "BspLightEstimator: inferred {} lights from {}",
                 estimated.size(),
                 foundPath->string() );
    return estimated;
}
