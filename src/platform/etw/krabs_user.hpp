#pragma once

// krabsetw for user-mode traces: krabs.hpp in its own order, without kt.hpp, client.hpp and
// kernel_providers.hpp (kernel traces, unused here). kt.hpp does not compile with clang, which
// the lint uses, and krabs's headers compile only in this order, after windows.h.
// clang-format off
#include <windows.h>
#include <krabs/compiler_check.hpp>
#include <krabs/ut.hpp>
#include <krabs/guid.hpp>
#include <krabs/trace.hpp>
#include <krabs/trace_context.hpp>
#include <krabs/errors.hpp>
#include <krabs/schema.hpp>
#include <krabs/schema_locator.hpp>
#include <krabs/parse_types.hpp>
#include <krabs/collection_view.hpp>
#include <krabs/size_provider.hpp>
#include <krabs/parser.hpp>
#include <krabs/property.hpp>
#include <krabs/provider.hpp>
#include <krabs/etw.hpp>
#include <krabs/tdh_helpers.hpp>
// clang-format on
