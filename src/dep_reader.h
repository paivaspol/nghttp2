/*
 * Custom class for reading dependencies from a text file.
 */
#ifndef DEP_READER_H
#define DEP_READER_H

#include <deque>
#include <functional>
#include <string>

#include <nghttp2/nghttp2.h>

namespace shrpx {

class DependencyReader {

 public:
  DependencyReader();
  ~DependencyReader();

  /*
   * Starts the DependencyReader, but all the callbacks are not enabled unless 
   * the reader is enabled to return the dependencies.
   */
  void Start();

  /*
   * Enables the DependencyReader to notify about the dependencies via the callback.
   */
  void StartReturningDependencies();

  std::string url() { return url_; };
  void set_url(std::string url) { url_ = url; };

  void set_on_new_dependency_callback(
      std::function<void(void)> on_new_dependency_callback) {
    on_new_dependency_callback_ = on_new_dependency_callback;
  };

  void set_on_all_dependencies_discovered(
      std::function<void(void)> on_all_dependencies_discovered) {
    on_all_dependencies_discovered_ = on_all_dependencies_discovered;
  };

  int32_t stream_id() { return stream_id_; };
  void set_stream_id(int32_t stream_id) { stream_id_ = stream_id; };

  /*
   * Returns a deque containing the dependencies.
   */
  std::deque<std::string> *ReadDependencies();

  /*
   * Returns a nghttp2_data_provider containing the dependencies.
   */
  nghttp2_data_provider GetDependenciesDataProvider();

  /*
   * Returns the size of the dependencies.
   */
  size_t GetDependenciesRaw(uint8_t *buf);
 
  bool can_start_notifying_upstream_; 
  int32_t stream_id_;
  std::deque<std::string> dependencies_;
  std::string url_;
  std::function<void(void)> on_new_dependency_callback_;
  std::function<void(void)> on_all_dependencies_discovered_;
};

/*
 * Implementation of the dependency_read_callback.
 */
ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data);

} // shrpx

#endif // DEP_READER_H
