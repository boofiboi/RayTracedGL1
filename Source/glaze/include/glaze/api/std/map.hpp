// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <map>

#include "glaze/api/name.hpp"

namespace glz
{
   template <class T>
   concept map = is_specialization_v<T, std::map>;
   
   template <map T>
   struct meta<T> {
      using Key = typename T::key_type;
      using V = typename T::mapped_type;
      static constexpr std::string_view name = "std_container";
   };
}

