/*
 * Custom class for reading dependencies from a text file.
 */
#ifndef DEP_READER_H
#define DEP_READER_H

#include "shrpx.h"

namespace shrpx {

class DependencyReader {

public:
  DependencyReader();
  ~DependencyReader();

  void hello_world();

};

} // shrpx

#endif // DEP_READER_H
