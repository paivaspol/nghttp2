/*
 * Custom class for reading dependencies from a text file.
 */
#ifndef DEP_READER_H
#define DEP_READER_H

#include <deque>
#include <string>

#include <nghttp2/nghttp2.h>

namespace shrpx {

class DependencyReader {

public:
  DependencyReader();
  ~DependencyReader();

  void hello_world();
  /*
   * Returns a deque containing the dependencies.
   */
  std::deque<std::string> ReadDependencies();

  /*
   * Returns a nghttp2_data_provider containing the dependencies.
   */
  nghttp2_data_provider GetDependenciesDataProvider();

  /*
   * Returns the size of the dependencies.
   */
  size_t GetDependenciesRaw(uint8_t *buf);
};

ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data);

} // shrpx

#endif // DEP_READER_H
