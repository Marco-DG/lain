#ifndef PREDEF_H
#define PREDEF_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt


       Pre-defined compiler macros
*/

#include "predef/architecture/sparc.h"

#include "predef/attribute/aligned.h"
#include "predef/attribute/always_inline.h"
#include "predef/attribute/const.h"
#include "predef/attribute/constructor.h"
#include "predef/attribute/fallthrough.h"
#include "predef/attribute/inline.h"
#include "predef/attribute/malloc.h"
#include "predef/attribute/maybe_unused.h"
#include "predef/attribute/nodiscard.h"
#include "predef/attribute/noescape.h"
#include "predef/attribute/noinline.h"
#include "predef/attribute/nonnull.h"
#include "predef/attribute/noreturn.h"
#include "predef/attribute/nothrow.h"
#include "predef/attribute/pure.h"
#include "predef/attribute/returns_nonnull.h"

#define aligned                         ATTRIBUTE_ALIGNED

/* The function will always be inlined, even if the compiler thinks doing so would be a bad idea. */
#define _always_inline                  ATTRIBUTE_ALWAYS_INLINE

/* The function has no side-effects, the return value depends only on the parameters, pointer arguments are not allowed. */
#define _const_return                   ATTRIBUTE_CONST

/* The function is automatically called when the containing header file is included. */
#define _constructor                    ATTRIBUTE_CONSTRUCTOR

/* Explicitly tell the compiler to fall through a case in the switch statement.
   Without this, some compilers may think that a "break; as been unintentionally omitted and emit a diagnostic. */
#define fallthrough                    ATTRIBUTE_FALLTHROUGH

/* Hint the compiler to inline the function. */
#define inline                          ATTRIBUTE_INLINE

/* Suppresses warnings on unused entities. */
#define maybe_unused                    ATTRIBUTE_MAYBE_UNUSED

/* The function does not return a pointer aliased by any other pointer, there are no pointers to valid objects in the storage pointed to. */
#define noalias                         ATTRIBUTE_MALLOC

/* The compiler should emit a diagnostic if the function return value is discarded without being checked. */
#define nodiscard                       ATTRIBUTE_NODISCARD

/* The pointer will not escape the function call.
   That is, no reference to the object the pointer points to that is derived from the parameter value will survive after the function returns.
   See: https://clang.llvm.org/docs/AttributeReference.html#noescape */
#define noescape                        ATTRIBUTE_NOESCAPE

/* The function will never be inlined, even if the compiler thinks doing so would be a good idea. */
#define _noinline                       ATTRIBUTE_NOINLINE

/* The function will always return a non-null value.
   This may allow the compiler to skip some null-pointer checks. */
#define nonnull_return                  ATTRIBUTE_RETURNS_NONNULL

/* List parameters which will never be NULL.
   If no argument index list is given to the nonnull attribute, all pointer arguments are marked as non-null. */
#define nonnull(...)                    ATTRIBUTE_NONNULL(__VA_ARGS__)

/* The function does not return. */
#define _noreturn                       ATTRIBUTE_NORETURN

/* The function will never throw a C++ exception. */
#define _nothrow                        ATTRIBUTE_NOTHROW

/* The function has no side-effects, the return value depends only on the parameters and/or global variables. */
#define _pure_return                    ATTRIBUTE_PURE

//#define expr                           _nodiscard static _nothrow _const_return
//#define func                           _nodiscard static _nothrow _pure_return
//#define proc                           _nodiscard static _nothrow
//#define proc_void                      static _nothrow

#include "predef/builtin/likely.h"
#include "predef/builtin/unlikely.h"
#include "predef/builtin/expect.h"

#define expect(expr, value)             BUILTIN_EXPECT(expr, value)
#define likely(expr)                    BUILTIN_LIKELY(expr)
#define unlikely(expr)                  BUILTIN_UNLIKELY(expr)

#include "predef/compiler/feature/has_attribute.h"
#include "predef/compiler/feature/has_builtin.h"
#include "predef/compiler//clang.h"
#include "predef/compiler//gcc.h"
#include "predef/compiler//ibm.h"

#include "predef/language/c.h"
#include "predef/language/cpp.h"

#include "predef/operator/alignas.h"
#include "predef/operator/alignof.h"
#include "predef/operator/reinterpret_cast.h"
#include "predef/operator/static_cast.h"

#define alignas(expr_or_type)           OPERATOR_ALIGNAS(expr_or_type)
#define alignof(type)                   OPERATOR_ALIGNOF(type)
#define reinterpret_cast(type, expr)    OPERATOR_REINTERPRET_CAST(type, expr)
#define static_cast(type, expr)         OPERATOR_STATIC_CAST(type, expr)

#include "predef/os/linux.h"
#include "predef/os/windows.h"

#include "predef/specifier/constexpr.h"
#include "predef/specifier/restrict.h"

#define constexpr                      ATTRIBUTE_CONSTEXPR

/* The pointer is not aliased. */
#define restrict                       ATTRIBUTE_RESTRICT


#include "predef/translation/current_function.h"

#define CURR_FILE                       __FILE__
#define CURR_FUNC                       TRANSLATION_CURRENT_FUNCTION
#define CURR_LINE                       __LINE__

#include "predef/wordsize.h"

#endif /* PREDEF_H */