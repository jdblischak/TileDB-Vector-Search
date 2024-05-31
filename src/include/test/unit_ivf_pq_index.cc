/**
 * @file   unit_ivf_pq_index.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2024 TileDB, Inc.
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

#include <catch2/catch_all.hpp>
#include "index/ivf_pq_index.h"
#include "test/utils/array_defs.h"
#include "test/utils/gen_graphs.h"
#include "test/utils/query_common.h"

struct dummy_pq_index {
  using feature_type = float;
  using flat_vector_feature_type = feature_type;
  using id_type = int;
  using indices_type = int;
  using centroid_feature_type = float;
  using pq_code_type = uint8_t;
  using pq_vector_feature_type = pq_code_type;
  using score_type = float;

  auto dimensions() const {
    return 128;
  }
  auto num_subspaces() const {
    return 16;
  }
  auto num_clusters() const {
    return 256;
  }
  auto sub_dimensions() const {
    return 8;
  }
  auto bits_per_subspace() const {
    return 8;
  }
};

void debug_flat_ivf_centroids(auto& index) {
  std::cout << "\nDebug Centroids:\n" << std::endl;
  for (size_t j = 0; j < index.get_flat_ivf_centroids().num_rows(); ++j) {
    for (size_t i = 0; i < index.get_flat_ivf_centroids().num_cols(); ++i) {
      std::cout << index.get_flat_ivf_centroids()(j, i) << " ";
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

TEST_CASE("default construct two", "[ivf_pq_index]") {
  ivf_pq_index<float, uint32_t, uint32_t> x;
  ivf_pq_index<float, uint32_t, uint32_t> y;
  CHECK(x.compare_cached_metadata(y));
  CHECK(y.compare_cached_metadata(x));
}

TEST_CASE("test kmeans initializations", "[ivf_index][init]") {
  const bool debug = false;

  std::vector<float> data = {8, 6, 7, 5, 3, 3, 7, 2, 1, 4, 1, 3, 0, 5, 1, 2,
                             9, 9, 5, 9, 2, 0, 2, 7, 7, 9, 8, 6, 7, 9, 6, 6};

  ColMajorMatrix<float> training_data(4, 8);
  std::copy(begin(data), end(data), training_data.data());

  auto index = ivf_pq_index<float, uint32_t, uint32_t>(
      /*4,*/ 3, 2, 10, 1e-4);

  index.set_flat_ivf_centroids(ColMajorMatrix<float>(4, 3));

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

  CHECK(index.get_flat_ivf_centroids().num_cols() == 3);
  CHECK(index.get_flat_ivf_centroids().num_rows() == 4);

  // debug_centroids(index);

  for (size_t i = 0; i < index.get_flat_ivf_centroids().num_cols() - 1; ++i) {
    for (size_t j = i + 1; j < index.get_flat_ivf_centroids().num_cols(); ++j) {
      CHECK(!std::equal(
          index.get_flat_ivf_centroids()[i].begin(),
          index.get_flat_ivf_centroids()[i].end(),
          index.get_flat_ivf_centroids()[j].begin()));
    }
  }

  size_t outer_counts = 0;
  for (size_t i = 0; i < index.get_flat_ivf_centroids().num_cols(); ++i) {
    size_t inner_counts = 0;
    for (size_t j = 0; j < training_data.num_cols(); ++j) {
      inner_counts += std::equal(
          index.get_flat_ivf_centroids()[i].begin(),
          index.get_flat_ivf_centroids()[i].end(),
          training_data[j].begin());
    }
    CHECK(inner_counts == 1);
    outer_counts += inner_counts;
  }
  CHECK(outer_counts == index.get_flat_ivf_centroids().num_cols());
}

TEST_CASE("test kmeans", "[ivf_index][kmeans]") {
  const bool debug = false;

  std::vector<float> data = {8, 6, 7, 5, 3, 3, 7, 2, 1, 4, 1, 3, 0, 5, 1, 2,
                             9, 9, 5, 9, 2, 0, 2, 7, 7, 9, 8, 6, 7, 9, 6, 6};

  ColMajorMatrix<float> training_data(4, 8);
  std::copy(begin(data), end(data), training_data.data());

  auto index = ivf_pq_index<float, size_t, size_t>(
      /*4,*/ 3, 2, 10, 1e-4);

  SECTION("random") {
    if (debug) {
      std::cout << "random" << std::endl;
    }
    index.train_ivf(training_data, kmeans_init::random);
  }

  SECTION("kmeans++") {
    if (debug) {
      std::cout << "kmeans++" << std::endl;
    }
    index.train_ivf(training_data, kmeans_init::kmeanspp);
  }
}

TEST_CASE("debug w/ sk", "[ivf_index]") {
  const bool debug = true;

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
    auto index = ivf_pq_index<float, size_t, size_t>(
        /*sklearn_centroids.num_rows(),*/
        sklearn_centroids.num_cols(),
        2,
        1,
        1e-4);
    index.set_flat_ivf_centroids(sklearn_centroids);
    index.train_ivf(training_data, kmeans_init::none);
    if (debug) {
      debug_flat_ivf_centroids(index);
    }
  }

  SECTION("two iterations") {
    if (debug) {
      std::cout << "two iterations" << std::endl;
    }
    auto index = ivf_pq_index<float, size_t, size_t>(
        /*sklearn_centroids.num_rows(),*/
        sklearn_centroids.num_cols(),
        2,
        2,
        1e-4);
    index.set_flat_ivf_centroids(sklearn_centroids);
    index.train_ivf(training_data, kmeans_init::none);
    if (debug) {
      debug_flat_ivf_centroids(index);
    }
    // debug_centroids(index);
  }

  SECTION("five iterations") {
    if (debug) {
      std::cout << "five iterations" << std::endl;
    }
    auto index = ivf_pq_index<float, size_t, size_t>(
        /* sklearn_centroids.num_rows(), */
        sklearn_centroids.num_cols(),
        2,
        5,
        1e-4);
    index.set_flat_ivf_centroids(sklearn_centroids);
    index.train_ivf(training_data, kmeans_init::none);
    if (debug) {
      debug_flat_ivf_centroids(index);
    }
    // debug_centroids(index);
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
    auto index = ivf_pq_index<float, size_t, size_t>(
        /* sklearn_centroids.num_rows(), */
        sklearn_centroids.num_cols(),
        2,
        5,
        1e-4);
    index.set_flat_ivf_centroids(sklearn_centroids);
    index.train_ivf(training_data, kmeans_init::none);
    if (debug) {
      debug_flat_ivf_centroids(index);
    }
    // debug_centroids(index);
  }

  SECTION("five iterations") {
    if (debug) {
      std::cout << "five iterations" << std::endl;
    }
    auto index = ivf_pq_index<float, size_t, size_t>(
        /* sklearn_centroids.num_rows(), */
        sklearn_centroids.num_cols(),
        2,
        5,
        1e-4);
    index.train_ivf(training_data, kmeans_init::random);
    if (debug) {
      debug_flat_ivf_centroids(index);
    }
    // debug_centroids(index);
  }
}

TEST_CASE("ivf_index write and read", "[ivf_index]") {
  size_t dimension = 128;
  size_t nlist = 100;
  size_t num_subspaces = 16;
  size_t max_iters = 4;
  size_t nprobe = 10;
  size_t k_nn = 10;
  size_t nthreads = 1;

  tiledb::Context ctx;
  tiledb::VFS vfs(ctx);
  std::string ivf_index_uri =
      (std::filesystem::temp_directory_path() / "tmp_ivf_index").string();
  if (vfs.is_dir(ivf_index_uri)) {
    vfs.remove_dir(ivf_index_uri);
  }
  auto training_set = tdbColMajorMatrix<float>(ctx, siftsmall_inputs_uri, 0);
  load(training_set);

  auto idx = ivf_pq_index<float, uint32_t, uint32_t>(
      /*dimension,*/ nlist, num_subspaces, max_iters, nthreads);

  idx.train_ivf(training_set, kmeans_init::kmeanspp);
  idx.add(training_set);

  ivf_index_uri =
      (std::filesystem::temp_directory_path() / "second_tmp_ivf_index")
          .string();
  if (vfs.is_dir(ivf_index_uri)) {
    vfs.remove_dir(ivf_index_uri);
  }
  idx.write_index(ctx, ivf_index_uri);
  auto idx2 = ivf_pq_index<float, uint32_t, uint32_t>(ctx, ivf_index_uri);
  idx2.read_index_infinite();

  CHECK(idx.compare_cached_metadata(idx2));
  CHECK(idx.compare_cluster_centroids(idx2));
  CHECK(idx.compare_flat_ivf_centroids(idx2));
  CHECK(idx.compare_pq_ivf_vectors(idx2));
  CHECK(idx.compare_ivf_index(idx2));
  CHECK(idx.compare_ivf_ids(idx2));
  CHECK(idx.compare_pq_ivf_vectors(idx2));
  CHECK(idx.compare_distance_tables(idx2));
}

TEST_CASE(
    "flat_pq_index: verify pq_encoding and pq_distances with siftsmall",
    "[flat_pq_index]") {
  tiledb::Context ctx;
  auto training_set = tdbColMajorMatrix<siftsmall_feature_type>(
      ctx, siftsmall_inputs_uri, 2500);
  training_set.load();

  auto pq_idx = ivf_pq_index<
      siftsmall_feature_type,
      siftsmall_ids_type,
      siftsmall_indices_type>(20, 16, 50);
  pq_idx.train_ivf(training_set);
  pq_idx.add(training_set);

  SECTION("pq_encoding") {
    auto avg_error = pq_idx.verify_pq_encoding(training_set);
    CHECK(avg_error < 0.08);
  }
  SECTION("pq_distances") {
    auto avg_error = pq_idx.verify_pq_distances(training_set);
    CHECK(avg_error < 0.15);
  }
  SECTION("asymmetric_pq_distances") {
    auto [max_error, avg_error] =
        pq_idx.verify_asymmetric_pq_distances(training_set);
    CHECK(avg_error < 0.08);
  }
  SECTION("symmetric_pq_distances") {
    auto [max_error, avg_error] =
        pq_idx.verify_symmetric_pq_distances(training_set);
    CHECK(avg_error < 0.15);
  }
}

// Current code requires that the number of vectors in the training set be at
// least as large as the number of clusters.
//
#if 0
TEMPLATE_TEST_CASE(
    "query stacked hypercube",
    "[flativf_index]",
    float,
    uint8_t) {
  size_t k_dist = GENERATE(0, 32);
  size_t k_near = k_dist;
  size_t k_far = k_dist;

  auto hypercube0 = build_hypercube<TestType>(k_near, k_far, 0xdeadbeef);
  auto hypercube1 = build_hypercube<TestType>(k_near, k_far, 0xbeefdead);

  auto hypercube2 = ColMajorMatrix<TestType>(6, num_vectors(hypercube0));
  auto hypercube4 = ColMajorMatrix<TestType>(12, num_vectors(hypercube0));

  for (size_t j = 0; j < 3; ++j) {
    for (size_t i = 0; i < num_vectors(hypercube4); ++i) {
      hypercube2(j, i) = hypercube0(j, i);
      hypercube2(j + 3, i) = hypercube1(j, i);

      hypercube4(j, i) = hypercube0(j, i);
      hypercube4(j + 3, i) = hypercube1(j, i);
      hypercube4(j + 6, i) = hypercube0(j, i);
      hypercube4(j + 9, i) = hypercube1(j, i);
    }
  }
  SECTION("nlist = 1") {
    size_t k_nn = 6;
    size_t nlist = 1;

    auto ivf_idx2 = ivf_pq_index<TestType, uint32_t, uint32_t>(
        /*128,*/ nlist, 2, 4, 1.e-4);  // dim nlist maxiter eps nthreads
    ivf_idx2.train_ivf(hypercube2);
    ivf_idx2.add(hypercube2);
    auto ivf_idx4 = ivf_pq_index<TestType, uint32_t, uint32_t>(
        /*128,*/ nlist, 2, 4, 1.e-4);
    ivf_idx4.train_ivf(hypercube4);
    ivf_idx4.add(hypercube4);

    auto top_k_ivf_scores = ColMajorMatrix<float>();
    auto top_k_ivf = ColMajorMatrix<unsigned>();
    auto top_k_scores = ColMajorMatrix<float>();
    auto top_k = ColMajorMatrix<uint64_t>();
    auto query2 = ColMajorMatrix<TestType>();
    auto query4 = ColMajorMatrix<TestType>();

    SECTION("query2/4 = 0...") {
      query2 = ColMajorMatrix<TestType>{{0, 0, 0, 0, 0, 0}};
      query4 = ColMajorMatrix<TestType>{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    }
    SECTION("query2/4 = 127...") {
      query2 = ColMajorMatrix<TestType>{{127, 127, 127, 127, 127, 127}};
      query4 = ColMajorMatrix<TestType>{
          {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}};
    }
    SECTION("query2/4 = 0...") {
      query2 = ColMajorMatrix<TestType>{{0, 0, 0, 127, 127, 127}};
      query4 = ColMajorMatrix<TestType>{
          {0, 0, 0, 0, 0, 0, 127, 127, 127, 127, 127, 127}};
    }
    SECTION("query2/4 = 127...") {
      query2 = ColMajorMatrix<TestType>{{127, 127, 127, 0, 0, 0}};
      query4 = ColMajorMatrix<TestType>{
          {127, 127, 127, 127, 127, 127, 0, 0, 0, 0, 0, 0}};
    }
    SECTION("query2/4 = 127...") {
      query2 = ColMajorMatrix<TestType>{
          {127, 0, 127, 0, 127, 0}, {0, 127, 0, 127, 0, 127}};
      query4 = ColMajorMatrix<TestType>{
          {127, 0, 127, 0, 127, 0, 127, 0, 127, 0, 127, 0},
          {0, 127, 0, 127, 0, 127, 0, 127, 0, 127, 0, 127}};
    }

    std::tie(top_k_scores, top_k) = detail::flat::qv_query_heap(
        hypercube2, query2, k_nn, 1, sum_of_squares_distance{});
    std::tie(top_k_ivf_scores, top_k_ivf) =
        ivf_idx2.query_infinite_ram(query2, k_nn, 1);  // k, nprobe
    size_t intersections0 = count_intersections(top_k_ivf, top_k, k_nn);
    double recall0 = intersections0 / ((double)top_k.num_cols() * k_nn);
    CHECK(intersections0 == k_nn * num_vectors(query2));
    CHECK(recall0 == 1.0);

    std::tie(top_k_scores, top_k) = detail::flat::qv_query_heap(
        hypercube4, query4, k_nn, 1, sum_of_squares_distance{});
    std::tie(top_k_ivf_scores, top_k_ivf) =
        ivf_idx4.query_infinite_ram(query4, k_nn, 1);  // k, nprobe

    size_t intersections1 = (long)count_intersections(top_k_ivf, top_k, k_nn);
    double recall1 = intersections1 / ((double)top_k.num_cols() * k_nn);
    CHECK(intersections1 == k_nn * num_vectors(query4));
    CHECK(recall1 == 1.0);
  }
}
#endif

TEST_CASE("Build index and query in place, infinite", "[ivf_pq_index]") {
  tiledb::Context ctx;
  // size_t nlist = GENERATE(1, 100);
  size_t nlist = 20;
  using s = siftsmall_test_init_defaults;
  using index = ivf_pq_index<s::feature_type, s::id_type, s::px_type>;

  auto init = siftsmall_test_init<index>(ctx, nlist, 16);

  auto&& [nprobe, k_nn, nthreads, max_iter, tolerance] = std::tie(
      init.nprobe, init.k_nn, init.nthreads, init.max_iter, init.tolerance);
  auto&& [_, training_set, query_set, groundtruth_set] = std::tie(
      init.idx, init.training_set, init.query_set, init.groundtruth_set);
  auto idx = init.get_write_read_idx();

  auto top_k_ivf_scores = ColMajorMatrix<float>();
  auto top_k_ivf = ColMajorMatrix<siftsmall_ids_type>();

  SECTION("infinite") {
    INFO("infinite");
    std::tie(top_k_ivf_scores, top_k_ivf) =
        idx.query_infinite_ram(query_set, k_nn, nprobe);
  }

  SECTION("finite") {
    INFO("finite");
    std::tie(top_k_ivf_scores, top_k_ivf) =
        idx.query_finite_ram(query_set, k_nn, nprobe);
  }

  // NOTE: Can be used to debug the results.debug_slice(top_k_ivf, "top_k_ivf");
  // debug_slice(top_k_ivf_scores, "top_k_ivf_scores");
  // debug_slice(groundtruth_set, "groundtruth_set");

  init.verify(top_k_ivf);
}
