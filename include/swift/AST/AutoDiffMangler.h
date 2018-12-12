//===--- ASTMangler.h - AutoDiff symbol mangling ----------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef __SWIFT_AST_AUTODIFFMANGLER_H
#define __SWIFT_AST_AUTODIFFMANGLER_H

namespace swift {
namespace Mangle {

// I am not sure how this should work. The problems are
// - ad pass does not have access to the original decls (I think) so I can't
//   really mangle them together with the new stuff. I guess I have to demangle
//   and remangle or something terrible like that?
// - the associated functions are like modifiers on the original functions so
//   even for them I'd kind of like to be able to inject myself into the
//   mangling process of the original function and add the right stuff at the
//   right time. can I do that in a totally separate class, or should I like add
//   a flag or something to the relevant methods in the ASTMangler that makes
//   them add mangled stuff ?

class AutoDiffMangler {
public:

}

} // end namespace Mangle
} // end namespace swift

#endif // __SWIFT_AST_AUTODIFFMANGLER_H
