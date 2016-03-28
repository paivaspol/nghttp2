#include <assert.h>

#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nghttp2/nghttp2.h>

#include "dep_reader.h"

namespace shrpx {

DependencyReader::DependencyReader()
 :  can_start_notifying_upstream_(false),
    stream_id_(0),
    url_(""),
    on_new_dependency_callback_(NULL),
    on_all_dependencies_discovered_(NULL) {
}

DependencyReader::~DependencyReader() {

}

void DependencyReader::Start() {
  assert(!url_.empty());
  assert(stream_id_ > 0);

  // First stage, read in from file and call the callback functions.
  std::ifstream infile("temp.txt");
  std::string line;
  while (std::getline(infile, line)) {
    dependencies_.push_back(line);
  } 
}

void DependencyReader::StartReturningDependencies() {
  can_start_notifying_upstream_ = true;
  if (!dependencies_.empty()) {
    on_new_dependency_callback_();
    on_all_dependencies_discovered_();
  }
}

std::deque<std::string> *DependencyReader::ReadDependencies() {
  std::deque<std::string> *result = new std::deque<std::string>();
  std::ifstream infile("temp.txt");
  std::string line;
  while (std::getline(infile, line)) {
    result->push_back(line);
  } 
  return result;
}

size_t DependencyReader::GetDependenciesRaw(uint8_t *buf) {
  std::deque<std::string> *dependencies = ReadDependencies();
  if (!dependencies->empty()) {
    std::string result_str = "";
    size_t cumulative_size_read = 0;
    for (std::deque<std::string>::iterator it = dependencies->begin();
        it != dependencies->end(); it++) {
      std::string dependency = *it;
      dependencies->pop_front();
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
  nghttp2_data_provider *provider = new nghttp2_data_provider();
  provider->source.ptr = reinterpret_cast<void *>(ReadDependencies());
  provider->read_callback = dependency_read_callback;
  std::cout << "[dep_reader.cc] Got the data provider." << std::endl;
  return *provider;
}

ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data) {
  std::cout << "[dep_reader.cc] READ CALLBACK" << std::endl;
  auto dependencies = static_cast<std::deque<std::string> *>(source->ptr);
  std::cout << "after dereferencing" << std::endl;
  if (!dependencies->empty()) {
    std::cout << "In not empty" << std::endl;
    /*
     * TODO: Can optimize using a greedy approach where
     * the list is sorted by the length of the dependency URL.
     */
    size_t cumulative_size_read = dependencies->front().size();
    size_t length_left = length;
    std::string result_str = "";
    while (!dependencies->empty() &&
           length_left - dependencies->front().size()  > 0) {
      std::string dependency = dependencies->front();
      // std::cout << "dependency: " << dependency << " dep_len: " << dependency.length() << std::endl;
      std::cout << "[dep_reader.cc] Trying to dereference dependency" << std::endl;
      cumulative_size_read += dependency.length();
      std::cout << "[dep_reader.cc] After trying to dereference dependency" << std::endl;
      length_left -= dependency.length();
      result_str += dependency + "\n";
      std::cout << "[dep_reader.cc] Popping dependencies" << std::endl;
      dependencies->pop_front();
      std::cout << "[dep_reader.cc] After popping dependencies" << std::endl;
    }

    std::cout << "before memcpy" << std::endl;
    /* Pack result string to uint8_t format. */
    std::memcpy(buf, result_str.c_str(), result_str.length() + 1);
    std::cout << "before returning" << std::endl;
    return result_str.length() + 1; // Account for the NULL terminal.
  } else {
    return 0;
  }
}

} // namespace shrpx

namespace {
/*void dep_reader_memcpy_helper(uint8_t *dest, const void *src, size_t len) {
  memcpy(dest, src, len);
}*/
} // namespace
