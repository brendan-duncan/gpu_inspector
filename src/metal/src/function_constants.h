// The function constants an application specialized a shader with.
//
// `MTLFunctionConstantValues` is write-only: it has `setConstantValue:type:atIndex:` and its
// siblings and no getter at all, and `MTLFunction` does not say what it was built with either. So
// the only way to know is to watch the values go in — the setters are hooked and what they carry
// is kept in a side table keyed by the object's pointer, the same shape as everything else in this
// library (see swizzle.h, "the objects themselves are never wrapped").
//
// This matters for the shader debugger rather than for the object list. A Unity shader is one
// library of `[[function_constant(n)]]`-guarded variants, and running it with the constants at
// their defaults steps the wrong branches — often a shader that reads no textures at all. With the
// values recorded, the debugged invocation is the variant the draw actually used.
//
// Unlike the driver's private classes, `MTLFunctionConstantValues` is a public class of
// Metal.framework, so it is looked up by name (`objc_getClass`) the way `CAMetalLayer` and
// `NSScreen` are elsewhere here: there is no object to discover it from, because the application
// has already finished setting the values by the time one reaches a hook.
#pragma once

#include <string>

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Hooks `MTLFunctionConstantValues`, once. Called when a library class is first hooked, which is
 * before any application can have created a specialized function from it.
 */
void HookFunctionConstantValues();

/**
 * The constants set on `values`, as the JSON array a tracked `MTLFunction` carries in
 * `constantValues`: `[{ "index": 0, "name": "USE_SHADOWS", "type": "bool", "value": true }]`.
 * Empty for nil, for an object whose setters were never seen, or for one that set nothing.
 *
 * The value is written decoded rather than as bytes, because every type a function constant may
 * have is a scalar or a short vector of one — there is no layout to get wrong, and the UI can use
 * it without a decoder of its own.
 */
std::string FunctionConstantsJson(id values);

/**
 * Remembers the values object a function was specialized with, retained, so the function can be
 * built again from *different* source with the same specialization — which is what shader editing
 * needs (shader_edit.h). Nothing else could supply it: `MTLFunctionConstantValues` has no getters,
 * and a function does not carry what it was built with, so an edit that forgot this would compile
 * the shader with every `[[function_constant]]` at its default and quietly draw another variant.
 *
 * Keyed by the function, and retained rather than weak: a values object is a bag of scalars, worth
 * a few dozen bytes, and the application usually drops it the moment the function is made.
 */
void RememberFunctionConstants(id function, id values);

/** The values `function` was specialized with, or nil. Not retained for the caller. */
id FunctionConstantsOf(id function);

}  // namespace mtlinsp
