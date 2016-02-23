#include <iostream>
#include <fstream>
#include <sstream>
#include <deque>

#include "dep_reader.h"

namespace shrpx {
DependencyReader::DependencyReader() {
}

DependencyReader::~DependencyReader() {

}

void DependencyReader::hello_world() {
  std::cout << "Hello World!" << std::endl;
}

std::deque<std::string> DependencyReader::ReadDependencies() {
  std::deque<std::string> result;
  std::ifstream infile("temp.txt");
  std::string line;
  while (std::getline(infile, line)) {
    result.push_back(line);
  } 
  return result;
}

size_t DependencyReader::GetDependenciesRaw(uint8_t *buf) {
  std::deque<std::string> dependencies = ReadDependencies();
  if (!dependencies.empty()) {
    std::string result_str = "";
    size_t cumulative_size_read = 0;
    for (std::deque<std::string>::iterator it = dependencies.begin();
        it != dependencies.end(); it++) {
      std::string dependency = *it;
      dependencies.pop_front();
      result_str += dependency + "\n";
      cumulative_size_read += dependency.size() + 1;
    }

    /* Pack result string to uint8_t format. */
    char *cstr = new char[result_str.length() + 1];
    std::strcpy(cstr, result_str.c_str());
    buf = reinterpret_cast<uint8_t *>(cstr);
    return cumulative_size_read + 1; // Account for the NULL terminal.
  } else {
    return 0;
  }
}

nghttp2_data_provider DependencyReader::GetDependenciesDataProvider() {
  nghttp2_data_provider provider;
  std::deque<std::string> dependencies = ReadDependencies();
  provider.source.ptr = &dependencies;
  provider.read_callback = dependency_read_callback;
  return provider;
}

ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data) {
  std::cout << "[dep_reader.cc] READ CALLBACK" << std::endl;
  auto dependencies = static_cast<std::deque<std::string> *>(source->ptr);
  if (!dependencies->empty()) {
    /*
     * TODO: Can optimize using a greedy approach where
     * the list is sorted by the length of the dependency URL.
     */
    size_t cumulative_size_read = dependencies->front().size();
    size_t length_left = length;
    std::string result_str = "";
    while (length_left - dependencies->front().size()  > 0) {
      std::string dependency = dependencies->front();
      dependencies->pop_front();
      cumulative_size_read += dependency.length();
      length_left -= dependency.length();
      result_str += dependency + "\n";
    }

    /* Pack result string to uint8_t format. */
    char *cstr = new char[result_str.length() + 1];
    std::strcpy(cstr, result_str.c_str());
    buf = reinterpret_cast<uint8_t *>(cstr);
    return cumulative_size_read + 1; // Account for the NULL terminal.
  } else {
    return 0;
  }
}

} // namespace shrpx
