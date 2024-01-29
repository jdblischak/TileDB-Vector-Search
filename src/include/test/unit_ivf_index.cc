/**
 * @file   unit_ivf_index.cc
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
 */

#include <catch2/catch_all.hpp>

#include <fstream>
#include <iostream>
#include <vector>

#include "../ivf_index.h"
#include "../linalg.h"

TEST_CASE("ivf_index: test test", "[ivf_index]") {
  REQUIRE(true);
}

// kmeans and kmeans indexing still WIP

void debug_centroids(auto& index) {
  std::cout << "\nDebug Centroids:\n" << std::endl;
  for (size_t j = 0; j < index.get_centroids().num_rows(); ++j) {
    for (size_t i = 0; i < index.get_centroids().num_cols(); ++i) {
      std::cout << index.get_centroids()(j, i) << " ";
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

TEST_CASE("ivf_index: test kmeans initializations", "[ivf_index][init]") {
  const bool debug = false;

  std::vector<float> data = {8, 6, 7, 5, 3, 3, 7, 2, 1, 4, 1, 3, 0, 5, 1, 2,
                             9, 9, 5, 9, 2, 0, 2, 7, 7, 9, 8, 6, 7, 9, 6, 6};

  ColMajorMatrix<float> training_data(4, 8);
  std::copy(begin(data), end(data), training_data.data());

  auto index = kmeans_index<float, uint32_t, uint32_t>(
      4, 3, 10, 1e-4, 1, Catch::rngSeed());

  SECTION("random") {
    if (debug) {
      std::cout << "random" << std::endl;
    }
    index.kmeans_random_init(training_data);
  }

  SECTION("kmeans++") {
    if (debug) {
      std::cout << "kmeans++" << std::endl;
    }
    index.kmeans_pp(training_data);
  }

  CHECK(index.get_centroids().num_cols() == 3);
  CHECK(index.get_centroids().num_rows() == 4);

  debug_centroids(index);

  for (size_t i = 0; i < index.get_centroids().num_cols() - 1; ++i) {
    for (size_t j = i + 1; j < index.get_centroids().num_cols(); ++j) {
      CHECK(!std::equal(
          index.get_centroids()[i].begin(),
          index.get_centroids()[i].end(),
          index.get_centroids()[j].begin()));
    }
  }

  size_t outer_counts = 0;
  for (size_t i = 0; i < index.get_centroids().num_cols(); ++i) {
    size_t inner_counts = 0;
    for (size_t j = 0; j < training_data.num_cols(); ++j) {
      inner_counts += std::equal(
          index.get_centroids()[i].begin(),
          index.get_centroids()[i].end(),
          training_data[j].begin());
    }
    CHECK(inner_counts == 1);
    outer_counts += inner_counts;
  }
  CHECK(outer_counts == index.get_centroids().num_cols());
}

TEST_CASE("ivf_index: test kmeans", "[ivf_index][kmeans]") {
  const bool debug = false;

  std::vector<float> data = {8, 6, 7, 5, 3, 3, 7, 2, 1, 4, 1, 3, 0, 5, 1, 2,
                             9, 9, 5, 9, 2, 0, 2, 7, 7, 9, 8, 6, 7, 9, 6, 6};

  ColMajorMatrix<float> training_data(4, 8);
  std::copy(begin(data), end(data), training_data.data());

  auto index =
      kmeans_index<float, size_t, size_t>(4, 3, 10, 1e-4, 1, Catch::rngSeed());

  SECTION("random") {
    if (debug) {
      std::cout << "random" << std::endl;
    }
    index.train(training_data, kmeans_init::random);
  }

  SECTION("kmeans++") {
    if (debug) {
      std::cout << "kmeans++" << std::endl;
    }
    index.train(training_data, kmeans_init::kmeanspp);
  }

  // Test???

  // debug_centroids(index);
}

TEST_CASE("ivf_index: debug w/ sk", "[ivf_index]") {
  const bool debug = false;

  ColMajorMatrix<float> training_data{
      {1.0573647, 5.082087},
      {-6.229642, -1.3590931},
      {0.7446737, 6.3828287},
      {-7.698864, -3.0493321},
      {2.1362762, -4.4448104},
      {1.04019, -4.0389647},
      {0.38996044, 5.7235265},
      {1.7470839, -4.717076}};
  ColMajorMatrix<float> queries{{-7.3712273, -1.1178735}};
  ColMajorMatrix<float> sklearn_centroids{
      {-6.964253, -2.2042127}, {1.6411834, -4.400284}, {0.7306664, 5.7294807}};

  SECTION("one iteration") {
    if (debug) {
      std::cout << "one iteration" << std::endl;
    }
    auto index = kmeans_index<float, size_t, size_t>(
        sklearn_centroids.num_rows(),
        sklearn_centroids.num_cols(),
        1,
        1e-4,
        1,
        Catch::rngSeed());
    index.set_centroids(sklearn_centroids);
    index.train(training_data, kmeans_init::none);
    debug_centroids(index);
  }

  SECTION("two iterations") {
    if (debug) {
      std::cout << "two iterations" << std::endl;
    }
    auto index = kmeans_index<float, size_t, size_t>(
        sklearn_centroids.num_rows(),
        sklearn_centroids.num_cols(),
        2,
        1e-4,
        1,
        Catch::rngSeed());
    index.set_centroids(sklearn_centroids);
    index.train(training_data, kmeans_init::none);
    debug_centroids(index);
  }

  SECTION("five iterations") {
    if (debug) {
      std::cout << "five iterations" << std::endl;
    }
    auto index = kmeans_index<float, size_t, size_t>(
        sklearn_centroids.num_rows(),
        sklearn_centroids.num_cols(),
        5,
        1e-4,
        1,
        Catch::rngSeed());
    index.set_centroids(sklearn_centroids);
    index.train(training_data, kmeans_init::none);
    debug_centroids(index);
  }

  SECTION("five iterations, perturbed") {
    if (debug) {
      std::cout << "five iterations, perturbed" << std::endl;
    }
    for (size_t i = 0; i < sklearn_centroids.num_cols(); ++i) {
      for (size_t j = 0; j < sklearn_centroids.num_rows(); ++j) {
        sklearn_centroids(j, i) *= 0.8;
      }
    }

    sklearn_centroids(0, 0) += 0.25;
    auto index = kmeans_index<float, size_t, size_t>(
        sklearn_centroids.num_rows(),
        sklearn_centroids.num_cols(),
        5,
        1e-4,
        1,
        Catch::rngSeed());
    index.set_centroids(sklearn_centroids);
    index.train(training_data, kmeans_init::none);
    debug_centroids(index);
  }

  SECTION("five iterations") {
    if (debug) {
      std::cout << "five iterations" << std::endl;
    }
    auto index = kmeans_index<float, size_t, size_t>(
        sklearn_centroids.num_rows(),
        sklearn_centroids.num_cols(),
        5,
        1e-4,
        1,
        Catch::rngSeed());
    index.train(training_data, kmeans_init::random);
    debug_centroids(index);
  }
}

// kmeans and kmeans indexing still WIP
#if 0

TEST_CASE("ivf_index: not a unit test per se", "[ivf_index]") {
  tiledb::Context ctx;
  auto A = tdbColMajorMatrix<float>(
      ctx,
      sift_inputs_uri);

  CHECK(A.num_rows() == 128);
  CHECK(A.num_cols() == 10'000);
  auto index =
      kmeans_index<float, uint32_t, size_t>(A.num_rows(), 1000, 10, 1e-4, 8);

  SECTION("kmeans++") {
    index.kmeans_pp(A);
  }
  SECTION("random") {
    index.kmeans_random_init(A);
  }

  index.train_no_init(A);
}

TEST_CASE("ivf_index: also not a unit test per se", "[ivf_index]") {
  tiledb::Context ctx;

  auto A = tdbColMajorMatrix<float>(ctx, sift_inputs_uri);

  CHECK(A.num_rows() == 128);
  CHECK(A.num_cols() == 10'000);
  auto index = kmeans_index<float>(A.num_rows(), 1000, 10, 1e-4, 8);

  //SECTION("kmeans++") {
  //  index.kmeans_pp(A);
  //}
  //SECTION("random") {
//    index.kmeans_random_init(A);
//  }

//  index.train_no_init(A);
//  index.add(A);

}
#endif
