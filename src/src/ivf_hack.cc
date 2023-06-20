/**
 * @file   ivf_hack.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2023 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * Driver program for experimenting with algorithms and data structures
 * for kmeans.
 *
 * The program can operate in one of two modes.
 *
 * 1) It takes a set of feature vectors and a set of centroid
 * vectors and creates a new set of feature vectors partitioned according
 * to their nearest centroid.  I then writes the partitioned vectors,
 * the partition index and a vector of the original vector IDs to disk.
 *
 * 2) Given a query vector, it finds the set of nearest centroids and
 * then searches the partitions corresponding to those centroids
 * for the nearest neighbors.
 *
 * @todo This should probably be broken into smaller functions.
 * @todo We need to add a good dose of parallelism.
 * @todo We need to add accuracy reporting as well as QPS.
 */

#include <algorithm>
#include <cmath>
#include <filesystem>
// #include <format>     // Not suppored by Apple clang
// #include <execution>  // Not suppored by Apple clang

#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <docopt.h>

#include "config.h"
#include "defs.h"
#include "ivf_query.h"
#include "linalg.h"
#include "stats.h"
#include "utils/logging.h"
#include "utils/timer.h"
#include "utils/utils.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

bool global_verbose = false;
bool global_debug = false;

#include <cstdint>

/**
 * Specify some types for the demo.  For now the types associated with the
 * vector db to be queried are hard-coded.
 */
#if 1
using db_type = uint8_t;
#else
using db_type = float;
#endif

using groundtruth_type = int32_t;
using centroids_type = float;
using shuffled_ids_type = uint64_t;
using indices_type = uint64_t;

static constexpr const char USAGE[] =
    R"(ivf_hack: demo hack feature vector search with kmeans index.
Usage:
    ivf_hack (-h | --help)
    ivf_hack --db_uri URI --centroids_uri URI --index_uri URI --parts_uri URI --ids_uri URI --query_uri URI
            [--groundtruth_uri URI] [--output_uri URI] [--k NN][--nprobe NN] [--nqueries NN]
            [--alg ALGO] [--finite] [--blocksize NN] [--nth]
            [--nthreads NN] [--region REGION] [--log FILE] [-d] [-v]

Options:
    -h, --help            show this screen
    --db_uri URI          database URI with feature vectors
    --centroids_uri URI   URI with centroid vectors
    --index_uri URI       URI with the paritioning index
    --parts_uri URI       URI with the partitioned data
    --ids_uri URI         URI with original IDs of vectors
    --query_uri URI       URI storing query vectors
    --groundtruth_uri URI URI storing ground truth vectors
    --output_uri URI      URI to store search results
    --k NN                number of nearest neighbors to search for [default: 10]
    --nprobe NN           number of centroid partitions to use [default: 100]
    --nqueries NN         number of query vectors to use (0 = all) [default: 0]
    --alg ALGO            which algorithm to use for query [default: qv_heap]
    --finite              use finite RAM (out of core) algorithm [default: false]
    --blocksize NN        number of vectors to process in an out of core block (0 = all) [default: 0]
    --nth                 use nth_element for top k [default: false]
    --nthreads NN         number of threads to use (0 = all) [default: 0]
    --region REGION       AWS S3 region [default: us-east-1]
    --log FILE            log info to FILE (- for stdout)
    -d, --debug           run in debug mode [default: false]
    -v, --verbose         run in verbose mode [default: false]
)";

int main(int argc, char* argv[]) {
  std::vector<std::string> strings(argv + 1, argv + argc);
  auto args = docopt::docopt(USAGE, strings, true);

  auto centroids_uri = args["--centroids_uri"].asString();
  auto db_uri = args["--db_uri"].asString();
  auto nthreads = args["--nthreads"].asLong();
  if (nthreads == 0) {
    nthreads = std::thread::hardware_concurrency();
  }
  global_debug = args["--debug"].asBool();
  global_verbose = args["--verbose"].asBool();

  auto part_uri = args["--parts_uri"].asString();
  auto index_uri = args["--index_uri"].asString();
  auto id_uri = args["--ids_uri"].asString();
  size_t nprobe = args["--nprobe"].asLong();
  size_t k_nn = args["--k"].asLong();
  auto query_uri = args["--query_uri"] ? args["--query_uri"].asString() : "";
  auto nqueries = (size_t)args["--nqueries"].asLong();
  auto blocksize = (size_t)args["--blocksize"].asLong();
  bool nth = args["--nth"].asBool();
  auto algorithm = args["--alg"].asString();
  bool finite = args["--finite"].asBool();

  float recall{0.0f};
  json recalls;
  tiledb::Context ctx;

  {
    scoped_timer _("query_time");

    if (is_local_array(centroids_uri) &&
        !std::filesystem::exists(centroids_uri)) {
      std::cerr << "Error: centroids URI does not exist: "
                << args["--centroids_uri"] << std::endl;
      return 1;
    }

    auto centroids = tdbColMajorMatrix<centroids_type>(ctx, centroids_uri);
    debug_matrix(centroids, "centroids");

    // Find the top k nearest neighbors accelerated by kmeans and do some
    // reporting

    // @todo Encapsulate these arrays in a class
    // auto shuffled_db = tdbColMajorMatrix<shuffled_db_type>(part_uri);
    // auto shuffled_ids = read_vector<shuffled_ids_type>(id_uri);
    // debug_matrix(shuffled_db, "shuffled_db");
    // debug_matrix(shuffled_ids, "shuffled_ids");

    auto indices = read_vector<indices_type>(ctx, index_uri);
    debug_matrix(indices, "indices");

    auto q =
        tdbColMajorMatrix<db_type, shuffled_ids_type>(ctx, query_uri, nqueries);
    debug_matrix(q, "q");

    auto top_k = [&]() {
      if (finite) {
        return detail::ivf::
            qv_query_heap_finite_ram<db_type, shuffled_ids_type>(
                ctx,
                part_uri,
                centroids,
                q,
                indices,
                id_uri,
                nprobe,
                k_nn,
                blocksize,
                nth,
                nthreads);
      } else {
        return detail::ivf::
            qv_query_heap_infinite_ram<db_type, shuffled_ids_type>(
                ctx,
                part_uri,
                centroids,
                q,
                indices,
                id_uri,
                nprobe,
                k_nn,
                nth,
                nthreads);
      }
    }();

    debug_matrix(top_k, "top_k");

    if (args["--groundtruth_uri"]) {
      auto groundtruth_uri = args["--groundtruth_uri"].asString();

      auto groundtruth =
          tdbColMajorMatrix<groundtruth_type>(ctx, groundtruth_uri, nqueries);

      if (global_debug) {
        std::cout << std::endl;

        debug_matrix(groundtruth, "groundtruth");
        debug_slice(groundtruth, "groundtruth");

        std::cout << std::endl;
        debug_matrix(top_k, "top_k");
        debug_slice(top_k, "top_k");

        std::cout << std::endl;
      }

      size_t total_intersected{0};
      size_t total_groundtruth = top_k.num_cols() * top_k.num_rows();
      for (size_t i = 0; i < top_k.num_cols(); ++i) {
        std::sort(begin(top_k[i]), end(top_k[i]));
        std::sort(begin(groundtruth[i]), begin(groundtruth[i]) + k_nn);
        debug_matrix(top_k, "top_k");
        debug_slice(top_k, "top_k");
        total_intersected += std::set_intersection(
            begin(top_k[i]),
            end(top_k[i]),
            begin(groundtruth[i]),
            end(groundtruth[i]),
            counter{});
      }

      recall = ((float)total_intersected) / ((float)total_groundtruth);
      std::cout << "# total intersected = " << total_intersected << " of "
                << total_groundtruth << " = "
                << "R@" << k_nn << " of " << recall << std::endl;
    }

    if (args["--output_uri"]) {
      auto output = ColMajorMatrix<int32_t>(top_k.num_rows(), top_k.num_cols());
      for (size_t i = 0; i < top_k.num_rows(); ++i) {
        for (size_t j = 0; j < top_k.num_cols(); ++j) {
          output(i, j) = top_k(i, j);
        }
      }

      write_matrix(ctx, output, args["--output_uri"].asString());
    }
  }

  // Quick and dirty way to get query info in summarizable form
  if (true || global_verbose) {
    std::cout << "# [ Repo ]: " << GIT_REPO_NAME << " @ " << GIT_BRANCH
              << std::endl;

    char tag = 'A';
    std::map<std::string, std::string> toc;

    if (true) {
      std::cout << std::setw(5) << "-|-";
      std::cout << std::setw(12) << "Algorithm";
      std::cout << std::setw(9) << "Queries";
      std::cout << std::setw(8) << "nprobe";
      std::cout << std::setw(8) << "k_nn";
      std::cout << std::setw(8) << "thrds";
      std::cout << std::setw(8) << "recall";

      auto units = std::string(" (s)");
      for (auto& timers :
           {_timing_data.get_timer_names(), _memory_data.get_usage_names()}) {
        for (auto& timer : timers) {
          std::string text;
          if (size(timer) < 3) {
            text = timer;
          } else {
            std::string key = "[" + std::string(1, tag) + "]";
            toc[key] = timer + units;
            ++tag;
            text = key;
          }
          std::cout << std::setw(12) << text;
        }
        units = std::string(" (MiB)");  // copilot scares me
      }

      std::cout << std::endl;
    }

    auto original_precision = std::cout.precision();

    std::cout << std::setw(5) << "-|-";
    std::cout << std::setw(12) << algorithm;
    std::cout << std::setw(9) << nqueries;
    std::cout << std::setw(8) << nprobe;
    std::cout << std::setw(8) << k_nn;
    std::cout << std::setw(8) << nthreads;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << std::setw(8) << recall;

    std::cout.precision(original_precision);
    std::cout << std::fixed << std::setprecision(3);
    auto timers = _timing_data.get_timer_names();
    for (auto& timer : timers) {
      auto ms =
          _timing_data.get_entries_summed<std::chrono::microseconds>(timer);
      if (ms < 1000) {
        std::cout << std::fixed << std::setprecision(6);
      } else if (ms < 10000) {
        std::cout << std::fixed << std::setprecision(5);
      } else if (ms < 100000) {
        std::cout << std::fixed << std::setprecision(4);
      } else {
        std::cout << std::fixed << std::setprecision(3);
      }
      std::cout << std::setw(12) << ms / 1000000.0;
    }

    std::cout << std::fixed << std::setprecision(0);

    auto usages = _memory_data.get_usage_names();
    for (auto& usage : usages) {
      auto mem = _memory_data.get_entries_summed(usage);
      if (mem < 1) {
        std::cout << std::fixed << std::setprecision(3);
      } else if (mem < 10) {
        std::cout << std::fixed << std::setprecision(2);
      } else if (mem < 100) {
        std::cout << std::fixed << std::setprecision(1);
      } else {
        std::cout << std::fixed << std::setprecision(0);
      }
      std::cout << std::setw(12) << _memory_data.get_entries_summed(usage);
    }
    std::cout << std::endl;
    std::cout << std::setprecision(original_precision);

    for (auto& t : toc) {
      std::cout << t.first << ": " << t.second << std::endl;
    }
  }

#if 0
  if (args["--log"]) {
    auto timings = get_timings();
    auto program_args = args_log(args);
    auto config = config_log(argv[0]);

    json log_log = {
        {"Config", config},
        {"Args", program_args},
        {"Recalls", recalls},
        {"Times", timings}};

    if (args["--log"].asString() == "-") {
      std::cout << log_log.dump(2) << std::endl;
    } else {
      std::ofstream outfile(args["--log"].asString(), std::ios_base::app);
      outfile << log_log.dump(2) << std::endl;
    }
  }
#endif
}
