//===--- FileSystemProvider.h - ---------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SOURCEKITD_FILESYSTEMPROVIDER_H
#define LLVM_SOURCEKITD_FILESYSTEMPROVIDER_H

#include "clang/Basic/InMemoryOutputFileSystem.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"

namespace SourceKit {

/// Subsequent requests will write temporary output files to this filesystem
/// rather than to the real filesystem.
///
/// Is not threadsafe.
///
/// \param FS may be null, which makes subsequent requests start writing
/// temporary output files to the real filesystem again.
void setGlobalInMemoryOutputFileSystem(
    llvm::IntrusiveRefCntPtr<clang::InMemoryOutputFileSystem> FS);

} // namespace SourceKit

#endif
