// RUN: %empty-directory(%t)
// RUN: %target-build-swift %S/Inputs/external_module_default_implementation.swift -parse-as-library -emit-module -o %t/A.swiftmodule
// RUN: %target-build-swift %S/Inputs/external_module_default_implementation.swift -parse-as-library -emit-object -o %t/A.o
// RUN: %target-build-swift %s %t/A.o -I %t -o %t/main.out
// RUN: %target-run %t/main.out

// REQUIRES: executable_test

import A

struct S: P {}
