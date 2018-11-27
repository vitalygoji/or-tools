// Copyright 2010-2018 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// SWIG Macros to use std::vector<Num> and const std::vector<Num>& in Java,
// where Num is an atomic numeric type.
//
// Normally we'd simply use %include "std_vector.i" with the %template
// directive (see http://www.swig.org/Doc1.3/Library.html#Library_nn15), but
// in google3 we can't, because exceptions are forbidden.
//
// TODO(user): move to base/swig/java.

%include "ortools/base/base.i"

%{
#include <vector>
#include "ortools/base/integral_types.h"
%}

%define VECTOR_AS_CSHARP_ARRAY(TYPE, CTYPE, CSHARPTYPE)
%typemap(ctype)    const std::vector<TYPE>&  %{ int length$argnum, CTYPE* %}
%typemap(imtype)   const std::vector<TYPE>&  %{ int length$argnum, CSHARPTYPE[] %}
%typemap(cstype)   const std::vector<TYPE>&  %{ CSHARPTYPE[] %}
%typemap(csin)     const std::vector<TYPE>&  "$csinput.Length, $csinput"
%typemap(freearg)  const std::vector<TYPE>&  { delete $1; }
%typemap(in)       const std::vector<TYPE>&  %{
  $1 = new std::vector<TYPE>;
  $1->reserve(length$argnum);
  for(int i = 0; i < length$argnum; ++i) {
    $1->emplace_back($input[i]);
  }
%}
%enddef // VECTOR_AS_CSHARP_ARRAY

%define MATRIX_AS_CSHARP_ARRAY_IN1(TYPE, CTYPE, CSHARPTYPE)
%typemap(ctype)  const std::vector<std::vector<TYPE> >&  %{
  int len$argnum_1, int len$argnum_2[], CTYPE*
%}
%typemap(imtype) const std::vector<std::vector<TYPE> >&  %{
  int len$argnum_1, int[] len$argnum_2, CSHARPTYPE[]
%}
%typemap(cstype) const std::vector<std::vector<TYPE> >&  %{ CSHARPTYPE[][] %}
%typemap(csin)   const std::vector<std::vector<TYPE> >&  "$csinput.GetLength(0), NestedArrayHelper.GetArraySecondSize($csinput), NestedArrayHelper.GetFlatArray($csinput)"
%typemap(in)     const std::vector<std::vector<TYPE> >&  (std::vector<std::vector<TYPE> > result) %{

  result.clear();
  result.resize(len$argnum_1);

  TYPE* inner_array = reinterpret_cast<TYPE*>($input);

  int actualIndex = 0;
  for (int index1 = 0; index1 < len$argnum_1; ++index1) {
    result[index1].reserve(len$argnum_2[index1]);
    for (int index2 = 0; index2 < len$argnum_2[index1]; ++index2) {
      const TYPE value = inner_array[actualIndex];
      result[index1].emplace_back(value);
      actualIndex++;
    }
  }

  $1 = &result;
%}
%enddef // MATRIX_AS_CSHARP_ARRAY_IN1
