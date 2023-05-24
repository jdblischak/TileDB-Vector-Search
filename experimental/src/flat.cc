/**
 * @file   flat.cc
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
 * Driver program for "flat" feature vector search.  Can read and search
 * from local files in "ANN" format or from simple dense TileDB arrays.
 *
 * The program has a lot of different options to enable exploration of the
 * performance of different formulations of the search algorithms.  It turns
 * out (not surprisingly) that for many of the multi-query problems, that
 * a gemm-based algorithm is fastest.  For other searches, particular with
 * just a small number of query vectors (e.g. 1), a brute-force search is
 * fastest.
 *
 * This program currently uses `sift_db` and `sift_array` structures to
 * hold the data, depending on whether the data comes from local file
 * or from a TileDB array but I have since written some better abstractions.
 * @todo Replace sift_db and sift_array with `tdbMatrix`
 *
 * Originally, I represented sets of feature vectors as a `std::vector` of
 * spans over a single allocation of memory.  The very first approach
 * used a `std::vector` to provide that, but I am gradually migrating
 * to using `stdx::mdspan` instead.  There are very efficient ways to
 * allocate memory and `mdspan` is much more lightweight than a `vector`
 * of `span`.
 *
 * Most of the functionality in this driver should be fairly straightforward
 * to follow.  The search algorithms all do a search, find the indices
 * of the top k matches, and compare those results to ground truth.
 *
 * Determining top k is done in one of two ways.  The "hard way" is to compute
 * all scores between the query and the database vectors and then find the
 * top k scores using `nth_element`.  The "easy way" is to use a priority
 * queue to keep a running list of the top k scores.  The easy way is much
 * faster in the qv and vq cases.  The hard way is currently the only way
 * to do top k in gemm, yet gemm tends to be the fastest.
 *
 * The difference between vq vs qv is the ordering of the two nested loops:
 * vq loops over the database vectors and then the queries, while qv loops
 * over the queries and then the database vectors.  There are some
 * ramifications in terms of resource usage and execution time between the
 * two approaches.
 *
 * With the vector of spans approach, each element of the outer std::vector
 * corresponds to a vector.  There isn't really an orientation per se.
 * I.e., A[i] returns a span comprising the ith vector in A.
 *
 * @todo Replace `sift_db` and `sift_array` with `tdbMatrix`
 * @todo Replace `std::vector` of `span` with `stdx::mdspan`
 * @todo Rewrite all query functions (as possible) to return top_k rather
 * than doing ground truth comparisons.
 *
 */

#include <algorithm>
#include <cmath>
// #include <execution>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <docopt.h>

#include "defs.h"
#include "flat_query.h"
#include "sift_array.h"
#include "sift_db.h"
#include "timer.h"

bool verbose = false;
bool debug = false;
bool global_debug = false;
std::string global_region {"us-east-1"};

static constexpr const char USAGE[] =
    R"(flat: feature vector search with flat index.
  Usage:
      tdb (-h | --help)
      tdb (--db_file FILE | --db_uri URI) (--q_file FILE | --q_uri URI) [--g_file FILE | --g_uri URI]
          [--k NN] [--L2 | --cosine] [--order ORDER][--hardway] [--blocked] [--output_uri URI]
          [--nthreads N] [--nqueries N] [--ndb N] [-d | -v]

  Options:
      -h, --help            show this screen
      --db_file FILE        database file with feature vectors
      --db_uri URI          database URI with feature vectors
      --q_file FILE         query file with feature vectors to search for
      --q_uri URI           query URI with feature vectors to search for
      --g_file FILE         ground truth file
      --g_uri URI           ground true URI
      --output_uri URI      output URI for results
      --k NN                number of nearest neighbors to find [default: 10]
      --L2                  use L2 distance (Euclidean) [default]
      --cosine              use cosine distance
      --jaccard             use Jaccard distance
      --order ORDER         which ordering to do comparisons [default: gemm]
      --blocked             use blocked gemm [default: false]
      --hardway             use hard way to compute distances [default: false]
      --nthreads N          number of threads to use in parallel loops (0 = all) [default: 0]
      --nqueries N          size of queries subset to compare (0 = all) [default: 0]
      --ndb N               size of vectors subset to compare (0 = all) [default: 0]
      -d, --debug           run in debug mode [default: false]
      -v, --verbose         run in verbose mode [default: false]
)";

int main(int argc, char* argv[]) {
  std::vector<std::string> strings(argv + 1, argv + argc);
  auto args = docopt::docopt(USAGE, strings, true);

  if (args["--help"].asBool()) {
    std::cout << USAGE << std::endl;
    return 0;
  }

  global_debug = debug = args["--debug"].asBool();
  verbose = args["--verbose"].asBool();
  auto hardway = args["--hardway"].asBool();

  std::string db_file{};
  std::string db_uri{};
  if (args["--db_file"]) {
    db_file = args["--db_file"].asString();
  } else if (args["--db_uri"]) {
    db_uri = args["--db_uri"].asString();
  } else {
    std::cout << "Must specify either --db_file or --db_uri" << std::endl;
    return 1;
  }

  std::string q_file{};
  std::string q_uri{};
  if (args["--q_file"]) {
    q_file = args["--q_file"].asString();
  } else if (args["--q_uri"]) {
    q_uri = args["--q_uri"].asString();
  } else {
    std::cout << "Must specify either --q_file or --q_uri" << std::endl;
    return 1;
  }

  std::string g_file{};
  std::string g_uri{};

  if (args["--g_file"]) {
    g_file = args["--g_file"].asString();
  } else if (args["--g_uri"]) {
    g_uri = args["--g_uri"].asString();
  }


  size_t k = args["--k"].asLong();
  size_t nthreads = args["--nthreads"].asLong();
  size_t nqueries = args["--nqueries"].asLong();
  size_t ndb = args["--ndb"].asLong();

  if (nthreads == 0) {
    nthreads = std::thread::hardware_concurrency();
  }

  // @todo verify only if debug is set?
  // @todo mix and match files and uris?  (Ultimately only want uris.)
  // @todo other types of input besides sift files
  if (!db_file.empty() && !q_file.empty() && !g_file.empty()) {
    if (db_file == q_file) {
      std::cout << "db_file and q_file must be different" << std::endl;
      return 1;
    }

    ms_timer load_time{"Load database, query, and ground truth"};
    tdbColMajorMatrix<float> db(db_file, ndb);
    tdbColMajorMatrix<float> q(q_file, nqueries);
    tdbColMajorMatrix<int> g(g_file, nqueries);
    load_time.stop();
    std::cout << load_time << std::endl;

    if (!(size(db[0]) == size(q[0]))) {
      throw std::runtime_error(
          "vector sizes do not match " + std::to_string(size(db[0])) + ", " +
          std::to_string(size(q[0])) + ", " + std::to_string(size(g[0])));
    }

    // std::vector<std::vector<int>> top_k(q.num_cols(), std::vector<int>(k, 0));
    //auto top_k = ColMajorMatrix<int>(q.num_cols(), k);
    auto top_k = ColMajorMatrix<int>(k, q.num_cols());
    //std::cout << "Using " << args["--order"].asString() << std::endl;

    /**
     * vq: for each vector in the database, compare with each query vector
     */
    if (args["--order"].asString() == "vq") {
      if (verbose) {
        std::cout << "Using vq loop nesting for query" << std::endl;
        if (hardway) {
          std::cout << "Doing it the hard way" << std::endl;
        }
      }
      query_vq(db, q, g, top_k, k, hardway, nthreads);
    } else if (args["--order"].asString() == "qv") {
      if (verbose) {
        std::cout << "Using qv nesting for query" << std::endl;
        if (hardway) {
          std::cout << "Doing it the hard way" << std::endl;
        }
      }
      query_qv(db, q, g, top_k, k, hardway, nthreads);
    } else if (args["--order"].asString() == "gemm") {
      if (verbose) {
        std::cout << "Using gemm for query" << std::endl;
      }
      if (args["--blocked"].asBool()) {
        std::cout << "Using blocked gemm for query" << std::endl;
        blocked_query_gemm(db, q, g, top_k, k, hardway, nthreads);
      } else {
        query_gemm(db, q, g, top_k, k, hardway, nthreads);
      }
    } else {
      std::cout << "Unknown ordering: " << args["--order"].asString()
                << std::endl;
      return 1;
    }
  } else if (!db_uri.empty() && !q_uri.empty()) {
    if (db_uri == q_uri) {
      std::cout << "db_uri and q_uri must be different" << std::endl;
      return 1;
    }
    // @todo other formats for arrays?

    ms_timer load_time{"Load database, query, and ground truth arrays"};
    tdbColMajorMatrix<float> db(db_uri, ndb);
    tdbColMajorMatrix<float> q(q_uri, nqueries);

    auto g = g_uri.empty() ? ColMajorMatrix<int>(0,0) : tdbColMajorMatrix<int>(g_uri);
    // tdbColMajorMatrix<int> g(g_uri);

    load_time.stop();
    std::cout << load_time << std::endl;

    if (!(size(db[0]) == size(q[0]))) {
      throw std::runtime_error(
          "vector sizes do not match " + std::to_string(size(db[0])) + ", " +
          std::to_string(size(q[0])) + ", " + std::to_string(size(g[0])));
    }

    // std::vector<std::vector<int>> top_k(size(q), std::vector<int>(k, 0));
    auto top_k = ColMajorMatrix<int>(k, q.num_cols());
    std::cout << "Using " << args["--order"].asString() << std::endl;

    /**
     * vq: for each vector in the database, compare with each query vector
     */
    if (args["--order"].asString() == "vq") {
      if (verbose) {
        std::cout << "Using vq loop nesting for query" << std::endl;
        if (hardway) {
          std::cout << "Doing it the hard way" << std::endl;
        }
      }
      query_vq(db, q, g, top_k, k, hardway, nthreads);
    } else if (args["--order"].asString() == "qv") {
      if (verbose) {
        std::cout << "Using qv nesting for query" << std::endl;
        if (hardway) {
          std::cout << "Doing it the hard way" << std::endl;
        }
      }
      query_qv(db, q, g, top_k, k, hardway, nthreads);
    } else if (args["--order"].asString() == "gemm") {

      if (verbose) {
        std::cout << "Using gemm for query" << std::endl;
      }

      if (args["--blocked"].asBool()) {
        std::cout << "Using blocked gemm for query" << std::endl;
        blocked_query_gemm(db, q, g, top_k, k, hardway, nthreads);
      } else {
        query_gemm(db, q, g, top_k, k, hardway, nthreads);
      }
    } else {
      std::cout << "Unknown ordering: " << args["--order"].asString()
                << std::endl;
      return 1;
    }

    if (args["--output_uri"]) {
      auto ground_truth = ColMajorMatrix<int>(top_k.size(), top_k[0].size());
      for (size_t i = 0; i < top_k.size(); ++i) {
        for (size_t j = 0; j < top_k[i].size(); ++j) {
          ground_truth(i, j) = top_k[j][i];
        }
      }
      write_matrix(ground_truth, args["--output_uri"].asString());
    }
  } else {
    std::cout << "Must specify either --db_file, --q_file, and --g_file or "
                 "--db_uri, --q_uri, and --g_uri"
              << std::endl;
    return 1;
  }
}
