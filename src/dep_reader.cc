#include <assert.h>

#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

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
    dependencies_.push_back(TokenizeTree(line));
  } 
}

std::pair<std::string, std::string> DependencyReader::TokenizeTree(std::string line) {
  std::stringstream ss(line);
  std::deque<std::string> tokenized_str;
  std::string token;
  while (getline(ss, token, ' ')) {
    tokenized_str.push_back(token);
  }
  return std::make_pair(tokenized_str[0], tokenized_str[1]);
}

void DependencyReader::StartReturningDependencies() {
  can_start_notifying_upstream_ = true;
  if (!dependencies_.empty()) {
    on_new_dependency_callback_();
  }
}

nghttp2_data_provider DependencyReader::GetDependenciesDataProvider() {
  std::cout << "[dep_reader.cc] Creating data provider." << std::endl;
  nghttp2_data_provider *provider = new nghttp2_data_provider();
  std::pair<std::string, std::string> *dependency = 
    new std::pair<std::string, std::string>(dependencies_.front());
  provider->source.ptr = reinterpret_cast<void *>(dependency);
  provider->read_callback = dependency_read_callback;
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
  auto dependency = static_cast<std::pair<std::string, std::string> *>(source->ptr);
  std::cout << "[dep_reader.cc] READ CALLBACK for " << dependency->second << std::endl;
  // std::cout << "after dereferencing" << std::endl;
  size_t length_left = length;
  ssize_t parent_length = dependency->first.length();
  ssize_t dependency_length = dependency->second.length();
  // std::cout << "before memcpy" << std::endl;
  /* Pack result string to uint8_t format. */
  std::string result = dependency->first + "\n" + dependency->second;
  std::memcpy(buf, result.c_str(), result.length() + 1);
  // std::cout << "before returning" << std::endl;
  delete dependency;
  source->ptr = NULL;
  return parent_length + dependency_length + 2; // Account for the NULL terminal.
}

} // namespace shrpx

namespace {
/*void dep_reader_memcpy_helper(uint8_t *dest, const void *src, size_t len) {
  memcpy(dest, src, len);
}*/
} // namespace
