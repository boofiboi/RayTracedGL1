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

#include "GltfExporter.h"

#include "BspLightEstimator.h"
#include "Const.h"
#include "SpanCounted.h"
#include "TextureExporter.h"
#include "Utils.h"

#include "Generated/ShaderCommonC.h"

#define CGLTF_VALIDATE_ENABLE_ASSERTS 1
#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION

#include "cgltf/cgltf_write.h"

#include <array>
#include <cassert>
#include <fstream>
#include <queue>
#include <span>
#include <type_traits>

namespace
{
template< class T >
void HashCombine( std::size_t& seed, const T& v )
{
    seed ^= std::hash< T >{}( v ) + 0x9e3779b9 + ( seed << 6 ) + ( seed >> 2 );
}

bool TransformIsLess( const RgTransform& a, const RgTransform& b )
{
    constexpr float eps = 0.0001f;
    for( size_t i = 0; i < std::size( a.matrix ); i++ )
    {
        for( size_t j = 0; j < std::size( a.matrix[ 0 ] ); j++ )
        {
            if( std::abs( a.matrix[ i ][ j ] - b.matrix[ i ][ j ] ) > eps )
            {
                return a.matrix[ i ][ j ] < b.matrix[ i ][ j ];
            }
        }
    }
    return false;
}

bool TransformsAreEqual( const RgTransform& a, const RgTransform& b )
{
    constexpr float eps = 0.0001f;
    for( size_t i = 0; i < std::size( a.matrix ); i++ )
    {
        for( size_t j = 0; j < std::size( a.matrix[ 0 ] ); j++ )
        {
            if( std::abs( a.matrix[ i ][ j ] - b.matrix[ i ][ j ] ) > eps )
            {
                return false;
            }
        }
    }
    return true;
}

uint64_t TransformHash( const RgTransform& a )
{
    uint64_t h = 0;
    for( size_t i = 0; i < std::size( a.matrix ); i++ )
    {
        for( size_t j = 0; j < std::size( a.matrix[ 0 ] ); j++ )
        {
            float v = std::abs( a.matrix[ i ][ j ] ) < 1e-6f ? 0.0f : a.matrix[ i ][ j ];
            HashCombine( h, v );
        }
    }
    return h;
}
}

template<>
struct std::hash< RgTransform >
{
    std::size_t operator()( RgTransform const& t ) const noexcept { return TransformHash( t ); }
};

namespace
{
template< typename T >
    requires( std::is_default_constructible_v< T > )
T ValueOrDefault( const std::optional< T > v )
{
    return v.value_or( T{} );
}

auto* ConvertRefToPtr( auto& ref )
{
    return &ref;
}

std::filesystem::path GetGltfFolder( const std::filesystem::path& gltfPath )
{
    return gltfPath.parent_path();
}

std::filesystem::path GetOriginalTexturesFolder( const std::filesystem::path& gltfPath )
{
    return GetGltfFolder( gltfPath ) / RTGL1::TEXTURES_FOLDER_JUNCTION_PREFIX;
}

std::filesystem::path GetGltfBinPath( std::filesystem::path gltfPath )
{
    return gltfPath.replace_extension( ".bin" );
}

std::string GetGltfBinURI( const std::filesystem::path& gltfPath )
{
    return GetGltfBinPath( gltfPath ).filename().string();
}
}



namespace RTGL1
{
struct DeepCopyOfPrimitive
{
    explicit DeepCopyOfPrimitive( const RgMeshPrimitiveInfo& c )
        : info( c ), editor( c.pEditorInfo ? *c.pEditorInfo : RgEditorInfo{} )
    {
        std::span fromVertices( c.pVertices, c.vertexCount );
        std::span fromIndices( c.pIndices, c.indexCount );
        // for copying
        static_assert(
            std::is_same_v< decltype( c.pVertices ), decltype( pVertices )::const_pointer > );
        static_assert(
            std::is_same_v< decltype( c.pIndices ), decltype( pIndices )::const_pointer > );
        static_assert( std::is_trivially_copyable_v< decltype( pVertices )::value_type > );
        static_assert( std::is_trivially_copyable_v< decltype( pIndices )::value_type > );

        // deep copy
        pPrimitiveNameInMesh = Utils::SafeCstr( c.pPrimitiveNameInMesh );
        pTextureName         = Utils::SafeCstr( c.pTextureName );
        pVertices.assign( fromVertices.begin(), fromVertices.end() );
        pIndices.assign( fromIndices.begin(), fromIndices.end() );

        FixupPointers( *this );
    }
    ~DeepCopyOfPrimitive() = default;

    DeepCopyOfPrimitive( DeepCopyOfPrimitive&& other ) noexcept
        : info( other.info )
        , editor( other.editor )
        , pPrimitiveNameInMesh( std::move( other.pPrimitiveNameInMesh ) )
        , pTextureName( std::move( other.pTextureName ) )
        , pVertices( std::move( other.pVertices ) )
        , pIndices( std::move( other.pIndices ) )
    {
        assert( other.info.vertexCount == other.pVertices.size() );
        assert( other.info.indexCount == other.pIndices.size() );

        FixupPointers( *this );
    }

    DeepCopyOfPrimitive& operator=( DeepCopyOfPrimitive&& other ) noexcept
    {
        assert( other.info.vertexCount == other.pVertices.size() );
        assert( other.info.indexCount == other.pIndices.size() );

        this->pPrimitiveNameInMesh = std::move( other.pPrimitiveNameInMesh );
        this->pTextureName         = std::move( other.pTextureName );
        this->pVertices            = std::move( other.pVertices );
        this->pIndices             = std::move( other.pIndices );

        // copy and fix
        this->info   = other.info;
        this->editor = other.editor;
        FixupPointers( *this );

        // invalidate other
        other.info = {};

        return *this;
    }

    // No copies
    DeepCopyOfPrimitive( const DeepCopyOfPrimitive& other )            = delete;
    DeepCopyOfPrimitive& operator=( const DeepCopyOfPrimitive& other ) = delete;

    std::span< const RgPrimitiveVertex > Vertices() const
    {
        return { pVertices.begin(), pVertices.end() };
    }

    std::span< const uint32_t > Indices() const { return { pIndices.begin(), pIndices.end() }; }

    std::string_view PrimitiveNameInMesh() const { return pPrimitiveNameInMesh; }
    std::string_view MaterialName() const { return pTextureName; }

    RgFloat4D Color() const { return Utils::UnpackColor4DPacked32( info.color ); }
    float     Emissive() const { return Utils::Saturate( info.emissive ); }

    float Roughness() const
    {
        return editor.pbrInfoExists ? Utils::Saturate( editor.pbrInfo.roughnessDefault ) : 1.0f;
    }
    float Metallic() const
    {
        return editor.pbrInfoExists ? Utils::Saturate( editor.pbrInfo.metallicDefault ) : 0.0f;
    }

    cgltf_alpha_mode AlphaMode() const
    {
        if( info.flags & RG_MESH_PRIMITIVE_ALPHA_TESTED )
        {
            return cgltf_alpha_mode_mask;
        }
        if( ( info.flags & RG_MESH_PRIMITIVE_TRANSLUCENT ) ||
            Utils::UnpackAlphaFromPacked32( info.color ) < MESH_TRANSLUCENT_ALPHA_THRESHOLD )
        {
            return cgltf_alpha_mode_blend;
        }
        return cgltf_alpha_mode_opaque;
    }

private:
    static void FixupPointers( DeepCopyOfPrimitive& inout )
    {
        inout.info.pPrimitiveNameInMesh = inout.pPrimitiveNameInMesh.data();
        inout.info.pTextureName         = inout.pTextureName.data();
        inout.info.pVertices            = inout.pVertices.data();
        inout.info.pIndices             = inout.pIndices.data();

        assert( inout.info.vertexCount == inout.pVertices.size() );
        assert( inout.info.indexCount == inout.pIndices.size() );

        inout.info.pEditorInfo = &inout.editor;
    }

private:
    RgMeshPrimitiveInfo info;
    RgEditorInfo        editor;

    // to maintain lifetimes
    std::string                      pPrimitiveNameInMesh;
    std::string                      pTextureName;
    std::vector< RgPrimitiveVertex > pVertices;
    std::vector< uint32_t >          pIndices;
};
}



RTGL1::GltfExporter::GltfExporter( const RgTransform& _worldTransform, float _oneGameUnitInMeters )
    : worldTransform( _worldTransform ), oneGameUnitInMeters( _oneGameUnitInMeters )
{
}

namespace
{

struct GltfBin
{
    explicit GltfBin( const std::filesystem::path& gltfPath )
        : uri( GetGltfBinURI( gltfPath ) )
        , file( GetGltfBinPath( gltfPath ), std::ios::out | std::ios::trunc | std::ios::binary )
        , fileOffset( 0 )
        , storage{}
    {
        assert( file );
    }

    bool IsOpen() const
    {
        return file.is_open();
    }

    cgltf_buffer* Get()
    {
        storage = cgltf_buffer{
            .name = nullptr,
            .size = fileOffset,
            .uri  = const_cast< char* >( uri.c_str() ),
        };
        return &storage;
    }

    // Returns begin of written data.
    template< typename T >
    size_t Write( std::span< T > bytes )
    {
        size_t begin = fileOffset;

        file.write( reinterpret_cast< const char* >( bytes.data() ),
                    std::streamsize( bytes.size_bytes() ) );
        fileOffset += bytes.size_bytes();

        return begin;
    }

private:
    std::string   uri;
    std::ofstream file;
    size_t        fileOffset;
    cgltf_buffer  storage;
};


auto MakeBufferViews( GltfBin& fbin, const RTGL1::DeepCopyOfPrimitive& prim )
{
    return std::to_array( {
#define BUFFER_VIEW_VERTICES 0
        cgltf_buffer_view{
            .name   = nullptr,
            .buffer = fbin.Get(),
            .offset = fbin.Write( prim.Vertices() ),
            .size   = prim.Vertices().size_bytes(),
            .stride = sizeof( decltype( prim.Vertices() )::element_type ),
            .type   = cgltf_buffer_view_type_vertices,
        },
#define BUFFER_VIEW_INDICES 1
        cgltf_buffer_view{
            .name   = nullptr,
            .buffer = fbin.Get(),
            .offset = fbin.Write( prim.Indices() ),
            .size   = prim.Indices().size_bytes(),
            .stride = 0,
            .type   = cgltf_buffer_view_type_indices,
        },
    } );
}
constexpr size_t BufferViewsPerPrim =
    std::size( std::invoke_result_t< decltype( MakeBufferViews ),
                                     GltfBin&,
                                     const RTGL1::DeepCopyOfPrimitive& >{} );

auto MakeAccessors( size_t                         vertexCount,
                    size_t                         indexCount,
                    std::span< cgltf_buffer_view > correspondingViews )
{
    assert( correspondingViews.size() == BufferViewsPerPrim );

    return std::to_array( {
#define ACCESSOR_POSITION 0
        cgltf_accessor{
            .name           = nullptr,
            .component_type = cgltf_component_type_r_32f,
            .normalized     = false,
            .type           = cgltf_type_vec3,
            .offset         = offsetof( RgPrimitiveVertex, position ),
            .count          = vertexCount,
            .stride         = sizeof( RgPrimitiveVertex ),
            .buffer_view    = &correspondingViews[ BUFFER_VIEW_VERTICES ],
            .has_min        = false,
            .min            = {},
            .has_max        = false,
            .max            = {},
        },
#define ACCESSOR_NORMAL 1
        cgltf_accessor{
            .name           = nullptr,
            .component_type = cgltf_component_type_r_32f,
            .normalized     = false,
            .type           = cgltf_type_vec3,
            .offset         = offsetof( RgPrimitiveVertex, normal ),
            .count          = vertexCount,
            .stride         = sizeof( RgPrimitiveVertex ),
            .buffer_view    = &correspondingViews[ BUFFER_VIEW_VERTICES ],
            .has_min        = true,
            .min            = { -1.f, -1.f, -1.f },
            .has_max        = true,
            .max            = { 1.f, 1.f, 1.f },
        },
#define ACCESSOR_TANGENT 2
        cgltf_accessor{
            .name           = nullptr,
            .component_type = cgltf_component_type_r_32f,
            .normalized     = false,
            .type           = cgltf_type_vec4,
            .offset         = offsetof( RgPrimitiveVertex, tangent ),
            .count          = vertexCount,
            .stride         = sizeof( RgPrimitiveVertex ),
            .buffer_view    = &correspondingViews[ BUFFER_VIEW_VERTICES ],
            .has_min        = true,
            .min            = { -1.f, -1.f, -1.f, -1.f },
            .has_max        = true,
            .max            = { 1.f, 1.f, 1.f, 1.f },
        },
#define ACCESSOR_TEXCOORD 3
        cgltf_accessor{
            .name           = nullptr,
            .component_type = cgltf_component_type_r_32f,
            .normalized     = false,
            .type           = cgltf_type_vec2,
            .offset         = offsetof( RgPrimitiveVertex, texCoord ),
            .count          = vertexCount,
            .stride         = sizeof( RgPrimitiveVertex ),
            .buffer_view    = &correspondingViews[ BUFFER_VIEW_VERTICES ],
            .has_min        = false,
            .min            = {},
            .has_max        = false,
            .max            = {},
        },
#define ACCESSOR_COLOR 4
        cgltf_accessor{
            .name           = nullptr,
            .component_type = cgltf_component_type_r_8u,
            .normalized     = false,
            .type           = cgltf_type_vec4,
            .offset         = offsetof( RgPrimitiveVertex, color ),
            .count          = vertexCount,
            .stride         = sizeof( RgPrimitiveVertex ),
            .buffer_view    = &correspondingViews[ BUFFER_VIEW_VERTICES ],
            .has_min        = false,
            .min            = {},
            .has_max        = false,
            .max            = {},
        },
#define ACCESSOR_INDEX 5
        cgltf_accessor{
            .name           = nullptr,
            .component_type = cgltf_component_type_r_32u,
            .normalized     = false,
            .type           = cgltf_type_scalar,
            .offset         = 0,
            .count          = indexCount,
            .stride         = sizeof( uint32_t ),
            .buffer_view    = &correspondingViews[ BUFFER_VIEW_INDICES ],
            .has_min        = false,
            .min            = {},
            .has_max        = false,
            .max            = {},
        },
    } );
}
constexpr size_t AccessorsPerPrim =
    std::size( std::invoke_result_t< decltype( MakeAccessors ),
                                     size_t,
                                     size_t,
                                     std::span< cgltf_buffer_view > >{} );
cgltf_accessor* GetIndicesAccessor( std::span< cgltf_accessor > correspondingAccessors )
{
    return &correspondingAccessors[ ACCESSOR_INDEX ];
}


auto MakeVertexAttributes( std::span< cgltf_accessor > correspondingAccessors )
{
    assert( correspondingAccessors.size() == AccessorsPerPrim );

    return std::to_array( {
        cgltf_attribute{
            .name  = const_cast< char* >( "POSITION" ),
            .type  = cgltf_attribute_type_position,
            .index = 0,
            .data  = &correspondingAccessors[ ACCESSOR_POSITION ],
        },
        cgltf_attribute{
            .name  = const_cast< char* >( "NORMAL" ),
            .type  = cgltf_attribute_type_normal,
            .index = 0,
            .data  = &correspondingAccessors[ ACCESSOR_NORMAL ],
        },
        cgltf_attribute{
            .name  = const_cast< char* >( "TANGENT" ),
            .type  = cgltf_attribute_type_tangent,
            .index = 0,
            .data  = &correspondingAccessors[ ACCESSOR_TANGENT ],
        },
        cgltf_attribute{
            .name  = const_cast< char* >( "TEXCOORD_0" ),
            .type  = cgltf_attribute_type_texcoord,
            .index = 0,
            .data  = &correspondingAccessors[ ACCESSOR_TEXCOORD ],
        },
        cgltf_attribute{
            .name  = const_cast< char* >( "COLOR" ),
            .type  = cgltf_attribute_type_color,
            .index = 0,
            .data  = &correspondingAccessors[ ACCESSOR_COLOR ],
        },
    } );
}
constexpr size_t AttributesPerPrim = std::size(
    std::invoke_result_t< decltype( MakeVertexAttributes ), std::span< cgltf_accessor > >{} );


#define copy_ranges swap_ranges


struct BeginCount
{
    ptrdiff_t offset;
    size_t    count;

    template< typename T >
    std::span< T > ToSpan( std::vector< T >& container ) const
    {
        return { container.begin() + offset, count };
    }

    template< typename T >
    T* ToPointer( std::vector< T >& container ) const
    {
        assert( count == 1 );
        return &container[ offset ];
    }

    template< typename T >
    std::vector< T* > ToVectorOfPointers( std::vector< T >& container ) const
    {
        std::vector< T* > ptrs;

        std::ranges::transform(
            ToSpan< T >( container ), std::back_inserter( ptrs ), ConvertRefToPtr< T > );

        return ptrs;
    }
};

template< typename T >
auto append_n( std::vector< T >& v, size_t toadd )
{
    auto begin = ptrdiff_t( v.size() );
    v.resize( v.size() + toadd );

    return BeginCount{ begin, toadd };
}


// Corresponds to RgMeshInfo
struct GltfRoot
{
    std::string name;
    RgTransform transform;

    cgltf_node* thisNode;

    std::span< cgltf_buffer_view > bufferViews;
    std::span< cgltf_accessor >    accessors;
    std::span< cgltf_attribute >   attributes;
    std::span< cgltf_primitive >   primitives;
    std::span< cgltf_material >    materials;
    cgltf_mesh*                    mesh;

    std::vector< std::shared_ptr< RTGL1::DeepCopyOfPrimitive > > source;
};

struct GltfStorage
{
    explicit GltfStorage( const RTGL1::MeshesToTheirPrimitives& scene, size_t lightCount )
    {
        struct Ranges
        {
            BeginCount bufferViews;
            BeginCount accessors;
            BeginCount attributes;
            BeginCount primitives;
            BeginCount materials;
            BeginCount mesh;
            BeginCount thisNode;
        };
        std::queue< Ranges > ranges;

        // alloc
        for( const auto& [ meshNode, prims ] : scene )
        {
            size_t primsPerMesh = prims.size();

            ranges.push( Ranges{
                .bufferViews = append_n( allBufferViews, primsPerMesh * BufferViewsPerPrim ),
                .accessors   = append_n( allAccessors, primsPerMesh * AccessorsPerPrim ),
                .attributes  = append_n( allAttributes, primsPerMesh * AttributesPerPrim ),
                .primitives  = append_n( allPrimitives, primsPerMesh ),
                .materials   = append_n( allMaterials, primsPerMesh ),
                .mesh        = append_n( allMeshes, 1 ),
                .thisNode    = append_n( allNodes, 1 ),
            } );
        }
        BeginCount lightsbc = append_n( allNodes, lightCount );
        BeginCount worldbc  = append_n( allNodes, 1 );

        // resolve pointers
        for( const auto& [ meshNode, prims ] : scene )
        {
            const Ranges r = ranges.front();
            ranges.pop();

            GltfRoot root = {
                .name        = meshNode.name,
                .transform   = meshNode.transform,
                .thisNode    = r.thisNode.ToPointer( allNodes ),
                .bufferViews = r.bufferViews.ToSpan( allBufferViews ),
                .accessors   = r.accessors.ToSpan( allAccessors ),
                .attributes  = r.attributes.ToSpan( allAttributes ),
                .primitives  = r.primitives.ToSpan( allPrimitives ),
                .materials   = r.materials.ToSpan( allMaterials ),
                .mesh        = r.mesh.ToPointer( allMeshes ),
                .source      = prims,
            };
            assert( root.source.size() == root.primitives.size() );

            worldChildren.push_back( root.thisNode );
            roots.push_back( std::move( root ) );
        }

        lightNodes = lightsbc.ToSpan( allNodes );
        for( cgltf_node& lightNode : lightNodes )
        {
            worldChildren.push_back( &lightNode );
        }

        world = worldbc.ToPointer( allNodes );

        assert( ranges.empty() );
        assert( worldChildren.size() == roots.size() + lightCount );
    }

    std::vector< cgltf_buffer_view > allBufferViews;
    std::vector< cgltf_accessor >    allAccessors;
    std::vector< cgltf_attribute >   allAttributes;
    std::vector< cgltf_primitive >   allPrimitives;
    std::vector< cgltf_material >    allMaterials;
    std::vector< cgltf_mesh >        allMeshes;
    std::vector< cgltf_node >        allNodes;

    std::vector< GltfRoot > roots; // each corresponds to RgMeshInfo

    cgltf_node*                world{ nullptr };
    std::vector< cgltf_node* > worldChildren;

    std::span< cgltf_node > lightNodes;
};


struct GltfTextures
{
    explicit GltfTextures( const std::set< std::string >& sceneMaterials,
                           const std::filesystem::path&   texturesFolder,
                           const RTGL1::TextureManager&   textureManager )
    {
        RTGL1::debug::Info( "Exporting textures..." );

        // alloc max and lock pointers
        allocStrings.resize( RTGL1::TEXTURES_PER_MATERIAL_COUNT * sceneMaterials.size() );
        allocImages.resize( RTGL1::TEXTURES_PER_MATERIAL_COUNT * sceneMaterials.size() );
        allocTextures.resize( RTGL1::TEXTURES_PER_MATERIAL_COUNT * sceneMaterials.size() );

        strings  = rgl::span_counted( std::span( allocStrings ) );
        images   = rgl::span_counted( std::span( allocImages ) );
        textures = rgl::span_counted( std::span( allocTextures ) );

        constexpr auto makeSampler = []( RgSamplerAddressMode addrU, RgSamplerAddressMode addrV ) {
            cgltf_sampler s{};
            s.wrap_s = addrU == RG_SAMPLER_ADDRESS_MODE_CLAMP ? cgltf_wrap_mode_clamp_to_edge : cgltf_wrap_mode_repeat;
            s.wrap_t = addrV == RG_SAMPLER_ADDRESS_MODE_CLAMP ? cgltf_wrap_mode_clamp_to_edge : cgltf_wrap_mode_repeat;
            return s;
        };
        allocSamplers = {
            makeSampler( RG_SAMPLER_ADDRESS_MODE_REPEAT, RG_SAMPLER_ADDRESS_MODE_REPEAT ),
            makeSampler( RG_SAMPLER_ADDRESS_MODE_REPEAT, RG_SAMPLER_ADDRESS_MODE_CLAMP ),
            makeSampler( RG_SAMPLER_ADDRESS_MODE_CLAMP, RG_SAMPLER_ADDRESS_MODE_REPEAT ),
            makeSampler( RG_SAMPLER_ADDRESS_MODE_CLAMP, RG_SAMPLER_ADDRESS_MODE_CLAMP ),
        };
        auto findSampler = [ this, makeSampler ]( const RTGL1::TextureManager::ExportResult& r ) {
            cgltf_sampler target = makeSampler( r.addressModeU, r.addressModeV );
            for( cgltf_sampler& found : allocSamplers )
            {
                if( std::memcmp( &found, &target, sizeof( cgltf_sampler ) ) == 0 )
                {
                    return &found;
                }
            }
            assert( 0 );
            return &allocSamplers[ 0 ];
        };


        // resolve
        for( const auto& materialName : sceneMaterials )
        {
            if( materialName.empty() )
            {
                continue;
            }

            static_assert( RTGL1::TEXTURES_PER_MATERIAL_COUNT == 4 );
            static_assert( RTGL1::TEXTURE_ALBEDO_ALPHA_INDEX == 0 );
            static_assert( RTGL1::TEXTURE_OCCLUSION_ROUGHNESS_METALLIC_INDEX == 1 );
            static_assert( RTGL1::TEXTURE_NORMAL_INDEX == 2 );
            static_assert( RTGL1::TEXTURE_EMISSIVE_INDEX == 3 );
            auto [ albedo, orm, normal, emissive ] = textureManager.ExportMaterialTextures(
                materialName.c_str(), texturesFolder, false );

            auto tryMakeCgltfTexture =
                [ this, &findSampler, &materialName ](
                    RTGL1::TextureManager::ExportResult&& r,
                    bool isAlbedoFallback = false ) -> cgltf_texture* {
                std::string relPath = std::move( r.relativePath );
                if( relPath.empty() )
                {
                    if( isAlbedoFallback )
                    {
                        relPath = RTGL1::TextureOverrides::GetTexturePath( "", materialName, "", ".tga" ).string();
                    }
                    else
                    {
                        return nullptr;
                    }
                }

                std::string&   str = strings.increment_and_get();
                cgltf_image&   img = images.increment_and_get();
                cgltf_texture& txd = textures.increment_and_get();

                str = std::string( RTGL1::TEXTURES_FOLDER_JUNCTION_PREFIX ) + relPath;
                std::ranges::replace( str, '\\', '/' );

                img = cgltf_image{
                    .name = const_cast< char* >( materialName.c_str() ),
                    .uri  = const_cast< char* >( str.c_str() ),
                };

                txd = cgltf_texture{
                    .image   = &img,
                    .sampler = findSampler( r ),
                };

                return &txd;
            };

            materialAccess[ materialName ] = TextureSet{
                .albedo   = tryMakeCgltfTexture( std::move( albedo ), true ),
                .orm      = tryMakeCgltfTexture( std::move( orm ) ),
                .normal   = tryMakeCgltfTexture( std::move( normal ) ),
                .emissive = tryMakeCgltfTexture( std::move( emissive ) ),
            };
        }
    }

    struct TextureSet
    {
        cgltf_texture* albedo{ nullptr };
        cgltf_texture* orm{ nullptr };
        cgltf_texture* normal{ nullptr };
        cgltf_texture* emissive{ nullptr };
    };

    TextureSet Access( std::string_view materialName ) const
    {
        auto iter = materialAccess.find( std::string( materialName ) );
        return iter != materialAccess.end() ? iter->second : TextureSet{};
    }

    auto Samplers() { return std::span( allocSamplers ); }
    auto Images() { return images.get_counted_subspan(); }
    auto Textures() { return textures.get_counted_subspan(); }

private:
    std::vector< std::string >   allocStrings;
    std::vector< cgltf_sampler > allocSamplers;
    std::vector< cgltf_image >   allocImages;
    std::vector< cgltf_texture > allocTextures;

    rgl::span_counted< std::string >   strings;
    rgl::span_counted< cgltf_image >   images;
    rgl::span_counted< cgltf_texture > textures;

    std::unordered_map< std::string, TextureSet > materialAccess;
};

cgltf_material MakeMaterial( const RTGL1::DeepCopyOfPrimitive& rgprim,
                             const GltfTextures&               textureStorage )
{
    auto txd = textureStorage.Access( rgprim.MaterialName() );

    cgltf_pbr_metallic_roughness metallicRoughness = {
        .base_color_texture         = { .texture = txd.albedo, .texcoord = 0 },
        .metallic_roughness_texture = { .texture = txd.orm, .texcoord = 0 },
        .base_color_factor          = { RG_ACCESS_VEC4( rgprim.Color().data ) },
        .metallic_factor            = rgprim.Metallic(),
        .roughness_factor           = rgprim.Roughness(),
    };

    // if there are PBR textures, set to RTGL1 defaults
    if( metallicRoughness.metallic_roughness_texture.texture &&
        metallicRoughness.metallic_roughness_texture.texture->image )
    {
        metallicRoughness.metallic_factor  = 0.0f;
        metallicRoughness.roughness_factor = 1.0f;
    }

    return cgltf_material{
        .name                        = nullptr,
        .has_pbr_metallic_roughness  = true,
        .has_pbr_specular_glossiness = false,
        .has_clearcoat               = false,
        .has_transmission            = false,
        .has_volume                  = false,
        .has_ior                     = false,
        .has_specular                = false,
        .has_sheen                   = false,
        .has_emissive_strength       = false,
        .has_iridescence             = false,
        .pbr_metallic_roughness      = metallicRoughness,
        .pbr_specular_glossiness     = {},
        .clearcoat                   = {},
        .ior                         = {},
        .specular                    = {},
        .sheen                       = {},
        .transmission                = {},
        .volume                      = {},
        .emissive_strength           = {},
        .iridescence                 = {},
        .normal_texture              = { .texture = txd.normal, .texcoord = 0 },
        .occlusion_texture           = { .texture = txd.orm, .texcoord = 0 },
        .emissive_texture            = { .texture = txd.emissive, .texcoord = 0 },
        .emissive_factor             = { rgprim.Emissive(), rgprim.Emissive(), rgprim.Emissive() },
        .alpha_mode                  = rgprim.AlphaMode(),
        .alpha_cutoff                = 0.5f,
        .double_sided                = false,
        .unlit                       = false,
        .extras                      = { .data = nullptr },
    };
}


constexpr RgFloat3D operator-( const RgFloat3D& c )
{
    return {
        -c.data[ 0 ],
        -c.data[ 1 ],
        -c.data[ 2 ],
    };
}
constexpr RgFloat3D operator+( const RgFloat3D& a, const RgFloat3D& b )
{
    return {
        a.data[ 0 ] + b.data[ 0 ],
        a.data[ 1 ] + b.data[ 1 ],
        a.data[ 2 ] + b.data[ 2 ],
    };
}
constexpr RgFloat3D operator*( const RgFloat3D& c, float f )
{
    return {
        f * c.data[ 0 ],
        f * c.data[ 1 ],
        f * c.data[ 2 ],
    };
}


#define POLYLIGHT_AS_SPHERE 1

#if POLYLIGHT_AS_SPHERE
RgFloat3D GetCenter( const RgFloat3D ( &positions )[ 3 ] )
{
    RgFloat3D center = { 0, 0, 0 };
    for( const auto& v : positions )
    {
        center = center + v;
    }
    return center * ( 1.0f / 3.0f );
}

RgFloat3D GetCenter( const RgPolygonalLightUploadInfo& poly )
{
    return GetCenter( poly.positions );
}

std::optional< RgFloat3D > TryGetNormal( const RgFloat3D ( &positions )[ 3 ] )
{
    float     area;
    RgFloat3D normal;
    if( !RTGL1::Utils::GetNormalAndArea( positions, normal, area ) )
    {
        return std::nullopt;
    }
    return normal;
}

RgFloat3D GetNormal( const RgFloat3D ( &positions )[ 3 ] )
{
    if( auto n = TryGetNormal( positions ) )
    {
        return n.value();
    }
    return { 0, 0, 0 };
}

RgFloat3D GetNormal( const RgPolygonalLightUploadInfo& poly )
{
    return GetNormal( poly.positions );
}
#endif


struct GltfLights
{
private:
    static float LuminousFluxToCandela( float lumens )
    {
        // to lumens per steradian
        return lumens / ( 4 * float( RTGL1::Utils::Pi ) );
    }

    static cgltf_light MakeLight( const RgDirectionalLightUploadInfo& sun )
    {
        auto fcolor = RTGL1::Utils::UnpackColor4DPacked32< RgFloat3D >( sun.color );

        return cgltf_light{
            .name      = nullptr,
            .color     = { RG_ACCESS_VEC3( fcolor.data ) },
            .intensity = sun.intensity,
            .type      = cgltf_light_type_directional,
        };
    }

    static cgltf_light MakeLight( const RgSphericalLightUploadInfo& sph )
    {
        auto fcolor = RTGL1::Utils::UnpackColor4DPacked32< RgFloat3D >( sph.color );

        return cgltf_light{
            .name      = nullptr,
            .color     = { RG_ACCESS_VEC3( fcolor.data ) },
            .intensity = LuminousFluxToCandela( sph.intensity ),
            .type      = cgltf_light_type_point,
        };
    }

    static cgltf_light MakeLight( const RgSpotLightUploadInfo& spot )
    {
        auto fcolor = RTGL1::Utils::UnpackColor4DPacked32< RgFloat3D >( spot.color );

        // Note: need to be careful, cgltf doesn't write angles if they're equal to default,
        // and Blender's importer fails if outerConeAngle/innerConeAngle are not specified :(
        auto clampAngle = []( float radians ) {
            constexpr float defaultInner = 0.0f;
            constexpr float defaultOuter = 3.14159265358979323846f / 4.0f;

            if( std::abs( radians - defaultInner ) < 0.05f ||
                std::abs( radians - defaultOuter ) < 0.05f )
            {
                radians += 0.05f;
            }

            return std::clamp( radians, RTGL1::Utils::DegToRad( 1 ), RTGL1::Utils::DegToRad( 89 ) );
        };

        return cgltf_light{
            .name                  = nullptr,
            .color                 = { RG_ACCESS_VEC3( fcolor.data ) },
            .intensity             = LuminousFluxToCandela( spot.intensity ),
            .type                  = cgltf_light_type_spot,
            .spot_inner_cone_angle = clampAngle( spot.angleInner ),
            .spot_outer_cone_angle = clampAngle( spot.angleOuter ),
        };
    }

    static cgltf_light MakeLight( const RgPolygonalLightUploadInfo& poly )
    {
#if POLYLIGHT_AS_SPHERE
        auto fcolor = RTGL1::Utils::UnpackColor4DPacked32< RgFloat3D >( poly.color );

        float     area;
        RgFloat3D normal;
        if( !RTGL1::Utils::GetNormalAndArea( poly.positions, normal, area ) )
        {
            area = 1.0f;
        }

        return cgltf_light{
            .name      = nullptr,
            .color     = { RG_ACCESS_VEC3( fcolor.data ) },
            .intensity = LuminousFluxToCandela( poly.intensity ),
            .type      = cgltf_light_type_point,
        };
#endif
    }


    static RgTransform MakeTransform( const RgDirectionalLightUploadInfo& sun )
    {
        return RTGL1::Utils::MakeTransform( { 0, 0, 0 }, -sun.direction );
    }

    static RgTransform MakeTransform( const RgSphericalLightUploadInfo& sph )
    {
        return RTGL1::Utils::MakeTransform( sph.position, { 0, 0, 1 } );
    }

    static RgTransform MakeTransform( const RgSpotLightUploadInfo& spot )
    {
        return RTGL1::Utils::MakeTransform( spot.position, -spot.direction );
    }

    static RgTransform MakeTransform( const RgPolygonalLightUploadInfo& poly )
    {
#if POLYLIGHT_AS_SPHERE
        RgFloat3D center = GetCenter( poly ) + GetNormal( poly ) * 0.1f;

        return RTGL1::Utils::MakeTransform( center, { 0, 0, 1 } );
#else
        return RG_TRANSFORM_IDENTITY;
#endif
    }

public:
    explicit GltfLights( const std::vector< RTGL1::GenericLight >& sceneLights,
                         std::span< cgltf_node >                   dstLightNodes )
    {
        assert( dstLightNodes.size() == sceneLights.size() );

#if POLYLIGHT_AS_SPHERE
        {
            auto foundPoly = std::ranges::find_if( sceneLights, []( const RTGL1::GenericLight& l ) {
                return std::visit(
                    []< typename T >( const T& ) {
                        return std::is_same_v< T, RgPolygonalLightUploadInfo >;
                    },
                    l );
            } );

            if( foundPoly != sceneLights.end() )
            {
                RTGL1::debug::Warning(
                    "GLTF doesn't support poly lights, exporting theme as spherical" );
            }
        }
#endif

        // lock pointers
        storage.resize( sceneLights.size() );
        extrasStorage.resize( sceneLights.size() );

        for( size_t i = 0; i < sceneLights.size(); i++ )
        {
            storage[ i ] = std::visit(
                []( auto&& specific ) { //
                    return MakeLight( specific );
                },
                sceneLights[ i ] );

            extrasStorage[ i ] = std::visit(
                []( auto&& specific ) { //
                    return RTGL1::json_parser::MakeJsonString( specific.extra );
                },
                sceneLights[ i ] );

            RgTransform lightTransform = std::visit(
                []( auto&& specific ) { //
                    return MakeTransform( specific );
                },
                sceneLights[ i ] );

            dstLightNodes[ i ] = cgltf_node{
                .name            = nullptr,
                .parent          = nullptr, /* later */
                .children        = nullptr,
                .children_count  = 0,
                .light           = &storage[ i ],
                .has_translation = false,
                .has_rotation    = false,
                .has_scale       = false,
                .has_matrix      = true,
                .matrix          = RG_TRANSFORM_TO_GLTF_MATRIX( lightTransform ),
                .extras          = {},
            };
        }

        // resolve extras ptrs
        for( size_t i = 0; i < sceneLights.size(); i++ )
        {
            storage[ i ].extras = {
                .data = extrasStorage[ i ].empty()
                            ? nullptr
                            : const_cast< char* >( extrasStorage[ i ].c_str() ),
            };
        }
    }

    auto Lights() { return std::span( storage ); }

private:
    std::vector< cgltf_light > storage;
    std::vector< std::string > extrasStorage;
};


struct LightClusterSample
{
    RgFloat3D position;
    RgFloat3D normal;
    float     areaGameUnitsSq;
    float     fluxWeight;
    bool      isOmnidirectional;
};

struct WorldTri
{
    RgFloat3D v[ 3 ];
    RgFloat3D bmin;
    RgFloat3D bmax;
};

float VecDot( const RgFloat3D& a, const RgFloat3D& b )
{
    return a.data[ 0 ] * b.data[ 0 ] + a.data[ 1 ] * b.data[ 1 ] + a.data[ 2 ] * b.data[ 2 ];
}

RgFloat3D VecSub( const RgFloat3D& a, const RgFloat3D& b )
{
    return { a.data[ 0 ] - b.data[ 0 ], a.data[ 1 ] - b.data[ 1 ], a.data[ 2 ] - b.data[ 2 ] };
}

RgFloat3D VecCross( const RgFloat3D& a, const RgFloat3D& b )
{
    return {
        a.data[ 1 ] * b.data[ 2 ] - a.data[ 2 ] * b.data[ 1 ],
        a.data[ 2 ] * b.data[ 0 ] - a.data[ 0 ] * b.data[ 2 ],
        a.data[ 0 ] * b.data[ 1 ] - a.data[ 1 ] * b.data[ 0 ],
    };
}

bool RayIntersectsTri( const RgFloat3D& origin,
                       const RgFloat3D& dir,
                       const WorldTri&  tri,
                       float            maxT,
                       float&           outT )
{
    constexpr float eps = 1e-6f;
    RgFloat3D       e1  = VecSub( tri.v[ 1 ], tri.v[ 0 ] );
    RgFloat3D       e2  = VecSub( tri.v[ 2 ], tri.v[ 0 ] );
    RgFloat3D       p   = VecCross( dir, e2 );
    float           det = VecDot( e1, p );
    if( std::abs( det ) < eps )
    {
        return false;
    }
    float     invDet = 1.0f / det;
    RgFloat3D s      = VecSub( origin, tri.v[ 0 ] );
    float     u      = invDet * VecDot( s, p );
    if( u < 0.0f || u > 1.0f )
    {
        return false;
    }
    RgFloat3D q = VecCross( s, e1 );
    float     v = invDet * VecDot( dir, q );
    if( v < 0.0f || u + v > 1.0f )
    {
        return false;
    }
    float t = invDet * VecDot( e2, q );
    if( t > 0.01f && t < maxT )
    {
        outT = t;
        return true;
    }
    return false;
}

bool HasSegmentClearance( const RgFloat3D&               a,
                         const RgFloat3D&               b,
                         const std::vector< WorldTri >& worldTris )
{
    RgFloat3D delta = VecSub( b, a );
    float     len   = RTGL1::Utils::Length( delta.data );
    if( len < 1e-3f )
    {
        return true;
    }
    RgFloat3D dir   = delta * ( 1.0f / len );
    float     minX  = std::min( a.data[ 0 ], b.data[ 0 ] ) - 0.1f;
    float     minY  = std::min( a.data[ 1 ], b.data[ 1 ] ) - 0.1f;
    float     minZ  = std::min( a.data[ 2 ], b.data[ 2 ] ) - 0.1f;
    float     maxX  = std::max( a.data[ 0 ], b.data[ 0 ] ) + 0.1f;
    float     maxY  = std::max( a.data[ 1 ], b.data[ 1 ] ) + 0.1f;
    float     maxZ  = std::max( a.data[ 2 ], b.data[ 2 ] ) + 0.1f;
    float     maxT  = len * 0.96f;

    for( const WorldTri& tri : worldTris )
    {
        if( tri.bmax.data[ 0 ] < minX || tri.bmin.data[ 0 ] > maxX ||
            tri.bmax.data[ 1 ] < minY || tri.bmin.data[ 1 ] > maxY ||
            tri.bmax.data[ 2 ] < minZ || tri.bmin.data[ 2 ] > maxZ )
        {
            continue;
        }
        float t = 0.0f;
        if( RayIntersectsTri( a, dir, tri, maxT, t ) && t > len * 0.04f )
        {
            return false;
        }
    }
    return true;
}

float ComputeClearanceAlongNormal( const RgFloat3D&               origin,
                                   const RgFloat3D&               normal,
                                   float                          maxProbeDist,
                                   const std::vector< WorldTri >& worldTris )
{
    float bestT = maxProbeDist;
    RgFloat3D endPt = origin + normal * maxProbeDist;
    float rMinX = std::min( origin.data[ 0 ], endPt.data[ 0 ] ) - 0.1f;
    float rMinY = std::min( origin.data[ 1 ], endPt.data[ 1 ] ) - 0.1f;
    float rMinZ = std::min( origin.data[ 2 ], endPt.data[ 2 ] ) - 0.1f;
    float rMaxX = std::max( origin.data[ 0 ], endPt.data[ 0 ] ) + 0.1f;
    float rMaxY = std::max( origin.data[ 1 ], endPt.data[ 1 ] ) + 0.1f;
    float rMaxZ = std::max( origin.data[ 2 ], endPt.data[ 2 ] ) + 0.1f;

    for( const WorldTri& tri : worldTris )
    {
        if( tri.bmax.data[ 0 ] < rMinX || tri.bmin.data[ 0 ] > rMaxX ||
            tri.bmax.data[ 1 ] < rMinY || tri.bmin.data[ 1 ] > rMaxY ||
            tri.bmax.data[ 2 ] < rMinZ || tri.bmin.data[ 2 ] > rMaxZ )
        {
            continue;
        }
        float t = 0.0f;
        if( RayIntersectsTri( origin, normal, tri, bestT, t ) )
        {
            bestT = t;
        }
    }
    return bestT;
}

struct QuantizedVertexKey
{
    int64_t x, y, z;
    bool    operator==( const QuantizedVertexKey& o ) const = default;
};

struct QuantizedVertexHash
{
    size_t operator()( const QuantizedVertexKey& k ) const noexcept
    {
        size_t h = 0;
        HashCombine( h, k.x );
        HashCombine( h, k.y );
        HashCombine( h, k.z );
        return h;
    }
};

struct QuantizedEdgeKey
{
    uint32_t a, b;
    bool     operator==( const QuantizedEdgeKey& o ) const = default;
};

struct QuantizedEdgeHash
{
    size_t operator()( const QuantizedEdgeKey& k ) const noexcept
    {
        size_t h = 0;
        HashCombine( h, k.a );
        HashCombine( h, k.b );
        return h;
    }
};

void EmitClusterSampleGroup( const std::vector< uint32_t >&       members,
                             const RgFloat3D*                     triVerts,
                             const RgFloat3D*                     triCentroids,
                             const RgFloat3D*                     triNormals,
                             const float*                         triAreas,
                             float                                oneGameUnitInMeters,
                             bool                                 subdivideElongated,
                             bool                                 allowOmnidirectional,
                             std::vector< LightClusterSample >&   outSamples )
{
    RgFloat3D sumPos{ 0, 0, 0 };
    RgFloat3D sumNorm{ 0, 0, 0 };
    float     totalArea = 0.0f;

    for( uint32_t idx : members )
    {
        float area = triAreas[ idx ];
        sumPos     = sumPos + triCentroids[ idx ] * area;
        sumNorm    = sumNorm + triNormals[ idx ] * area;
        totalArea += area;
    }

    if( totalArea <= 1e-4f )
    {
        return;
    }

    RgFloat3D center  = sumPos * ( 1.0f / totalArea );
    float     normLen = RTGL1::Utils::Length( sumNorm.data );
    float     areaM2  = totalArea * oneGameUnitInMeters * oneGameUnitInMeters;
    float     scale   = std::clamp( std::sqrt( areaM2 / 0.25f ), 0.25f, 1.0f );

    if( allowOmnidirectional && normLen / totalArea < 0.25f )
    {
        RgFloat3D hintNormal{ 0, 0, 0 };
        if( normLen / totalArea >= 0.02f && normLen > 1e-6f )
        {
            hintNormal    = sumNorm * ( 1.0f / normLen );
            float maxProj = 0.0f;
            for( uint32_t idx : members )
            {
                for( int k = 0; k < 3; k++ )
                {
                    maxProj = std::max(
                        maxProj, VecDot( VecSub( triVerts[ idx * 3 + k ], center ), hintNormal ) );
                }
            }
            if( maxProj > 0.0f )
            {
                center = center + hintNormal * maxProj;
            }
        }
        outSamples.push_back( LightClusterSample{
            .position          = center,
            .normal            = hintNormal,
            .areaGameUnitsSq   = totalArea,
            .fluxWeight        = scale,
            .isOmnidirectional = true,
        } );
        return;
    }

    if( normLen < 1e-6f )
    {
        return;
    }

    RgFloat3D normal = sumNorm * ( 1.0f / normLen );

    if( allowOmnidirectional )
    {
        float maxProj = 0.0f;
        for( uint32_t idx : members )
        {
            for( int k = 0; k < 3; k++ )
            {
                maxProj = std::max( maxProj, VecDot( VecSub( triVerts[ idx * 3 + k ], center ), normal ) );
            }
        }
        if( maxProj > 0.0f )
        {
            center = center + normal * maxProj;
        }
    }

    int       subCount = 1;
    RgFloat3D majorAxis{ 0, 0, 0 };
    float     majorSpan = 0.0f;

    if( subdivideElongated && members.size() >= 2 )
    {
        RgFloat3D tmpUp = std::abs( normal.data[ 2 ] ) < 0.9f ? RgFloat3D{ 0, 0, 1 }
                                                             : RgFloat3D{ 1, 0, 0 };
        RgFloat3D tangentU = RTGL1::Utils::Normalize( VecCross( tmpUp, normal ) );
        RgFloat3D tangentV = RTGL1::Utils::Normalize( VecCross( normal, tangentU ) );

        double cxx = 0.0, cyy = 0.0, cxy = 0.0;
        for( uint32_t idx : members )
        {
            float area = triAreas[ idx ];
            for( int k = 0; k < 3; k++ )
            {
                RgFloat3D rel = VecSub( triVerts[ idx * 3 + k ], center );
                double    u   = VecDot( rel, tangentU );
                double    v   = VecDot( rel, tangentV );
                cxx += u * u * double( area );
                cyy += v * v * double( area );
                cxy += u * v * double( area );
            }
        }

        double trace = cxx + cyy;
        double diff  = cxx - cyy;
        double disc  = std::sqrt( diff * diff + 4.0 * cxy * cxy );
        double lam1  = 0.5 * ( trace + disc );

        if( lam1 > 1e-4 )
        {
            double ang = 0.5 * std::atan2( 2.0 * cxy, diff );
            majorAxis  = RTGL1::Utils::Normalize(
                tangentU * float( std::cos( ang ) ) + tangentV * float( std::sin( ang ) ) );
            RgFloat3D minorAxis = RTGL1::Utils::Normalize( VecCross( normal, majorAxis ) );

            float minU = 1e30f, maxU = -1e30f;
            float minV = 1e30f, maxV = -1e30f;
            for( uint32_t idx : members )
            {
                for( int k = 0; k < 3; k++ )
                {
                    RgFloat3D rel = VecSub( triVerts[ idx * 3 + k ], center );
                    float     u   = VecDot( rel, majorAxis );
                    float     v   = VecDot( rel, minorAxis );
                    minU          = std::min( minU, u );
                    maxU          = std::max( maxU, u );
                    minV          = std::min( minV, v );
                    maxV          = std::max( maxV, v );
                }
            }
            majorSpan       = maxU - minU;
            float minorSpan = std::max( 1e-3f, maxV - minV );
            float aspect    = majorSpan / minorSpan;
            float lenMeters = majorSpan * oneGameUnitInMeters;
            if( aspect > 2.5f && lenMeters > 0.6f )
            {
                subCount = std::clamp( int( std::lround( aspect ) ), 2, 8 );
            }
        }
    }

    float eachWeight = scale / float( subCount );
    if( subCount == 1 )
    {
        outSamples.push_back( LightClusterSample{
            .position          = center,
            .normal            = normal,
            .areaGameUnitsSq   = totalArea,
            .fluxWeight        = eachWeight,
            .isOmnidirectional = false,
        } );
    }
    else
    {
        float halfSpan = majorSpan * 0.5f * ( 1.0f - 1.0f / float( subCount ) );
        for( int s = 0; s < subCount; s++ )
        {
            float     t   = ( subCount > 1 ) ? ( -1.0f + 2.0f * float( s ) / float( subCount - 1 ) ) : 0.0f;
            RgFloat3D pos = center + majorAxis * ( t * halfSpan );
            outSamples.push_back( LightClusterSample{
                .position          = pos,
                .normal            = normal,
                .areaGameUnitsSq   = totalArea / float( subCount ),
                .fluxWeight        = eachWeight,
                .isOmnidirectional = false,
            } );
        }
    }
}

void ExtractCoplanarLightClusters( const RgTransform&                   transform,
                                   const RgPrimitiveVertex*             pVertices,
                                   uint32_t                             vertexCount,
                                   const uint32_t*                      pIndices,
                                   uint32_t                             indexCount,
                                   float                                oneGameUnitInMeters,
                                   bool                                 subdivideElongated,
                                   bool                                 collapseCompactFixtures,
                                   std::vector< LightClusterSample >&   outSamples )
{
    if( !pVertices || !pIndices || vertexCount == 0 || indexCount < 3 )
    {
        return;
    }

    uint32_t triCount = indexCount / 3;
    if( triCount > 1024 )
    {
        RTGL1::debug::Warning(
            "The amount of triangles on a primitive with attached light is too high ({})",
            triCount );
    }

    std::vector< RgFloat3D > triVerts( triCount * 3 );
    std::vector< uint32_t >  triQ( triCount * 3 );
    std::vector< RgFloat3D > triCentroids( triCount );
    std::vector< RgFloat3D > triNormals( triCount );
    std::vector< float >     triAreas( triCount, 0.0f );
    std::vector< uint8_t >   triValid( triCount, 0 );

    std::unordered_map< QuantizedVertexKey, uint32_t, QuantizedVertexHash > vertMap;
    vertMap.reserve( vertexCount );

    auto quantizeVert = [ &vertMap ]( const RgFloat3D& p ) -> uint32_t {
        QuantizedVertexKey k{
            int64_t( std::llround( double( p.data[ 0 ] ) * 100.0 ) ),
            int64_t( std::llround( double( p.data[ 1 ] ) * 100.0 ) ),
            int64_t( std::llround( double( p.data[ 2 ] ) * 100.0 ) ),
        };
        auto [ it, inserted ] = vertMap.emplace( k, uint32_t( vertMap.size() ) );
        return it->second;
    };

    for( uint32_t t = 0; t < triCount; t++ )
    {
        RgFloat3D v[ 3 ];
        for( int k = 0; k < 3; k++ )
        {
            const float* src    = pVertices[ pIndices[ t * 3 + k ] ].position;
            v[ k ]              = RTGL1::Utils::ApplyTransform( transform, RgFloat3D{ RG_ACCESS_VEC3( src ) } );
            triVerts[ t * 3 + k ] = v[ k ];
            triQ[ t * 3 + k ]     = quantizeVert( v[ k ] );
        }
        RgFloat3D unnorm = RTGL1::Utils::GetUnnormalizedNormal( v );
        float     len    = RTGL1::Utils::Length( unnorm.data );
        triAreas[ t ]    = len * 0.5f;
        if( triAreas[ t ] > 1e-4f )
        {
            triValid[ t ]     = 1;
            triNormals[ t ]   = unnorm * ( 1.0f / len );
            triCentroids[ t ] = ( v[ 0 ] + v[ 1 ] + v[ 2 ] ) * ( 1.0f / 3.0f );
        }
    }

    std::unordered_map< uint32_t, std::vector< uint32_t > >             vertToTris;
    std::unordered_map< QuantizedEdgeKey, std::vector< uint32_t >, QuantizedEdgeHash > edgeToTris;
    vertToTris.reserve( vertMap.size() );
    edgeToTris.reserve( triCount * 3 );

    for( uint32_t t = 0; t < triCount; t++ )
    {
        if( !triValid[ t ] )
        {
            continue;
        }
        for( int e = 0; e < 3; e++ )
        {
            uint32_t a = triQ[ t * 3 + e ];
            uint32_t b = triQ[ t * 3 + ( e + 1 ) % 3 ];
            vertToTris[ a ].push_back( t );
            if( a > b )
            {
                std::swap( a, b );
            }
            edgeToTris[ QuantizedEdgeKey{ a, b } ].push_back( t );
        }
    }

    std::vector< uint32_t > parent( triCount );
    for( uint32_t t = 0; t < triCount; t++ )
    {
        parent[ t ] = t;
    }

    auto findRoot = [ &parent ]( uint32_t x ) {
        while( parent[ x ] != x )
        {
            parent[ x ] = parent[ parent[ x ] ];
            x           = parent[ x ];
        }
        return x;
    };

    auto unite = [ &parent, &findRoot ]( uint32_t a, uint32_t b ) {
        uint32_t ra = findRoot( a );
        uint32_t rb = findRoot( b );
        if( ra != rb )
        {
            parent[ rb ] = ra;
        }
    };

    if( collapseCompactFixtures )
    {
        for( const auto& [ vq, list ] : vertToTris )
        {
            for( size_t i = 1; i < list.size(); i++ )
            {
                unite( list[ 0 ], list[ i ] );
            }
        }
    }
    else
    {
        for( const auto& [ edge, list ] : edgeToTris )
        {
            for( size_t i = 0; i < list.size(); i++ )
            {
                for( size_t j = i + 1; j < list.size(); j++ )
                {
                    if( VecDot( triNormals[ list[ i ] ], triNormals[ list[ j ] ] ) > 0.99f &&
                        std::abs( VecDot( VecSub( triCentroids[ list[ j ] ], triCentroids[ list[ i ] ] ),
                                          triNormals[ list[ i ] ] ) ) < 0.5f )
                    {
                        unite( list[ i ], list[ j ] );
                    }
                }
            }
        }
    }

    std::unordered_map< uint32_t, std::vector< uint32_t > > islands;
    for( uint32_t t = 0; t < triCount; t++ )
    {
        if( triValid[ t ] )
        {
            islands[ findRoot( t ) ].push_back( t );
        }
    }

    for( const auto& [ root, members ] : islands )
    {
        if( !collapseCompactFixtures )
        {
            EmitClusterSampleGroup( members,
                                    triVerts.data(),
                                    triCentroids.data(),
                                    triNormals.data(),
                                    triAreas.data(),
                                    oneGameUnitInMeters,
                                    subdivideElongated,
                                    false,
                                    outSamples );
            continue;
        }

        RgFloat3D bmin{ 1e30f, 1e30f, 1e30f };
        RgFloat3D bmax{ -1e30f, -1e30f, -1e30f };
        for( uint32_t idx : members )
        {
            for( int k = 0; k < 3; k++ )
            {
                const RgFloat3D& p = triVerts[ idx * 3 + k ];
                bmin.data[ 0 ]     = std::min( bmin.data[ 0 ], p.data[ 0 ] );
                bmin.data[ 1 ]     = std::min( bmin.data[ 1 ], p.data[ 1 ] );
                bmin.data[ 2 ]     = std::min( bmin.data[ 2 ], p.data[ 2 ] );
                bmax.data[ 0 ]     = std::max( bmax.data[ 0 ], p.data[ 0 ] );
                bmax.data[ 1 ]     = std::max( bmax.data[ 1 ], p.data[ 1 ] );
                bmax.data[ 2 ]     = std::max( bmax.data[ 2 ], p.data[ 2 ] );
            }
        }
        float diagMeters = RTGL1::Utils::Length( VecSub( bmax, bmin ).data ) * oneGameUnitInMeters;

        if( diagMeters <= 2.5f )
        {
            EmitClusterSampleGroup( members,
                                    triVerts.data(),
                                    triCentroids.data(),
                                    triNormals.data(),
                                    triAreas.data(),
                                    oneGameUnitInMeters,
                                    false,
                                    true,
                                    outSamples );
        }
        else
        {
            std::unordered_map< uint32_t, uint32_t > subParent;
            subParent.reserve( members.size() );
            for( uint32_t idx : members )
            {
                subParent[ idx ] = idx;
            }
            auto findSub = [ &subParent ]( uint32_t x ) {
                while( subParent[ x ] != x )
                {
                    subParent[ x ] = subParent[ subParent[ x ] ];
                    x              = subParent[ x ];
                }
                return x;
            };
            for( const auto& [ edge, list ] : edgeToTris )
            {
                for( size_t i = 0; i < list.size(); i++ )
                {
                    if( !subParent.contains( list[ i ] ) )
                    {
                        continue;
                    }
                    for( size_t j = i + 1; j < list.size(); j++ )
                    {
                        if( !subParent.contains( list[ j ] ) )
                        {
                            continue;
                        }
                        if( VecDot( triNormals[ list[ i ] ], triNormals[ list[ j ] ] ) > 0.99f &&
                            std::abs( VecDot(
                                VecSub( triCentroids[ list[ j ] ], triCentroids[ list[ i ] ] ),
                                triNormals[ list[ i ] ] ) ) < 0.5f )
                        {
                            uint32_t ra = findSub( list[ i ] );
                            uint32_t rb = findSub( list[ j ] );
                            if( ra != rb )
                            {
                                subParent[ rb ] = ra;
                            }
                        }
                    }
                }
            }
            std::unordered_map< uint32_t, std::vector< uint32_t > > subGroups;
            for( uint32_t idx : members )
            {
                subGroups[ findSub( idx ) ].push_back( idx );
            }
            for( const auto& [ sroot, smem ] : subGroups )
            {
                EmitClusterSampleGroup( smem,
                                        triVerts.data(),
                                        triCentroids.data(),
                                        triNormals.data(),
                                        triAreas.data(),
                                        oneGameUnitInMeters,
                                        false,
                                        true,
                                        outSamples );
            }
        }
    }
}

void ExtractPrimitiveLightPositions( const RgMeshInfo&                     mesh,
                                     const RgMeshPrimitiveInfo&            prim,
                                     std::vector< RTGL1::PositionNormal >& initial )
{
    assert( initial.empty() );

    std::vector< LightClusterSample > clusters;
    ExtractCoplanarLightClusters( mesh.transform,
                                  prim.pVertices,
                                  prim.vertexCount,
                                  prim.pIndices,
                                  prim.indexCount,
                                  1.0f,
                                  false,
                                  false,
                                  clusters );
    initial.reserve( clusters.size() );
    for( const LightClusterSample& c : clusters )
    {
        initial.push_back( RTGL1::PositionNormal{
            .position = c.position,
            .normal   = c.normal,
        } );
    }
}


bool PrepareFolder( const std::filesystem::path& gltfPath )
{
    using namespace RTGL1;

    auto folder = GetGltfFolder( gltfPath );

    auto createEmptyFoldersFor = [ &folder, &gltfPath ]() {
        // create empty folder for .gltf
        std::error_code ec;

        std::filesystem::create_directories( folder, ec );
        if( ec )
        {
            debug::Warning( "{}: std::filesystem::create_directories error: {}",
                            folder.string(),
                            ec.message() );
            return false;
        }
        assert( std::filesystem::is_directory( folder ) );

        // create junction folder to store original textures
        auto junction = GetOriginalTexturesFolder( gltfPath );

        // but there's no privilege to create symlinks,
        // so create a folder that contains texture copies from ovrd/mat or ovrd/matdev
#if 1
        std::filesystem::create_directories( junction, ec );
        if( ec )
        {
            debug::Warning( "{}: std::filesystem::create_directories error: {}",
                            folder.string(),
                            ec.message() );
            return false;
        }
        assert( std::filesystem::is_directory( folder ) );
#else
        auto ovrdTextures = ovrdFolder / TEXTURES_FOLDER_DEV;

        std::filesystem::create_directory_symlink( ovrdTextures, junction, ec );
        if( ec )
        {
            debug::Warning( "{}: std::filesystem::create_directory_symlink error: {}",
                            junction.string(),
                            ec.message() );
            return false;
        }
        assert( std::filesystem::is_symlink( junction ) );
        assert( std::filesystem::equivalent( ovrdTextures, junction ) );
#endif

        return true;
    };

    if( !std::filesystem::exists( folder ) )
    {
        return createEmptyFoldersFor();
    }

#ifdef RG_USE_SURFACE_WIN32
    {
        auto msg = std::format( "Folder already exists:\n{}\n\n"
                                "Are you sure you want to write ON TOP of its contents?",
                                std::filesystem::absolute( folder ).string() );

        int msgboxID = MessageBox(
            nullptr, msg.c_str(), "Overwrite folder", MB_ICONSTOP | MB_YESNO | MB_DEFBUTTON2 );

        return msgboxID == IDYES;
    }
#else
    debug::Warning( "{}: Folder already exists, overwrite disabled", folder.string() );
    return false;
#endif // RG_USE_SURFACE_WIN32
}

}

void RTGL1::GltfExporter::AddPrimitive( const RgMeshInfo&          mesh,
                                        const RgMeshPrimitiveInfo& primitive )
{
    if( Utils::IsCstrEmpty( mesh.pMeshName ) ||
        Utils::IsCstrEmpty( primitive.pPrimitiveNameInMesh ) )
    {
        debug::Warning( "Exporter requires mesh primitives to have pMeshName and "
                        "pPrimitiveNameInMesh specified. Ignoring primitive with ID: {} - {}",
                        mesh.uniqueObjectID,
                        primitive.primitiveIndexInMesh );
        return;
    }

    if( primitive.indexCount == 0 || primitive.pIndices == nullptr ||
        primitive.vertexCount == 0 || primitive.pVertices == nullptr )
    {
        debug::Warning( "Exporter doesn't support primitives without vertices or index buffer: "
                        "{} - {} (with ID: {} - {})",
                        mesh.pMeshName,
                        primitive.pPrimitiveNameInMesh,
                        mesh.uniqueObjectID,
                        primitive.primitiveIndexInMesh );
        return;
    }

    scene[ GltfMeshNode{
               mesh.pMeshName,
               mesh.transform,
           } ]
        .emplace_back( std::make_shared< DeepCopyOfPrimitive >( primitive ) );

    if( !Utils::IsCstrEmpty( primitive.pTextureName ) )
    {
        sceneMaterials.insert( primitive.pTextureName );
    }
}

void RTGL1::GltfExporter::AddPrimitiveLights( const RgMeshInfo&          mesh,
                                              const RgMeshPrimitiveInfo& primitive )
{
    if( primitive.pEditorInfo && primitive.pEditorInfo->attachedLightExists &&
        primitive.vertexCount > 0 && primitive.indexCount >= 3 && primitive.pVertices &&
        primitive.pIndices )
    {
        attachedLightPrimitives.push_back( AttachedLightPrimitive{
            .primitive = std::make_shared< DeepCopyOfPrimitive >( primitive ),
            .transform = mesh.transform,
            .lightInfo = primitive.pEditorInfo->attachedLight,
        } );
    }
}

void RTGL1::GltfExporter::ResolveAttachedPrimitiveLights()
{
    constexpr float goldSrcEngineLightScale   = 0.12f;
    constexpr float goldSrcAttachedLightScale = 0.55f;

    for( GenericLight& gl : sceneLights )
    {
        if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
        {
            sph->intensity *= goldSrcEngineLightScale;
        }
        else if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &gl ) )
        {
            sp->intensity *= goldSrcEngineLightScale;
        }
    }

    if( attachedLightPrimitives.empty() )
    {
        return;
    }

    std::vector< WorldTri > worldTris;
    for( const auto& [ meshNode, prims ] : scene )
    {
        for( const auto& prim : prims )
        {
            auto verts = prim->Vertices();
            auto inds  = prim->Indices();
            for( size_t t = 0; t + 2 < inds.size(); t += 3 )
            {
                WorldTri wt{};
                for( int k = 0; k < 3; k++ )
                {
                    const float* p = verts[ inds[ t + k ] ].position;
                    wt.v[ k ] = Utils::ApplyTransform( meshNode.transform,
                                                       RgFloat3D{ RG_ACCESS_VEC3( p ) } );
                }
                wt.bmin = {
                    std::min( { wt.v[ 0 ].data[ 0 ], wt.v[ 1 ].data[ 0 ], wt.v[ 2 ].data[ 0 ] } ),
                    std::min( { wt.v[ 0 ].data[ 1 ], wt.v[ 1 ].data[ 1 ], wt.v[ 2 ].data[ 1 ] } ),
                    std::min( { wt.v[ 0 ].data[ 2 ], wt.v[ 1 ].data[ 2 ], wt.v[ 2 ].data[ 2 ] } ),
                };
                wt.bmax = {
                    std::max( { wt.v[ 0 ].data[ 0 ], wt.v[ 1 ].data[ 0 ], wt.v[ 2 ].data[ 0 ] } ),
                    std::max( { wt.v[ 0 ].data[ 1 ], wt.v[ 1 ].data[ 1 ], wt.v[ 2 ].data[ 1 ] } ),
                    std::max( { wt.v[ 0 ].data[ 2 ], wt.v[ 1 ].data[ 2 ], wt.v[ 2 ].data[ 2 ] } ),
                };
                worldTris.push_back( wt );
            }
        }
    }

    float maxDesiredOffset     = 0.1f / oneGameUnitInMeters;
    float maxDesiredOmniOffset = 0.2f / oneGameUnitInMeters;
    float minOffset            = 0.01f / oneGameUnitInMeters;
    float fixtureMatchSq       = ( 1.25f / oneGameUnitInMeters ) * ( 1.25f / oneGameUnitInMeters );
    float closeSpotSq          = ( 0.75f / oneGameUnitInMeters ) * ( 0.75f / oneGameUnitInMeters );
    float candDedupSq          = ( 0.85f / oneGameUnitInMeters ) * ( 0.85f / oneGameUnitInMeters );

    std::vector< GenericLight > addedLights;

    for( const AttachedLightPrimitive& item : attachedLightPrimitives )
    {
        auto verts = item.primitive->Vertices();
        auto inds  = item.primitive->Indices();

        std::vector< LightClusterSample > clusters;
        ExtractCoplanarLightClusters( item.transform,
                                      verts.data(),
                                      uint32_t( verts.size() ),
                                      inds.data(),
                                      uint32_t( inds.size() ),
                                      oneGameUnitInMeters,
                                      false,
                                      true,
                                      clusters );

        for( const LightClusterSample& c : clusters )
        {
            if( c.isOmnidirectional )
            {
                RgFloat3D pos     = c.position;
                bool      hasHint = RTGL1::Utils::Length( c.normal.data ) > 0.5f;
                if( hasHint )
                {
                    float clearance = ComputeClearanceAlongNormal(
                        c.position + c.normal * minOffset,
                        c.normal,
                        maxDesiredOmniOffset * 2.5f,
                        worldTris );
                    float safeOffset = std::clamp( clearance * 0.4f, minOffset, maxDesiredOmniOffset );
                    pos              = c.position + c.normal * safeOffset;
                }

                bool hasForwardSpot = false;
                for( const GenericLight& existing : sceneLights )
                {
                    if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &existing ) )
                    {
                        RgFloat3D d = VecSub( sp->position, pos );
                        if( VecDot( d, d ) < candDedupSq &&
                            ( !hasHint || VecDot( sp->direction, c.normal ) > 0.3f ) &&
                            HasSegmentClearance( sp->position, pos, worldTris ) )
                        {
                            hasForwardSpot = true;
                            break;
                        }
                    }
                }
                if( hasForwardSpot )
                {
                    continue;
                }

                bool dupAdded = false;
                for( const GenericLight& added : addedLights )
                {
                    if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &added ) )
                    {
                        RgFloat3D d = VecSub( sph->position, pos );
                        if( VecDot( d, d ) < candDedupSq )
                        {
                            dupAdded = true;
                            break;
                        }
                    }
                    else if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &added ) )
                    {
                        RgFloat3D d = VecSub( sp->position, pos );
                        if( VecDot( d, d ) < candDedupSq )
                        {
                            dupAdded = true;
                            break;
                        }
                    }
                }
                if( dupAdded )
                {
                    continue;
                }

                RgLightExtraInfo preservedExtra{};
                std::erase_if( sceneLights, [ & ]( const GenericLight& gl ) {
                    if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
                    {
                        RgFloat3D d = VecSub( sph->position, pos );
                        if( VecDot( d, d ) < fixtureMatchSq &&
                            HasSegmentClearance( sph->position, pos, worldTris ) )
                        {
                            if( !preservedExtra.exists && sph->extra.exists )
                            {
                                preservedExtra = sph->extra;
                            }
                            return true;
                        }
                    }
                    else if( hasHint )
                    {
                        if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &gl ) )
                        {
                            RgFloat3D d = VecSub( sp->position, pos );
                            if( VecDot( d, d ) < fixtureMatchSq &&
                                VecDot( sp->direction, c.normal ) < -0.3f &&
                                HasSegmentClearance( sp->position, pos, worldTris ) )
                            {
                                if( !preservedExtra.exists && sp->extra.exists )
                                {
                                    preservedExtra = sp->extra;
                                }
                                return true;
                            }
                        }
                    }
                    return false;
                } );

                addedLights.emplace_back( RgSphericalLightUploadInfo{
                    .uniqueID     = 0,
                    .isExportable = true,
                    .extra        = preservedExtra,
                    .color        = item.lightInfo.color,
                    .intensity    = item.lightInfo.intensity * c.fluxWeight * goldSrcAttachedLightScale,
                    .position     = pos,
                    .radius       = 0.05f / oneGameUnitInMeters,
                } );
                continue;
            }

            float clearance  = ComputeClearanceAlongNormal(
                c.position + c.normal * minOffset, c.normal, maxDesiredOffset * 2.5f, worldTris );
            float safeOffset = std::clamp( clearance * 0.4f, minOffset, maxDesiredOffset );
            RgFloat3D pos    = c.position + c.normal * safeOffset;

            bool hasMatchingSpot = false;
            RgFloat3D matchedSpotPos{};
            for( const GenericLight& existing : sceneLights )
            {
                if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &existing ) )
                {
                    RgFloat3D d = VecSub( sp->position, pos );
                    if( VecDot( d, d ) < fixtureMatchSq &&
                        VecDot( sp->direction, c.normal ) > 0.5f &&
                        HasSegmentClearance( sp->position, pos, worldTris ) )
                    {
                        hasMatchingSpot = true;
                        matchedSpotPos  = sp->position;
                        break;
                    }
                }
            }

            if( hasMatchingSpot )
            {
                std::erase_if( sceneLights, [ & ]( const GenericLight& gl ) {
                    if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
                    {
                        RgFloat3D d = VecSub( sph->position, matchedSpotPos );
                        return VecDot( d, d ) < closeSpotSq &&
                               HasSegmentClearance( sph->position, matchedSpotPos, worldTris );
                    }
                    return false;
                } );
                continue;
            }

            bool dupAdded = false;
            for( const GenericLight& added : addedLights )
            {
                if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &added ) )
                {
                    RgFloat3D d = VecSub( sp->position, pos );
                    if( VecDot( d, d ) < candDedupSq &&
                        VecDot( sp->direction, c.normal ) > 0.5f )
                    {
                        dupAdded = true;
                        break;
                    }
                }
            }
            if( dupAdded )
            {
                continue;
            }

            RgLightExtraInfo preservedExtra{};
            float frontLimit = -0.3f / oneGameUnitInMeters;
            std::erase_if( sceneLights, [ & ]( const GenericLight& gl ) {
                if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
                {
                    RgFloat3D d = VecSub( sph->position, pos );
                    if( VecDot( d, d ) < fixtureMatchSq &&
                        VecDot( VecSub( sph->position, c.position ), c.normal ) > frontLimit &&
                        HasSegmentClearance( sph->position, pos, worldTris ) )
                    {
                        if( !preservedExtra.exists && sph->extra.exists )
                        {
                            preservedExtra = sph->extra;
                        }
                        return true;
                    }
                }
                return false;
            } );

            addedLights.emplace_back( RgSpotLightUploadInfo{
                .uniqueID     = 0,
                .isExportable = true,
                .extra        = preservedExtra,
                .color        = item.lightInfo.color,
                .intensity    = item.lightInfo.intensity * c.fluxWeight * goldSrcAttachedLightScale,
                .position     = pos,
                .direction    = c.normal,
                .radius       = 0.05f / oneGameUnitInMeters,
                .angleOuter   = RTGL1::Utils::DegToRad( 89 ),
                .angleInner   = RTGL1::Utils::DegToRad( 75 ),
            } );
        }
    }

    sceneLights.insert( sceneLights.end(), addedLights.begin(), addedLights.end() );

    float tinyFillSq = ( 0.45f / oneGameUnitInMeters ) * ( 0.45f / oneGameUnitInMeters );
    std::vector< RgFloat3D > spotPositions;
    for( const GenericLight& gl : sceneLights )
    {
        if( auto* sp = std::get_if< RgSpotLightUploadInfo >( &gl ) )
        {
            spotPositions.push_back( sp->position );
        }
    }
    if( !spotPositions.empty() )
    {
        std::erase_if( sceneLights, [ & ]( const GenericLight& gl ) {
            if( auto* sph = std::get_if< RgSphericalLightUploadInfo >( &gl ) )
            {
                for( const RgFloat3D& spPos : spotPositions )
                {
                    RgFloat3D d = VecSub( sph->position, spPos );
                    if( VecDot( d, d ) < tinyFillSq &&
                        HasSegmentClearance( sph->position, spPos, worldTris ) )
                    {
                        return true;
                    }
                }
            }
            return false;
        } );
    }

    attachedLightPrimitives.clear();
}

void RTGL1::GltfExporter::MakeLightsForPrimitiveDynamic(
    const RgMeshInfo&                          mesh,
    const RgMeshPrimitiveInfo&                 primitive,
    float                                      oneGameUnitInMeters,
    std::vector< PositionNormal >&             tempStorage,
    std::vector< RgSphericalLightUploadInfo >& resultStorage )
{
    assert( tempStorage.empty() );
    assert( resultStorage.empty() );

    if( primitive.pEditorInfo && primitive.pEditorInfo->attachedLightExists )
    {
        ExtractPrimitiveLightPositions( mesh, primitive, tempStorage );

        float radius = 0.05f / oneGameUnitInMeters;
        float offset = 0.05f / oneGameUnitInMeters;

        for( const auto& [ position, normal ] : tempStorage )
        {
            resultStorage.emplace_back( RgSphericalLightUploadInfo{
                .uniqueID     = 0,
                .isExportable = false,
                .extra        = {},
                .color        = primitive.pEditorInfo->attachedLight.color,
                .intensity    = primitive.pEditorInfo->attachedLight.intensity,
                .position     = position + normal * offset,
                .radius       = radius,
            } );
        }
    }
}

void RTGL1::GltfExporter::AddLight( const GenericLightPtr& light )
{
    constexpr auto unbox = []( const GenericLightPtr& ptr ) {
        return std::visit( []( auto&& specific ) -> GenericLight { return *specific; }, ptr );
    };

    sceneLights.push_back( unbox( light ) );
}

void RTGL1::GltfExporter::ExportToFiles( const std::filesystem::path& gltfPath,
                                         const TextureManager&        textureManager,
                                         const TextureMetaManager&    textureMeta,
                                         const std::filesystem::path& bspPath,
                                         std::string_view             mapName,
                                         const std::filesystem::path& scenesFolder )
{
    if( scene.empty() )
    {
        debug::Warning( "Nothing to export. Check uploaded primitives window" );
        return;
    }

    if( gltfPath.empty() )
    {
        debug::Warning( "Can't export: Destination path is empty" );
        return;
    }

    if( !PrepareFolder( gltfPath ) )
    {
        debug::Warning( "Denied to write to the folder {}",
                        std::filesystem::absolute( GetGltfFolder( gltfPath ) ).string() );
        return;
    }

    ResolveAttachedPrimitiveLights();

    {
        auto bspLights = BspLightEstimator::EstimateLights(
            bspPath, mapName, scenesFolder, textureMeta, sceneLights, oneGameUnitInMeters );
        sceneLights.insert( sceneLights.end(), bspLights.begin(), bspLights.end() );
    }


    const char* primExtrasExample  = nullptr; // "{ portalOutPosition\" : [0,0,0] }";
    const char* sceneExtrasExample = nullptr; // "{ tonemapping_enable\" : 1 }";


    debug::Info( "Export start..." );


    // lock pointers
    GltfBin fbin( gltfPath );

    if( !fbin.IsOpen() )
    {
        debug::Warning( "Can't export: failed to open file {}",
                        std::filesystem::absolute( GetGltfBinPath( gltfPath ) ).string() );
        return;
    }

    GltfStorage  storage( scene, sceneLights.size() );
    GltfTextures textureStorage(
        sceneMaterials, GetOriginalTexturesFolder( gltfPath ), textureManager );
    GltfLights lightStorage( sceneLights, storage.lightNodes );


    // for each RgMesh
    for( GltfRoot& root : storage.roots )
    {
        // for each RgMeshPrimitive
        for( size_t i = 0; i < root.source.size(); i++ )
        {
            const DeepCopyOfPrimitive& rgprim = *root.source[ i ];

            std::span viewsDst( root.bufferViews.begin() + ptrdiff_t( BufferViewsPerPrim * i ),
                                BufferViewsPerPrim );
            {
                std::ranges::move( MakeBufferViews( fbin, rgprim ), viewsDst.begin() );
            }

            std::span accessorsDst( root.accessors.begin() + ptrdiff_t( AccessorsPerPrim * i ),
                                    AccessorsPerPrim );
            {
                std::ranges::move(
                    MakeAccessors( rgprim.Vertices().size(), rgprim.Indices().size(), viewsDst ),
                    accessorsDst.begin() );
            }

            std::span vertAttrsDst( root.attributes.begin() + ptrdiff_t( AttributesPerPrim * i ),
                                    AttributesPerPrim );
            {
                std::ranges::move( MakeVertexAttributes( accessorsDst ), vertAttrsDst.begin() );
            }

            root.materials[ i ] = MakeMaterial( rgprim, textureStorage );

            root.primitives[ i ] = cgltf_primitive{
                .type             = cgltf_primitive_type_triangles,
                .indices          = GetIndicesAccessor( accessorsDst ),
                .material         = &root.materials[ i ],
                .attributes       = std::data( vertAttrsDst ),
                .attributes_count = std::size( vertAttrsDst ),
                .extras           = {},
            };
        }

        *root.mesh = cgltf_mesh{
            .name             = const_cast< char* >( root.name.c_str() ),
            .primitives       = std::data( root.primitives ),
            .primitives_count = std::size( root.primitives ),
            .extras           = {},
        };

        *root.thisNode = cgltf_node{
            .name                    = const_cast< char* >( root.name.c_str() ),
            .parent                  = nullptr, /* later */
            .children                = nullptr,
            .children_count          = 0,
            .mesh                    = root.mesh,
            .camera                  = nullptr,
            .light                   = nullptr,
            .has_matrix              = true,
            .matrix                  = RG_TRANSFORM_TO_GLTF_MATRIX( root.transform ),
            .extras                  = { .data = const_cast< char* >( primExtrasExample ) },
            .has_mesh_gpu_instancing = false,
            .mesh_gpu_instancing     = {},
        };
    }

    // main root node
    {
        *storage.world = cgltf_node{
            .name           = const_cast< char* >( RTGL1_MAIN_ROOT_NODE ),
            .parent         = nullptr,
            .children       = std::data( storage.worldChildren ),
            .children_count = std::size( storage.worldChildren ),
            .has_matrix     = true,
            .matrix         = RG_TRANSFORM_TO_GLTF_MATRIX( worldTransform ),
            .extras         = { .data = nullptr },
        };
        for( cgltf_node* child : storage.worldChildren )
        {
            child->parent = storage.world;
        }
    }

    cgltf_scene gltfScene = {
        .name        = const_cast< char* >( "default" ),
        .nodes       = &storage.world,
        .nodes_count = 1,
        .extras      = { .data = const_cast< char* >( sceneExtrasExample ) },
    };

    // Note: const_cast, assuming that cgltf_write_file makes only read-only access
    cgltf_data data = {
        .asset =
            cgltf_asset{
                .copyright   = nullptr,
                .generator   = const_cast< char* >( "RTGL1" ),
                .version     = const_cast< char* >( "2.0" ),
                .min_version = nullptr,
            },
        .meshes             = std::data( storage.allMeshes ),
        .meshes_count       = std::size( storage.allMeshes ),
        .materials          = std::data( storage.allMaterials ),
        .materials_count    = std::size( storage.allMaterials ),
        .accessors          = std::data( storage.allAccessors ),
        .accessors_count    = std::size( storage.allAccessors ),
        .buffer_views       = std::data( storage.allBufferViews ),
        .buffer_views_count = std::size( storage.allBufferViews ),
        .buffers            = fbin.Get(),
        .buffers_count      = 1,
        .images             = std::data( textureStorage.Images() ),
        .images_count       = std::size( textureStorage.Images() ),
        .textures           = std::data( textureStorage.Textures() ),
        .textures_count     = std::size( textureStorage.Textures() ),
        .samplers           = std::data( textureStorage.Samplers() ),
        .samplers_count     = std::size( textureStorage.Samplers() ),
        .lights             = std::data( lightStorage.Lights() ),
        .lights_count       = std::size( lightStorage.Lights() ),
        .nodes              = std::data( storage.allNodes ),
        .nodes_count        = std::size( storage.allNodes ),
        .scenes             = &gltfScene,
        .scenes_count       = 1,
        .scene              = &gltfScene,
    };

    cgltf_options options = {};
    cgltf_result  r;

    r = cgltf_validate( &data );
    if( r != cgltf_result_success )
    {
        debug::Warning( "cgltf_validate fail: {}", int( r ) );
        return;
    }

    r = cgltf_write_file( &options, gltfPath.string().c_str(), &data );
    if( r != cgltf_result_success )
    {
        debug::Warning( "cgltf_write_file fail: {}", int( r ) );
        return;
    }

    debug::Info( "Export successful: {}",
                 std::filesystem::absolute( GetGltfFolder( gltfPath ) ).string() );
}

void RTGL1::GltfExporter::ExportTexturesOnly( const std::filesystem::path& gltfPath,
                                              const TextureManager&        textureManager,
                                              bool                         overwriteExisting )
{
    if( sceneMaterials.empty() )
    {
        debug::Warning( "Nothing to export. Check uploaded primitives window" );
        return;
    }

    if( gltfPath.empty() )
    {
        debug::Warning( "Can't export: Destination path is empty" );
        return;
    }

    if( !PrepareFolder( gltfPath ) )
    {
        debug::Warning( "Denied to write to the folder {}",
                        std::filesystem::absolute( GetGltfFolder( gltfPath ) ).string() );
        return;
    }

    debug::Info( "Exporting textures..." );

    const auto texturesFolder = GetOriginalTexturesFolder( gltfPath );

    for( const auto& materialName : sceneMaterials )
    {
        if( materialName.empty() )
        {
            continue;
        }

        textureManager.ExportMaterialTextures(
            materialName.c_str(), texturesFolder, overwriteExisting );
    }

    debug::Info( "Textures export successful: {}",
                 std::filesystem::absolute( texturesFolder ).string() );
}

bool RTGL1::GltfMeshNode::operator==( const GltfMeshNode& other ) const
{
    return this->name == other.name && TransformsAreEqual( this->transform, other.transform );
}

bool RTGL1::GltfMeshNode::operator<( const GltfMeshNode& other ) const
{
    if( this->name != other.name )
    {
        return this->name < other.name;
    }
    return TransformIsLess( this->transform, other.transform );
}

uint64_t RTGL1::GltfMeshNode::Hash() const
{
    uint64_t h = 0;
    HashCombine( h, name );
    HashCombine( h, transform );

    return h;
}
