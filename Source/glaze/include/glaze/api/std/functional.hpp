// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <functional>

#include "glaze/api/name.hpp"
#include "glaze/core/common.hpp"

namespace glz
{
   namespace detail
   {
      template <class T>
      struct function_traits;

      template <class R, class... Args>
      struct function_traits<std::function<R(Args...)>>
      {
         static constexpr size_t N = sizeof...(Args);
         using result_type = R;
         using arguments = std::tuple<Args...>;
      };
      
      template <const std::string_view& Str, class Tuple, size_t I = 0>
      struct expander
      {
         static constexpr auto impl() noexcept {
            const auto N = std::tuple_size_v<Tuple>;
            if constexpr (I >= N) {
               return Str;
            }
            else if constexpr (I == N - 1) {
               return expander<detail::join_v<Str, name_v<std::tuple_element_t<I, Tuple>>>, Tuple, I + 1>::value;
            }
            else {
               return expander<detail::join_v<Str, name_v<std::tuple_element_t<I, Tuple>>, chars<",">>, Tuple, I + 1>::value;
            }
         }
         
         static constexpr std::string_view value = impl();
      };
      
      template <const std::string_view& Str, class Tuple>
      inline constexpr std::string_view expander_v = expander<Str, Tuple>::value;
   }
   
   template <class T>
   concept function = is_specialization_v<T, std::function>;
   
   template <function T>
   struct meta<T> {
      static constexpr std::string_view name = "std::function";
   };
}
