#include <assert.h>
#include <dirent.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <tuple>
#include <utility>

#include <nghttp2/nghttp2.h>

#include "dep_reader.h"

namespace shrpx {

DependencyReader::DependencyReader() 
  : dependency_filename_(""),
    on_new_dependency_callback_(NULL), 
    base_dependency_directory_("dependencies") {
}

DependencyReader::DependencyReader(std::string dependency_filename) :
  dependency_filename_(dependency_filename)
{

}

DependencyReader::~DependencyReader() { }

void DependencyReader::SetDependencyTreeFilename(const std::string dependency_filename) {
  dependency_filename_ = dependency_filename;
}

void DependencyReader::Start(const std::string website) {
  // Reads in the dependency tree from the file.
  std::cout << "[dep_reader.cc] In Start" << std::endl;
  dependencies_ = ReadAndGenerateDependencyTree(
      base_dependency_directory_, EscapeURL(website));
}

bool DependencyReader::StartReturningDependencies(const std::string url) {
  std::string escaped_url = RemoveTrailingSlash(url);
  std::cout << "[dep_reader.cc] Start Returning Dependencies for url: " << escaped_url << std::endl;
  if (outstanding_dependencies_to_stream_id_.count(escaped_url) == 0) {
    // The URL doesn't have any dependent resources.
    return false;
  }

  can_start_notifying_upstream_[escaped_url] = true;
  if (!dependencies_.empty()) {
    on_new_dependency_callback_(escaped_url, outstanding_dependencies_to_stream_id_[url]);
  }
  return true;
}

// nghttp2_data_provider DependencyReader::GetDependenciesDataProvider(
//     const std::string url) {
//   assert(outstanding_dependencies_.count(url) > 0);
// 
//   std::cout << "[dep_reader.cc] Creating data provider." << std::endl;
//   nghttp2_data_provider *provider = new nghttp2_data_provider();
//   std::pair<std::string, std::string> *dependency = 
//     new std::pair<std::string, std::string>(outstanding_dependencies_[url].front());
//   provider->source.ptr = reinterpret_cast<void *>(dependency);
//   provider->read_callback = dependency_read_callback;
//   outstanding_dependencies_[url].pop_front();
//   if (outstanding_dependencies_[url].empty()) {
//     outstanding_dependencies_.erase(url);
//   }
//   return *provider;
// }

nghttp2_data_provider DependencyReader::GetDependenciesDataProvider(
    const std::string url) {
  // assert(outstanding_dependencies_.count(url) > 0);

  std::string escaped_url = RemoveTrailingSlash(url);
  if (outstanding_dependencies_.count(escaped_url) > 0) {
    nghttp2_data_provider *provider = new nghttp2_data_provider();
    std::cout << "[dep_reader.cc] Creating data provider." << std::endl;
    std::deque<std::pair<std::string, std::string>> *dependencies = 
      new std::deque<std::pair<std::string, std::string>>(outstanding_dependencies_[escaped_url]);
    provider->source.ptr = reinterpret_cast<void *>(dependencies);
    provider->read_callback = dependency_read_callback;
    // outstanding_dependencies_.erase(url);
    return *provider;
  } else {
    nghttp2_data_provider *provider = NULL;
    return *provider;
  }
}

uint32_t DependencyReader::num_dependencies_remaining(const std::string url) {
  std::string escaped_url = RemoveTrailingSlash(url);
  if (outstanding_dependencies_.count(escaped_url) == 0) {
    return 0;
  }
  return outstanding_dependencies_[escaped_url].size();
}

bool DependencyReader::still_have_dependencies(const std::string url) {
  for (auto it = outstanding_dependencies_.begin(); it != outstanding_dependencies_.end(); ++it) {
    std::cout << "[dep_reader.cc] dependency_key: " << it->first << std::endl;
  }
  std::string escaped_url = RemoveTrailingSlash(url);
  return outstanding_dependencies_.count(escaped_url) > 0;
}

void DependencyReader::RegisterForGettingDependencies(const std::string url,
                                                      int32_t stream_id) {
  std::cout << "[dep_reader.cc] Registering url: " << url << std::endl;
  std::string escaped_url = RemoveTrailingSlash(url);
  if (dependencies_.count(escaped_url) == 0) {
    // There aren't any dependencies for this URL.
    return;
  }

  outstanding_dependencies_[escaped_url] = dependencies_[escaped_url];
  outstanding_dependencies_to_stream_id_[escaped_url] = stream_id;
  can_start_notifying_upstream_[escaped_url] = false;
  std::cout << "[dep_reader.cc] outstanding_dependencies_ size: " << outstanding_dependencies_.size() << std::endl;
  for (auto it = outstanding_dependencies_.begin(); it != outstanding_dependencies_.end(); ++it) {
    std::cout << "[dep_reader.cc] key: " << it->first << " size: " << it->second.size() << std::endl;
  }
}

std::string DependencyReader::EscapeURL(const std::string url) {
  std::string result_url = url;
  if (url.find(kHttpsPrefix) == 0) {
    result_url = url.substr(kHttpsPrefix.length());
  } else if (url.find(kHttpPrefix) == 0) {
    result_url = url.substr(kHttpPrefix.length());
  }
  if (url.find(kWwwPrefix) == 0) {
    result_url = url.substr(kWwwPrefix.length());
  }
  return result_url;
}

std::string DependencyReader::RemoveTrailingSlash(const std::string url) {
  std::string result(url);
  std::string slash("/");
  while (result.substr(result.size() - slash.size()) == "/") {
    result = result.substr(0, result.size() - slash.size());
  }
  return result;
}

//////////////////////////////////////////////////////////////////////////////////
// Methods for constructing the dependency tree.
std::map<std::string, std::deque<std::pair<std::string, std::string>>>
  DependencyReader::ReadAndGenerateDependencyTree(
      const std::string dependency_directory,
      const std::string url) {
  std::cout << "[dep_reader.cc] here (1)" << std::endl;
  auto start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::map<std::string, std::deque<std::pair<std::string, std::string>>> result;
  std::string dependency_tree_filename = dependency_filename_;
  if (dependency_tree_filename.empty()) {
    std::string working_directory_prefix = "/home/vaspol/Research/MobileWebOptimization/page_load_setup/build/bin";
    std::string site_directory = working_directory_prefix + kDelimeter + dependency_directory + kDelimeter;
    std::string dependency_tree_filename = site_directory + kDelimeter + kDependencyTreeFilename;
    std::cout << "[dep_reader.cc] url: " << url << std::endl;
  }

  std::ifstream infile(dependency_tree_filename);
  std::cout << "[dep_reader.cc] dep filename: " << dependency_tree_filename << std::endl;
  std::string line;
  while (std::getline(infile, line)) {
    std::cout << "[dep_reader.cc] dep line: " << line << std::endl;
    auto dependency_line = TokenizeTree(line);
    // std::string origin = std::get<0>(dependency_line);
    std::string origin = RemoveTrailingSlash(std::get<0>(dependency_line));
    std::string parent = RemoveTrailingSlash(std::get<1>(dependency_line));
    std::string dependency = std::get<2>(dependency_line);
    if (result.count(origin) == 0) {
      result.insert(std::make_pair(origin, 
            std::deque<std::pair<std::string, std::string>>()));
    }
    if (dependency != url)
      result[origin].push_back(std::make_pair(parent, dependency));
  }
  for (auto it = result.begin(); it != result.end(); ++it) {
    std::cout << "key: " << it->first << std::endl;
  }
  auto end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::cout << "[DepReader] Dependency Reader generation: " << start << " " << end << std::endl;
  return result;
}

std::tuple<std::string, std::string, std::string> DependencyReader::TokenizeTree(
    std::string line) {
  std::stringstream ss(line);
  std::deque<std::string> tokenized_str;
  std::string token;
  while (getline(ss, token, ' ')) {
    tokenized_str.push_back(token);
  }
  return std::make_tuple(tokenized_str[0], tokenized_str[1], tokenized_str[2]);
}

// ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
//                            uint8_t *buf, size_t length, uint32_t *data_flags,
//                            nghttp2_data_source *source, void *user_data) {
//   auto dependency = static_cast<std::pair<std::string, std::string> *>(source->ptr);
//   std::cout << "[dep_reader.cc] READ CALLBACK for " << dependency->second << std::endl;
//   // std::cout << "after dereferencing" << std::endl;
//   size_t length_left = length;
//   ssize_t parent_length = dependency->first.length();
//   ssize_t dependency_length = dependency->second.length();
//   // std::cout << "before memcpy" << std::endl;
//   /* Pack result string to uint8_t format. */
//   std::string result = dependency->first + "\n" + dependency->second;
//   std::memcpy(buf, result.c_str(), result.length() + 1);
//   // std::cout << "before returning" << std::endl;
//   delete dependency;
//   source->ptr = NULL;
//   return parent_length + dependency_length + 2; // Account for the NULL terminal.
// }

ssize_t dependency_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *user_data) {
  // TODO:
  // - Fix when all the dependencies are already sent.
  std::cout << "[dep_reader] here (1) " << source << std::endl;
  auto dependencies = static_cast<std::deque<std::pair<std::string, std::string>> *>(source->ptr);
  // std::cout << "[dep_reader] here (2) " << dependencies << " len: " << dependencies->size() << std::endl;
  auto dependency = dependencies->front();
  // std::cout << "[dep_reader] here (3)" << std::endl;
  dependencies->pop_front();
  // std::cout << "[dep_reader] here (4)" << std::endl;
  size_t length_left = length;
  ssize_t parent_length = dependency.first.length();
  ssize_t dependency_length = dependency.second.length();
  // std::cout << "before memcpy" << std::endl;
  /* Pack result string to uint8_t format. */
  std::string result = dependency.first + "\n" + dependency.second;
  std::memcpy(buf, result.c_str(), result.length() + 1);
  // Properly handle the flag.
  if (dependencies->size() == 0) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
  }
  // std::cout << "before returning" << std::endl;
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::cout << "[DepReader] read callback " << now << " parent: " << dependency.first << " url: " << dependency.second << std::endl;
  return parent_length + dependency_length + 2; // Account for the NULL terminal.
}

} // namespace shrpx

namespace {
/*void dep_reader_memcpy_helper(uint8_t *dest, const void *src, size_t len) {
  memcpy(dest, src, len);
}*/
} // namespace
