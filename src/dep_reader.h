/*
 * Custom class for reading dependencies from a text file.
 */
#ifndef DEP_READER_H
#define DEP_READER_H

#include <deque>
#include <functional>
#include <future>
#include <string>
#include <thread>

#include <nghttp2/nghttp2.h>

namespace shrpx {

class DependencyReader {

 public:
  DependencyReader();
  DependencyReader(std::string dependency_filename);
  ~DependencyReader();

  void SetDependencyTreeFilename(std::string dependency_filename);

  /*
   * Starts the DependencyReader, but all the callbacks are not enabled unless 
   * the reader is enabled to return the dependencies.
   * Returns |true| when the url has dependencies toresolve.
   * Otherwise, return |false|
   */
  void Start(std::string website);

  /*
   * Enables the DependencyReader to notify about the dependencies via the callback.
   */
  bool StartReturningDependencies(std::string url);

  void set_on_new_dependency_callback(
      std::function<void(std::string, int32_t)> on_new_dependency_callback) {
    on_new_dependency_callback_ = on_new_dependency_callback;
  };

  /*
   * Returns the number of dependencies remanining.
   */
  uint32_t num_dependencies_remaining(const std::string url);

  /*
   * Returns whether the reader still _currently_ have dependencies.
   */
  bool still_have_dependencies(const std::string url);

  /*
   * Registers to the reader that someone is expecting to get dependencies.
   */
  void RegisterForGettingDependencies(const std::string url, int32_t stream_id);

  /*
   * Returns a nghttp2_data_provider containing the dependencies.
   */
  nghttp2_data_provider GetDependenciesDataProvider(const std::string url);

 private:
  const std::string kHttpPrefix = "http://";
  const std::string kHttpsPrefix = "https://";
  const std::string kWwwPrefix = "www.";
  const std::string kDependencyTreeFilename = "dependency_tree.txt";
  const std::string kDelimeter = "/";

  std::string dependency_filename_;

  // Returns a map containing the dependency tree.
  std::map<std::string, std::deque<std::pair<std::string, std::string>>>
    ReadAndGenerateDependencyTree(const std::string dependency_directory,
                                  const std::string url);

  // Tokenize the line into a tuple, where the first argument of the pair contains
  // parent, the second argument of the pair contains the child, and the third
  // argument contains the origin.
  std::tuple<std::string, std::string, std::string> TokenizeTree(std::string line);

  // Returns an escaped url i.e. removed protocol, www.
  std::string EscapeURL(std::string url);

  // Removes the trailing slash from the url.
  std::string RemoveTrailingSlash(std::string url);

  // The directory base that contains the dependency tree.
  std::string base_dependency_directory_;

  // Callback function for when receiving new dependencies.
  std::function<void(std::string, int32_t)> on_new_dependency_callback_;

  // Tree structure:
  //  - Origin (Domain) : Deque<String> (Children of Domain)>
  std::map<std::string, std::deque<std::pair<std::string, std::string>>> 
    dependencies_;

  // A structure for storing the number of remaining outstanding dependencies.
  std::map<std::string, std::deque<std::pair<std::string, std::string>>> 
    outstanding_dependencies_;
  std::map<std::string, int32_t> outstanding_dependencies_to_stream_id_;

  // Mapping from the URL to whether the reader can start notifying the upstream of
  // the dependencies.
  std::map<std::string, bool> can_start_notifying_upstream_; 

};

/*
 * Implementation of the dependency_read_callback.
 */
ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data);

} // shrpx

#endif // DEP_READER_H
