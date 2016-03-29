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
    std::cout << "[dep_reader.cc] Line: " << line << std::endl;
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
  std::cout << "[dep_reader.cc] Creating data provider." << std::endl;
  nghttp2_data_provider *provider = new nghttp2_data_provider();
  std::cout << "[dep_reader.cc] Dependencies front: " << dependencies_.front() << std::endl;
  std::string *dependency = new std::string(dependencies_.front());
  std::cout << "[dep_reader.cc] Creating string instance." << std::endl;
  provider->source.ptr = reinterpret_cast<void *>(dependency);
  provider->read_callback = dependency_read_callback;
  std::cout << "[dep_reader.cc] Got the data provider." << std::endl;
  dependencies_.pop_front();
  return *provider;
}

uint32_t DependencyReader::num_dependencies_remaining() {
  return dependencies_.size();
}

bool DependencyReader::still_have_dependencies() {
  return !dependencies_.empty();
}

ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data) {
  auto dependency = static_cast<std::string *>(source->ptr);
  std::cout << "[dep_reader.cc] READ CALLBACK for " << *dependency << std::endl;
  // std::cout << "after dereferencing" << std::endl;
  size_t length_left = length;
  ssize_t dependency_length = dependency->length();
  // std::cout << "before memcpy" << std::endl;
  /* Pack result string to uint8_t format. */
  std::memcpy(buf, dependency->c_str(), dependency_length + 1);
  // std::cout << "before returning" << std::endl;
  delete dependency;
  source->ptr = NULL;
  return dependency_length + 1; // Account for the NULL terminal.
}

} // namespace shrpx

namespace {
/*void dep_reader_memcpy_helper(uint8_t *dest, const void *src, size_t len) {
  memcpy(dest, src, len);
}*/
} // namespace
